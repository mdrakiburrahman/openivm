#ifndef REFRESH_INSERT_RULE_HPP
#define REFRESH_INSERT_RULE_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

class RefreshInsertRule : public OptimizerExtension {
public:
	RefreshInsertRule();

	struct RefreshInsertOptimizerInfo : OptimizerExtensionInfo {
		explicit RefreshInsertOptimizerInfo() {
		}
	};

	static void RefreshInsertRuleFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb

#endif // REFRESH_INSERT_RULE_HPP
