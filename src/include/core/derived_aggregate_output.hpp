#ifndef DERIVED_AGGREGATE_OUTPUT_HPP
#define DERIVED_AGGREGATE_OUTPUT_HPP

#include "duckdb.hpp"

namespace duckdb {

struct DerivedAggregateOutput {
	string output_column;
	string expression_sql;
};

struct AggregateInput {
	string output_column;
	string aggregate_type;
};

struct DerivedAggregateOutputInfo {
	bool complete = false;
	vector<DerivedAggregateOutput> outputs;
	vector<AggregateInput> inputs;
};

} // namespace duckdb

#endif // DERIVED_AGGREGATE_OUTPUT_HPP
