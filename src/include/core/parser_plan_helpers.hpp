#ifndef OPENIVM_PARSER_PLAN_HELPERS_HPP
#define OPENIVM_PARSER_PLAN_HELPERS_HPP

#include "core/incremental_checker.hpp"
#include "core/refresh_metadata.hpp"
#include "duckdb.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

struct PlanRewriteNeeds;
class BoundAggregateExpression;
bool IsDerivedAggregate(const BoundAggregateExpression &aggregate);
class Planner;

// Compiler-only source boundary; generated columns retain their logical identity.
void PlanWithSourceColumns(Planner &planner, unique_ptr<SQLStatement> statement);
TableFunction SourceColumnsFunction();
ScalarFunction CandidateValueKeyFunction();

BoundColumnRefExpression *GetColumnRefThroughCasts(Expression *expr, string *cast_type = nullptr);

struct DuckLakeSourceTableInfo {
	string table_name;
	string catalog_name;
	string schema_name;
	int64_t table_id = -1;
};

struct SourceTableInfo {
	string table_name;
	string catalog_name;
	string schema_name;
};

struct OccurrenceColumnRef {
	string table;
	idx_t occurrence = 0;
	string column;
};

struct ProjectionSourceOccurrence {
	string table;
	idx_t occurrence = 0;
	TableIndex table_index {0};
};

struct ProjectionLineageEdge {
	OccurrenceColumnRef from;
	OccurrenceColumnRef to;
};

struct CreateMVPlanNodeFacts {
	LogicalOperator *plan_node = nullptr;
	vector<idx_t> child_ids;
	vector<idx_t> group_column_search_ids;
	vector<TableIndex> subtree_table_indices;
	vector<string> subtree_base_tables;
	bool subtree_base_tables_complete = true;
	bool contains_table_function = false;
};

struct CreateMVPlanFacts {
	LogicalOperator *root = nullptr;
	vector<CreateMVPlanNodeFacts> plan_nodes_post_order;
	idx_t max_table_index = 0;
	PlanAnalysis analysis;
	unordered_map<string, SourceTableInfo> source_table_info;
	case_insensitive_map_t<idx_t> source_name_counts;
	unordered_map<string, DuckLakeSourceTableInfo> ducklake_table_info;
	LogicalProjection *first_projection = nullptr;
	vector<LogicalProjection *> projections;
	vector<LogicalAggregate *> aggregates;
	vector<LogicalComparisonJoin *> comparison_joins;
	vector<LogicalJoin *> outer_joins;
	vector<LogicalWindow *> windows;
	unordered_map<const LogicalOperator *, idx_t> plan_node_ids;
	unordered_map<TableIndex, LogicalProjection *> projections_by_index;
	unordered_map<TableIndex, LogicalGet *> gets_by_index;
	unordered_map<TableIndex, LogicalSetOperation *> setops_by_index;
	unordered_map<TableIndex, LogicalCTERef *> cte_refs_by_table_index;
	unordered_map<TableIndex, LogicalOperator *> cte_defs_by_index;
	unordered_map<string, string> join_parents;
	vector<BoundColumnRefExpression *> join_refs;
	unordered_set<string> single_delim_key_bindings;
	unordered_map<TableIndex, ProjectionSourceOccurrence> occurrence_by_index;
	vector<ProjectionSourceOccurrence> source_occurrences;
	vector<ProjectionLineageEdge> projection_lineage_edges;
	bool has_union_before_aggregate = false;
	bool has_unsupported_set_operation = false;
	bool has_pivot = false;
	bool has_filter_above_aggregate = false;
	bool has_cardinality_changing_filter = false;
	bool has_hidden_minmax_having_column = false;
	bool has_computed_minmax_aggregate_projection = false;
	bool has_computed_sum_aggregate_projection = false;
	bool has_top_level_redundant_distinct = false;
	bool has_descendant_distinct = false;
};

void StripPublicationModifiers(ClientContext &context, unique_ptr<LogicalOperator> &plan, vector<string> &output_names,
                               string &suffix, string &ordering);
// Append every input column of a TOP_N, or of the ORDER BY under a LIMIT, as an ascending tie-breaker. Returns
// whether keys were added. A nested top-k is maintained by diffing its selection over current and pre-delta input,
// which is only sound when both pick the same tuples among ties.
bool TotalizeTopKOrder(LogicalOperator &selection);
// Totalize every top-k left in the stored plan. Call after StripPublicationModifiers: the view's outermost ORDER
// BY/LIMIT is published by the user view, so every TOP_N/LIMIT still present is maintained by diffing its selection.
// Returns whether any remains.
bool TotalizeTopKOrders(LogicalOperator &plan);
/// Collect rewrite requirements while preparing CTEs, then inline eligible CTEs.
PlanRewriteNeeds InlineCtesIfPresent(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan);
// Qualify an auxiliary-state source by the catalog and schema the view bound it to.
string QualifyCreateSourceTable(ClientContext &context, const string &table_name, const CreateMVPlanFacts &facts,
                                const string &current_catalog, const string &current_schema, const string &default_db,
                                const string &default_schema);
CreateMVPlanFacts BuildCreateMVPlanFacts(LogicalOperator *plan, const string &current_catalog);
bool ResolvesToOutputBinding(TableIndex table_index, ProjectionIndex column_index, TableIndex group_index,
                             size_t group_count, const CreateMVPlanFacts &facts, bool through_casts, int depth = 0);
bool ProducesAtMostOneRow(LogicalOperator &node);
bool IsRedundantDistinctOverGroupKeys(LogicalOperator &node);
void AddJoinKeyColumn(const unique_ptr<Expression> &expr,
                      unordered_map<TableIndex, unordered_set<ProjectionIndex>> &join_key_cols);
bool OuterJoinAggregateNeedsRecompute(const CreateMVPlanFacts &facts, TableIndex group_index);
// delta_view_catalog_prefix must be the same prefix the view's delta table was created with
// (internal_catalog_prefix). Without it the generated INSERT targets an unqualified delta table,
// which fails for MVs whose state lives in another catalog -- e.g. DuckLake-backed views, where the
// delta table is dl.main.openivm_delta_<view>.
// The returned SQL carries one LJSEC_INNER_DELTA_ placeholder per level where the inner side's pending
// changes belong; refresh substitutes it (see openivm_constants.hpp) using the recorded inner identity.
// The Larson & Zhou secondary-delta correction for every LEFT JOIN level; empty sql when unsupported.
RefreshMetadata::LeftJoinSecondaryMeta BuildLeftJoinSecondaryDeltaSQL(Binder &binder, const CreateMVPlanFacts &facts,
                                                                      const vector<string> &output_names,
                                                                      const string &view_name,
                                                                      const string &delta_view_catalog_prefix);
bool OuterJoinPreservedSideHasTableFunction(const CreateMVPlanFacts &facts);
vector<string> DeriveGroupColumnNames(const CreateMVPlanFacts &facts, TableIndex group_index, size_t group_count,
                                      const vector<string> &output_names);
LogicalAggregate *FindViewOutputAggregate(LogicalOperator *root);
class AstNode;
string BuildDistinctSelectedViewSQL(AstNode &node, const vector<string> &input_columns,
                                    const vector<string> &output_columns);
void PopulateDistinctAggregateInputs(ClientContext &context, Binder &binder, const CreateMVPlanFacts &facts,
                                     const vector<string> &output_names, RefreshMetadata::DistinctAuxMeta &meta);
vector<string> DeriveScalarDelimKeyColumnNames(const CreateMVPlanFacts &facts, const vector<string> &output_names);
vector<string> DeriveAggregateGroupColumnNames(const CreateMVPlanFacts &facts, const vector<string> &output_names,
                                               bool include_first_aggregate);
void ResolveWindowPartitionOutputNames(const CreateMVPlanFacts &facts, vector<string> &partition_columns,
                                       const vector<string> &output_names);
string BuildRefreshLineageJson(const vector<string> &entries);
bool BuildWindowPartitionLineageOps(const CreateMVPlanFacts &facts, const vector<string> &partition_columns,
                                    vector<RefreshMetadata::WindowPartitionLineageOp> &out,
                                    vector<RefreshMetadata::WindowPartitionLineageOp> *direct_out = nullptr);
bool BuildProjectionKeyLineage(const CreateMVPlanFacts &facts, const vector<string> &output_names,
                               RefreshMetadata::ProjectionKeyLineage &out);
/// A same-named source/output key for a single-source, key-local plan; empty if
/// unproven.
string DeriveDirectSourceGroupKey(const CreateMVPlanFacts &facts, const vector<string> &output_names);
bool BuildLeftJoinKeySource(const CreateMVPlanFacts &facts, RefreshMetadata::LeftJoinKeySource &out);
bool BuildLeftJoinNullableSources(const CreateMVPlanFacts &facts, RefreshMetadata::LeftJoinNullableSources &out);
bool PlanNeedsOriginalSqlForLpts(const CreateMVPlanFacts &facts);
void ResolveAggregateGroupColumnsThroughJoinKeys(const CreateMVPlanFacts &facts, vector<string> &aggregate_columns,
                                                 const vector<string> &output_names);
string ExtractFullOuterJoinMetadata(const CreateMVPlanFacts &facts);
vector<RefreshMetadata::FullOuterScope> BuildFullOuterScopes(const CreateMVPlanFacts &facts);
void ValidateMaterializedViewOutputNames(const QueryNode &query, const vector<string> &original_names,
                                         const vector<string> &output_names, idx_t visible_count);
vector<string> PrepareOutputNames(LogicalOperator *select_plan, const vector<string> &planner_names);
bool IsPacLoaded(ClientContext &context);
void ForwardPacSettingsIfLoaded(ClientContext &context, Connection &con);

} // namespace duckdb

#endif // OPENIVM_PARSER_PLAN_HELPERS_HPP
