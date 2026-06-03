#include "delta/delta_operator.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

DeltaPlanFragment CompileUnnestDelta(DeltaOperatorInput input) {
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::UNNEST_LINEAR);
	if (input.plan->children.size() != 1) {
		throw InternalException("DeltaUnnest: expected exactly one child");
	}

	auto child_mul = input.CompileChild(input.plan->children[0], input.root);
	input.plan->children[0] = std::move(child_mul.op);
	input.plan->ResolveOperatorTypes();

	auto output_bindings = input.plan->GetColumnBindings();
	auto output_types = input.plan->types;
	vector<unique_ptr<Expression>> projection_exprs;
	for (idx_t i = 0; i < output_bindings.size(); i++) {
		if (output_bindings[i] == child_mul.mul_binding) {
			continue;
		}
		projection_exprs.push_back(make_uniq<BoundColumnRefExpression>(output_types[i], output_bindings[i]));
	}
	projection_exprs.push_back(
	    make_uniq<BoundColumnRefExpression>(openivm::MULTIPLICITY_COL, input.mul_type, child_mul.mul_binding));

	auto projection = make_uniq<LogicalProjection>(input.context.input.optimizer.binder.GenerateTableIndex(),
	                                               std::move(projection_exprs));
	projection->children.push_back(std::move(input.plan));
	projection->ResolveOperatorTypes();

	auto projection_bindings = projection->GetColumnBindings();
	auto mul_binding = projection_bindings.back();
	OPENIVM_DEBUG_PRINT("[DeltaUnnest] Done, mul_binding: table=%lu col=%lu\n", (unsigned long)mul_binding.table_index,
	                    (unsigned long)mul_binding.column_index);
	return {std::move(projection), mul_binding};
}

} // namespace duckdb
