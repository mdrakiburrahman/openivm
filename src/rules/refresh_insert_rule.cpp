#include "rules/refresh_insert_rule.hpp"
#include "compile_facts.hpp"
#include "rules/schema_evolution.hpp"
#include "core/metadata_location.hpp"
#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/parser_ddl.hpp"
#include "core/refresh_metadata.hpp"
#include "core/refresh_locks.hpp"
#include "core/sql_utils.hpp"
#include "rules/column_hider.hpp"
#include "rules/transactional_delta_capture.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_alter.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

class TransactionalHelperUndoState : public ClientContextState {
public:
	static TransactionalHelperUndoState &Get(ClientContext &context) {
		auto state =
		    context.registered_state->GetOrCreate<TransactionalHelperUndoState>("openivm_transactional_helper_undo");
		if (!state->mutation_guard) {
			state->mutation_guard = make_uniq<MutationLockGuard>(context);
		}
		return *state;
	}

	void AddRestoreSQL(string sql) {
		restore_sql.push_back(std::move(sql));
	}

	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		Clear();
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		OPENIVM_DEBUG_PRINT("[TRANSACTIONAL DDL] restoring %zu metadata snapshots\n", restore_sql.size());
		try {
			Connection con(*context.db);
			TransactionalMVLockState::ShareMutationOwner(context, *con.context);
			for (auto it = restore_sql.rbegin(); it != restore_sql.rend(); ++it) {
				con.BeginTransaction();
				auto result = con.Query(*it);
				if (!result->HasError()) {
					result = con.Query("COMMIT");
				} else {
					con.Query("ROLLBACK");
				}
				if (result->HasError()) {
					Printer::Print(
					    "OpenIVM could not restore materialized view metadata after a rolled-back DDL statement: " +
					    result->GetError());
				}
			}
		} catch (std::exception &ex) {
			// Transaction callbacks must always release the mutation gate.
			Printer::Print(string("OpenIVM could not restore materialized view metadata after a rolled-back DDL "
			                      "statement: ") +
			               ex.what());
		}
		Clear();
	}

private:
	void Clear() {
		restore_sql.clear();
		mutation_guard.reset();
	}

	vector<string> restore_sql;
	unique_ptr<MutationLockGuard> mutation_guard;
};

// Restores the snapshotted rows. Native catalogs replace them by primary key; remote SQL
// catalogs have no INSERT OR REPLACE, so the rows sharing their key are deleted and then
// reinserted (the caller runs both in one transaction).
static string BuildRestoreRowsSQL(QueryResult &rows, const string &table_name, const vector<string> &key_columns,
                                  bool native) {
	if (rows.RowCount() == 0) {
		return "";
	}
	auto names = IdentifiersToStrings(rows.GetNames());
	vector<idx_t> key_indexes;
	for (auto &key : key_columns) {
		for (idx_t col = 0; col < names.size(); col++) {
			if (StringUtil::CIEquals(names[col], key)) {
				key_indexes.push_back(col);
				break;
			}
		}
	}
	if (key_indexes.size() != key_columns.size()) {
		throw CatalogException("OpenIVM metadata snapshot of %s lacks its key columns", table_name);
	}
	string columns = SqlUtils::JoinQuotedColumns(names);
	vector<string> row_values;
	vector<string> row_keys;
	for (idx_t row = 0; row < rows.RowCount(); row++) {
		vector<string> cells;
		for (idx_t col = 0; col < rows.ColumnCount(); col++) {
			cells.push_back(rows.Collection().GetValue(col, row).ToSQLString());
		}
		row_values.push_back("(" + StringUtil::Join(cells, ", ") + ")");
		row_keys.push_back("(" +
		                   StringUtil::Join(key_indexes, key_indexes.size(), " AND ",
		                                    [&](idx_t col) {
			                                    return SqlUtils::QuoteIdentifier(names[col]) +
			                                           " IS NOT DISTINCT FROM " +
			                                           rows.Collection().GetValue(col, row).ToSQLString();
		                                    }) +
		                   ")");
	}
	string values = StringUtil::Join(row_values, ", ");
	string keys = StringUtil::Join(row_keys, " OR ");
	if (native) {
		return "INSERT OR REPLACE INTO " + table_name + " (" + columns + ") VALUES " + values;
	}
	return "DELETE FROM " + table_name + " WHERE " + keys + ";\nINSERT INTO " + table_name + " (" + columns +
	       ") VALUES " + values + ";\n";
}

static void RegisterMetadataRestore(ClientContext &context, Connection &con, const string &table_name,
                                    const string &predicate) {
	auto location = RefreshMetadata::CurrentLocation(con);
	auto target = SqlUtils::FullName(location.catalog, location.schema, table_name);
	auto rows = SqlUtils::QueryOrThrow(con, "SELECT * FROM " + target + " WHERE " + predicate,
	                                   "OpenIVM could not snapshot helper metadata: %s");
	auto restore = BuildRestoreRowsSQL(*rows, target, {"view_name"}, location.IsNative());
	if (!restore.empty()) {
		TransactionalHelperUndoState::Get(context).AddRestoreSQL(std::move(restore));
	}
}

static void SnapshotSchemaEvolutionMetadata(ClientContext &context, Connection &con, const MetadataLocation &location) {
	TransactionalMVLockState::ShareMutationOwner(context, *con.context);
	RefreshMetadata::UseLocation(con, location);
	RefreshMetadata(con).SnapshotTransaction(context);
	if (auto state = TransactionalMVMetadataState::TryGet(context)) {
		state->Apply(con);
	}
}

// The physical delta may sit in an internal schema; metadata still keys it by its delta name.
static void AlterDeltaInCallerTransaction(ClientContext &context, AlterTableInfo &source_alter,
                                          const string &catalog_name, TableCatalogEntry &delta) {
	auto delta_alter = source_alter.Copy();
	delta_alter->SetQualifiedName(Identifier(catalog_name), Identifier(delta.schema.name.GetIdentifierName()),
	                              Identifier(delta.name.GetIdentifierName()));
	auto &catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
	MetaTransaction::Get(context).ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::ALTER_TABLE);
	catalog.Alter(context, *delta_alter);
}

// The target table of a source DML operator, or null for any other operator.
static optional_ptr<TableCatalogEntry> GetDMLTarget(LogicalOperator &dml) {
	switch (dml.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
		return &dml.Cast<LogicalInsert>().table;
	case LogicalOperatorType::LOGICAL_DELETE:
		return &dml.Cast<LogicalDelete>().table;
	case LogicalOperatorType::LOGICAL_UPDATE:
		return &dml.Cast<LogicalUpdate>().table;
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return &dml.Cast<LogicalMergeInto>().table;
	default:
		return nullptr;
	}
}

// Wrap the operator in slot with wrapper, returning the wrapper.
template <class T>
static T &WrapOperator(unique_ptr<LogicalOperator> &slot, unique_ptr<T> wrapper) {
	auto &result = *wrapper;
	wrapper->children.push_back(std::move(slot));
	slot = std::move(wrapper);
	return result;
}

// Registry writes anywhere in a plan, not only the statement's top-level DML.
static void MarkOwnershipRegistryWrites(ClientContext &context, LogicalOperator &op) {
	auto target = GetDMLTarget(op);
	if (target && TransactionalMVLockState::IsOwnershipRegistry(target->name.GetIdentifierName())) {
		TransactionalMVLockState::Get(context).MarkOwnershipMetadataWrite();
	}
	for (auto &child : op.children) {
		MarkOwnershipRegistryWrites(context, *child);
	}
}

static optional_ptr<TableCatalogEntry> GetNativeDMLTable(LogicalOperator &dml) {
	auto table = GetDMLTarget(dml);
	if (table->name.GetIdentifierName().empty() ||
	    IncrementalTableNames::IsMaintenanceTable(table->name.GetIdentifierName()) || !table->catalog.IsDuckCatalog()) {
		return nullptr;
	}
	return table;
}

static void PreserveGroupRepresentatives(LogicalUpdate &update) {
	// In-place updates drop SQL-equal -0.0/+0.0 assignments. The zero-sign fix-up
	// assigns the complete row, so it can be replaced even without a key index.
	if (update.update_is_del_and_insert || !update.table.catalog.IsDuckCatalog() ||
	    !StringUtil::StartsWith(update.table.name.GetIdentifierName(), openivm::DATA_TABLE_PREFIX) ||
	    update.columns.size() != update.table.GetColumns().PhysicalColumnCount()) {
		return;
	}
	// Sign-count aliases encode the stored grouping column's position.
	for (auto column : update.columns) {
		auto sign_count = string(openivm::GROUP_SIGN_COUNT_COL_PREFIX) + to_string(column.index);
		if (!update.table.ColumnExists(Identifier(sign_count))) {
			continue;
		}
		update.update_is_del_and_insert = true;
		OPENIVM_DEBUG_PRINT("[INSERT RULE] replacing floating group representative in '%s'\n",
		                    update.table.name.GetIdentifierName().c_str());
		return;
	}
}

static void ResolveInsertDefaults(OptimizerExtensionInput &input, LogicalInsert &insert) {
	if (insert.column_index_map.empty()) {
		return;
	}
	auto child_bindings = insert.children[0]->GetColumnBindings();
	vector<unique_ptr<Expression>> expressions;
	for (auto &column : insert.table.GetColumns().Physical()) {
		auto mapped_index = insert.column_index_map[column.Physical()];
		if (mapped_index == DConstants::INVALID_INDEX) {
			expressions.push_back(insert.bound_defaults[column.StorageOid()]->Copy());
		} else {
			if (mapped_index >= child_bindings.size()) {
				throw InternalException("OpenIVM insert column mapping is out of range");
			}
			expressions.push_back(make_uniq<BoundColumnRefExpression>(column.Type(), child_bindings[mapped_index]));
		}
	}
	auto projection = make_uniq<LogicalProjection>(input.optimizer.binder.GenerateTableIndex(), std::move(expressions));
	projection->children.push_back(std::move(insert.children[0]));
	insert.children[0] = std::move(projection);
	insert.column_index_map = physical_index_vector_t<idx_t>();
	insert.expected_types = insert.table.GetTypes();
}

static idx_t FindBindingIndex(const vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
	for (idx_t index = 0; index < bindings.size(); index++) {
		if (bindings[index] == binding) {
			return index;
		}
	}
	throw InternalException("OpenIVM could not resolve a DML input binding");
}

static idx_t FindExpressionBindingIndex(LogicalOperator &child, const Expression &expression) {
	if (expression.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		throw InternalException("OpenIVM expected a bound row-id column reference");
	}
	return FindBindingIndex(child.GetColumnBindings(), expression.Cast<BoundColumnRefExpression>().Binding());
}

// The target-table scan feeding an UPDATE/DELETE through projections only.
// Joins (e.g. UPDATE ... FROM) can repeat target rows, and a residual filter
// would make the scan read added columns for rows the DML never touches.
static optional_ptr<LogicalGet> FindTargetScan(LogicalOperator &op, TableCatalogEntry &table) {
	reference<LogicalOperator> current(op);
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION && current.get().children.size() == 1) {
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		return nullptr;
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (get.GetTable().get() != &table || !get.children.empty() || !get.projected_input.empty()) {
		return nullptr;
	}
	for (auto &column_id : get.GetColumnIds()) {
		// A struct-pushdown or retyped scan column is not the stored value.
		if (!column_id.IsVirtualColumn() &&
		    (column_id.HasChildren() || column_id.HasType() || column_id.IsPushdownExtract())) {
			return nullptr;
		}
	}
	return &get;
}

static optional_idx FindScannedColumn(LogicalGet &get, idx_t logical_index) {
	auto &column_ids = get.GetColumnIds();
	for (idx_t index = 0; index < column_ids.size(); index++) {
		if (!column_ids[index].IsVirtualColumn() && column_ids[index].GetPrimaryIndex() == logical_index) {
			return index;
		}
	}
	return optional_idx();
}

// Scanning the missing columns beats one row-id fetch per target row only when
// every scanned row is a target (no pushed-down filter). A filtered scan decodes
// each added column for every vector holding a single target, which loses to
// the fetch for sparse targets, and estimates cannot bound how sparse they are.
static bool ScannedPreimageIsCheap(LogicalGet &get, TableCatalogEntry &table) {
	if (!get.table_filters.HasFilters()) {
		return true;
	}
	for (auto &column : table.GetColumns().Physical()) {
		if (!FindScannedColumn(get, column.Logical().index).IsValid()) {
			return false;
		}
	}
	return true;
}

// Make every physical column of the scanned table an output of `op` by appending
// to the scan and to projections. Scan column positions (and so all bindings)
// stay fixed. Returns the bindings in physical column order.
static vector<ColumnBinding> ExposeTargetColumns(LogicalOperator &op, LogicalGet &get, TableCatalogEntry &table) {
	if (&op == &get) {
		vector<ColumnBinding> bindings;
		for (auto &column : table.GetColumns().Physical()) {
			auto position = FindScannedColumn(get, column.Logical().index);
			if (!position.IsValid()) {
				position = get.AddColumnId(column.Logical().index).GetIndex();
			}
			ProjectionIndex projection(position.GetIndex());
			if (!get.projection_ids.empty() && std::find(get.projection_ids.begin(), get.projection_ids.end(),
			                                             projection) == get.projection_ids.end()) {
				get.projection_ids.push_back(projection);
			}
			bindings.emplace_back(get.table_index, projection);
		}
		if (get.projection_ids.size() == get.GetColumnIds().size()) {
			// Table scans treat a full-size projection list as the identity order.
			// Bindings name scan columns, so outputting all of them in order is equivalent.
			get.projection_ids.clear();
		}
		return bindings;
	}
	D_ASSERT(op.type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto child_bindings = ExposeTargetColumns(*op.children[0], get, table);
	auto &projection = op.Cast<LogicalProjection>();
	auto types = table.GetTypes();
	vector<ColumnBinding> bindings;
	for (idx_t column = 0; column < child_bindings.size(); column++) {
		optional_idx position;
		for (idx_t index = 0; index < projection.expressions.size(); index++) {
			auto &expression = *projection.expressions[index];
			if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
			    expression.Cast<BoundColumnRefExpression>().Binding() == child_bindings[column]) {
				position = index;
				break;
			}
		}
		if (!position.IsValid()) {
			position = projection.expressions.size();
			projection.expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(types[column], child_bindings[column]));
		}
		bindings.emplace_back(projection.table_index, ProjectionIndex(position.GetIndex()));
	}
	return bindings;
}

static bool IsIdentityPrefix(const vector<ProjectionIndex> &projection_ids) {
	for (idx_t index = 0; index < projection_ids.size(); index++) {
		if (projection_ids[index].GetIndex() != index) {
			return false;
		}
	}
	return true;
}

// Capture fetches each UPDATE/DELETE preimage by row id unless its input is a
// plain target scan that can carry the whole row cheaply.
static void CaptureScannedPreimages(LogicalTransactionalDeltaCapture &capture) {
	auto &child = *capture.children[0];
	auto get = FindTargetScan(child, capture.base_table);
	// Capture consumes its input by position (row id, output count). Completing a
	// scan's projection list resets it to scan order, so a scan feeding capture
	// directly must already output a prefix of that order.
	if (!get || !ScannedPreimageIsCheap(*get, capture.base_table) ||
	    (&child == get.get() && !IsIdentityPrefix(get->projection_ids))) {
		OPENIVM_DEBUG_PRINT("[INSERT RULE] '%s' capture fetches preimages by row id\n",
		                    capture.base_table.name.GetIdentifierName().c_str());
		return;
	}
	auto original_bindings = child.GetColumnBindings();
#ifdef DEBUG
	auto original_scan_bindings = get->GetColumnBindings();
	auto original_scan_columns = get->GetColumnIds();
#endif
	auto exposed = ExposeTargetColumns(child, *get, capture.base_table);
	auto bindings = child.GetColumnBindings();
#ifdef DEBUG
	// Widening only appends: existing outputs keep their positions and the scan
	// keeps every column position its bindings name.
	D_ASSERT(bindings.size() >= original_bindings.size());
	D_ASSERT(std::equal(original_bindings.begin(), original_bindings.end(), bindings.begin()));
	auto scan_bindings = get->GetColumnBindings();
	for (auto &binding : original_scan_bindings) {
		D_ASSERT(std::find(scan_bindings.begin(), scan_bindings.end(), binding) != scan_bindings.end());
	}
	for (idx_t index = 0; index < original_scan_columns.size(); index++) {
		D_ASSERT(get->GetColumnIds()[index] == original_scan_columns[index]);
	}
#endif
	for (auto &binding : exposed) {
		capture.old_row_columns.push_back(FindBindingIndex(bindings, binding));
	}
	capture.output_column_count = original_bindings.size();
	OPENIVM_DEBUG_PRINT("[INSERT RULE] '%s' capture reads %zu preimage columns from its scan\n",
	                    capture.base_table.name.GetIdentifierName().c_str(), exposed.size());
}

static void ResolveUpdateDefaults(OptimizerExtensionInput &input, LogicalUpdate &update) {
	if (std::none_of(update.expressions.begin(), update.expressions.end(), [](const unique_ptr<Expression> &expr) {
		    return expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT;
	    })) {
		return;
	}

	auto child_bindings = update.children[0]->GetColumnBindings();
	auto child_types = update.children[0]->types;
	if (child_bindings.empty() || child_bindings.size() != child_types.size()) {
		throw InternalException("OpenIVM cannot normalize UPDATE defaults without a row-id input");
	}

	auto projection_index = input.optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> projection_expressions;
	projection_expressions.reserve(child_bindings.size() + update.expressions.size());
	for (idx_t index = 0; index + 1 < child_bindings.size(); index++) {
		projection_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(child_types[index], child_bindings[index]));
	}

	vector<idx_t> default_indexes(update.expressions.size(), DConstants::INVALID_INDEX);
	for (idx_t index = 0; index < update.expressions.size(); index++) {
		if (update.expressions[index]->GetExpressionType() != ExpressionType::VALUE_DEFAULT) {
			continue;
		}
		default_indexes[index] = projection_expressions.size();
		projection_expressions.push_back(update.bound_defaults[update.columns[index].index]->Copy());
	}

	const auto row_id_output_index = projection_expressions.size();
	projection_expressions.push_back(make_uniq<BoundColumnRefExpression>(child_types.back(), child_bindings.back()));
	for (idx_t index = 0; index < update.expressions.size(); index++) {
		auto return_type = update.expressions[index]->GetReturnType();
		idx_t output_index;
		if (default_indexes[index] != DConstants::INVALID_INDEX) {
			output_index = default_indexes[index];
		} else {
			output_index = FindExpressionBindingIndex(*update.children[0], *update.expressions[index]);
			D_ASSERT(output_index + 1 < child_bindings.size());
		}
		update.expressions[index] = make_uniq<BoundColumnRefExpression>(
		    return_type, ColumnBinding(TableIndex(projection_index), ProjectionIndex(output_index)));
	}

	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_expressions));
	projection->children.push_back(std::move(update.children[0]));
	update.children[0] = std::move(projection);
	D_ASSERT(update.children[0]->GetColumnBindings().size() == row_id_output_index + 1);
}

void RefreshInsertRule::RefreshInsertRuleFunction(OptimizerExtensionInput &input,
                                                  duckdb::unique_ptr<LogicalOperator> &plan) {
	auto root = plan.get();

	if (root->type == LogicalOperatorType::LOGICAL_CREATE_TABLE) {
		auto &create = root->Cast<LogicalCreateTable>();
		auto &catalog = create.schema.catalog;
		if (!catalog.IsDuckCatalog() || create.info->Base().temporary) {
			return;
		}
		auto &info = create.info->Base();
		// MV lifecycle programs publish their own signed output changes. Their
		// backing/publication rebuilds are not replacements of user sources.
		auto table_name = info.GetQualifiedName().Name().GetIdentifierName();
		if (IncrementalTableNames::IsMaintenanceTable(table_name)) {
			return;
		}
		auto catalog_name = catalog.GetName().GetIdentifierName();
		auto schema_name = create.schema.name.GetIdentifierName();
		auto old_table = Catalog::GetEntry<TableCatalogEntry>(
		    input.context, QualifiedName(Identifier(catalog_name), Identifier(schema_name), Identifier(table_name)),
		    OnEntryNotFound::RETURN_NULL);
		if (old_table && info.on_conflict != OnCreateConflict::REPLACE_ON_CONFLICT) {
			return;
		}
		auto delta =
		    TransactionalMVLockState::Get(input.context).FindSourceDeltaForWrite(catalog_name, schema_name, table_name);
		auto token = RefreshMetadata::SourceGeneration(input.context, catalog_name, schema_name, table_name);
		if (delta) {
			idx_t count = 0;
			bool compatible = true;
			for (auto &column : delta->GetColumns().Physical()) {
				if (column.Name() == openivm::MULTIPLICITY_COL || column.Name() == openivm::TIMESTAMP_COL) {
					continue;
				}
				count++;
				compatible = compatible && info.columns.ColumnExists(column.Name()) &&
				             info.columns.GetColumn(column.Name()).Type() == column.Type();
			}
			if (!compatible || count != info.columns.LogicalColumnCount()) {
				throw BinderException("OpenIVM replacement schema is incompatible with retained deltas for source "
				                      "'%s'; drop dependent materialized views before changing its schema",
				                      table_name);
			}
		}
		if (delta || !token.empty() || !create.children.empty() ||
		    info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
			WrapOperator(plan, make_uniq<LogicalSourceLifecycle>(old_table, delta));
		}
		return;
	}

	// Handle ALTER TABLE: sync delta table schema or block if referenced column is affected
	if (root->type == LogicalOperatorType::LOGICAL_ALTER) {
		auto *alter_info = dynamic_cast<AlterTableInfo *>(root->Cast<LogicalAlter>().info.get());
		if (!alter_info) {
			return;
		}

		auto source = Catalog::GetEntry<TableCatalogEntry>(
		    input.context,
		    QualifiedName(Identifier(alter_info->GetQualifiedName().Catalog().GetIdentifierName()),
		                  Identifier(alter_info->GetQualifiedName().Schema().GetIdentifierName()),
		                  Identifier(alter_info->GetQualifiedName().Name().GetIdentifierName())),
		    OnEntryNotFound::RETURN_NULL);
		if (!source) {
			return;
		}
		// SQL identifiers are case-insensitive; metadata stores the catalog spelling.
		string table_name = source->name.GetIdentifierName();
		string delta_name = SqlUtils::DeltaName(table_name);
		auto source_locus = std::make_pair(source->ParentCatalog().GetName().GetIdentifierName(),
		                                   source->schema.name.GetIdentifierName());

		// Resolve only the source's delta table in the caller transaction. Scanning
		// information_schema touches unrelated external catalogs and can silently
		// skip schema synchronization when one of them is unavailable. Source
		// admission keeps an untracked ALTER local; tracked ownership escalates to
		// the mutation gate and is rechecked there.
		auto delta_entry = TransactionalMVLockState::Get(input.context)
		                       .FindSourceDeltaForWrite(source_locus.first, source_locus.second, table_name);
		auto wrap_generation = [&]() {
			if (source->catalog.IsDuckCatalog()) {
				WrapOperator(plan, make_uniq<LogicalSourceLifecycle>(source, delta_entry));
			}
		};
		if (!delta_entry) {
			wrap_generation();
			return;
		}
		// Schema evolution rewrites the delta: an ownership change until commit. The
		// tracked lookup above already holds the gate, so this never waits.
		TransactionalMVLockState::Get(input.context).MarkOwnershipMetadataWrite();

		switch (alter_info->alter_table_type) {
		case AlterTableType::RENAME_TABLE:
		case AlterTableType::ALTER_COLUMN_TYPE:
			throw CatalogException("Cannot %s on IVM-tracked source '%s': OpenIVM cannot preserve its delta capture "
			                       "and dependent view definitions across this change. Drop its dependent "
			                       "materialized views first, alter the source, then recreate the views.",
			                       alter_info->alter_table_type == AlterTableType::RENAME_TABLE ? "rename table"
			                                                                                    : "change column type",
			                       SqlUtils::FullName(source_locus.first, source_locus.second, table_name));
		case AlterTableType::ADD_COLUMN:
			OPENIVM_DEBUG_PRINT("[INSERT RULE] ALTER TABLE ADD COLUMN '%s' — syncing delta table\n",
			                    alter_info->Cast<AddColumnInfo>().new_column.Name().c_str());
			AlterDeltaInCallerTransaction(input.context, *alter_info, source_locus.first, *delta_entry);
			break;
		case AlterTableType::REMOVE_COLUMN: {
			string col_name = alter_info->Cast<RemoveColumnInfo>().removed_column.GetIdentifierName();
			for (auto &location : RefreshMetadata::MetadataLocations(input.context, openivm::DELTA_TABLES_TABLE)) {
				Connection con(*input.context.db);
				SnapshotSchemaEvolutionMetadata(input.context, con, location);
				auto referencing_mv = FirstMVReferencingColumn(input.context, con, delta_name, source_locus.first,
				                                               source_locus.second, table_name, col_name);
				if (!referencing_mv.empty()) {
					throw CatalogException("Cannot drop column '" + col_name +
					                       "': it is referenced by materialized view '" + referencing_mv +
					                       "'. Drop the view first.");
				}
			}
			OPENIVM_DEBUG_PRINT("[INSERT RULE] ALTER TABLE DROP COLUMN '%s' — syncing delta table\n", col_name.c_str());
			AlterDeltaInCallerTransaction(input.context, *alter_info, source_locus.first, *delta_entry);
			break;
		}
		case AlterTableType::RENAME_COLUMN: {
			auto &rename_info = alter_info->Cast<RenameColumnInfo>();
			string old_name = rename_info.old_name.GetIdentifierName();
			string new_name = rename_info.new_name.GetIdentifierName();
			auto dependent_view_predicate =
			    "view_name IN (SELECT view_name FROM " + string(openivm::DELTA_TABLES_TABLE) + " WHERE " +
			    RefreshMetadata::SourcePredicate(delta_name, source_locus.first, source_locus.second) + ")";
			vector<MetadataLocation> affected_locations;
			for (auto &location : RefreshMetadata::MetadataLocations(input.context, openivm::DELTA_TABLES_TABLE)) {
				Connection con(*input.context.db);
				SnapshotSchemaEvolutionMetadata(input.context, con, location);
				auto rows = SqlUtils::QueryOrThrow(con,
				                                   "SELECT 1 FROM " + string(openivm::VIEWS_TABLE) + " WHERE " +
				                                       dependent_view_predicate + " LIMIT 1",
				                                   "%s");
				if (!rows->RowCount()) {
					continue;
				}
				MetadataLocator::RequireWritable(location, "rename source column");
				MetadataLocator::RejectExplicitTransaction(input.context, location, source_locus.first,
				                                           "ALTER TABLE RENAME COLUMN");
				affected_locations.push_back(location);
			}
			for (auto &location : affected_locations) {
				Connection con(*input.context.db);
				SnapshotSchemaEvolutionMetadata(input.context, con, location);
				// DuckDB permits writes to one native catalog per transaction. Keep
				// the existing audited helper/undo path for other committed catalogs.
				if (!StringUtil::CIEquals(location.catalog, source_locus.first)) {
					RegisterMetadataRestore(input.context, con, openivm::VIEWS_TABLE, dependent_view_predicate);
				}
				RewriteDependentViewMetadataForRename(input.context, con, delta_name, source_locus.first,
				                                      source_locus.second, table_name, old_name, new_name);
			}
			OPENIVM_DEBUG_PRINT("[INSERT RULE] ALTER TABLE RENAME COLUMN '%s' → '%s' — syncing delta table\n",
			                    old_name.c_str(), new_name.c_str());
			AlterDeltaInCallerTransaction(input.context, *alter_info, source_locus.first, *delta_entry);
			break;
		}
		default:
			break;
		}
		wrap_generation();
		return;
	}

	// A cached EXECUTE has no logical child. A rebound EXECUTE does: capture
	// its DML below just like a directly submitted statement.
	if (root->type == LogicalOperatorType::LOGICAL_EXECUTE && root->children.empty()) {
		auto &prepared = root->Cast<LogicalExecute>().prepared;
		if (prepared && prepared->physical_plan) {
			AdmitPlanMutations(input.context, prepared->physical_plan->Root());
		}
		return;
	}

	if (plan->children.empty()) {
		return;
	}
	MarkOwnershipRegistryWrites(input.context, *plan);

	auto dml_owner = &plan;
	auto dml = dml_owner->get();
	while (!GetDMLTarget(*dml)) {
		if (dml->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && dml->children.size() == 2) {
			// WITH ... AS MATERIALIZED: the statement's DML is in the consumer side.
			dml_owner = &dml->children[1];
			dml = dml_owner->get();
			continue;
		}
		if (dml->children.size() != 1) {
			return;
		}
		dml_owner = &dml->children[0];
		dml = dml_owner->get();
	}

	if (dml->type == LogicalOperatorType::LOGICAL_UPDATE) {
		PreserveGroupRepresentatives(dml->Cast<LogicalUpdate>());
	}
	auto table = GetNativeDMLTable(*dml);
	if (!table) {
		return;
	}
	auto delta_table =
	    TransactionalMVLockState::Get(input.context)
	        .FindSourceDeltaForWrite(table->catalog.GetName().GetIdentifierName(),
	                                 table->schema.name.GetIdentifierName(), table->name.GetIdentifierName());
	if (delta_table) {
		switch (dml->type) {
		case LogicalOperatorType::LOGICAL_INSERT: {
			auto &insert = dml->Cast<LogicalInsert>();
			ResolveInsertDefaults(input, insert);
			WrapOperator(insert.children[0], make_uniq<LogicalTransactionalDeltaCapture>(insert.table, *delta_table,
			                                                                             DeltaCaptureMode::INSERT));
			OPENIVM_DEBUG_PRINT("[INSERT RULE] transactional INSERT delta capture for '%s'\n", table->name.c_str());
			break;
		}
		case LogicalOperatorType::LOGICAL_DELETE: {
			auto &delete_op = dml->Cast<LogicalDelete>();
			// DuckDB binds the row id first for native tables; unique-index and
			// RETURNING columns follow it.
			D_ASSERT(!delete_op.expressions.empty());
			auto row_id_index = FindExpressionBindingIndex(*delete_op.children[0], *delete_op.expressions[0]);
			CaptureScannedPreimages(WrapOperator(
			    delete_op.children[0],
			    make_uniq<LogicalTransactionalDeltaCapture>(delete_op.table, *delta_table, DeltaCaptureMode::DELETE,
			                                                vector<unique_ptr<Expression>> {}, vector<PhysicalIndex> {},
			                                                optional_idx(row_id_index))));
			OPENIVM_DEBUG_PRINT("[INSERT RULE] transactional DELETE delta capture for '%s'\n", table->name.c_str());
			break;
		}
		case LogicalOperatorType::LOGICAL_UPDATE: {
			auto &update = dml->Cast<LogicalUpdate>();
			ResolveUpdateDefaults(input, update);
			if (!update.update_is_del_and_insert &&
			    std::any_of(update.columns.begin(), update.columns.end(), [&](PhysicalIndex column) {
				    return SqlUtils::EqualityCanHideValueIdentity(update.table.GetColumns().GetColumn(column).Type());
			    })) {
				// DuckDB can skip SQL-equal storage updates: proposed SET values need
				// not be the persisted postimages (e.g. -0.0 -> +0.0 or 1 month -> 30 days).
				WrapOperator(*dml_owner, make_uniq<LogicalTransactionalActionDeltaCapture>(update.table, *delta_table));
				OPENIVM_DEBUG_PRINT("[INSERT RULE] native UPDATE postimage capture for '%s'\n", table->name.c_str());
				break;
			}
			vector<unique_ptr<Expression>> update_expressions;
			update_expressions.reserve(update.expressions.size());
			for (idx_t index = 0; index < update.expressions.size(); index++) {
				update_expressions.push_back(update.expressions[index]->Copy());
			}
			auto row_id_index = update.children[0]->GetColumnBindings().size() - 1;
			CaptureScannedPreimages(WrapOperator(
			    update.children[0], make_uniq<LogicalTransactionalDeltaCapture>(
			                            update.table, *delta_table, DeltaCaptureMode::UPDATE,
			                            std::move(update_expressions), update.columns, optional_idx(row_id_index))));
			OPENIVM_DEBUG_PRINT("[INSERT RULE] transactional UPDATE delta capture for '%s'\n", table->name.c_str());
			break;
		}
		case LogicalOperatorType::LOGICAL_MERGE_INTO: {
			auto &merge = dml->Cast<LogicalMergeInto>();
			WrapOperator(*dml_owner, make_uniq<LogicalTransactionalActionDeltaCapture>(merge.table, *delta_table));
			OPENIVM_DEBUG_PRINT("[INSERT RULE] transactional MERGE action delta capture for '%s'\n",
			                    table->name.c_str());
			break;
		}
		default:
			return;
		}
	}
	// Put the execution marker in the DML input pipeline, before its native sink.
	// Plain EXPLAIN builds this operator but never initializes its runtime state.
	WrapOperator(dml->children[0], make_uniq<LogicalSourceWriteExecution>(*table, delta_table));
}
} // namespace duckdb
