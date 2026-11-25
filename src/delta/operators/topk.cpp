#include "delta/delta_operator.hpp"
#include "core/openivm_debug.hpp"
#include "core/parser_plan_helpers.hpp"
#include "delta/operators/join.hpp"
#include "upsert/refresh_index_regen.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"

namespace duckdb {

namespace {

// A selection whose output depends on which rows rank first: TOP_N, or LIMIT (normally over ORDER BY).
static bool IsTopKSelection(const LogicalOperator &op) {
	return op.type == LogicalOperatorType::LOGICAL_TOP_N || op.type == LogicalOperatorType::LOGICAL_LIMIT;
}

// Whether `target` sits on the view's output path, below only projections and ORDER BY/LIMIT/TOP_N. Such a
// top-k is published by the user-facing view, so the maintained state stays unbounded.
static bool OnOutputPath(LogicalOperator &node, const LogicalOperator &target) {
	if (&node == &target) {
		return true;
	}
	if (node.type != LogicalOperatorType::LOGICAL_PROJECTION && node.type != LogicalOperatorType::LOGICAL_ORDER_BY &&
	    !IsTopKSelection(node)) {
		return false;
	}
	for (auto &child : node.children) {
		if (OnOutputPath(*child, target)) {
			return true;
		}
	}
	return false;
}

// The input of a top-k selection: below TOP_N, or below the ORDER BY that a LIMIT reads (through projections).
static unique_ptr<LogicalOperator> &TopKInput(unique_ptr<LogicalOperator> &selection) {
	auto *slot = &selection->children[0];
	if (selection->type == LogicalOperatorType::LOGICAL_LIMIT) {
		while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*slot)->children.size() == 1) {
			slot = &(*slot)->children[0];
		}
		if ((*slot)->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
			return (*slot)->children[0];
		}
		return selection->children[0];
	}
	return *slot;
}

// A renumbered copy under an identity projection with a fresh table index, so its outputs are positional.
static unique_ptr<LogicalOperator> PositionalCopy(ClientContext &context, Binder &binder,
                                                  const LogicalOperator &subtree) {
	auto copy = renumber_and_rebind_subtree(subtree.Copy(context), binder).op;
	copy->ResolveOperatorTypes();
	auto projection = LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(copy));
	projection->ResolveOperatorTypes();
	return std::move(projection);
}

// The input's pre-delta bag: current rows minus the input's delta, consolidated per tuple and expanded back to rows.
static unique_ptr<LogicalOperator> BuildOldInputBag(const DeltaOperatorInput &input, ClientContext &context,
                                                    Binder &binder, const LogicalOperator &topk_input) {
	auto current = PositionalCopy(context, binder, topk_input);
	auto delta_source = PositionalCopy(context, binder, topk_input);
	LogicalOperator *delta_root = delta_source.get();
	auto delta = input.CompileCopiedSubtree(delta_source, delta_root);
	auto old_state = CreateRegularOldNode(binder, std::move(current), std::move(delta), input.mul_type);
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

// ΔTopK(R) = TopK(R_new) - TopK(R_old) as a signed relation. Base scans read R_new; R_old is R_new minus ΔR. CREATE
// appends every input column to the ORDER BY as a tie-breaker, so both selections pick the same tuples among ties.
static DeltaPlanFragment CompileNestedTopKDelta(DeltaOperatorInput input) {
	ClientContext &context = input.context.input.context;
	Binder &binder = input.context.input.optimizer.binder;
	OPENIVM_DEBUG_PRINT("[DeltaTopK] Nested %s: diffing the selection over current and pre-delta input\n",
	                    LogicalOperatorToString(input.plan->type).c_str());
	// Views created before CREATE totalized nested top-k orders still need both selections to agree on ties.
	TotalizeTopKOrder(*input.plan);
	input.plan->ResolveOperatorTypes();
	const auto original_bindings = input.plan->GetColumnBindings();
	auto types = input.plan->types;

	auto old_bag = BuildOldInputBag(input, context, binder, *TopKInput(input.plan));
	auto old_selection = renumber_and_rebind_subtree(input.plan->Copy(context), binder).op;
	auto &old_input = TopKInput(old_selection);
	const auto replaced_bindings = old_input->GetColumnBindings();
	const auto bag_bindings = old_bag->GetColumnBindings();
	if (replaced_bindings.size() != bag_bindings.size()) {
		throw InternalException("DeltaTopK: pre-delta input has %llu columns, expected %llu", bag_bindings.size(),
		                        replaced_bindings.size());
	}
	old_input = std::move(old_bag);
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < replaced_bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(replaced_bindings[i], bag_bindings[i]);
	}
	replacer.stop_operator = old_input.get();
	replacer.VisitOperator(*old_selection);

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
	// Below another operator (aggregate, filter, join, ...) a top-k is not linear: ΔTopK(R) != TopK(ΔR). A copied
	// subtree is always a join input, hence nested.
	if (IsTopKSelection(*input.plan) && (input.copied_subtree || !OnOutputPath(*input.root, *input.plan))) {
		LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::TOPK_DIFF);
		return CompileNestedTopKDelta(std::move(input));
	}
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::TOPK_STRIP);
	OPENIVM_DEBUG_PRINT("[DeltaTopK] Stripping %s -- top-k is non-linear; delta is computed unbounded\n",
	                    LogicalOperatorToString(input.plan->type).c_str());

	// On the output path, LIMIT, TOP_N, and standalone ORDER BY are stripped from the delta plan:
	//   - LIMIT/TOP_N: ΔLIMIT(R) != LIMIT(ΔR). The maintained state stays
	//     unbounded, and the user-facing view applies ORDER BY/LIMIT at read time.
	//   - ORDER BY: a materialized view is a table; tables have no inherent order.
	//     The runtime ordering is meaningless for IVM purposes.
	// We recurse into the only child, take its rewritten plan + multiplicity binding,
	// and return them directly — the LIMIT/TOP_N/ORDER_BY node is discarded.
	auto child_mul = input.CompileChild(input.plan->children[0], input.root);
	return {std::move(child_mul.op), child_mul.mul_binding};
}

} // namespace duckdb
