#include "upsert/refresh_internal.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/sql_utils.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

static bool QueryLooksLikeJoin(const string &view_query_sql, const vector<string> &delta_table_names) {
	return delta_table_names.size() > 1 || view_query_sql.find("JOIN") != string::npos ||
	       view_query_sql.find("join") != string::npos;
}

static void AccumulateDuckLakeDeltaSummary(DeltaActivityResult &summary, RefreshMetadata &metadata, Connection &con,
                                           const string &view_name, const string &delta_table,
                                           const string &view_catalog_name, const string &view_schema_name,
                                           const string &attached_db_catalog_name,
                                           const string &attached_db_schema_name) {
	auto loc = ResolveDuckLakeSourceLocation(con, view_name, delta_table, view_catalog_name, view_schema_name,
	                                         attached_db_catalog_name, attached_db_schema_name);
	auto last_snap = metadata.GetLastSnapshotId(view_name, delta_table);
	auto cur_snap = metadata.GetCurrentDuckLakeSnapshot(loc.catalog_name);
	if (cur_snap < 0 || last_snap < 0) {
		summary.any_has_deletes = true; // conservative
		summary.tables_with_changes++;
		summary.active_delta_table_names.push_back(delta_table);
		return;
	}
	if (last_snap == cur_snap) {
		return;
	}
	string insertions = SqlUtils::DuckLakeTableFunction("ducklake_table_insertions", loc.catalog_name, loc.schema_name,
	                                                    loc.table_name, last_snap, cur_snap);
	string deletions = SqlUtils::DuckLakeTableFunction("ducklake_table_deletions", loc.catalog_name, loc.schema_name,
	                                                   loc.table_name, last_snap, cur_snap);
	auto count_result =
	    con.Query("SELECT (SELECT COUNT(*) FROM " + insertions + "), (SELECT COUNT(*) FROM " + deletions + ")");
	if (count_result->HasError()) {
		summary.any_has_deletes = true;
		summary.tables_with_changes++;
		summary.active_delta_table_names.push_back(delta_table);
		return;
	}
	auto insert_count = count_result->GetValue(0, 0).GetValue<int64_t>();
	auto delete_count = count_result->GetValue(1, 0).GetValue<int64_t>();
	if (insert_count == 0 && delete_count == 0) {
		summary.ducklake_snapshot_advances.push_back({delta_table, cur_snap});
		return;
	}
	summary.tables_with_changes++;
	summary.active_delta_table_names.push_back(delta_table);
	if (delete_count > 0) {
		summary.any_has_deletes = true;
	}
}

static void AccumulateStandardDeltaSummary(DeltaActivityResult &summary, RefreshMetadata &metadata,
                                           const string &view_name, const string &delta_table,
                                           const string &view_catalog_name, const string &view_schema_name) {
	summary.all_ducklake = false;
	auto ts_string = metadata.GetLastUpdate(view_name, delta_table);
	if (ts_string.empty()) {
		summary.any_has_deletes = true; // conservative
		summary.tables_with_changes++;
		summary.active_delta_table_names.push_back(delta_table);
		return;
	}
	string delta_table_sql =
	    metadata.ResolveDeltaQualifiedName(view_name, delta_table, view_catalog_name, view_schema_name);
	auto stats = metadata.GetStandardDeltaChangeStats(delta_table_sql, ts_string);
	if (!stats.ok) {
		summary.any_has_deletes = true; // conservative
		summary.tables_with_changes++;
		summary.active_delta_table_names.push_back(delta_table);
		return;
	}
	if (stats.total == 0) {
		return;
	}
	summary.tables_with_changes++;
	summary.active_delta_table_names.push_back(delta_table);
	if (stats.deletes > 0) {
		summary.any_has_deletes = true;
	}
}

static bool IsInsertOnlyDeltaSummary(const DeltaActivityResult &summary, const vector<string> &delta_table_names) {
	if (summary.any_has_deletes) {
		return false;
	}
	if (!summary.has_join) {
		return true;
	}
	if (summary.all_ducklake) {
		return true;
	}
	if (summary.tables_with_changes <= 1 && delta_table_names.size() > 1) {
		return true;
	}
	return false;
}

static DeltaActivityResult AnalyzeDeltaChanges(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                               const string &view_query_sql, const vector<string> &delta_table_names,
                                               const string &view_catalog_name, const string &view_schema_name,
                                               const string &attached_db_catalog_name,
                                               const string &attached_db_schema_name, bool cross_system) {
	DeltaActivityResult summary;
	summary.has_join = QueryLooksLikeJoin(view_query_sql, delta_table_names);
	for (auto &dt : delta_table_names) {
		if (metadata.IsDuckLakeTable(view_name, dt)) {
			AccumulateDuckLakeDeltaSummary(summary, metadata, con, view_name, dt, view_catalog_name, view_schema_name,
			                               attached_db_catalog_name, attached_db_schema_name);
		} else {
			AccumulateStandardDeltaSummary(summary, metadata, view_name, dt, view_catalog_name, view_schema_name);
		}
	}
	return summary;
}

DeltaActivityResult BuildDeltaActivityResult(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                             const string &view_query_sql, const vector<string> &delta_table_names,
                                             const string &view_catalog_name, const string &view_schema_name,
                                             const string &attached_db_catalog_name,
                                             const string &attached_db_schema_name) {
	return AnalyzeDeltaChanges(metadata, con, view_name, view_query_sql, delta_table_names, view_catalog_name,
	                           view_schema_name, attached_db_catalog_name, attached_db_schema_name, false);
}

DeltaFastPathFlags ResolveDeltaFastPathFlags(ClientContext &context, RefreshMetadata &metadata, Connection &con,
                                             const string &view_name, const string &view_query_sql,
                                             const vector<string> &delta_table_names, const string &view_catalog_name,
                                             const string &view_schema_name, const string &attached_db_catalog_name,
                                             const string &attached_db_schema_name, bool cross_system,
                                             const DeltaActivityResult *precomputed_delta_activity) {
	DeltaActivityResult summary;
	if (precomputed_delta_activity) {
		summary = *precomputed_delta_activity;
	} else {
		summary =
		    AnalyzeDeltaChanges(metadata, con, view_name, view_query_sql, delta_table_names, view_catalog_name,
		                        view_schema_name, attached_db_catalog_name, attached_db_schema_name, cross_system);
	}
	DeltaFastPathFlags flags;
	flags.insert_only = IsInsertOnlyDeltaSummary(summary, delta_table_names);
	flags.skip_agg_delete = flags.insert_only;
	flags.skip_proj_delete = flags.insert_only;
	flags.minmax_incremental = flags.insert_only;
	flags.active_delta_table_names = summary.active_delta_table_names;

	if (!SqlUtils::GetBoolSetting(context, "openivm_skip_aggregate_delete", true)) {
		flags.skip_agg_delete = false;
	}
	if (!SqlUtils::GetBoolSetting(context, "openivm_skip_projection_delete", true)) {
		flags.skip_proj_delete = false;
	}
	if (!SqlUtils::GetBoolSetting(context, "openivm_minmax_incremental", true)) {
		flags.minmax_incremental = false;
	}
	OPENIVM_DEBUG_PRINT("[UPSERT] insert_only=%d, skip_agg_delete=%d, skip_proj_delete=%d, minmax_incremental=%d\n",
	                    flags.insert_only, flags.skip_agg_delete, flags.skip_proj_delete, flags.minmax_incremental);
	return flags;
}

} // namespace duckdb
