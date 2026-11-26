#include "delta/delta_operator.hpp"
#include "core/openivm_debug.hpp"
#include "core/parser_plan_helpers.hpp"
#include "delta/delta_helpers.hpp"
#include "delta/operators/join.hpp"
#include "upsert/refresh_index_regen.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"

namespace duckdb {

namespace {

// A selection whose output depends on which rows rank first: TOP_N, or LIMIT (normally over ORDER BY).
static bool IsTopKSelection(const LogicalOperator &op) {
	return op.type == LogicalOperatorType::LOGICAL_TOP_N || op.type == LogicalOperatorType::LOGICAL_LIMIT;
}

// The pre-delta bag of a catalog scan: current rows minus the scan's delta, consolidated per tuple (by value
// identity) and expanded back to rows. It exposes the scan's columns in the scan's order.
static unique_ptr<LogicalOperator> BuildOldScanBag(const DeltaOperatorInput &input, ClientContext &context,
                                                   Binder &binder, unique_ptr<LogicalOperator> scan,
                                                   DeltaSourceCache &source_cache) {
	auto delta_source = renumber_and_rebind_subtree(scan->Copy(context), binder).op;
	LogicalOperator *delta_root = delta_source.get();
	auto delta = CompileRegularLeafDelta(input, context, binder, delta_source, delta_root, source_cache);
	auto old_state = CreateRegularOldNode(binder, std::move(scan), std::move(delta), input.mul_type);
	auto compact =
	    CompactDeltaNode(context, binder, std::move(old_state.op), binder.GenerateTableIndex(), old_state.mul_binding);

	// Repeat each tuple by its old weight. A negative weight (impossible for a consistent delta) yields no rows.
	auto compact_bindings = compact.op->GetColumnBindings();
	auto compact_types = compact.op->types;
	Optimizer optimizer(binder, context);
	auto weight = BoundCastExpression::AddCastToType(
	    context, make_uniq<BoundColumnRefExpression>(input.mul_type, compact.mul_binding), LogicalType::BIGINT);
	auto unnest_expression = make_uniq<BoundUnnestExpression>(LogicalType::BIGINT);
	unnest_expression->ChildMutable() = optimizer.BindScalarFunction(Identifier("range"), std::move(weight));
	auto unnest = make_uniq<LogicalUnnest>(binder.GenerateTableIndex());
	unnest->expressions.push_back(std::move(unnest_expression));
	unnest->children.push_back(std::move(compact.op));
	unnest->ResolveOperatorTypes();

	vector<unique_ptr<Expression>> columns;
	for (idx_t i = 0; i + 1 < compact_bindings.size(); i++) {
		columns.push_back(make_uniq<BoundColumnRefExpression>(compact_types[i], compact_bindings[i]));
	}
	auto bag = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(columns));
	bag->children.push_back(std::move(unnest));
	bag->ResolveOperatorTypes();
	return std::move(bag);
}

// Replace every catalog scan below `node` with its pre-delta bag, so the subtree (aggregates, joins, nested top-k,
// ...) evaluates over the old state. References above a scan are rebound up to `root`. Table functions and
// constant relations do not change between refreshes and stay as they are.
static void ReplaceScansWithOldState(const DeltaOperatorInput &input, ClientContext &context, Binder &binder,
                                     unique_ptr<LogicalOperator> &node, LogicalOperator &root,
                                     DeltaSourceCache &source_cache, unordered_set<idx_t> &ctes_in_subtree) {
	if (node->type == LogicalOperatorType::LOGICAL_GET && node->Cast<LogicalGet>().GetTable().get()) {
		node->ResolveOperatorTypes();
		const auto scan_bindings = node->GetColumnBindings();
		node = BuildOldScanBag(input, context, binder, std::move(node), source_cache);
		const auto bag_bindings = node->GetColumnBindings();
		if (scan_bindings.size() != bag_bindings.size()) {
			throw InternalException("DeltaTopK: pre-delta scan has %llu columns, expected %llu", bag_bindings.size(),
			                        scan_bindings.size());
		}
		// Positions are preserved, so positional join projection maps above the scan stay valid.
		ColumnBindingReplacer replacer;
		for (idx_t i = 0; i < scan_bindings.size(); i++) {
			replacer.replacement_bindings.emplace_back(scan_bindings[i], bag_bindings[i]);
		}
		replacer.stop_operator = node.get();
		replacer.VisitOperator(root);
		return;
	}
	if (node->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		// Its definition is rewritten below too, so references to it read the old state.
		ctes_in_subtree.insert(node->Cast<LogicalCTE>().table_index.index);
	} else if (node->type == LogicalOperatorType::LOGICAL_CTE_REF &&
	           !ctes_in_subtree.count(node->Cast<LogicalCTERef>().cte_index.index)) {
		// A CTE defined above the top-k would be read in its current state; never pass that off as the old one.
		throw NotImplementedException("Incremental maintenance of a top-k over a CTE defined outside of it");
	}
	for (auto &child : node->children) {
		ReplaceScansWithOldState(input, context, binder, child, root, source_cache, ctes_in_subtree);
	}
}

// The selection's output with a constant multiplicity appended.
static unique_ptr<LogicalOperator> WeightSelection(Binder &binder, unique_ptr<LogicalOperator> selection,
                                                   int32_t weight) {
	selection->ResolveOperatorTypes();
	auto bindings = selection->GetColumnBindings();
	vector<unique_ptr<Expression>> columns;
	for (idx_t i = 0; i < bindings.size(); i++) {
		columns.push_back(make_uniq<BoundColumnRefExpression>(selection->types[i], bindings[i]));
	}
	columns.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(weight)));
	auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(columns));
	projection->children.push_back(std::move(selection));
	projection->ResolveOperatorTypes();
	return std::move(projection);
}

// ΔTopK(R) = TopK(R_new) - TopK(R_old) as a signed relation. R_new reads the current base tables. R_old evaluates
// the same subtree over each base table's pre-delta state, so it is exact whatever the input computes (an
// aggregate's delta, for one, is not a row delta of its output). CREATE appends every input column to the ORDER BY
// as a tie-breaker, so both selections pick the same tuples among ties.
static DeltaPlanFragment CompileTopKDiff(DeltaOperatorInput input) {
	ClientContext &context = input.context.input.context;
	Binder &binder = input.context.input.optimizer.binder;
	OPENIVM_DEBUG_PRINT("[DeltaTopK] %s: diffing the selection over current and pre-delta base tables\n",
	                    LogicalOperatorToString(input.plan->type).c_str());
	// Views created before CREATE totalized top-k orders still need both selections to agree on ties.
	TotalizeTopKOrder(*input.plan);
	input.plan->ResolveOperatorTypes();
	const auto original_bindings = input.plan->GetColumnBindings();
	auto types = input.plan->types;

	auto old_selection = renumber_and_rebind_subtree(input.plan->Copy(context), binder).op;
	DeltaSourceCache source_cache;
	unordered_set<idx_t> ctes_in_subtree;
	ReplaceScansWithOldState(input, context, binder, old_selection->children[0], *old_selection, source_cache,
	                         ctes_in_subtree);

	vector<unique_ptr<LogicalOperator>> terms;
	terms.push_back(WeightSelection(binder, renumber_and_rebind_subtree(input.plan->Copy(context), binder).op, 1));
	terms.push_back(WeightSelection(binder, std::move(old_selection), -1));
	types.push_back(input.mul_type);
	auto result = AssembleJoinUnionAll(terms, types, binder);

	auto mul_binding =
	    ReplaceJoinOutputBindings(*input.root, *input.plan, original_bindings, result->GetColumnBindings());
	OPENIVM_DEBUG_PRINT("[DeltaTopK] Done, mul_binding: table=%lu col=%lu\n",
	                    (unsigned long)mul_binding.table_index.index,
	                    (unsigned long)mul_binding.column_index.GetIndex());
	return {std::move(result), mul_binding};
}

} // namespace

DeltaPlanFragment CompileTopKDelta(DeltaOperatorInput input) {
	// A top-k is not linear: ΔTopK(R) != TopK(ΔR). CREATE moves the view's outermost ORDER BY/LIMIT to the
	// user-facing view, so any TOP_N/LIMIT still in the stored query selects rows for an enclosing operator (or for
	// a nested top-k), and its selection must be maintained exactly.
	if (IsTopKSelection(*input.plan)) {
		LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::TOPK_DIFF);
		return CompileTopKDiff(std::move(input));
	}
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::TOPK_STRIP);
	// A standalone ORDER BY is stripped: a materialized view is a table, and tables have no inherent order. We
	// recurse into the only child, take its rewritten plan + multiplicity binding, and return them directly.
	OPENIVM_DEBUG_PRINT("[DeltaTopK] Stripping %s -- order is not maintained state\n",
	                    LogicalOperatorToString(input.plan->type).c_str());
	auto child_mul = input.CompileChild(input.plan->children[0], input.root);
	return {std::move(child_mul.op), child_mul.mul_binding};
}

} // namespace duckdb
