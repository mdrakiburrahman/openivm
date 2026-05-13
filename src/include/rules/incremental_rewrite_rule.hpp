#ifndef INCREMENTAL_REWRITE_RULE_HPP
#define INCREMENTAL_REWRITE_RULE_HPP

#include "rules/rule.hpp"

namespace duckdb {

class IncrementalRewriteRule : public OptimizerExtension {
public:
	IncrementalRewriteRule() {
		optimize_function = IncrementalRewriteRuleFunction;
	}

	static void AddInsertNode(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan,
	                          string &view_name, string &view_catalog_name, string &view_schema_name);

	/// Orchestrator: dispatches to the correct IncrementalRule based on operator type.
	static ModifiedPlan RewritePlan(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan, string &view,
	                                LogicalOperator *&root);

	static void IncrementalRewriteRuleFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb

#endif // INCREMENTAL_REWRITE_RULE_HPP
