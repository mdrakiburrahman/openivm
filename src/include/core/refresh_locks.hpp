#ifndef REFRESH_LOCKS_HPP
#define REFRESH_LOCKS_HPP

#include "duckdb.hpp"
#include "core/refresh_metadata.hpp"
#include "core/scoped_optimizer_settings.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

class AttachedDatabase;
class Catalog;

// Serializes OpenIVM mutations for one database. The gate is re-entrant for one
// logical owner because helper connections can execute on different worker threads.
class MutationGate : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "openivm_mutation_gate";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // The database's mutation gate must never be evicted.
	}

	// A capture lock admits tracked DML only; every other lock may change source
	// delta ownership. A waiter with an interrupt_context stops waiting and throws
	// InterruptException once that context is interrupted or exceeds max_execution_time.
	void Lock(const void *owner, bool capture = false, ClientContext *interrupt_context = nullptr);
	bool TryLock(const void *owner);
	void Unlock(const void *owner);

	// Cached ownership lookups. Ownership rows change only in transactions that
	// write an ownership registry; each is marked from its first registry write
	// until it has ended, bumping the epoch at both ends. Delta table entries are
	// pinned by the catalog version in the cache key and re-resolved (with their
	// oid) in the caller's snapshot. So a lookup made while no registry write is
	// active, before the epoch moves again, reads the committed ownership that
	// every transaction with that catalog version reads. Non-capture gate holds
	// also bump the epoch, but only as a conservative extra: some scoped holds
	// end before their caller's transaction commits.
	struct CachedSourceDelta {
		bool tracked = false;
		string schema;
		string name;
		idx_t oid = 0;
	};
	// Invalid while an ownership change is in progress.
	optional_idx ObserveOwnershipEpoch();
	bool OwnershipEpochIs(idx_t epoch);
	void MarkOwnershipChange(const void *owner);
	void BeginOwnershipWrite();
	void EndOwnershipWrite();
	bool FindCachedSourceDelta(const string &key, idx_t epoch, CachedSourceDelta &result);
	void CacheSourceDelta(const string &key, idx_t epoch, CachedSourceDelta entry);

private:
	friend class MutationGateTestAccess;
	friend class SourceAdmissionGuard;
	// Readers are admitted native writers, tracked or untracked.
	// Publication excludes them only for the qualified source being published.
	struct SourceAdmission {
		transaction_t last_write_commit = 0;
		unordered_map<const void *, idx_t> readers;
		const void *publisher = nullptr;
		idx_t publication_depth = 0;
		idx_t waiting_readers = 0;
		idx_t waiting_publishers = 0;
	};
	void LockSource(const void *owner, const string &source, idx_t catalog_oid, bool publication,
	                ClientContext *interrupt_context);
	void UnlockSource(const void *owner, const string &source, idx_t catalog_oid, bool publication,
	                  VisibilityBound oldest_snapshot);
	void RecordSourceCommit(const string &source, idx_t catalog_oid, transaction_t commit_id);
	bool HasSourceCommitAfter(const string &source, idx_t catalog_oid, VisibilityBound snapshot);
	mutex lock;
	std::condition_variable condition;
	const void *active_owner = nullptr;
	idx_t depth = 0;
	// Integration tests synchronize on an actual wait without timing the caller.
	idx_t waiting_owners = 0;
	bool capture_only_hold = false;
	idx_t ownership_epoch = 0;
	idx_t ownership_writers = 0;
	idx_t cached_epoch = 0;
	unordered_map<string, CachedSourceDelta> source_deltas;
	unordered_map<idx_t, unordered_map<string, unique_ptr<SourceAdmission>>> sources;
	unordered_map<const void *, idx_t> admitted_writers;
};

class RefreshLocks {
public:
	// The database owns the gate; guards retain the exact gate they acquired.
	static shared_ptr<MutationGate> AcquireGate(DatabaseInstance &db);
};

class MutationLockGuard {
	shared_ptr<MutationGate> gate;
	const void *owner;

public:
	explicit MutationLockGuard(ClientContext &owner_p);

	MutationLockGuard(DatabaseInstance &db_p, const void *owner_p, bool capture = false,
	                  ClientContext *interrupt_context = nullptr)
	    : gate(RefreshLocks::AcquireGate(db_p)), owner(owner_p) {
		gate->Lock(owner, capture, interrupt_context);
	}
	~MutationLockGuard() {
		gate->Unlock(owner);
	}
	MutationLockGuard(const MutationLockGuard &) = delete;
	MutationLockGuard &operator=(const MutationLockGuard &) = delete;
	MutationLockGuard(MutationLockGuard &&) = delete;
	MutationLockGuard &operator=(MutationLockGuard &&) = delete;
};

class SourceAdmissionGuard {
public:
	SourceAdmissionGuard(DatabaseInstance &db, const void *owner, Catalog &catalog, string source, bool publication,
	                     ClientContext *interrupt_context = nullptr);
	~SourceAdmissionGuard();
	SourceAdmissionGuard(const SourceAdmissionGuard &) = delete;
	SourceAdmissionGuard &operator=(const SourceAdmissionGuard &) = delete;
	const string &Source() const {
		return source;
	}
	void RecordCommit();
	// Called under the owning transaction state's mutex, only by execution.
	void MarkExecuted() {
		executed = true;
	}
	bool HasCommitOutsideSnapshot(VisibilityBound snapshot);

private:
	shared_ptr<MutationGate> gate;
	const void *owner;
	string source;
	bool publication;
	bool executed = false;
	shared_ptr<AttachedDatabase> source_database;
};

// Native lifecycle and refresh programs are returned as multiple DuckDB
// statements. Retain the mutation gate until the caller transaction ends.
class TransactionalMVLockState : public ClientContextState {
public:
	explicit TransactionalMVLockState(ClientContext &context);
	~TransactionalMVLockState() override;
	static TransactionalMVLockState &Get(ClientContext &context);

	// Lifecycle and refresh callers may change ownership; capture callers only
	// admit tracked DML (see MutationGate::Lock).
	void AcquireMutationLock(bool capture = false);
	// Writes to an ownership registry outside the gate (e.g. direct metadata DML)
	// keep cached ownership lookups disabled until this transaction ends.
	void MarkOwnershipMetadataWrite();
	static bool IsOwnershipRegistry(const string &table_name);
	// Untracked writers retain source admission through commit. Tracked writers
	// release temporary admission before entering the mutation gate, then recheck
	// ownership and retain admission again under that gate.
	unique_ptr<SourceAdmissionGuard> AcquireSourceWriteAdmission(const string &catalog, const string &schema,
	                                                             const string &table);
	void RetainSourceWriteAdmission(unique_ptr<SourceAdmissionGuard> admission);
	optional_ptr<TableCatalogEntry> FindSourceDeltaForWrite(const string &catalog, const string &schema,
	                                                        const string &table);
	void MarkSourceWriteExecuted(const string &catalog, const string &schema, const string &table,
	                             const string &planned_delta);
	void AcquireSourcePublicationLock(const string &catalog, const string &schema, const string &table);
	void VerifySourceSnapshot(const string &catalog, const string &schema, const string &table);
	vector<unique_ptr<SourceAdmissionGuard>> TakeSourcePublicationLocks();
	void RestoreSourcePublicationLocks(vector<unique_ptr<SourceAdmissionGuard>> locks);
	void DeferDeltaCleanup(vector<string> statements);
	//! A refresh program returned to the caller's transaction runs after the refresh pragma returns. Keep
	//! the refresh optimizer overrides until that transaction ends.
	void DisableRefreshOptimizers(ClientContext &context);
	void DeferUnusedSourceDeltaCleanup(const RefreshMetadata::DeltaSource &source);
	// Helper-committed allocations must be reclaimed if the caller fails to publish ownership.
	void RecordSourceDeltaAllocation(const RefreshMetadata::DeltaSource &source);
	vector<RefreshMetadata::DeltaSource> TakeSourceDeltaAllocations();
	void CleanupSourceDeltaAllocations(const vector<RefreshMetadata::DeltaSource> &sources);
	void RecordSourceGenerationInvalidation(const string &catalog, const string &schema, const string &table);
	bool HasInvalidatedSourceGeneration(const string &catalog, const string &schema, const string &table);
	void RecordSourceDeltaDrop(const string &catalog, const string &schema, const string &table);
	bool HasDroppedSourceDelta(const string &catalog, const string &schema, const string &table);
	optional_ptr<TableCatalogEntry> FindDeferredSourceDelta(const string &catalog, const string &schema,
	                                                        const string &table);
	// Reused replay helpers may reassert the same owner while their guard is held.
	void SetMutationOwner(const void *owner_token);
	const void *GetMutationOwner();
	// Let a helper connection act under the caller's mutation ownership.
	static void ShareMutationOwner(ClientContext &from, ClientContext &to) {
		Get(to).SetMutationOwner(Get(from).GetMutationOwner());
	}

	void TransactionBegin(MetaTransaction &transaction, ClientContext &context) override;
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override;
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override;
	// A prepared statement re-executes its cached plan without the optimizer rule that takes
	// the gate for tracked DML; take it here, still before execution locks any table.
	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override;

private:
	void CleanupUnusedSourceDeltas(ClientContext &context, const vector<RefreshMetadata::DeltaSource> &sources,
	                               vector<string> cleanup);
	void Release();
	string SourceDeltaCacheKey(const string &catalog, const string &schema, const string &table);
	bool FindCachedSourceDelta(const string &catalog, const string &key, optional_idx epoch,
	                           optional_ptr<TableCatalogEntry> &delta);
	void CacheSourceDelta(const string &key, optional_idx epoch, optional_ptr<TableCatalogEntry> delta);

	mutex state_lock;
	shared_ptr<MutationGate> gate;
	// Ownership epoch observed when this transaction began; invalid if unknown.
	optional_idx begin_ownership_epoch;
	bool ownership_writer = false;
	unique_ptr<MutationLockGuard> mutation_guard;
	unordered_map<string, unique_ptr<SourceAdmissionGuard>> source_write_admissions;
	unordered_map<string, unique_ptr<SourceAdmissionGuard>> source_publication_locks;
	ClientContext &owner;
	const void *mutation_owner;
	vector<string> deferred_delta_cleanup;
	unique_ptr<ScopedDisabledOptimizers> refresh_optimizers;
	vector<RefreshMetadata::DeltaSource> deferred_source_delta_cleanup;
	vector<RefreshMetadata::DeltaSource> allocated_source_deltas;
	unordered_set<string> dropped_source_deltas;
	unordered_set<string> invalidated_source_generations;
};

} // namespace duckdb

#endif // REFRESH_LOCKS_HPP
