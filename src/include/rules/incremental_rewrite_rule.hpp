#ifndef INCREMENTAL_REWRITE_RULE_HPP
#define INCREMENTAL_REWRITE_RULE_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

class IncrementalRewriteRule : public OptimizerExtension {
public:
	IncrementalRewriteRule() {
		optimize_function = IncrementalRewriteRuleFunction;
	}

	static void AddInsertNode(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan,
	                          const string &view_name, const string &view_catalog_name, const string &view_schema_name);

	static void IncrementalRewriteRuleFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

	// Point con at the view's metadata (snapshotting an explicit transaction's metadata) and parse its stored
	// definition, with time-travel pins peeled off.
	static unique_ptr<SQLStatement> LoadStoredViewStatement(ClientContext &context, Connection &con,
	                                                        const string &view_catalog, const string &view);
};

} // namespace duckdb

#endif // INCREMENTAL_REWRITE_RULE_HPP
