#include "core/ivm_view_classifier.hpp"

#include "core/openivm_debug.hpp"
#include "rules/column_hider.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

#include <unordered_set>

namespace duckdb {

namespace {

static void AddStrategyReason(vector<DeltaStrategyReason> &strategy_reasons, DeltaStrategyReason reason) {
	for (auto existing : strategy_reasons) {
		if (existing == reason) {
			return;
		}
	}
	strategy_reasons.push_back(reason);
}

static void AddModelFeature(vector<DeltaModelFeature> &features, DeltaModelFeature feature) {
	for (auto existing : features) {
		if (existing == feature) {
			return;
		}
	}
	features.push_back(feature);
}

static void AddVisibleGroupNames(vector<string> &group_columns, const vector<string> &names) {
	for (auto &name : names) {
		if (!IncrementalTableNames::IsInternalColumn(name)) {
			group_columns.push_back(name);
		}
	}
}

static void DeduplicateGroupColumns(vector<string> &group_columns) {
	unordered_set<string> seen_group;
	for (auto &name : group_columns) {
		if (IncrementalTableNames::IsInternalColumn(name)) {
			continue;
		}
		string candidate = name;
		idx_t suffix = 1;
		while (seen_group.count(candidate)) {
			candidate = name + "_" + to_string(suffix++);
		}
		seen_group.insert(candidate);
		name = candidate;
	}
}

static void AddJoinKeyGroupColumns(const CreateMVPlanFacts &facts, const vector<string> &output_names,
                                   vector<string> &aggregate_columns, vector<DeltaStrategyReason> &strategy_reasons) {
	auto *top_proj_ptr = facts.first_projection;
	auto *cjoin = facts.first_comparison_join;
	if (!top_proj_ptr || !cjoin) {
		return;
	}

	unordered_map<idx_t, unordered_set<idx_t>> join_key_cols;
	for (auto &cond : cjoin->conditions) {
		AddJoinKeyColumn(cond.left, join_key_cols);
		AddJoinKeyColumn(cond.right, join_key_cols);
	}

	auto &top_proj = *top_proj_ptr;
	for (idx_t expr_i = 0; expr_i < top_proj.expressions.size(); expr_i++) {
		auto &expr = top_proj.expressions[expr_i];
		if (expr->type != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &bcr = expr->Cast<BoundColumnRefExpression>();
		auto it = join_key_cols.find(bcr.binding.table_index);
		if (it == join_key_cols.end() || !it->second.count(bcr.binding.column_index)) {
			continue;
		}

		string col_name;
		if (!expr->alias.empty()) {
			col_name = expr->alias;
		} else if (expr_i < output_names.size() && !output_names[expr_i].empty() &&
		           !IncrementalTableNames::IsInternalColumn(output_names[expr_i])) {
			col_name = output_names[expr_i];
		} else {
			col_name = bcr.GetName();
		}
		if (IncrementalTableNames::IsInternalColumn(col_name)) {
			continue;
		}

		bool exists = false;
		for (auto &existing : aggregate_columns) {
			if (StringUtil::CIEquals(existing, col_name)) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			aggregate_columns.push_back(col_name);
			AddStrategyReason(strategy_reasons, DeltaStrategyReason::JOIN_KEY_GROUP_FALLBACK);
		}
	}
}

static void BuildModelFeatures(DeltaViewModel &model, const PlanAnalysis &analysis, const DeltaViewModelInput &input) {
	if (input.has_unsupported_incremental_construct || !analysis.incremental_compatible) {
		AddModelFeature(model.features, DeltaModelFeature::FULL_ONLY);
	}
	if (analysis.found_projection || (!analysis.found_aggregation && !analysis.found_join && !analysis.found_window)) {
		AddModelFeature(model.features, DeltaModelFeature::LINEAR);
	}
	if (analysis.found_join && !analysis.found_left_join && !analysis.found_full_outer &&
	    !analysis.found_semi_anti_join) {
		AddModelFeature(model.features, DeltaModelFeature::BILINEAR);
	}
	if (analysis.found_left_join || analysis.found_full_outer) {
		AddModelFeature(model.features, DeltaModelFeature::OUTER_JOIN_STATEFUL);
	}
	if (analysis.found_aggregation) {
		if (analysis.found_minmax || analysis.found_count_distinct || analysis.found_list ||
		    analysis.found_filtered_list) {
			AddModelFeature(model.features, DeltaModelFeature::AGGREGATE_NON_LINEAR);
		} else {
			AddModelFeature(model.features, DeltaModelFeature::AGGREGATE_LINEAR);
		}
	}
	if (analysis.found_window && !model.window_partition_columns.empty()) {
		AddModelFeature(model.features, DeltaModelFeature::WINDOW_AFFECTED_PARTITION);
	}
	if (analysis.found_semi_anti_join && !input.semi_anti_aux_candidate) {
		AddModelFeature(model.features, DeltaModelFeature::FULL_ONLY);
	}
	if (analysis.found_grouping_sets && model.group_columns.empty()) {
		AddModelFeature(model.features, DeltaModelFeature::FULL_ONLY);
	}
	if (analysis.found_filtered_list && model.group_columns.empty()) {
		AddModelFeature(model.features, DeltaModelFeature::FULL_ONLY);
	}
	if (!model.HasFeature(DeltaModelFeature::FULL_ONLY)) {
		if (input.distinct_aux_candidate) {
			AddModelFeature(model.features, DeltaModelFeature::DISTINCT_STATEFUL);
		}
		if (input.semi_anti_aux_candidate) {
			AddModelFeature(model.features, DeltaModelFeature::SEMI_ANTI_STATEFUL);
		}
	}
}

static void BuildGroupColumns(DeltaViewModel &model, const CreateMVPlanFacts &facts,
                              const vector<string> &output_names) {
	const auto &analysis = facts.analysis;
	const auto group_count = analysis.group_count;
	const auto group_index = analysis.group_index;
	const bool has_union_over_agg = analysis.found_aggregation && facts.has_union_before_aggregate;
	model.union_distinct_over_agg = analysis.found_distinct && model.distinct_at_top && has_union_over_agg;

	if (model.union_distinct_over_agg) {
		model.group_columns = DeriveGroupColumnNames(facts, group_index, group_count, output_names);
	} else if (model.distinct_at_top) {
		model.group_columns = analysis.aggregate_columns;
	} else if (analysis.found_distinct && analysis.aggregate_columns.empty()) {
		model.group_columns = analysis.aggregate_columns;
	} else if (group_count > 0 && group_index != DConstants::INVALID_INDEX) {
		model.group_columns = DeriveGroupColumnNames(facts, group_index, group_count, output_names);
	}

	if (analysis.found_delim_join && analysis.found_aggregation && model.group_columns.empty() &&
	    output_names.size() > analysis.aggregate_types.size()) {
		idx_t key_count = output_names.size() - analysis.aggregate_types.size();
		for (idx_t i = 0; i < key_count; i++) {
			if (!output_names[i].empty() && !IncrementalTableNames::IsInternalColumn(output_names[i])) {
				model.group_columns.push_back(output_names[i]);
			}
		}
		if (!model.group_columns.empty()) {
			AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::DELIM_AGGREGATE_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] DELIM/DEPENDENT aggregate: using %zu visible key columns for "
			                    "GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	if (analysis.found_delim_join && analysis.found_single_join && !analysis.found_aggregation &&
	    model.group_columns.empty()) {
		auto before = model.group_columns.size();
		AddVisibleGroupNames(model.group_columns, DeriveScalarDelimKeyColumnNames(facts, output_names));
		if (model.group_columns.size() > before) {
			AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::SCALAR_DELIM_PROJECTION_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Scalar DELIM/DEPENDENT projection: using %zu visible key columns "
			                    "for GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	if (analysis.found_nested_aggregate && model.group_columns.empty()) {
		auto before = model.group_columns.size();
		AddVisibleGroupNames(model.group_columns, DeriveAggregateGroupColumnNames(facts, output_names, false));
		if (model.group_columns.size() > before) {
			AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::NESTED_AGGREGATE_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Nested aggregate: using %zu visible inner group columns for "
			                    "GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	if (analysis.found_aggregation && model.group_columns.empty()) {
		auto before = model.group_columns.size();
		AddVisibleGroupNames(model.group_columns, DeriveAggregateGroupColumnNames(facts, output_names, true));
		if (model.group_columns.size() > before) {
			AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::REPEATED_CTE_AGGREGATE_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Repeated CTE aggregate under join: using %zu visible group columns "
			                    "for GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	DeduplicateGroupColumns(model.group_columns);

	if (analysis.found_join && group_count > 0 && !has_union_over_agg) {
		AddJoinKeyGroupColumns(facts, output_names, model.group_columns, model.strategy_reasons);
	}
	if (analysis.found_join && analysis.found_aggregation && !model.group_columns.empty()) {
		ResolveAggregateGroupColumnsThroughJoinKeys(facts, model.group_columns, output_names);
	}

	if (analysis.found_join && analysis.found_aggregation && !model.group_columns.empty()) {
		idx_t expected_linear_outputs = model.group_columns.size() + model.aggregate_types.size();
		if (output_names.size() > expected_linear_outputs) {
			AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::JOIN_AGGREGATE_PROJECTION_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Join-over-aggregate exposes %zu columns but only %zu are "
			                    "group/aggregate outputs -- using GROUP_RECOMPUTE\n",
			                    output_names.size(), expected_linear_outputs);
		}
	}

	if (has_union_over_agg) {
		AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::UNION_OVER_AGGREGATE);
	}
	if (analysis.found_nested_aggregate) {
		AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::NESTED_AGGREGATE_GROUP_FALLBACK);
	}
}

static void SelectRefreshType(DeltaViewModel &model, const PlanAnalysis &analysis,
                              bool has_unsupported_incremental_construct) {
	if (has_unsupported_incremental_construct) {
		model.type = RefreshType::FULL_REFRESH;
	} else if (analysis.found_window) {
		model.type = RefreshType::WINDOW_PARTITION;
	} else if (analysis.found_grouping_sets) {
		model.type = model.group_columns.empty() ? RefreshType::FULL_REFRESH : RefreshType::GROUP_RECOMPUTE;
	} else if (analysis.found_semi_anti_join && analysis.found_aggregation) {
		model.type = RefreshType::FULL_REFRESH;
	} else if (analysis.found_semi_anti_join && !analysis.found_aggregation) {
		model.type = model.HasFeature(DeltaModelFeature::SEMI_ANTI_STATEFUL) ? RefreshType::SEMI_ANTI_RECOMPUTE
		                                                                     : RefreshType::FULL_REFRESH;
	} else if (!analysis.incremental_compatible) {
		model.type = RefreshType::FULL_REFRESH;
		model.warn_unsupported_incremental = true;
	} else if (analysis.found_filtered_list && !model.group_columns.empty()) {
		model.type = RefreshType::GROUP_RECOMPUTE;
	} else if (analysis.found_filtered_list) {
		model.type = RefreshType::FULL_REFRESH;
	} else if (analysis.found_count_distinct && !model.group_columns.empty()) {
		model.type = RefreshType::GROUP_RECOMPUTE;
	} else if (analysis.found_distinct && !model.distinct_at_top && analysis.found_aggregation) {
		model.type = model.HasFeature(DeltaModelFeature::DISTINCT_STATEFUL) ? RefreshType::DISTINCT_INCREMENTAL
		                                                                    : RefreshType::GROUP_RECOMPUTE;
	} else if (model.union_distinct_over_agg && !model.group_columns.empty()) {
		model.type = RefreshType::GROUP_RECOMPUTE;
	} else if (analysis.found_distinct && model.distinct_at_top && !model.group_columns.empty()) {
		model.type = RefreshType::AGGREGATE_GROUP;
	} else if (analysis.found_having && analysis.found_aggregation && !model.group_columns.empty()) {
		model.type = RefreshType::AGGREGATE_HAVING;
	} else if (!model.strategy_reasons.empty() && !model.group_columns.empty()) {
		model.type = RefreshType::GROUP_RECOMPUTE;
	} else if (analysis.found_aggregation && !model.group_columns.empty()) {
		model.type = RefreshType::AGGREGATE_GROUP;
	} else if (analysis.found_aggregation && model.group_columns.empty()) {
		model.type = RefreshType::SIMPLE_AGGREGATE;
	} else if (analysis.found_projection && !analysis.found_aggregation) {
		model.type = RefreshType::SIMPLE_PROJECTION;
	} else {
		model.type = RefreshType::FULL_REFRESH;
		model.warn_unrecognized_pattern = true;
	}
}

static void AttachAuxRequirements(DeltaViewModel &model, const DeltaViewModelInput &input) {
	if (model.type == RefreshType::DISTINCT_INCREMENTAL) {
		D_ASSERT(input.distinct_aux_candidate);
		model.distinct_aux = *input.distinct_aux_candidate;
	}
	if (model.type == RefreshType::SIMPLE_AGGREGATE && input.filtered_group_count_aux_candidate) {
		model.filtered_group_count_aux = *input.filtered_group_count_aux_candidate;
	}
	if (model.type == RefreshType::SEMI_ANTI_RECOMPUTE) {
		D_ASSERT(input.semi_anti_aux_candidate);
		model.semi_anti_aux = *input.semi_anti_aux_candidate;
	}
}

static void ValidateModelInvariants(const DeltaViewModel &model) {
	D_ASSERT(!model.HasFeature(DeltaModelFeature::DISTINCT_STATEFUL) || model.HasDistinctAux());
	D_ASSERT(!model.HasFeature(DeltaModelFeature::SEMI_ANTI_STATEFUL) || model.HasSemiAntiAux());
	D_ASSERT(!model.HasFeature(DeltaModelFeature::WINDOW_AFFECTED_PARTITION) ||
	         !model.window_partition_columns.empty());
	D_ASSERT(model.type != RefreshType::DISTINCT_INCREMENTAL || model.HasFeature(DeltaModelFeature::DISTINCT_STATEFUL));
	D_ASSERT(model.type != RefreshType::SEMI_ANTI_RECOMPUTE || model.HasFeature(DeltaModelFeature::SEMI_ANTI_STATEFUL));
}

} // namespace

const char *DeltaStrategyReasonName(DeltaStrategyReason reason) {
	switch (reason) {
	case DeltaStrategyReason::UNION_OVER_AGGREGATE:
		return "UNION_OVER_AGGREGATE";
	case DeltaStrategyReason::JOIN_KEY_GROUP_FALLBACK:
		return "JOIN_KEY_GROUP_FALLBACK";
	case DeltaStrategyReason::DELIM_AGGREGATE_GROUP_FALLBACK:
		return "DELIM_AGGREGATE_GROUP_FALLBACK";
	case DeltaStrategyReason::SCALAR_DELIM_PROJECTION_GROUP_FALLBACK:
		return "SCALAR_DELIM_PROJECTION_GROUP_FALLBACK";
	case DeltaStrategyReason::JOIN_AGGREGATE_PROJECTION_FALLBACK:
		return "JOIN_AGGREGATE_PROJECTION_FALLBACK";
	case DeltaStrategyReason::NESTED_AGGREGATE_GROUP_FALLBACK:
		return "NESTED_AGGREGATE_GROUP_FALLBACK";
	case DeltaStrategyReason::REPEATED_CTE_AGGREGATE_GROUP_FALLBACK:
		return "REPEATED_CTE_AGGREGATE_GROUP_FALLBACK";
	case DeltaStrategyReason::OUTER_JOIN_AGGREGATE_RECOMPUTE:
		return "OUTER_JOIN_AGGREGATE_RECOMPUTE";
	default:
		return "UNKNOWN";
	}
}

const char *DeltaModelFeatureName(DeltaModelFeature feature) {
	switch (feature) {
	case DeltaModelFeature::LINEAR:
		return "LINEAR";
	case DeltaModelFeature::BILINEAR:
		return "BILINEAR";
	case DeltaModelFeature::OUTER_JOIN_STATEFUL:
		return "OUTER_JOIN_STATEFUL";
	case DeltaModelFeature::AGGREGATE_LINEAR:
		return "AGGREGATE_LINEAR";
	case DeltaModelFeature::AGGREGATE_NON_LINEAR:
		return "AGGREGATE_NON_LINEAR";
	case DeltaModelFeature::DISTINCT_STATEFUL:
		return "DISTINCT_STATEFUL";
	case DeltaModelFeature::WINDOW_AFFECTED_PARTITION:
		return "WINDOW_AFFECTED_PARTITION";
	case DeltaModelFeature::SEMI_ANTI_STATEFUL:
		return "SEMI_ANTI_STATEFUL";
	case DeltaModelFeature::FULL_ONLY:
		return "FULL_ONLY";
	default:
		return "UNKNOWN";
	}
}

bool DeltaViewModel::HasFeature(DeltaModelFeature feature) const {
	for (auto existing : features) {
		if (existing == feature) {
			return true;
		}
	}
	return false;
}

bool IsDistinctAtTop(const PlanAnalysis &analysis, const vector<string> &output_names) {
	if (!analysis.found_distinct || analysis.aggregate_columns.empty() || output_names.empty()) {
		return false;
	}

	unordered_set<string> output_lc;
	for (auto &name : output_names) {
		output_lc.insert(StringUtil::Lower(name));
	}

	for (auto &target : analysis.aggregate_columns) {
		if (!output_lc.count(StringUtil::Lower(target))) {
			return false;
		}
	}
	return true;
}

DeltaViewModel BuildDeltaViewModel(const DeltaViewModelInput &input) {
	D_ASSERT(input.facts);
	D_ASSERT(input.output_names);
	auto &facts = *input.facts;
	auto &analysis = facts.analysis;
	auto &output_names = *input.output_names;

	DeltaViewModel model;
	model.aggregate_types = analysis.aggregate_types;
	model.window_partition_columns = analysis.window_partition_columns;
	ResolveWindowPartitionOutputNames(facts, model.window_partition_columns, output_names);
	if (!input.keep_window_join_partitions) {
		model.window_partition_columns.clear();
	}

	model.has_minmax_metadata = analysis.found_minmax || analysis.found_count_distinct || analysis.found_list;
	model.distinct_at_top = IsDistinctAtTop(analysis, output_names);
	BuildGroupColumns(model, facts, output_names);

	if ((analysis.found_left_join || analysis.found_full_outer) && analysis.found_aggregation &&
	    OuterJoinAggregateNeedsRecompute(facts, analysis.group_index)) {
		model.has_minmax_metadata = true;
		AddStrategyReason(model.strategy_reasons, DeltaStrategyReason::OUTER_JOIN_AGGREGATE_RECOMPUTE);
		OPENIVM_DEBUG_PRINT("[CREATE MV] LEFT/OUTER JOIN aggregate with computed aggregate or projection wrapper -- "
		                    "using group-recompute metadata\n");
	}

	if (analysis.found_full_outer) {
		model.full_outer_join_cols = ExtractFullOuterJoinMetadata(facts);
	}

	BuildModelFeatures(model, analysis, input);
	SelectRefreshType(model, analysis, input.has_unsupported_incremental_construct);
	AttachAuxRequirements(model, input);
	ValidateModelInvariants(model);
	return model;
}

void PopulateDeltaViewModelLineage(DeltaViewModel &model, const CreateMVPlanFacts &facts,
                                   const vector<string> &output_names) {
	model.lineage_entries.clear();
	const auto &analysis = facts.analysis;
	if (model.type == RefreshType::WINDOW_PARTITION) {
		string lineage_entry = BuildWindowPartitionLineageEntryJson(facts, model.window_partition_columns);
		if (!lineage_entry.empty()) {
			model.lineage_entries.push_back(std::move(lineage_entry));
		}
		return;
	}
	if (model.type == RefreshType::SIMPLE_PROJECTION && !analysis.found_left_join && !analysis.found_full_outer) {
		string lineage_entry = BuildProjectionKeyLineageEntryJson(facts, output_names);
		if (!lineage_entry.empty()) {
			model.lineage_entries.push_back(std::move(lineage_entry));
		}
	}
}

} // namespace duckdb
