#pragma once

namespace duckdb {

class BoundFunctionExpression;
class ExtensionLoader;

bool IsSparkExpressionMarker(const BoundFunctionExpression &expr);
void RegisterSparkExpressionMarker(ExtensionLoader &loader);

} // namespace duckdb
