#include "delta/delta_operator.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "delta/operators/join.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>

namespace duckdb {

DeltaPlanFragment CompileUnnestDelta(DeltaOperatorInput input) {
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::UNNEST_LINEAR);
	if (input.plan->children.size() != 1) {
		throw InternalException("DeltaUnnest: expected exactly one child");
	}

	auto original_bindings = input.plan->GetColumnBindings();
	auto *original_unnest = input.plan.get();
	auto child_mul = input.CompileChild(input.plan->children[0], input.root);
	input.plan->children[0] = std::move(child_mul.op);
	input.plan->ResolveOperatorTypes();
	AppendBindingsToJoinProjectionMaps(*input.plan, {child_mul.mul_binding});

	auto output_bindings = input.plan->GetColumnBindings();
	auto output_types = input.plan->types;
	vector<unique_ptr<Expression>> projection_exprs;
	auto projection_index = input.context.input.optimizer.binder.GenerateTableIndex();
	ColumnBindingReplacer replacer;
	idx_t visible_output_idx = 0;
	for (idx_t i = 0; i < output_bindings.size(); i++) {
		if (output_bindings[i] == child_mul.mul_binding) {
			continue;
		}
		// Equality is already out of bounds for the original_bindings access below.
		if (visible_output_idx >= original_bindings.size()) { // mull-ignore: cxx_ge_to_gt
			throw InternalException("DeltaUnnest: rewritten plan exposed more than %llu visible output bindings",
			                        (idx_t)original_bindings.size());
		}
		projection_exprs.push_back(make_uniq<BoundColumnRefExpression>(output_types[i], output_bindings[i]));
		replacer.replacement_bindings.emplace_back(
		    original_bindings[visible_output_idx],
		    ColumnBinding(TableIndex(projection_index), ProjectionIndex(visible_output_idx)));
		visible_output_idx++;
	}
	if (visible_output_idx != original_bindings.size()) {
		throw InternalException("DeltaUnnest: expected %llu visible output bindings, found %llu",
		                        (idx_t)original_bindings.size(), visible_output_idx);
	}
	projection_exprs.push_back(
	    make_uniq<BoundColumnRefExpression>(openivm::MULTIPLICITY_COL, input.mul_type, child_mul.mul_binding));

	replacer.stop_operator = original_unnest;
	replacer.VisitOperator(*input.root);

	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(projection_exprs));
	projection->children.push_back(std::move(input.plan));
	projection->ResolveOperatorTypes();

	auto mul_binding = projection->GetColumnBindings().back();
	OPENIVM_DEBUG_PRINT("[DeltaUnnest] Done, mul_binding: table=%lu col=%lu\n",
	                    (unsigned long)mul_binding.table_index.index,
	                    (unsigned long)mul_binding.column_index.GetIndex());
	return {std::move(projection), mul_binding};
}

} // namespace duckdb
