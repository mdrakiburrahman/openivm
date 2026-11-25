#include "core/refresh_locks.hpp"
#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "rules/transactional_delta_capture.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

#include "core/sql_utils.hpp"
#include "core/vector_utils.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <cstdio>

namespace duckdb {

void MutationGate::Lock(const void *owner, bool capture) {
	std::unique_lock<mutex> guard(lock);
	if (active_owner == owner) {
		depth++;
		if (!capture) {
			capture_only_hold = false;
			ownership_epoch++;
		}
		return;
	}
	// A lifecycle owner may be waiting for this transaction's untracked writes.
	// Never wait in the opposite order when a later statement needs the global
	// gate (tracked DML, refresh, or DDL); roll back and retry that transaction.
	if (active_owner && admitted_writers.count(owner)) {
		throw TransactionException("OpenIVM lifecycle conflicts with pending source writes; retry the transaction");
	}
	waiting_owners++;
	condition.wait(guard, [&]() { return active_owner == nullptr; });
	waiting_owners--;
	active_owner = owner;
	depth = 1;
	capture_only_hold = capture;
	if (!capture) {
		ownership_epoch++;
	}
}

void MutationGate::Unlock(const void *owner) {
	std::lock_guard<mutex> guard(lock);
	if (active_owner != owner || depth == 0) {
		// Report accounting errors in release builds without unlocking another owner's gate.
		fprintf(stderr,
		        "[openivm] mutation gate release mismatch: owner=%p active_owner=%p depth=%llu. "
		        "The gate stays held; subsequent OpenIVM mutations on this database will block.\n",
		        owner, active_owner, static_cast<unsigned long long>(depth));
		fflush(stderr);
		D_ASSERT(false);
		return;
	}
	depth--;
	if (depth == 0) {
		if (!capture_only_hold) {
			// The holder's transaction has ended: later lookups see its changes.
			ownership_epoch++;
		}
		active_owner = nullptr;
		// Global and source waiters share this condition variable.
		condition.notify_all();
	}
}

bool MutationGate::TryLock(const void *owner) {
	std::lock_guard<mutex> guard(lock);
	if (active_owner && active_owner != owner) {
		return false;
	}
	active_owner = owner;
	depth++;
	capture_only_hold = false;
	ownership_epoch++;
	return true;
}

optional_idx MutationGate::ObserveOwnershipEpoch() {
	std::lock_guard<mutex> guard(lock);
	if (ownership_writers || (active_owner && !capture_only_hold)) {
		return optional_idx();
	}
	return ownership_epoch;
}

bool MutationGate::OwnershipEpochIs(idx_t epoch) {
	std::lock_guard<mutex> guard(lock);
	return ownership_epoch == epoch;
}

void MutationGate::MarkOwnershipChange(const void *owner) {
	std::lock_guard<mutex> guard(lock);
	D_ASSERT(active_owner == owner);
	capture_only_hold = false;
	ownership_epoch++;
}

void MutationGate::BeginOwnershipWrite() {
	std::lock_guard<mutex> guard(lock);
	ownership_writers++;
	ownership_epoch++;
}

void MutationGate::EndOwnershipWrite() {
	std::lock_guard<mutex> guard(lock);
	D_ASSERT(ownership_writers > 0);
	ownership_writers--;
	ownership_epoch++;
}

bool MutationGate::FindCachedSourceDelta(const string &key, idx_t epoch, CachedSourceDelta &result) {
	std::lock_guard<mutex> guard(lock);
	if (ownership_epoch != epoch || cached_epoch != epoch) {
		return false;
	}
	auto entry = source_deltas.find(key);
	if (entry == source_deltas.end()) {
		return false;
	}
	result = entry->second;
	return true;
}

void MutationGate::CacheSourceDelta(const string &key, idx_t epoch, CachedSourceDelta entry) {
	static constexpr idx_t MAX_CACHED_SOURCE_DELTAS = 4096;
	std::lock_guard<mutex> guard(lock);
	if (ownership_epoch != epoch) {
		return;
	}
	if (cached_epoch != epoch || source_deltas.size() >= MAX_CACHED_SOURCE_DELTAS) {
		source_deltas.clear();
		cached_epoch = epoch;
	}
	source_deltas[key] = std::move(entry);
}

void MutationGate::LockSource(const void *owner, const string &source, idx_t catalog_oid, bool publication) {
	std::unique_lock<mutex> guard(lock);
	auto &entry = sources[catalog_oid][source];
	if (!entry) {
		entry = make_uniq<SourceAdmission>();
	}
	auto &admission = *entry;
	if (publication) {
		D_ASSERT(active_owner == owner);
		if (admission.publisher == owner) {
			admission.publication_depth++;
			return;
		}
		auto ready = [&]() {
			return !admission.publisher &&
			       (admission.readers.empty() || (admission.readers.size() == 1 && admission.readers.count(owner)));
		};
		admission.waiting_publishers++;
		condition.wait(guard, ready);
		admission.waiting_publishers--;
		admission.publisher = owner;
		admission.publication_depth = 1;
		return;
	}
	auto ready = [&]() {
		return admission.publisher == owner ||
		       (!admission.publisher && (!admission.waiting_publishers || admission.readers.count(owner)));
	};
	bool waited = !ready();
	if (waited && admitted_writers.count(owner)) {
		// The publisher can need a different source already held by this writer.
		throw TransactionException("OpenIVM source publication conflicts with pending writes; retry the transaction");
	}
	admission.waiting_readers++;
	condition.wait(guard, ready);
	admission.waiting_readers--;
	admission.readers[owner]++;
	admitted_writers[owner]++;
}

void MutationGate::RecordSourceCommit(const string &source, idx_t catalog_oid, transaction_t commit_id) {
	std::lock_guard<mutex> guard(lock);
	auto &admission = *sources.at(catalog_oid).at(source);
	admission.last_write_commit = MaxValue(admission.last_write_commit, commit_id);
}

bool MutationGate::HasSourceCommitAfter(const string &source, idx_t catalog_oid, VisibilityBound snapshot) {
	std::lock_guard<mutex> guard(lock);
	return sources.at(catalog_oid).at(source)->last_write_commit >= snapshot;
}

void MutationGate::UnlockSource(const void *owner, const string &source, idx_t catalog_oid, bool publication,
                                VisibilityBound oldest_snapshot) {
	std::lock_guard<mutex> guard(lock);
	auto catalog_it = sources.find(catalog_oid);
	D_ASSERT(catalog_it != sources.end());
	if (catalog_it == sources.end()) {
		return;
	}
	auto &catalog_sources = catalog_it->second;
	auto source_it = catalog_sources.find(source);
	D_ASSERT(source_it != catalog_sources.end());
	if (source_it == catalog_sources.end()) {
		return;
	}
	auto &admission = *source_it->second;
	if (publication) {
		D_ASSERT(admission.publisher == owner && admission.publication_depth > 0);
		if (--admission.publication_depth == 0) {
			admission.publisher = nullptr;
		}
	} else {
		auto reader_it = admission.readers.find(owner);
		D_ASSERT(reader_it != admission.readers.end());
		if (reader_it == admission.readers.end()) {
			return;
		}
		auto writer_it = admitted_writers.find(owner);
		D_ASSERT(writer_it != admitted_writers.end());
		if (writer_it == admitted_writers.end()) {
			return;
		}
		auto &readers = reader_it->second;
		if (--readers == 0) {
			admission.readers.erase(owner);
		}
		if (--writer_it->second == 0) {
			admitted_writers.erase(owner);
		}
	}
	if (!admission.publisher && admission.readers.empty() && !admission.waiting_readers &&
	    !admission.waiting_publishers && admission.last_write_commit < oldest_snapshot) {
		catalog_sources.erase(source);
		if (catalog_sources.empty()) {
			sources.erase(catalog_oid);
		}
	}
	condition.notify_all();
}

shared_ptr<MutationGate> RefreshLocks::AcquireGate(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<MutationGate>(MutationGate::ObjectType());
}

MutationLockGuard::MutationLockGuard(ClientContext &context)
    : MutationLockGuard(DatabaseInstance::GetDatabase(context),
                        TransactionalMVLockState::Get(context).GetMutationOwner()) {
}

SourceAdmissionGuard::SourceAdmissionGuard(DatabaseInstance &db, const void *owner_p, Catalog &catalog, string source_p,
                                           bool publication_p)
    : gate(RefreshLocks::AcquireGate(db)), owner(owner_p), source(std::move(source_p)), publication(publication_p),
      source_database(catalog.GetAttached().shared_from_this()) {
	gate->LockSource(owner, source, catalog.GetOid(), publication);
}

SourceAdmissionGuard::~SourceAdmissionGuard() {
	auto oldest_snapshot = source_database->GetCatalog().IsDuckCatalog()
	                           ? DuckTransactionManager::Get(*source_database).LowestVisibilityBound()
	                           : VisibilityBound::IncludingUncommitted();
	gate->UnlockSource(owner, source, source_database->GetCatalog().GetOid(), publication, oldest_snapshot);
}

void SourceAdmissionGuard::RecordCommit() {
	D_ASSERT(!publication);
	if (executed && source_database->GetCatalog().IsDuckCatalog()) {
		// Native transaction objects may already have been reclaimed when the
		// commit callback runs. The manager supplies a safe upper bound instead.
		// Record it before releasing admission, so publication cannot miss a writer.
		auto commit_id = DuckTransactionManager::Get(*source_database).GetLastCommit();
		gate->RecordSourceCommit(source, source_database->GetCatalog().GetOid(), commit_id);
		OPENIVM_DEBUG_PRINT("[SOURCE ADMISSION] Recorded commit upper bound %llu for %s\n",
		                    static_cast<unsigned long long>(commit_id), source.c_str());
	}
}

bool SourceAdmissionGuard::HasCommitOutsideSnapshot(VisibilityBound snapshot) {
	D_ASSERT(publication);
	return gate->HasSourceCommitAfter(source, source_database->GetCatalog().GetOid(), snapshot);
}

const void *TransactionalMVLockState::GetMutationOwner() {
	lock_guard<mutex> guard(state_lock);
	return mutation_owner;
}

TransactionalMVLockState::TransactionalMVLockState(ClientContext &context)
    : gate(RefreshLocks::AcquireGate(DatabaseInstance::GetDatabase(context))), owner(context),
      mutation_owner(&context) {
}

TransactionalMVLockState::~TransactionalMVLockState() {
	// A context destroyed without a transaction callback must not disable caching forever.
	if (ownership_writer) {
		gate->EndOwnershipWrite();
	}
}

void TransactionalMVLockState::TransactionBegin(MetaTransaction &transaction, ClientContext &context) {
	// Source snapshots start lazily, after this observation.
	auto epoch = gate->ObserveOwnershipEpoch();
	lock_guard<mutex> guard(state_lock);
	begin_ownership_epoch = epoch;
}

TransactionalMVLockState &TransactionalMVLockState::Get(ClientContext &context) {
	return *context.registered_state->GetOrCreate<TransactionalMVLockState>("openivm_transactional_mv_locks", context);
}

void TransactionalMVLockState::AcquireMutationLock(bool capture) {
	// Parallel delta-capture workers share this state. Losing a guard here leaks its gate depth.
	lock_guard<mutex> guard(state_lock);
	if (!mutation_guard) {
		mutation_guard = make_uniq<MutationLockGuard>(DatabaseInstance::GetDatabase(owner), mutation_owner, capture);
		OPENIVM_DEBUG_PRINT("[LOCK] acquired database mutation lock owner=%p capture=%d\n", static_cast<void *>(&owner),
		                    capture ? 1 : 0);
	} else if (!capture) {
		// Lifecycle work after tracked DML in the same transaction.
		gate->MarkOwnershipChange(mutation_owner);
	}
}

void TransactionalMVLockState::MarkOwnershipMetadataWrite() {
	lock_guard<mutex> guard(state_lock);
	if (!ownership_writer) {
		ownership_writer = true;
		gate->BeginOwnershipWrite();
		OPENIVM_DEBUG_PRINT("[DELTA OWNERSHIP] Registry write disables cached lookups until transaction end\n");
	}
}

bool TransactionalMVLockState::IsOwnershipRegistry(const string &table_name) {
	return StringUtil::CIEquals(table_name, openivm::SOURCE_DELTA_MAP_TABLE) ||
	       StringUtil::CIEquals(table_name, openivm::DELTA_TABLES_TABLE);
}

string TransactionalMVLockState::SourceDeltaCacheKey(const string &catalog, const string &schema, const string &table) {
	auto &source_catalog = Catalog::GetCatalog(owner, Identifier(catalog));
	if (!source_catalog.IsDuckCatalog()) {
		return string();
	}
	// The catalog version pins the caller's structural snapshot, including its own DDL.
	auto version = source_catalog.GetCatalogVersion(owner);
	if (!version.IsValid()) {
		return string();
	}
	return to_string(source_catalog.GetOid()) + ":" + to_string(version.GetIndex()) + ":" +
	       StringUtil::Lower(SqlUtils::FullName(catalog, schema, table));
}

bool TransactionalMVLockState::FindCachedSourceDelta(const string &catalog, const string &key, optional_idx epoch,
                                                     optional_ptr<TableCatalogEntry> &delta) {
	MutationGate::CachedSourceDelta entry;
	if (key.empty() || !epoch.IsValid() || !gate->FindCachedSourceDelta(key, epoch.GetIndex(), entry)) {
		return false;
	}
	delta = nullptr;
	if (entry.tracked) {
		// Resolve in the caller's transaction; a different incarnation is a miss.
		delta = Catalog::GetEntry<TableCatalogEntry>(
		    owner, QualifiedName(Identifier(catalog), Identifier(entry.schema), Identifier(entry.name)),
		    OnEntryNotFound::RETURN_NULL);
		if (!delta || delta->oid != entry.oid) {
			return false;
		}
	}
	OPENIVM_DEBUG_PRINT("[DELTA OWNERSHIP] Cached lookup %s -> %s\n", key.c_str(),
	                    entry.tracked ? entry.name.c_str() : "untracked");
	return true;
}

void TransactionalMVLockState::CacheSourceDelta(const string &key, optional_idx epoch,
                                                optional_ptr<TableCatalogEntry> delta) {
	if (key.empty() || !epoch.IsValid()) {
		return;
	}
	MutationGate::CachedSourceDelta entry;
	if (delta) {
		entry.tracked = true;
		entry.schema = delta->schema.name.GetIdentifierName();
		entry.name = delta->name.GetIdentifierName();
		entry.oid = delta->oid;
	}
	gate->CacheSourceDelta(key, epoch.GetIndex(), std::move(entry));
}

unique_ptr<SourceAdmissionGuard> TransactionalMVLockState::AcquireSourceWriteAdmission(const string &catalog,
                                                                                       const string &schema,
                                                                                       const string &table) {
	auto source = StringUtil::Lower(SqlUtils::FullName(catalog, schema, table));
	lock_guard<mutex> guard(state_lock);
	if (source_write_admissions.count(source)) {
		return nullptr;
	}
	return make_uniq<SourceAdmissionGuard>(DatabaseInstance::GetDatabase(owner), mutation_owner,
	                                       Catalog::GetCatalog(owner, Identifier(catalog)), std::move(source), false);
}

void TransactionalMVLockState::RetainSourceWriteAdmission(unique_ptr<SourceAdmissionGuard> admission) {
	if (!admission) {
		return;
	}
	lock_guard<mutex> guard(state_lock);
	auto source = admission->Source();
	source_write_admissions.emplace(std::move(source), std::move(admission));
}

optional_ptr<TableCatalogEntry>
TransactionalMVLockState::FindSourceDeltaForWrite(const string &catalog, const string &schema, const string &table) {
	// Reserve only this source while deciding whether capture is needed. First
	// publication excludes untracked writers, without serializing independent DML.
	auto admission = AcquireSourceWriteAdmission(catalog, schema, table);
	optional_idx begin_epoch;
	{
		lock_guard<mutex> guard(state_lock);
		begin_epoch = begin_ownership_epoch;
	}
	// No ownership change started since this transaction began: its snapshot
	// and the committed state agree, as for any transaction at this epoch.
	auto key = SourceDeltaCacheKey(catalog, schema, table);
	optional_ptr<TableCatalogEntry> delta_table;
	bool cached = FindCachedSourceDelta(catalog, key, begin_epoch, delta_table);
	auto lookup_epoch = cached ? begin_epoch : gate->ObserveOwnershipEpoch();
	if (!cached) {
		// Waiting does not advance the caller's metadata snapshot. Resolve committed
		// ownership as well as its own writes, without adopting tables by name.
		delta_table = RefreshMetadata::FindSourceDeltaForMutation(owner, catalog, schema, table);
		CacheSourceDelta(key, begin_epoch, delta_table);
	}
	if (!delta_table) {
		RetainSourceWriteAdmission(std::move(admission));
		return nullptr;
	}
	// CREATE holds the global gate before waiting for source admissions. Drop
	// this temporary admission before waiting on that gate, then recheck because
	// ownership may have changed in the gap. Prior untracked writes stay reserved.
	admission.reset();
	AcquireMutationLock(true);
	// Keep committed writes known even if the final consumer drops its delta
	// before a transaction with an older source snapshot attempts a new CREATE.
	RetainSourceWriteAdmission(AcquireSourceWriteAdmission(catalog, schema, table));
	if (lookup_epoch.IsValid() && gate->OwnershipEpochIs(lookup_epoch.GetIndex())) {
		// No ownership change started or ended in the gap: the recheck would read
		// the same committed state.
		return delta_table;
	}
	OPENIVM_DEBUG_PRINT("[DELTA OWNERSHIP] Ownership changed while admitting %s.%s.%s; rechecking\n", catalog.c_str(),
	                    schema.c_str(), table.c_str());
	delta_table = RefreshMetadata::FindSourceDeltaForMutation(owner, catalog, schema, table);
	CacheSourceDelta(key, begin_epoch, delta_table);
	return delta_table;
}

void TransactionalMVLockState::MarkSourceWriteExecuted(const string &catalog, const string &schema, const string &table,
                                                       const string &planned_delta) {
	auto source = StringUtil::Lower(SqlUtils::FullName(catalog, schema, table));
	{
		lock_guard<mutex> guard(state_lock);
		auto entry = source_write_admissions.find(source);
		if (entry != source_write_admissions.end()) {
			entry->second->MarkExecuted();
			return;
		}
	}
	// A cached prepared plan can execute in a later transaction without another
	// optimizer pass. Reacquire admission and verify that its capture target still
	// matches before allowing the native DML sink to execute.
	auto delta = FindSourceDeltaForWrite(catalog, schema, table);
	auto current_delta =
	    delta ? SqlUtils::FullName(delta->catalog.GetName(), delta->schema.name, delta->name) : string();
	if (current_delta != planned_delta) {
		throw TransactionException("OpenIVM source capture changed since preparation; retry the transaction");
	}
	lock_guard<mutex> guard(state_lock);
	source_write_admissions.at(source)->MarkExecuted();
	OPENIVM_DEBUG_PRINT("[SOURCE ADMISSION] Executing source write %s\n", source.c_str());
}

void TransactionalMVLockState::AcquireSourcePublicationLock(const string &catalog, const string &schema,
                                                            const string &table) {
	auto source = StringUtil::Lower(SqlUtils::FullName(catalog, schema, table));
	lock_guard<mutex> guard(state_lock);
	if (source_publication_locks.count(source)) {
		return;
	}
	auto admission = make_uniq<SourceAdmissionGuard>(DatabaseInstance::GetDatabase(owner), mutation_owner,
	                                                 Catalog::GetCatalog(owner, Identifier(catalog)), source, true);
	OPENIVM_DEBUG_PRINT("[LOCK] acquired source publication lock %s owner=%p\n", source.c_str(), mutation_owner);
	source_publication_locks.emplace(std::move(source), std::move(admission));
}

void TransactionalMVLockState::VerifySourceSnapshot(const string &catalog, const string &schema, const string &table) {
	auto &source_catalog = Catalog::GetCatalog(owner, Identifier(catalog));
	if (!source_catalog.IsDuckCatalog()) {
		return;
	}
	auto snapshot = DuckTransaction::Get(owner, source_catalog).GetSnapshotView().visibility_bound;
	auto source = StringUtil::Lower(SqlUtils::FullName(catalog, schema, table));
	lock_guard<mutex> guard(state_lock);
	if (source_publication_locks.at(source)->HasCommitOutsideSnapshot(snapshot)) {
		throw TransactionException("OpenIVM cannot establish source capture from an older source snapshot for "
		                           "'%s'; retry the transaction",
		                           SqlUtils::FullName(catalog, schema, table));
	}
}

vector<unique_ptr<SourceAdmissionGuard>> TransactionalMVLockState::TakeSourcePublicationLocks() {
	lock_guard<mutex> guard(state_lock);
	vector<unique_ptr<SourceAdmissionGuard>> locks;
	for (auto &entry : source_publication_locks) {
		locks.push_back(std::move(entry.second));
	}
	source_publication_locks.clear();
	return locks;
}

void TransactionalMVLockState::RestoreSourcePublicationLocks(vector<unique_ptr<SourceAdmissionGuard>> locks) {
	lock_guard<mutex> guard(state_lock);
	D_ASSERT(source_publication_locks.empty());
	for (auto &admission : locks) {
		auto source = admission->Source();
		source_publication_locks.emplace(std::move(source), std::move(admission));
	}
}

void TransactionalMVLockState::SetMutationOwner(const void *owner_token) {
	lock_guard<mutex> guard(state_lock);
	if ((mutation_guard || !source_write_admissions.empty() || !source_publication_locks.empty()) &&
	    mutation_owner != owner_token) {
		throw InternalException("OpenIVM cannot change mutation ownership after acquiring the mutation lock");
	}
	mutation_owner = owner_token;
}

void TransactionalMVLockState::DeferDeltaCleanup(vector<string> statements) {
	lock_guard<mutex> guard(state_lock);
	for (auto &statement : statements) {
		AddUnique(deferred_delta_cleanup, std::move(statement));
	}
}

void TransactionalMVLockState::TransactionCommit(MetaTransaction &transaction, ClientContext &context) {
	vector<string> cleanup;
	vector<RefreshMetadata::DeltaSource> source_cleanup;
	{
		lock_guard<mutex> guard(state_lock);
		for (auto &entry : source_write_admissions) {
			entry.second->RecordCommit();
		}
		cleanup.swap(deferred_delta_cleanup);
		source_cleanup.swap(deferred_source_delta_cleanup);
		source_cleanup.insert(source_cleanup.end(), allocated_source_deltas.begin(), allocated_source_deltas.end());
		allocated_source_deltas.clear();
	}
	CleanupUnusedSourceDeltas(context, source_cleanup, std::move(cleanup));
	Release();
}

void TransactionalMVLockState::CleanupUnusedSourceDeltas(ClientContext &context,
                                                         const vector<RefreshMetadata::DeltaSource> &source_cleanup,
                                                         vector<string> cleanup) {
	if (!cleanup.empty() || !source_cleanup.empty()) {
		// The caller transaction has ended, but the mutation gate still protects
		// the committed ownership check and physical cleanup from another CREATE.
		try {
			Connection con(*context.db);
			Get(*con.context).SetMutationOwner(GetMutationOwner());
			// A later CREATE in the same transaction may have reused a delta that
			// replacement scheduled for cleanup. Check committed ownership again.
			for (auto &source : source_cleanup) {
				if (!RefreshMetadata(con).IsMaterializedViewDelta(source) &&
				    !RefreshMetadata::HasSourceDeltaConsumers(con, source)) {
					cleanup.push_back("DROP TABLE IF EXISTS " + SqlUtils::FullName(source.catalog_name,
					                                                               source.delta_schema_name,
					                                                               source.delta_table_name));
				}
			}
			for (auto &sql : cleanup) {
				auto result = con.Query(sql);
				if (result->HasError()) {
					Printer::Print("OpenIVM transaction ended; external delta cleanup deferred: " + result->GetError());
				}
			}
		} catch (const std::exception &ex) {
			Printer::Print(string("OpenIVM transaction ended; external delta cleanup deferred: ") + ex.what());
		}
	}
}

void TransactionalMVLockState::DeferUnusedSourceDeltaCleanup(const RefreshMetadata::DeltaSource &source) {
	lock_guard<mutex> guard(state_lock);
	deferred_source_delta_cleanup.push_back(source);
}

void TransactionalMVLockState::RecordSourceDeltaAllocation(const RefreshMetadata::DeltaSource &source) {
	lock_guard<mutex> guard(state_lock);
	allocated_source_deltas.push_back(source);
	OPENIVM_DEBUG_PRINT("[DELTA OWNERSHIP] Retained allocation %s.%s.%s until publication\n",
	                    source.catalog_name.c_str(), source.delta_schema_name.c_str(), source.delta_table_name.c_str());
}

vector<RefreshMetadata::DeltaSource> TransactionalMVLockState::TakeSourceDeltaAllocations() {
	lock_guard<mutex> guard(state_lock);
	vector<RefreshMetadata::DeltaSource> allocations;
	allocations.swap(allocated_source_deltas);
	return allocations;
}

void TransactionalMVLockState::CleanupSourceDeltaAllocations(const vector<RefreshMetadata::DeltaSource> &sources) {
	CleanupUnusedSourceDeltas(owner, sources, {});
}

void TransactionalMVLockState::RecordSourceGenerationInvalidation(const string &catalog, const string &schema,
                                                                  const string &table) {
	lock_guard<mutex> guard(state_lock);
	invalidated_source_generations.insert(StringUtil::Lower(SqlUtils::FullName(catalog, schema, table)));
}
bool TransactionalMVLockState::HasInvalidatedSourceGeneration(const string &catalog, const string &schema,
                                                              const string &table) {
	lock_guard<mutex> guard(state_lock);
	return invalidated_source_generations.count(StringUtil::Lower(SqlUtils::FullName(catalog, schema, table))) != 0;
}

void TransactionalMVLockState::RecordSourceDeltaDrop(const string &catalog, const string &schema, const string &table) {
	lock_guard<mutex> guard(state_lock);
	dropped_source_deltas.insert(StringUtil::Lower(SqlUtils::FullName(catalog, schema, table)));
}

bool TransactionalMVLockState::HasDroppedSourceDelta(const string &catalog, const string &schema, const string &table) {
	lock_guard<mutex> guard(state_lock);
	return dropped_source_deltas.count(StringUtil::Lower(SqlUtils::FullName(catalog, schema, table))) != 0;
}

optional_ptr<TableCatalogEntry>
TransactionalMVLockState::FindDeferredSourceDelta(const string &catalog, const string &schema, const string &table) {
	lock_guard<mutex> guard(state_lock);
	for (auto &source : deferred_source_delta_cleanup) {
		if (StringUtil::CIEquals(catalog, source.catalog_name) && StringUtil::CIEquals(schema, source.schema_name) &&
		    StringUtil::CIEquals(RefreshMetadata::SourceTableName(source.table_name, catalog, schema),
		                         SqlUtils::DeltaName(table))) {
			// Ownership was removed by an earlier replacement, but this transaction
			// still reserves the physical delta until its post-commit cleanup check.
			return Catalog::GetEntry<TableCatalogEntry>(owner,
			                                            QualifiedName(Identifier(catalog),
			                                                          Identifier(source.delta_schema_name),
			                                                          Identifier(source.delta_table_name)),
			                                            OnEntryNotFound::RETURN_NULL);
		}
	}
	return nullptr;
}

void TransactionalMVLockState::TransactionRollback(MetaTransaction &transaction, ClientContext &context) {
	auto allocations = TakeSourceDeltaAllocations();
	// Existing deltas scheduled for final-consumer cleanup belong to the restored
	// owners after rollback. Only newly helper-committed allocations need checking.
	CleanupUnusedSourceDeltas(context, allocations, {});
	Release();
}

RebindQueryInfo TransactionalMVLockState::OnRebindPreparedStatement(ClientContext &context,
                                                                    BindPreparedStatementCallbackInfo &info,
                                                                    RebindQueryInfo current_rebind) {
	auto &plan = info.prepared_statement.physical_plan;
	if (plan && !info.prepared_statement.properties.IsReadOnly()) {
		AdmitPlanMutations(context, plan->Root());
	}
	return RebindQueryInfo::DO_NOT_REBIND;
}

void TransactionalMVLockState::Release() {
	lock_guard<mutex> guard(state_lock);
	OPENIVM_DEBUG_PRINT("[LOCK] release database mutation lock owner=%p\n", static_cast<void *>(&owner));
	deferred_delta_cleanup.clear();
	deferred_source_delta_cleanup.clear();
	allocated_source_deltas.clear();
	dropped_source_deltas.clear();
	invalidated_source_generations.clear();
	source_write_admissions.clear();
	source_publication_locks.clear();
	mutation_guard.reset();
	if (ownership_writer) {
		ownership_writer = false;
		gate->EndOwnershipWrite();
	}
}

} // namespace duckdb
