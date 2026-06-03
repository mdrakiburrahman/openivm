#include "delta/delta_operator.hpp"

#include "core/openivm_debug.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"

namespace duckdb {

DeltaPlanFragment CompileConstantZeroDelta(DeltaOperatorInput input) {
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::CONSTANT_ZERO_DELTA);
	vector<LogicalType> output_types = input.plan->types;
	output_types.push_back(input.mul_type);

	vector<ColumnBinding> bindings;
	auto table_index = input.context.input.optimizer.binder.GenerateTableIndex();
	for (idx_t i = 0; i < output_types.size(); i++) {
		bindings.emplace_back(table_index, i);
	}
	auto mul_binding = bindings.back();
	auto empty = make_uniq<LogicalEmptyResult>(output_types, std::move(bindings));
	empty->ResolveOperatorTypes();

	OPENIVM_DEBUG_PRINT("[Delta Operator] %s constant leaf -- returning empty delta\n",
	                    LogicalOperatorToString(input.plan->type).c_str());
	return {std::move(empty), mul_binding};
}

DeltaPlanFragment CompileStaticConstantLeaf(DeltaOperatorInput input) {
	LogDeltaOperatorStrategy(input, DeltaOperatorStrategy::CONSTANT_STATIC);
	auto bindings = input.plan->GetColumnBindings();
	if (bindings.empty()) {
		throw NotImplementedException("%s has no column bindings", LogicalOperatorToString(input.plan->type));
	}
	OPENIVM_DEBUG_PRINT("[Delta Operator] %s constant leaf -- returning unchanged for copied subtree\n",
	                    LogicalOperatorToString(input.plan->type).c_str());
	return {std::move(input.plan), bindings[0]};
}

} // namespace duckdb
