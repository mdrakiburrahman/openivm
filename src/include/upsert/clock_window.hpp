#ifndef OPENIVM_CLOCK_WINDOW_HPP
#define OPENIVM_CLOCK_WINDOW_HPP

#pragma once

#include "compile_facts.hpp"
#include "duckdb.hpp"

namespace duckdb {
namespace openivm {

enum class ClockMode : uint8_t {
	// The clock fact does not apply to this view (it is not FULL_REFRESH).
	NOT_APPLICABLE,
	// The default FULL_REFRESH program is emitted, with no as-of baked in.
	FULL_DEFAULT,
	// A full recompute with the as-of baked in.
	FULL_AT_AS_OF,
	// Group recompute over the source-delta keys plus the groups whose clock predicates flipped.
	INCREMENTAL
};

const char *ClockModeName(ClockMode mode);

struct ClockPlan {
	ClockMode mode = ClockMode::NOT_APPLICABLE;
	string reason;
	// The view query with every clock reference replaced by the as-of literal, set for FULL_AT_AS_OF and
	// INCREMENTAL. `query` is the text the group recompute statements embed; `full_query` is the text DuckDB binds
	// before rendering a full recompute for the target dialect. They differ only for Spark timestamps.
	string query;
	string full_query;
	// Positional (group column order) key query of the groups whose clock predicates differ between
	// `previous_as_of` and `as_of`, set for INCREMENTAL.
	string crossing_groups;
};

// Decides how a FULL_REFRESH view of the shape
//   SELECT <group columns>, <aggregate over CASE WHEN col <cmp> current_timestamp|current_date ...>
//   FROM <one table> GROUP BY <group columns>
// is refreshed at `clock.as_of`. `view_query_sql` is the qualified stored query. Every result other than
// INCREMENTAL is a full program; only INCREMENTAL scopes the refresh to the affected groups.
ClockPlan PlanClockWindow(Connection &con, const ClockFacts &clock, const string &view_query_sql,
                          const vector<string> &group_columns, bool has_delta_sources, SqlDialect dialect);

} // namespace openivm
} // namespace duckdb

#endif
