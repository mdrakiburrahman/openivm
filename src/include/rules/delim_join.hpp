#ifndef OPENIVM_DELIM_JOIN_RULE_HPP
#define OPENIVM_DELIM_JOIN_RULE_HPP

#include "rules/rule.hpp"

namespace duckdb {

class ClientContext;
class LogicalOperator;

bool RewriteSafeSemiAntiDelimGets(ClientContext &context, unique_ptr<LogicalOperator> &plan);

class IncrementalDelimJoinRule : public IncrementalRule {
public:
	ModifiedPlan Rewrite(PlanWrapper pw) override;

	Linearity GetLinearity() const override {
		return Linearity::BILINEAR;
	}
};

} // namespace duckdb

#endif
