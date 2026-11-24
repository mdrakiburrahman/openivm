#include "upsert/refresh_internal.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/sql_utils.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

static string DuckLakeChangeContains(const string &key, const string &table_id) {
	return "COALESCE(list_contains(changes['" + key + "'], '" + table_id + "'), false)";
}

static string BuildDuckLakeSnapshotActivitySQL(const string &catalog_name, int64_t table_id, int64_t last_snapshot_id,
                                               int64_t current_snapshot_id) {
	string id = to_string(table_id);
	string insert_expr =
	    DuckLakeChangeContains("tables_inserted_into", id) + " OR " + DuckLakeChangeContains("inlined_insert", id);
	string delete_expr =
	    DuckLakeChangeContains("tables_deleted_from", id) + " OR " + DuckLakeChangeContains("inlined_delete", id);
	string structural_expr =
	    DuckLakeChangeContains("tables_altered", id) + " OR " + DuckLakeChangeContains("tables_dropped", id);
	return "SELECT COALESCE(bool_or(" + insert_expr + "), false), COALESCE(bool_or(" + delete_expr +
	       "), false), COALESCE(bool_or(" + structural_expr + "), false) FROM ducklake_snapshots('" +
	       SqlUtils::EscapeValue(catalog_name) + "') WHERE snapshot_id > " + to_string(last_snapshot_id) +
	       " AND snapshot_id <= " + to_string(current_snapshot_id);
}

DuckLakeTableActivity ProbeDuckLakeSnapshotActivity(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                                    const string &table_name, const string &catalog_name,
                                                    const string &schema_name, int64_t last_snapshot_id,
                                                    int64_t current_snapshot_id) {
	DuckLakeTableActivity activity;
	if (current_snapshot_id < 0 || last_snapshot_id < 0) {
		return activity;
	}
	auto identity = metadata.ResolveDuckLakeSourceIdentity(view_name, table_name, catalog_name, schema_name,
	                                                       last_snapshot_id, current_snapshot_id);
	if (!identity.resolved || identity.current_table_id < 0) {
		return activity;
	}
	if (identity.changed) {
		activity.ok = true;
		activity.has_changes = true;
		activity.has_deletes = true;
		activity.requires_full_refresh = true;
		return activity;
	}

	// Older transactional refreshes could advance the watermark and leave the old ID.
	// Equal snapshots prove there are no deltas only after verifying the identity.
	if (last_snapshot_id == current_snapshot_id) {
		activity.ok = true;
		return activity;
	}
	if (identity.stored_table_id < 0) {
		activity.source_table_id_to_backfill = identity.current_table_id;
	}

	// `ducklake_snapshots().changes` is a cheap catalog-level manifest keyed by DuckLake table id.
	// It lets us skip unrelated catalog snapshots without opening table_insertions/deletions.
	// Structural changes invalidate row-level deltas, so they force a full recompute.
	auto result = con.Query(BuildDuckLakeSnapshotActivitySQL(catalog_name, identity.current_table_id, last_snapshot_id,
	                                                         current_snapshot_id));
	if (result->HasError() || result->RowCount() == 0) {
		return activity;
	}

	auto has_inserts =
	    !result->Collection().GetValue(0, 0).IsNull() && result->Collection().GetValue(0, 0).GetValue<bool>();
	auto has_deletes =
	    !result->Collection().GetValue(1, 0).IsNull() && result->Collection().GetValue(1, 0).GetValue<bool>();
	auto has_structural =
	    !result->Collection().GetValue(2, 0).IsNull() && result->Collection().GetValue(2, 0).GetValue<bool>();
	activity.ok = true;
	activity.has_changes = has_inserts || has_deletes || has_structural;
	activity.has_deletes = has_deletes || has_structural;
	activity.requires_full_refresh = has_structural;
	return activity;
}

DuckLakeTableActivity ProbeDuckLakeTableChanges(Connection &con, const DuckLakeSourceLocation &loc, int64_t table_id,
                                                int64_t after_snapshot_id, int64_t through_snapshot_id) {
	DuckLakeTableActivity activity;
	if (after_snapshot_id < 0 || through_snapshot_id < after_snapshot_id) {
		return activity;
	}
	if (after_snapshot_id == through_snapshot_id) {
		activity.ok = true;
		return activity;
	}
	if (table_id >= 0) {
		// The manifest also reports ALTER and DROP, and a recreated table drops the old id.
		auto sql = BuildDuckLakeSnapshotActivitySQL(loc.catalog_name, table_id, after_snapshot_id, through_snapshot_id);
		auto result = con.Query(sql);
		if (!result->HasError() && result->RowCount() == 1) {
			activity.ok = true;
			for (idx_t col = 0; col < 3; col++) {
				auto changed = result->Collection().GetValue(col, 0);
				activity.has_changes = activity.has_changes || (!changed.IsNull() && changed.GetValue<bool>());
			}
			return activity;
		}
	}
	RefreshMetadata metadata(con);
	auto stats = GetDuckLakeDeltaChangeStats(metadata, loc, after_snapshot_id, through_snapshot_id);
	activity.ok = stats.ok;
	activity.has_changes = stats.total > 0;
	return activity;
}

// Records a source. One whose changes could not be verified (`!ok`) counts as changed with deletes.
static void AddDeltaSource(DeltaActivityResult &summary, DeltaActivityResult::Source source) {
	if (!source.ok) {
		source.has_changes = true;
		source.has_deletes = true;
	}
	if (source.has_changes) {
		summary.tables_with_changes++;
		summary.active_delta_table_names.push_back(source.delta_table_name);
		summary.any_has_deletes = summary.any_has_deletes || source.has_deletes;
		summary.requires_full_refresh = summary.requires_full_refresh || source.requires_full_refresh;
	}
	summary.sources.push_back(std::move(source));
}

static void ApplyChangeStats(DeltaActivityResult::Source &source, const RefreshMetadata::DeltaChangeStats &stats) {
	source.ok = stats.ok;
	if (stats.ok) {
		source.pending_rows = stats.total;
		source.delete_rows = stats.deletes;
		source.has_changes = stats.total > 0;
		source.has_deletes = stats.deletes > 0;
	}
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
	DeltaActivityResult::Source source;
	source.delta_table_name = delta_table;
	source.source_table_name = loc.table_name.empty() ? delta_table : loc.table_name;
	source.catalog_name = loc.catalog_name;
	source.schema_name = loc.schema_name;
	source.ducklake = true;
	source.last_snapshot_id = last_snap;
	source.current_snapshot_id = cur_snap;
	if (cur_snap < 0 || last_snap < 0) {
		AddDeltaSource(summary, std::move(source));
		return;
	}
	auto metadata_activity = ProbeDuckLakeSnapshotActivity(metadata, con, view_name, delta_table, loc.catalog_name,
	                                                       loc.schema_name, last_snap, cur_snap);
	source.backfill_source_table_id = metadata_activity.source_table_id_to_backfill >= 0;
	if (metadata_activity.ok) {
		source.ok = true;
		source.has_changes = metadata_activity.has_changes;
		source.has_deletes = metadata_activity.has_deletes;
		source.requires_full_refresh = metadata_activity.requires_full_refresh;
	} else {
		ApplyChangeStats(source, GetDuckLakeDeltaChangeStats(metadata, loc, last_snap, cur_snap));
	}
	if (source.ok && !source.has_changes && last_snap != cur_snap) {
		// Nothing in the range touched this table; consume the unrelated snapshots.
		summary.ducklake_snapshot_advances.emplace_back(delta_table, cur_snap,
		                                                metadata_activity.source_table_id_to_backfill);
	}
	AddDeltaSource(summary, std::move(source));
}

// A native source, with the physical delta and watermark its change stats are read from.
static DeltaActivityResult::Source DescribeStandardDeltaSource(RefreshMetadata &metadata, const string &view_name,
                                                               const string &delta_table,
                                                               const string &view_catalog_name,
                                                               const string &view_schema_name) {
	DeltaActivityResult::Source source;
	source.delta_table_name = delta_table;
	auto loc = metadata.GetSourceLocation(view_name, delta_table, view_catalog_name, view_schema_name);
	// A qualified key (sources with colliding names) still names the unqualified base table.
	source.source_table_name = BaseTableNameFromDeltaKey(loc.table_name);
	source.catalog_name = loc.catalog_name;
	source.schema_name = loc.schema_name;
	source.delta_table_sql =
	    metadata.ResolveDeltaQualifiedName(view_name, delta_table, view_catalog_name, view_schema_name);
	source.last_update = metadata.GetLastUpdateValue(view_name, delta_table);
	return source;
}

static bool IsInsertOnlyDeltaSummary(const DeltaActivityResult &summary, const vector<string> &delta_table_names) {
	if (summary.requires_full_refresh) {
		return false;
	}
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

DeltaActivityResult BuildDeltaActivityResult(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                             const vector<string> &delta_table_names, const string &view_catalog_name,
                                             const string &view_schema_name, const string &attached_db_catalog_name,
                                             const string &attached_db_schema_name) {
	DeltaActivityResult summary;
	summary.has_join = metadata.HasJoin(view_name) || delta_table_names.size() > 1;
	// Native deltas are counted together, in one statement; DuckLake sources are probed per snapshot range.
	vector<DeltaActivityResult::Source> standard_sources;
	vector<pair<string, string>> standard_deltas;
	for (auto &dt : delta_table_names) {
		if (!metadata.IsDuckLakeTable(view_name, dt)) {
			standard_sources.push_back(
			    DescribeStandardDeltaSource(metadata, view_name, dt, view_catalog_name, view_schema_name));
			auto &last_update = standard_sources.back().last_update;
			// An empty watermark yields unverified stats.
			standard_deltas.emplace_back(standard_sources.back().delta_table_sql,
			                             last_update.IsNull() ? "" : last_update.ToString());
		}
	}
	auto standard_stats = metadata.GetStandardDeltaChangeStats(standard_deltas);
	idx_t standard_index = 0;
	for (auto &dt : delta_table_names) {
		if (metadata.IsDuckLakeTable(view_name, dt)) {
			AccumulateDuckLakeDeltaSummary(summary, metadata, con, view_name, dt, view_catalog_name, view_schema_name,
			                               attached_db_catalog_name, attached_db_schema_name);
			continue;
		}
		summary.all_ducklake = false;
		auto &source = standard_sources[standard_index];
		ApplyChangeStats(source, standard_stats[standard_index++]);
		AddDeltaSource(summary, std::move(source));
	}
	OPENIVM_DEBUG_PRINT("[UPSERT] Delta activity of %s: %zu of %zu sources changed\n", view_name.c_str(),
	                    summary.active_delta_table_names.size(), delta_table_names.size());
	return summary;
}

DeltaFastPathFlags ResolveDeltaFastPathFlags(ClientContext &context, RefreshMetadata &metadata, Connection &con,
                                             const string &view_name, const vector<string> &delta_table_names,
                                             const string &view_catalog_name, const string &view_schema_name,
                                             const string &attached_db_catalog_name,
                                             const string &attached_db_schema_name,
                                             const DeltaActivityResult *precomputed_delta_activity,
                                             const openivm::CompileFacts *facts) {
	DeltaFastPathFlags flags;
	if (facts && facts->compile_only) {
		// compile_only sees empty tables and cannot detect insert-only itself, so the insert-only fast paths
		// run only when an external classifier has PROVEN this batch is append-only. Without that flag we keep
		// the conservative all-delta SQL.
		flags.active_delta_table_names = delta_table_names;
		flags.insert_only = facts->assume_insert_only;
	} else {
		auto summary =
		    precomputed_delta_activity
		        ? *precomputed_delta_activity
		        : BuildDeltaActivityResult(metadata, con, view_name, delta_table_names, view_catalog_name,
		                                   view_schema_name, attached_db_catalog_name, attached_db_schema_name);
		flags.insert_only = IsInsertOnlyDeltaSummary(summary, delta_table_names);
		flags.active_delta_table_names = summary.active_delta_table_names;
	}
	// Each insert-only fast path has its own kill switch.
	flags.skip_agg_delete =
	    flags.insert_only && SqlUtils::GetBoolSetting(context, "openivm_skip_aggregate_delete", true);
	flags.skip_proj_delete =
	    flags.insert_only && SqlUtils::GetBoolSetting(context, "openivm_skip_projection_delete", true);
	flags.minmax_incremental =
	    flags.insert_only && SqlUtils::GetBoolSetting(context, "openivm_minmax_incremental", true);
	OPENIVM_DEBUG_PRINT("[UPSERT] insert_only=%d, skip_agg_delete=%d, skip_proj_delete=%d, minmax_incremental=%d "
	                    "active_sources=%zu\n",
	                    flags.insert_only, flags.skip_agg_delete, flags.skip_proj_delete, flags.minmax_incremental,
	                    flags.active_delta_table_names.size());
	return flags;
}

} // namespace duckdb
