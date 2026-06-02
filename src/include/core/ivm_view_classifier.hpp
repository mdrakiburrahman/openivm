#ifndef IVM_VIEW_CLASSIFIER_HPP
#define IVM_VIEW_CLASSIFIER_HPP

#include "core/openivm_constants.hpp"
#include "core/parser_plan_helpers.hpp"

namespace duckdb {

enum class DeltaStrategyReason {
	UNION_OVER_AGGREGATE,
	JOIN_KEY_GROUP_FALLBACK,
	DELIM_AGGREGATE_GROUP_FALLBACK,
	SCALAR_DELIM_PROJECTION_GROUP_FALLBACK,
	JOIN_AGGREGATE_PROJECTION_FALLBACK,
	NESTED_AGGREGATE_GROUP_FALLBACK,
	REPEATED_CTE_AGGREGATE_GROUP_FALLBACK,
	OUTER_JOIN_AGGREGATE_RECOMPUTE
};

struct DeltaViewModelInput {
	const CreateMVPlanFacts *facts = nullptr;
	const vector<string> *output_names = nullptr;
	bool has_unsupported_incremental_construct = false;
	bool distinct_incremental_supported = false;
	bool semi_anti_recompute_supported = false;
	bool keep_window_join_partitions = true;
};

struct DeltaViewModel {
	RefreshType type = RefreshType::FULL_REFRESH;
	vector<DeltaStrategyReason> strategy_reasons;
	vector<string> group_columns;
	vector<string> window_partition_columns;
	vector<string> aggregate_types;
	string full_outer_join_cols;
	bool has_minmax_metadata = false;
	bool distinct_at_top = false;
	bool union_distinct_over_agg = false;
	bool warn_unsupported_incremental = false;
	bool warn_unrecognized_pattern = false;
};

const char *DeltaStrategyReasonName(DeltaStrategyReason reason);
bool IsDistinctAtTop(const PlanAnalysis &analysis, const vector<string> &output_names);
DeltaViewModel BuildDeltaViewModel(const DeltaViewModelInput &input);

} // namespace duckdb

#endif // IVM_VIEW_CLASSIFIER_HPP
