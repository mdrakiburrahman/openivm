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

	static void IncrementalRewriteRuleFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

	// Point con at the view's metadata (snapshotting an explicit transaction's metadata) and parse its stored
	// definition, with time-travel pins peeled off. A missing definition throws a CatalogException with missing_error.
	static unique_ptr<SQLStatement> LoadStoredViewStatement(ClientContext &context, Connection &con,
	                                                        const string &view_catalog, const string &view,
	                                                        const string &missing_error);
};

} // namespace duckdb

#endif // INCREMENTAL_REWRITE_RULE_HPP
