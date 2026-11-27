#include "core/spark_expression_marker.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

static void SparkExpressionMarker(DataChunk &, ExpressionState &, Vector &) {
	throw InvalidInputException("Spark expression marker cannot be evaluated by OpenIVM");
}

static unique_ptr<FunctionData> BindSparkExpressionMarker(BindScalarFunctionInput &input) {
	Value enabled;
	if (!input.GetClientContext().TryGetCurrentSetting("openivm_spark_expression_markers", enabled) ||
	    enabled.IsNull() || !enabled.GetValue<bool>()) {
		throw BinderException("Spark expression markers are restricted to compiler-only use");
	}
	input.GetBoundFunction().SetReturnType(input.GetArguments()[1]->GetReturnType());
	return nullptr;
}

bool IsSparkExpressionMarker(const BoundFunctionExpression &expr) {
	using ScalarCallback = void (*)(DataChunk &, ExpressionState &, Vector &);
	const auto callback_fn = expr.Function().GetFunctionCallback();
	const auto callback = callback_fn.target<ScalarCallback>();
	return callback && *callback == SparkExpressionMarker;
}

void RegisterSparkExpressionMarker(ExtensionLoader &loader) {
	auto &db_config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	db_config.AddExtensionOption("openivm_spark_expression_markers",
	                             "enable compiler-only opaque Spark expression markers", LogicalType::BOOLEAN,
	                             Value::BOOLEAN(false));
	ScalarFunction marker("__openivm_spark_expression", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY,
	                      SparkExpressionMarker, BindSparkExpressionMarker, nullptr, nullptr, LogicalType::ANY);
	// DuckDB rejects volatile window keys; a fallible marker cannot be folded into a value.
	marker.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	marker.SetFallible();
	marker.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(marker);
}

} // namespace duckdb
