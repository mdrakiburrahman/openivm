#ifndef IVM_SCAN_RULE_HPP
#define IVM_SCAN_RULE_HPP

#include "rules/rule.hpp"

namespace duckdb {

class IncrementalScanRule : public IncrementalRule {
public:
	ModifiedPlan Rewrite(PlanWrapper pw) override;
	Linearity GetLinearity() const override {
		return Linearity::LINEAR;
	}
};

} // namespace duckdb

#endif
