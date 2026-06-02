#include "core/ivm_view_classifier.hpp"

#include "core/openivm_debug.hpp"
#include "rules/column_hider.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_join.hpp"

#include <algorithm>
#include <unordered_set>

namespace duckdb {

namespace {

template <class T>
static void AddUnique(vector<T> &entries, T entry) {
	for (auto existing : entries) {
		if (existing == entry) {
			return;
		}
	}
	entries.push_back(entry);
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
			AddUnique(strategy_reasons, DeltaStrategyReason::JOIN_KEY_GROUP_FALLBACK);
		}
	}
}

static bool ContainsStringCI(const vector<string> &values, const string &candidate) {
	for (auto &value : values) {
		if (StringUtil::CIEquals(value, candidate)) {
			return true;
		}
	}
	return false;
}

static void AddStringCI(vector<string> &values, const string &candidate) {
	if (!candidate.empty() && !ContainsStringCI(values, candidate)) {
		values.push_back(candidate);
	}
}

static vector<string> SourceTablesFromFacts(const CreateMVPlanFacts &facts) {
	vector<string> tables;
	for (auto &occurrence : facts.source_occurrences) {
		AddStringCI(tables, occurrence.table);
	}
	if (tables.empty()) {
		for (auto &entry : facts.source_table_info) {
			AddStringCI(tables, entry.second.table_name);
		}
		std::sort(tables.begin(), tables.end(), [](const string &left, const string &right) {
			return StringUtil::Lower(left) < StringUtil::Lower(right);
		});
	}
	return tables;
}

static vector<string> SourceTablesFromChildren(const DeltaViewModel &model, const vector<idx_t> &children) {
	vector<string> tables;
	for (auto child_id : children) {
		if (child_id >= model.nodes.size()) {
			continue;
		}
		for (auto &source : model.nodes[child_id].source_tables) {
			AddStringCI(tables, source);
		}
	}
	return tables;
}

static void SplitOutputColumns(const vector<string> &input, vector<string> &visible, vector<string> &hidden) {
	for (auto &name : input) {
		if (name.empty()) {
			continue;
		}
		if (IncrementalTableNames::IsInternalColumn(name)) {
			hidden.push_back(name);
		} else {
			visible.push_back(name);
		}
	}
}

static DeltaModelNodeKind NodeKindForOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return DeltaModelNodeKind::SCAN;
	case LogicalOperatorType::LOGICAL_FILTER:
		return DeltaModelNodeKind::FILTER;
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return DeltaModelNodeKind::PROJECT;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return DeltaModelNodeKind::AGGREGATE;
	case LogicalOperatorType::LOGICAL_WINDOW:
		return DeltaModelNodeKind::WINDOW;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return DeltaModelNodeKind::DISTINCT;
	case LogicalOperatorType::LOGICAL_UNION:
		return DeltaModelNodeKind::UNION;
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_LIMIT:
		return DeltaModelNodeKind::TOP_K;
	case LogicalOperatorType::LOGICAL_UNNEST:
		return DeltaModelNodeKind::UNNEST;
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		return DeltaModelNodeKind::CTE;
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		auto *join = dynamic_cast<LogicalJoin *>(&op);
		if (join && (join->join_type == JoinType::SEMI || join->join_type == JoinType::ANTI ||
		             join->join_type == JoinType::MARK)) {
			return DeltaModelNodeKind::SEMI_ANTI;
		}
		return DeltaModelNodeKind::JOIN;
	}
	default:
		return DeltaModelNodeKind::OTHER;
	}
}

static DeltaRuleKind RuleKindForNode(DeltaModelNodeKind kind, LogicalOperator &op, const DeltaViewModel &model,
                                     const PlanAnalysis &analysis) {
	if (!model.unsupported_reasons.empty()) {
		return DeltaRuleKind::FULL_ONLY;
	}
	switch (kind) {
	case DeltaModelNodeKind::SCAN:
	case DeltaModelNodeKind::FILTER:
	case DeltaModelNodeKind::PROJECT:
	case DeltaModelNodeKind::UNION:
	case DeltaModelNodeKind::UNNEST:
	case DeltaModelNodeKind::CTE:
		return DeltaRuleKind::LINEAR;
	case DeltaModelNodeKind::JOIN: {
		auto *join = dynamic_cast<LogicalJoin *>(&op);
		if (join && (join->join_type == JoinType::LEFT || join->join_type == JoinType::RIGHT ||
		             join->join_type == JoinType::OUTER)) {
			return DeltaRuleKind::STATEFUL;
		}
		return DeltaRuleKind::PRODUCT;
	}
	case DeltaModelNodeKind::AGGREGATE:
		return analysis.found_minmax || analysis.found_count_distinct || analysis.found_list ||
		               analysis.found_filtered_list
		           ? DeltaRuleKind::NON_LINEAR
		           : DeltaRuleKind::STATEFUL;
	case DeltaModelNodeKind::WINDOW:
	case DeltaModelNodeKind::DISTINCT:
	case DeltaModelNodeKind::SEMI_ANTI:
	case DeltaModelNodeKind::TOP_K:
		return DeltaRuleKind::STATEFUL;
	default:
		return DeltaRuleKind::FULL_ONLY;
	}
}

static idx_t AddModelNode(DeltaViewModel &model, DeltaModelNode node) {
	node.id = model.nodes.size();
	model.nodes.push_back(std::move(node));
	return model.nodes.back().id;
}

static void AddNodeAuxRequirements(DeltaModelNode &node, const DeltaViewModel &model) {
	if (model.HasDistinctAux() && node.kind == DeltaModelNodeKind::DISTINCT) {
		AddUnique(node.required_aux_states, DeltaAuxStateKind::DISTINCT_COUNT);
	}
	if (model.HasFilteredGroupCountAux() && node.kind == DeltaModelNodeKind::AGGREGATE) {
		AddUnique(node.required_aux_states, DeltaAuxStateKind::FILTERED_GROUP_COUNT);
	}
	if (model.HasSemiAntiAux() && node.kind == DeltaModelNodeKind::SEMI_ANTI) {
		AddUnique(node.required_aux_states, DeltaAuxStateKind::SEMI_ANTI_MATCH);
	}
}

static idx_t BuildModelNodeForPlan(DeltaViewModel &model, LogicalOperator *op, const CreateMVPlanFacts &facts,
                                   const vector<string> &output_names) {
	if (!op) {
		return DConstants::INVALID_INDEX;
	}
	vector<idx_t> children;
	for (auto &child : op->children) {
		auto child_id = BuildModelNodeForPlan(model, child.get(), facts, output_names);
		if (child_id != DConstants::INVALID_INDEX) {
			children.push_back(child_id);
		}
	}

	DeltaModelNode node;
	node.kind = NodeKindForOperator(*op);
	node.rule = RuleKindForNode(node.kind, *op, model, facts.analysis);
	node.children = std::move(children);
	node.source_tables = SourceTablesFromChildren(model, node.children);
	AddNodeAuxRequirements(node, model);

	if (node.kind == DeltaModelNodeKind::SCAN) {
		auto *get = dynamic_cast<LogicalGet *>(op);
		if (get) {
			auto occurrence_it = facts.occurrence_by_index.find(get->table_index);
			if (occurrence_it != facts.occurrence_by_index.end()) {
				AddStringCI(node.source_tables, occurrence_it->second.table);
			}
			SplitOutputColumns(get->names, node.output_columns, node.hidden_columns);
		}
	}
	if (node.kind == DeltaModelNodeKind::AGGREGATE) {
		node.affected_key_columns = model.group_columns;
	}
	if (node.kind == DeltaModelNodeKind::WINDOW) {
		node.affected_key_columns = model.window_partition_columns;
	}
	if (op == facts.root) {
		SplitOutputColumns(output_names, node.output_columns, node.hidden_columns);
	}
	return AddModelNode(model, std::move(node));
}

static void BuildModelNodes(DeltaViewModel &model, const CreateMVPlanFacts &facts, const vector<string> &output_names) {
	model.nodes.clear();
	model.root_node = BuildModelNodeForPlan(model, facts.root, facts, output_names);
}

static void BuildUnsupportedReasons(DeltaViewModel &model, const CreateMVPlanFacts &facts,
                                    const DeltaViewModelInput &input) {
	const auto &analysis = facts.analysis;
	if (facts.has_unsupported_set_operation) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_SET_OPERATION);
	}
	if (facts.has_pivot) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_PIVOT);
	}
	if (analysis.found_volatile_expression) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_FUNCTION);
	}
	if (analysis.found_non_foldable_unnest) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_UNNEST);
	}
	if (analysis.found_unsupported_aggregate) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_AGGREGATE);
	}
	if (analysis.found_unsupported_filtered_aggregate) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_FILTERED_AGGREGATE);
	}
	if (analysis.found_unsupported_join_type) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_JOIN_TYPE);
	}
	if (analysis.found_unsupported_order_by) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_ORDER_BY);
	}
	if (analysis.found_unsupported_operator) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_OPERATOR);
	}
	if (analysis.found_grouping_sets && model.group_columns.empty()) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::GROUPING_SETS_NO_KEYS);
	}
	if (analysis.found_filtered_list && model.group_columns.empty()) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::FILTERED_LIST_NO_KEYS);
	}
	if (analysis.found_semi_anti_join && analysis.found_aggregation) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::SEMI_ANTI_WITH_AGGREGATE);
	}
	if (analysis.found_semi_anti_join && !analysis.found_aggregation && !input.semi_anti_aux_candidate) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::SEMI_ANTI_MISSING_AUX);
	}
	if (!analysis.incremental_compatible && model.unsupported_reasons.empty()) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNSUPPORTED_OPERATOR);
	}
}

static void BuildModelFeatures(DeltaViewModel &model, const PlanAnalysis &analysis, const DeltaViewModelInput &input) {
	if (!model.unsupported_reasons.empty()) {
		AddUnique(model.features, DeltaModelFeature::FULL_ONLY);
	}
	if (analysis.found_projection || (!analysis.found_aggregation && !analysis.found_join && !analysis.found_window)) {
		AddUnique(model.features, DeltaModelFeature::LINEAR);
	}
	if (analysis.found_join && !analysis.found_left_join && !analysis.found_full_outer &&
	    !analysis.found_semi_anti_join) {
		AddUnique(model.features, DeltaModelFeature::BILINEAR);
	}
	if (analysis.found_left_join || analysis.found_full_outer) {
		AddUnique(model.features, DeltaModelFeature::OUTER_JOIN_STATEFUL);
	}
	if (analysis.found_aggregation) {
		if (analysis.found_minmax || analysis.found_count_distinct || analysis.found_list ||
		    analysis.found_filtered_list) {
			AddUnique(model.features, DeltaModelFeature::AGGREGATE_NON_LINEAR);
		} else {
			AddUnique(model.features, DeltaModelFeature::AGGREGATE_LINEAR);
		}
	}
	if (analysis.found_having) {
		AddUnique(model.features, DeltaModelFeature::AGGREGATE_HAVING);
	}
	if (analysis.found_count_distinct) {
		AddUnique(model.features, DeltaModelFeature::COUNT_DISTINCT_STATEFUL);
	}
	if (analysis.found_filtered_list) {
		AddUnique(model.features, DeltaModelFeature::FILTERED_LIST_STATEFUL);
	}
	if (analysis.found_grouping_sets) {
		AddUnique(model.features, DeltaModelFeature::GROUPING_SETS_STATEFUL);
	}
	if (analysis.found_window && !model.window_partition_columns.empty()) {
		AddUnique(model.features, DeltaModelFeature::WINDOW_AFFECTED_PARTITION);
	}
	if (!model.HasFeature(DeltaModelFeature::FULL_ONLY)) {
		if (input.distinct_aux_candidate) {
			AddUnique(model.features, DeltaModelFeature::DISTINCT_STATEFUL);
		}
		if (input.semi_anti_aux_candidate) {
			AddUnique(model.features, DeltaModelFeature::SEMI_ANTI_STATEFUL);
		}
	}
}

static bool IsAggregateRefreshType(RefreshType type) {
	return type == RefreshType::SIMPLE_AGGREGATE || type == RefreshType::AGGREGATE_GROUP ||
	       type == RefreshType::AGGREGATE_HAVING;
}

static void BuildUpdateSemantics(DeltaViewModel &model, const PlanAnalysis &analysis) {
	if (model.type != RefreshType::FULL_REFRESH) {
		AddUnique(model.update_semantics, DeltaUpdateSemantics::DELETE_SENSITIVE);
		AddUnique(model.update_semantics, DeltaUpdateSemantics::UPDATE_SENSITIVE);
	}
	if (model.type == RefreshType::SIMPLE_PROJECTION) {
		AddUnique(model.update_semantics, DeltaUpdateSemantics::APPEND_ONLY_SAFE);
		AddUnique(model.update_semantics, DeltaUpdateSemantics::PROJECTION_DELETE_SKIP_SAFE);
	}
	if (IsAggregateRefreshType(model.type) && !analysis.found_count_distinct && !analysis.found_list &&
	    !analysis.found_filtered_list) {
		AddUnique(model.update_semantics, DeltaUpdateSemantics::APPEND_ONLY_SAFE);
		AddUnique(model.update_semantics, DeltaUpdateSemantics::AGGREGATE_DELETE_SKIP_SAFE);
	}
	if (analysis.found_minmax && !analysis.found_count_distinct && !analysis.found_list) {
		AddUnique(model.update_semantics, DeltaUpdateSemantics::MINMAX_INSERT_ONLY_SAFE);
	}
}

static void AddAffectedDomain(DeltaViewModel &model, DeltaAffectedDomain domain) {
	for (auto &existing : model.affected_domains) {
		if (existing.kind != domain.kind) {
			continue;
		}
		bool same_keys = existing.key_columns.size() == domain.key_columns.size();
		for (idx_t i = 0; same_keys && i < existing.key_columns.size(); i++) {
			same_keys = StringUtil::CIEquals(existing.key_columns[i], domain.key_columns[i]);
		}
		if (same_keys) {
			return;
		}
	}
	model.affected_domains.push_back(std::move(domain));
}

static void BuildBaseAffectedDomains(DeltaViewModel &model, const CreateMVPlanFacts &facts) {
	if (!model.group_columns.empty()) {
		DeltaAffectedDomain domain;
		domain.kind = DeltaAffectedDomainKind::GROUP;
		domain.key_columns = model.group_columns;
		domain.source_tables = SourceTablesFromFacts(facts);
		domain.delta_local = !facts.analysis.found_join && facts.source_occurrences.size() <= 1;
		domain.needs_base_lookup = facts.analysis.found_join;
		AddAffectedDomain(model, std::move(domain));
	}
	if (model.HasSemiAntiAux()) {
		DeltaAffectedDomain domain;
		domain.kind = DeltaAffectedDomainKind::SEMI_ANTI_PREDICATE;
		domain.key_columns = model.semi_anti_aux.left_cols;
		AddStringCI(domain.source_tables, model.semi_anti_aux.left_table);
		AddStringCI(domain.source_tables, model.semi_anti_aux.right_table);
		domain.delta_local = false;
		domain.needs_base_lookup = true;
		AddAffectedDomain(model, std::move(domain));
	}
}

static bool ModelHasNodeKind(const DeltaViewModel &model, DeltaModelNodeKind kind) {
	for (auto &node : model.nodes) {
		if (node.kind == kind) {
			return true;
		}
	}
	return false;
}

static bool ModelHasUnsupportedReason(const DeltaViewModel &model, DeltaUnsupportedReason reason) {
	for (auto existing : model.unsupported_reasons) {
		if (existing == reason) {
			return true;
		}
	}
	return false;
}

static bool ModelHasNonStructuralUnsupportedReason(const DeltaViewModel &model) {
	for (auto reason : model.unsupported_reasons) {
		switch (reason) {
		case DeltaUnsupportedReason::UNSUPPORTED_FUNCTION:
		case DeltaUnsupportedReason::UNSUPPORTED_UNNEST:
		case DeltaUnsupportedReason::UNSUPPORTED_AGGREGATE:
		case DeltaUnsupportedReason::UNSUPPORTED_FILTERED_AGGREGATE:
		case DeltaUnsupportedReason::UNSUPPORTED_JOIN_TYPE:
		case DeltaUnsupportedReason::UNSUPPORTED_ORDER_BY:
		case DeltaUnsupportedReason::UNSUPPORTED_OPERATOR:
			return true;
		default:
			break;
		}
	}
	return false;
}

static bool ModelHasAuxState(const DeltaViewModel &model, DeltaAuxStateKind state) {
	for (auto &node : model.nodes) {
		for (auto existing : node.required_aux_states) {
			if (existing == state) {
				return true;
			}
		}
	}
	return false;
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
			AddUnique(model.strategy_reasons, DeltaStrategyReason::DELIM_AGGREGATE_GROUP_FALLBACK);
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
			AddUnique(model.strategy_reasons, DeltaStrategyReason::SCALAR_DELIM_PROJECTION_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Scalar DELIM/DEPENDENT projection: using %zu visible key columns "
			                    "for GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	if (analysis.found_nested_aggregate && model.group_columns.empty()) {
		auto before = model.group_columns.size();
		AddVisibleGroupNames(model.group_columns, DeriveAggregateGroupColumnNames(facts, output_names, false));
		if (model.group_columns.size() > before) {
			AddUnique(model.strategy_reasons, DeltaStrategyReason::NESTED_AGGREGATE_GROUP_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Nested aggregate: using %zu visible inner group columns for "
			                    "GROUP_RECOMPUTE\n",
			                    model.group_columns.size());
		}
	}

	if (analysis.found_aggregation && model.group_columns.empty()) {
		auto before = model.group_columns.size();
		AddVisibleGroupNames(model.group_columns, DeriveAggregateGroupColumnNames(facts, output_names, true));
		if (model.group_columns.size() > before) {
			AddUnique(model.strategy_reasons, DeltaStrategyReason::REPEATED_CTE_AGGREGATE_GROUP_FALLBACK);
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
			AddUnique(model.strategy_reasons, DeltaStrategyReason::JOIN_AGGREGATE_PROJECTION_FALLBACK);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Join-over-aggregate exposes %zu columns but only %zu are "
			                    "group/aggregate outputs -- using GROUP_RECOMPUTE\n",
			                    output_names.size(), expected_linear_outputs);
		}
	}

	if (has_union_over_agg) {
		AddUnique(model.strategy_reasons, DeltaStrategyReason::UNION_OVER_AGGREGATE);
	}
	if (analysis.found_nested_aggregate) {
		AddUnique(model.strategy_reasons, DeltaStrategyReason::NESTED_AGGREGATE_GROUP_FALLBACK);
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
	D_ASSERT(model.root_node == DConstants::INVALID_INDEX || model.root_node < model.nodes.size());
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
	case DeltaModelFeature::AGGREGATE_HAVING:
		return "AGGREGATE_HAVING";
	case DeltaModelFeature::COUNT_DISTINCT_STATEFUL:
		return "COUNT_DISTINCT_STATEFUL";
	case DeltaModelFeature::FILTERED_LIST_STATEFUL:
		return "FILTERED_LIST_STATEFUL";
	case DeltaModelFeature::GROUPING_SETS_STATEFUL:
		return "GROUPING_SETS_STATEFUL";
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

const char *DeltaModelNodeKindName(DeltaModelNodeKind kind) {
	switch (kind) {
	case DeltaModelNodeKind::SCAN:
		return "SCAN";
	case DeltaModelNodeKind::FILTER:
		return "FILTER";
	case DeltaModelNodeKind::PROJECT:
		return "PROJECT";
	case DeltaModelNodeKind::JOIN:
		return "JOIN";
	case DeltaModelNodeKind::AGGREGATE:
		return "AGGREGATE";
	case DeltaModelNodeKind::WINDOW:
		return "WINDOW";
	case DeltaModelNodeKind::DISTINCT:
		return "DISTINCT";
	case DeltaModelNodeKind::SEMI_ANTI:
		return "SEMI_ANTI";
	case DeltaModelNodeKind::UNION:
		return "UNION";
	case DeltaModelNodeKind::TOP_K:
		return "TOP_K";
	case DeltaModelNodeKind::UNNEST:
		return "UNNEST";
	case DeltaModelNodeKind::CTE:
		return "CTE";
	default:
		return "OTHER";
	}
}

const char *DeltaRuleKindName(DeltaRuleKind kind) {
	switch (kind) {
	case DeltaRuleKind::LINEAR:
		return "LINEAR";
	case DeltaRuleKind::PRODUCT:
		return "PRODUCT";
	case DeltaRuleKind::STATEFUL:
		return "STATEFUL";
	case DeltaRuleKind::NON_LINEAR:
		return "NON_LINEAR";
	case DeltaRuleKind::FULL_ONLY:
		return "FULL_ONLY";
	default:
		return "UNKNOWN";
	}
}

const char *DeltaUnsupportedReasonName(DeltaUnsupportedReason reason) {
	switch (reason) {
	case DeltaUnsupportedReason::UNSUPPORTED_SET_OPERATION:
		return "UNSUPPORTED_SET_OPERATION";
	case DeltaUnsupportedReason::UNSUPPORTED_PIVOT:
		return "UNSUPPORTED_PIVOT";
	case DeltaUnsupportedReason::UNSUPPORTED_FUNCTION:
		return "UNSUPPORTED_FUNCTION";
	case DeltaUnsupportedReason::UNSUPPORTED_UNNEST:
		return "UNSUPPORTED_UNNEST";
	case DeltaUnsupportedReason::UNSUPPORTED_AGGREGATE:
		return "UNSUPPORTED_AGGREGATE";
	case DeltaUnsupportedReason::UNSUPPORTED_FILTERED_AGGREGATE:
		return "UNSUPPORTED_FILTERED_AGGREGATE";
	case DeltaUnsupportedReason::UNSUPPORTED_JOIN_TYPE:
		return "UNSUPPORTED_JOIN_TYPE";
	case DeltaUnsupportedReason::UNSUPPORTED_ORDER_BY:
		return "UNSUPPORTED_ORDER_BY";
	case DeltaUnsupportedReason::UNSUPPORTED_OPERATOR:
		return "UNSUPPORTED_OPERATOR";
	case DeltaUnsupportedReason::GROUPING_SETS_NO_KEYS:
		return "GROUPING_SETS_NO_KEYS";
	case DeltaUnsupportedReason::FILTERED_LIST_NO_KEYS:
		return "FILTERED_LIST_NO_KEYS";
	case DeltaUnsupportedReason::SEMI_ANTI_WITH_AGGREGATE:
		return "SEMI_ANTI_WITH_AGGREGATE";
	case DeltaUnsupportedReason::SEMI_ANTI_MISSING_AUX:
		return "SEMI_ANTI_MISSING_AUX";
	case DeltaUnsupportedReason::UNRECOGNIZED_PATTERN:
		return "UNRECOGNIZED_PATTERN";
	default:
		return "UNKNOWN";
	}
}

const char *DeltaUpdateSemanticsName(DeltaUpdateSemantics semantics) {
	switch (semantics) {
	case DeltaUpdateSemantics::APPEND_ONLY_SAFE:
		return "APPEND_ONLY_SAFE";
	case DeltaUpdateSemantics::DELETE_SENSITIVE:
		return "DELETE_SENSITIVE";
	case DeltaUpdateSemantics::UPDATE_SENSITIVE:
		return "UPDATE_SENSITIVE";
	case DeltaUpdateSemantics::MINMAX_INSERT_ONLY_SAFE:
		return "MINMAX_INSERT_ONLY_SAFE";
	case DeltaUpdateSemantics::PROJECTION_DELETE_SKIP_SAFE:
		return "PROJECTION_DELETE_SKIP_SAFE";
	case DeltaUpdateSemantics::AGGREGATE_DELETE_SKIP_SAFE:
		return "AGGREGATE_DELETE_SKIP_SAFE";
	default:
		return "UNKNOWN";
	}
}

const char *DeltaAffectedDomainKindName(DeltaAffectedDomainKind kind) {
	switch (kind) {
	case DeltaAffectedDomainKind::GROUP:
		return "GROUP";
	case DeltaAffectedDomainKind::WINDOW_PARTITION:
		return "WINDOW_PARTITION";
	case DeltaAffectedDomainKind::PROJECTION_KEY:
		return "PROJECTION_KEY";
	case DeltaAffectedDomainKind::SEMI_ANTI_PREDICATE:
		return "SEMI_ANTI_PREDICATE";
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

idx_t DeltaViewModel::LineageEntryCount() const {
	idx_t count = 0;
	if (!window_lineage_ops.empty()) {
		count++;
	}
	if (has_projection_lineage) {
		count++;
	}
	return count;
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

DeltaPlanDecision BuildDeltaPlanDecision(const DeltaViewModel &model) {
	DeltaPlanDecision decision;
	auto finish = [&decision](RefreshType type, const char *reason) -> DeltaPlanDecision {
		decision.refresh_type = type;
		decision.reasons.push_back(reason);
		return decision;
	};
	if (model.nodes.empty()) {
		decision.exact_refresh_type = false;
		return finish(RefreshType::FULL_REFRESH, "operator_model_missing");
	}

	const bool has_window = ModelHasNodeKind(model, DeltaModelNodeKind::WINDOW);
	const bool has_grouping_sets = model.HasFeature(DeltaModelFeature::GROUPING_SETS_STATEFUL);
	const bool has_semi_anti = ModelHasNodeKind(model, DeltaModelNodeKind::SEMI_ANTI);
	const bool has_aggregate = ModelHasNodeKind(model, DeltaModelNodeKind::AGGREGATE);
	const bool has_projection = ModelHasNodeKind(model, DeltaModelNodeKind::PROJECT);
	const bool has_distinct = ModelHasNodeKind(model, DeltaModelNodeKind::DISTINCT);
	const bool has_groups = !model.group_columns.empty();

	if (ModelHasUnsupportedReason(model, DeltaUnsupportedReason::UNSUPPORTED_SET_OPERATION) ||
	    ModelHasUnsupportedReason(model, DeltaUnsupportedReason::UNSUPPORTED_PIVOT)) {
		return finish(RefreshType::FULL_REFRESH, "unsupported_create_shape");
	}
	if (has_window) {
		return finish(RefreshType::WINDOW_PARTITION, "window_partition");
	}
	if (has_grouping_sets) {
		return finish(has_groups ? RefreshType::GROUP_RECOMPUTE : RefreshType::FULL_REFRESH,
		              has_groups ? "grouping_sets_group_recompute" : "grouping_sets_full");
	}
	if (has_semi_anti && has_aggregate) {
		return finish(RefreshType::FULL_REFRESH, "semi_anti_aggregate_full");
	}
	if (has_semi_anti) {
		auto type = ModelHasAuxState(model, DeltaAuxStateKind::SEMI_ANTI_MATCH) ? RefreshType::SEMI_ANTI_RECOMPUTE
		                                                                        : RefreshType::FULL_REFRESH;
		return finish(type, type == RefreshType::SEMI_ANTI_RECOMPUTE ? "semi_anti_aux" : "semi_anti_missing_aux");
	}
	if (ModelHasNonStructuralUnsupportedReason(model)) {
		return finish(RefreshType::FULL_REFRESH, "unsupported_incremental_construct");
	}
	if (model.HasFeature(DeltaModelFeature::FILTERED_LIST_STATEFUL)) {
		return finish(has_groups ? RefreshType::GROUP_RECOMPUTE : RefreshType::FULL_REFRESH,
		              has_groups ? "filtered_list_group_recompute" : "filtered_list_full");
	}
	if (model.HasFeature(DeltaModelFeature::COUNT_DISTINCT_STATEFUL) && has_groups) {
		return finish(RefreshType::GROUP_RECOMPUTE, "count_distinct_group_recompute");
	}
	if (has_distinct && !model.distinct_at_top && has_aggregate) {
		auto type = ModelHasAuxState(model, DeltaAuxStateKind::DISTINCT_COUNT) ? RefreshType::DISTINCT_INCREMENTAL
		                                                                       : RefreshType::GROUP_RECOMPUTE;
		return finish(type, type == RefreshType::DISTINCT_INCREMENTAL ? "distinct_aux" : "distinct_group_recompute");
	}
	if (model.union_distinct_over_agg && has_groups) {
		return finish(RefreshType::GROUP_RECOMPUTE, "union_distinct_over_aggregate");
	}
	if (has_distinct && model.distinct_at_top && has_groups) {
		return finish(RefreshType::AGGREGATE_GROUP, "top_distinct_group");
	}
	if (model.HasFeature(DeltaModelFeature::AGGREGATE_HAVING) && has_aggregate && has_groups) {
		return finish(RefreshType::AGGREGATE_HAVING, "aggregate_having");
	}
	if (!model.strategy_reasons.empty() && has_groups) {
		return finish(RefreshType::GROUP_RECOMPUTE, "strategy_group_recompute");
	}
	if (has_aggregate && has_groups) {
		return finish(RefreshType::AGGREGATE_GROUP, "aggregate_group");
	}
	if (has_aggregate) {
		return finish(RefreshType::SIMPLE_AGGREGATE, "simple_aggregate");
	}
	if (has_projection) {
		return finish(RefreshType::SIMPLE_PROJECTION, "simple_projection");
	}
	return finish(RefreshType::FULL_REFRESH, "unrecognized_pattern");
}

bool DeltaPlanDecisionMatchesModel(const DeltaPlanDecision &decision, const DeltaViewModel &model) {
	return !decision.exact_refresh_type || decision.refresh_type == model.type;
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
		AddUnique(model.strategy_reasons, DeltaStrategyReason::OUTER_JOIN_AGGREGATE_RECOMPUTE);
		OPENIVM_DEBUG_PRINT("[CREATE MV] LEFT/OUTER JOIN aggregate with computed aggregate or projection wrapper -- "
		                    "using group-recompute metadata\n");
	}

	if (analysis.found_full_outer) {
		model.full_outer_join_cols = ExtractFullOuterJoinMetadata(facts);
	}

	BuildUnsupportedReasons(model, facts, input);
	BuildModelFeatures(model, analysis, input);
	SelectRefreshType(model, analysis, input.has_unsupported_incremental_construct);
	if (model.warn_unrecognized_pattern) {
		AddUnique(model.unsupported_reasons, DeltaUnsupportedReason::UNRECOGNIZED_PATTERN);
		AddUnique(model.features, DeltaModelFeature::FULL_ONLY);
	}
	AttachAuxRequirements(model, input);
	BuildUpdateSemantics(model, analysis);
	BuildBaseAffectedDomains(model, facts);
	if (input.build_operator_model) {
		BuildModelNodes(model, facts, output_names);
	}
	ValidateModelInvariants(model);
	return model;
}

void PopulateDeltaViewModelLineage(DeltaViewModel &model, const CreateMVPlanFacts &facts,
                                   const vector<string> &output_names) {
	model.window_lineage_ops.clear();
	model.has_projection_lineage = false;
	model.projection_lineage = RefreshMetadata::ProjectionKeyLineage();
	model.lineage_facts.clear();
	const auto &analysis = facts.analysis;
	if (model.type == RefreshType::WINDOW_PARTITION) {
		if (BuildWindowPartitionLineageOps(facts, model.window_partition_columns, model.window_lineage_ops)) {
			DeltaAffectedDomain domain;
			domain.kind = DeltaAffectedDomainKind::WINDOW_PARTITION;
			domain.key_columns = model.window_partition_columns;
			domain.delta_local = true;
			for (auto &op : model.window_lineage_ops) {
				AddStringCI(domain.source_tables, op.source);
				if (op.kind == "lookup") {
					domain.delta_local = false;
					domain.needs_base_lookup = true;
				}
				DeltaLineageFact fact;
				fact.kind = DeltaLineageKind::WINDOW_PARTITION;
				fact.source_table = op.source;
				fact.source_column = op.source_col;
				fact.output_column = op.output_col;
				fact.lookup_table = op.lookup;
				fact.lookup_column = op.lookup_col;
				fact.lookup_output_column = op.lookup_out;
				model.lineage_facts.push_back(std::move(fact));
			}
			AddAffectedDomain(model, std::move(domain));
		}
		return;
	}
	if (model.type == RefreshType::SIMPLE_PROJECTION && !analysis.found_left_join && !analysis.found_full_outer) {
		if (BuildProjectionKeyLineage(facts, output_names, model.projection_lineage)) {
			model.has_projection_lineage = true;
			DeltaAffectedDomain domain;
			domain.kind = DeltaAffectedDomainKind::PROJECTION_KEY;
			domain.key_columns.push_back(model.projection_lineage.output_col);
			domain.delta_local = true;
			for (auto &arm : model.projection_lineage.arms) {
				AddStringCI(domain.source_tables, arm.source);
				if (!arm.steps.empty()) {
					domain.delta_local = false;
					domain.needs_base_lookup = true;
				}
				DeltaLineageFact fact;
				fact.kind = DeltaLineageKind::PROJECTION_KEY;
				fact.source_table = arm.source;
				fact.source_occurrence = arm.occurrence;
				fact.source_column = arm.source_col;
				fact.output_column = model.projection_lineage.output_col;
				if (!arm.steps.empty()) {
					fact.lookup_table = arm.steps.back().table;
					fact.lookup_column = arm.steps.back().lookup_col;
					fact.lookup_output_column = arm.steps.back().lookup_out;
				}
				model.lineage_facts.push_back(std::move(fact));
			}
			AddAffectedDomain(model, std::move(domain));
		}
	}
	if (model.HasSemiAntiAux()) {
		for (auto &col : model.semi_anti_aux.left_cols) {
			DeltaLineageFact fact;
			fact.kind = DeltaLineageKind::SEMI_ANTI_PREDICATE;
			fact.source_table = model.semi_anti_aux.left_table;
			fact.source_column = col;
			fact.output_column = col;
			model.lineage_facts.push_back(std::move(fact));
		}
	}
}

string BuildDeltaViewModelLineageJson(const DeltaViewModel &model) {
	vector<string> entries;
	if (!model.window_lineage_ops.empty()) {
		entries.push_back(RefreshMetadata::WindowPartitionLineageToJson(model.window_lineage_ops));
	}
	if (model.has_projection_lineage) {
		entries.push_back(RefreshMetadata::ProjectionKeyLineageToJson(model.projection_lineage));
	}
	return BuildRefreshLineageJson(entries);
}

} // namespace duckdb
