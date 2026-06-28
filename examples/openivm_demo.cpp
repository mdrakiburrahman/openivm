//
// OpenIVM end-to-end demo — a debuggable tour of the incremental-view-maintenance
// pipeline. Each scenario is self-contained so you can set a breakpoint, press F5,
// and step from a single SQL statement all the way into OpenIVM's C++ internals.
//
// The interesting code paths to step into:
//   openivm_compile_with_facts(view, facts)  (Scenario 1 — the openivm-spark
//                                "compile-only" path: emit refresh SQL, run nothing)
//                             -> src/compile_facts.cpp        (OpenIvmCompileWithFactsBind)
//                                src/upsert/refresh_sql.cpp   (GenerateRefreshSQL)
//   CREATE MATERIALIZED VIEW  -> src/core/parser.cpp          (parser extension)
//                                src/core/incremental_checker.cpp (RefreshType)
//   INSERT / DELETE / UPDATE  -> src/rules/refresh_insert_rule.cpp (delta capture)
//   PRAGMA refresh('...')     -> src/upsert/refresh.cpp       (refresh handler)
//                                src/rules/incremental_rewrite_rule.cpp (dispatch)
//                                src/rules/join.cpp            (inclusion-exclusion)
//                                src/upsert/refresh_compiler.cpp (upsert SQL)
//
// See examples/README.md for the recommended breakpoints per scenario.
//

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/connection.hpp"
#include "core/openivm_extension.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using duckdb::Connection;
using duckdb::idx_t;
using duckdb::MaterializedQueryResult;

namespace {

void Banner(const std::string &title) {
	std::cout << "\n=====================================================================\n"
	          << "  " << title << "\n"
	          << "=====================================================================\n";
}

// Auto-incrementing id stamped on every Run/Show so each query's debug trace is
// easy to bracket and study.
int g_run_seq = 0;

constexpr int BANNER_WIDTH = 70;

std::string PlainRule() {
	return std::string(BANNER_WIDTH, '-');
}

std::string LabeledRule(const std::string &label) {
	const std::string mid = " " + label + " ";
	const int dashes = std::max(0, BANNER_WIDTH - static_cast<int>(mid.size()));
	const int left = dashes / 2;
	return std::string(left, '-') + mid + std::string(dashes - left, '-');
}

std::string Centered(const std::string &text) {
	const int pad = std::max(0, (BANNER_WIDTH - static_cast<int>(text.size())) / 2);
	return std::string(pad, ' ') + text;
}

std::unique_ptr<MaterializedQueryResult> RunInternal(Connection &con, const std::string &sql, bool show_result) {
	const std::string tag = "#" + std::to_string(++g_run_seq);

	std::cout << "\n"
	          << LabeledRule("START " + tag) << "\n"
	          << PlainRule() << "\n"
	          << Centered(tag) << "\n"
	          << PlainRule() << "\n\n"
	          << sql << "\n\n"
	          << PlainRule() << "\n"
	          << std::flush; // flush stdout first so the stderr debug logs land after the SQL

	auto result = con.Query(sql);
	if (result->HasError()) {
		std::cerr << "  !! ERROR: " << result->GetError() << "\n";
		throw std::runtime_error("query failed: " + sql);
	}

	if (show_result) {
		std::cout << "\n" << result->ToString();
	}
	std::cout << LabeledRule("END " + tag) << "\n" << std::flush;
	return result;
}

std::unique_ptr<MaterializedQueryResult> Run(Connection &con, const std::string &sql) {
	return RunInternal(con, sql, false);
}

void Show(Connection &con, const std::string &sql) {
	RunInternal(con, sql, true);
}

// Bag-equality check in BOTH directions (the OpenIVM correctness contract):
// the materialized view must contain exactly the rows a full recomputation would,
// including duplicates. Any row returned by either EXCEPT ALL is a bug.
void VerifyEquivalent(Connection &con, const std::string &mv_relation, const std::string &base_query) {
	const std::string missing =
	    "SELECT COUNT(*) FROM ((" + base_query + ") EXCEPT ALL (SELECT * FROM " + mv_relation + ")) _missing";
	const std::string extra =
	    "SELECT COUNT(*) FROM ((SELECT * FROM " + mv_relation + ") EXCEPT ALL (" + base_query + ")) _extra";

	auto m = con.Query(missing);
	auto e = con.Query(extra);
	if (m->HasError() || e->HasError()) {
		std::cerr << "  !! VERIFY query failed: " << (m->HasError() ? m->GetError() : e->GetError()) << "\n";
		throw std::runtime_error("verify failed for " + mv_relation);
	}
	const auto missing_rows = m->GetValue(0, 0).GetValue<int64_t>();
	const auto extra_rows = e->GetValue(0, 0).GetValue<int64_t>();
	std::cout << "\n[verify] " << mv_relation << ": missing=" << missing_rows << " extra=" << extra_rows << "  -> "
	          << ((missing_rows == 0 && extra_rows == 0) ? "PASS (MV == base query, bag-equal)" : "FAIL") << "\n";
	if (missing_rows != 0 || extra_rows != 0) {
		throw std::runtime_error("MV not bag-equal to base query: " + mv_relation);
	}
}

// Ensures a writable directory for OpenIVM's compiled-SQL reference files and
// returns its path (empty string if it could not be created — compile still
// works, the files are just an optional artifact). openivm-spark sets
// openivm_files_path the same way so it can read the program back from disk.
std::string EnsureCompiledDir() {
	const char *tmp = std::getenv("TMPDIR");
	const std::string base = (tmp && *tmp) ? std::string(tmp) : std::string("/tmp");
	const std::string dir = base + "/openivm_demo_compiled";
	try {
		auto fs = duckdb::FileSystem::CreateLocal();
		if (!fs->DirectoryExists(dir)) {
			fs->CreateDirectory(dir);
		}
		return dir;
	} catch (...) {
		return "";
	}
}

// Calls openivm_compile_with_facts(view, facts) and prints each emitted statement
// (the refresh program) on its own lines. Returns the statement count. This is
// the compile-only entry point: it emits SQL but never mutates the MV.
idx_t ShowCompiledProgram(Connection &con, const std::string &view_name, const std::string &facts_json) {
	const std::string sql = "SELECT stmt_order, stmt_kind, sql FROM openivm_compile_with_facts('" + view_name + "', '" +
	                        facts_json + "') ORDER BY stmt_order";
	auto result = Run(con, sql);
	const idx_t rows = result->RowCount();
	std::cout << "  -> " << rows << " statement(s) emitted (compile-only — the MV is NOT modified):\n";
	for (idx_t r = 0; r < rows; r++) {
		std::cout << "\n  [" << result->GetValue(0, r).GetValue<int32_t>() << "] (" << result->GetValue(1, r).ToString()
		          << ")\n"
		          << result->GetValue(2, r).ToString() << "\n";
	}
	return rows;
}

// Asserts a query returns at least one row, throwing `message` otherwise.
void ExpectAtLeastOneRow(Connection &con, const std::string &sql, const std::string &message) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error("check query failed: " + result->GetError());
	}
	if (result->RowCount() == 0) {
		throw std::runtime_error(message);
	}
}

// ── Scenario 1: compile-only (openivm-spark's CompileFacts path) ─────────────
// openivm-spark drives OpenIVM without ever running PRAGMA refresh: it calls
// openivm_compile_with_facts('<view>', '<CompileFacts JSON>'), which returns the
// refresh-SQL program (one row per statement) and leaves the MV untouched. This
// is the single best place to learn how OpenIVM compiles a refresh — breakpoint
// OpenIvmCompileWithFactsBind (src/compile_facts.cpp) and step into
// GenerateRefreshSQL (src/upsert/refresh_sql.cpp).
void ScenarioCompileWithFacts(Connection &con) {
	Banner("Scenario 1: compile-only  (openivm_compile_with_facts -> emitted refresh SQL, nothing executed)");

	const std::string compiled_dir = EnsureCompiledDir();
	if (!compiled_dir.empty()) {
		Run(con, "SET openivm_files_path='" + compiled_dir + "'");
	}

	Run(con, "CREATE TABLE compile_sales (region VARCHAR, amount INT)");
	Run(con, "INSERT INTO compile_sales VALUES ('east', 100), ('west', 200)");
	Run(con, "CREATE MATERIALIZED VIEW mv_compile_region AS "
	         "SELECT region, SUM(amount) AS total, COUNT(*) AS cnt FROM compile_sales GROUP BY region");

	// Snapshot the MV so we can prove the compile call is side-effect free.
	Run(con, "CREATE TABLE mv_compile_region_before AS SELECT * FROM mv_compile_region");

	// Mutate the base table: the pending deltas are now non-empty.
	Run(con, "INSERT INTO compile_sales VALUES ('east', 50), ('north', 75)");
	Run(con, "DELETE FROM compile_sales WHERE region = 'west'");

	// (1) The exact call openivm-spark makes: Spark target dialect.
	std::cout << "\n--- target_dialect=spark  (the exact openivm-spark CompileFacts payload) ---\n";
	if (ShowCompiledProgram(con, "mv_compile_region",
	                        R"({"target_dialect":"spark","compile_only":true,"force_view_delta_cascade":true})") == 0) {
		throw std::runtime_error("spark compile emitted no statements");
	}

	// (2) Same view, DuckDB dialect — identical C++ path; only the final LPTS
	// dialect translation differs.
	std::cout << "\n--- target_dialect=duckdb  (same view, for contrast) ---\n";
	if (ShowCompiledProgram(con, "mv_compile_region", R"({"target_dialect":"duckdb","compile_only":true})") == 0) {
		throw std::runtime_error("duckdb compile emitted no statements");
	}

	// The emitted program must reference a delta table it would apply at refresh time.
	ExpectAtLeastOneRow(
	    con,
	    R"(SELECT 1 FROM openivm_compile_with_facts('mv_compile_region', '{"target_dialect":"duckdb","compile_only":true}') WHERE stmt_kind = 'data' AND sql LIKE '%openivm_delta_%')",
	    "compiled program references no delta table");

	// compile-only is a pure function: the MV is byte-for-byte unchanged.
	VerifyEquivalent(con, "mv_compile_region", "SELECT * FROM mv_compile_region_before");

	if (!compiled_dir.empty()) {
		std::cout << "\n[artifact] full compiled program also written under " << compiled_dir
		          << "/openivm_upsert_queries_mv_compile_region.sql\n";
	}
}

// ── Scenario 2: grouped aggregate ───────────────────────────────────────────
// Exercises CompileAggregateGroups (CTE consolidates per-group delta, then
// MERGE INTO updates/inserts the data table).
void ScenarioAggregate(Connection &con) {
	Banner("Scenario 2: grouped aggregate  (SUM / COUNT GROUP BY -> MERGE upsert)");

	Run(con, "CREATE TABLE sales (region VARCHAR, product VARCHAR, amount INT)");
	Run(con, "INSERT INTO sales VALUES ('US','Widget',100), ('EU','Gadget',200), ('US','Bolt',50)");

	const std::string base_query = "SELECT region, SUM(amount) AS total, COUNT(*) AS cnt FROM sales GROUP BY region";
	Run(con, "CREATE MATERIALIZED VIEW regional_totals AS " + base_query);
	Show(con, "SELECT * FROM regional_totals ORDER BY region");

	// Batch conflicting DML before a single refresh so delta consolidation has
	// real work: one insert into an existing group, one new group, one delete,
	// one update (delete+insert under the hood).
	Run(con, "INSERT INTO sales VALUES ('US','Gear',25)");
	Run(con, "INSERT INTO sales VALUES ('JP','Gizmo',300)");
	Run(con, "DELETE FROM sales WHERE product = 'Gadget'");
	Run(con, "UPDATE sales SET amount = amount + 10 WHERE product = 'Widget'");

	Run(con, "PRAGMA refresh('regional_totals')");
	Show(con, "SELECT * FROM regional_totals ORDER BY region");
	VerifyEquivalent(con, "regional_totals", base_query);
}

// ── Scenario 3: inner join ──────────────────────────────────────────────────
// Exercises IncrementalJoinRule: for N tables it builds 2^N-1 inclusion-exclusion
// terms (delta-vs-current scans) combined with UNION ALL.
void ScenarioInnerJoin(Connection &con) {
	Banner("Scenario 3: inner join  (delta x current inclusion-exclusion terms)");

	Run(con, "CREATE TABLE orders (order_id INT, product_id INT, qty INT)");
	Run(con, "CREATE TABLE products (product_id INT, name VARCHAR, price INT)");
	Run(con, "INSERT INTO orders VALUES (1,10,2), (2,20,1), (3,10,5)");
	Run(con, "INSERT INTO products VALUES (10,'Widget',100), (20,'Gadget',200)");

	const std::string base_query = "SELECT o.order_id, p.name, o.qty * p.price AS line_total "
	                               "FROM orders o JOIN products p ON o.product_id = p.product_id";
	Run(con, "CREATE MATERIALIZED VIEW order_lines AS " + base_query);
	Show(con, "SELECT * FROM order_lines ORDER BY order_id");

	// Delta on BOTH sides before one refresh (exercises the cross terms).
	Run(con, "INSERT INTO products VALUES (30,'Sprocket',75)");
	Run(con, "INSERT INTO orders VALUES (4,30,4), (5,20,3)");
	Run(con, "DELETE FROM orders WHERE order_id = 2");
	Run(con, "UPDATE products SET price = 150 WHERE product_id = 10");

	Run(con, "PRAGMA refresh('order_lines')");
	Show(con, "SELECT * FROM order_lines ORDER BY order_id");
	VerifyEquivalent(con, "order_lines", base_query);
}

// ── Scenario 4: projection + filter ─────────────────────────────────────────
// Exercises CompileProjectionsFilters: counting-based consolidation (GROUP BY +
// generate_series/rowid) to preserve bag semantics under inserts and deletes.
void ScenarioProjectionFilter(Connection &con) {
	Banner("Scenario 4: projection + filter  (counting-based bag consolidation)");

	Run(con, "CREATE TABLE events (id INT, kind VARCHAR, score INT)");
	Run(con, "INSERT INTO events VALUES (1,'click',5), (2,'view',1), (3,'click',9), (4,'purchase',50)");

	const std::string base_query = "SELECT id, kind, score * 2 AS doubled FROM events WHERE score >= 5";
	Run(con, "CREATE MATERIALIZED VIEW hot_events AS " + base_query);
	Show(con, "SELECT * FROM hot_events ORDER BY id");

	// Mix of rows that enter the filter, leave it, and stay out.
	Run(con, "INSERT INTO events VALUES (5,'click',7), (6,'view',2)");
	Run(con, "DELETE FROM events WHERE id = 3");
	Run(con, "UPDATE events SET score = 100 WHERE id = 2"); // crosses the filter boundary

	Run(con, "PRAGMA refresh('hot_events')");
	Show(con, "SELECT * FROM hot_events ORDER BY id");
	VerifyEquivalent(con, "hot_events", base_query);
}

} // namespace

int main() {
	duckdb::DuckDB db(":memory:");
	db.LoadStaticExtension<duckdb::OpenivmExtension>();
	Connection con(db);

	try {
		ScenarioCompileWithFacts(con);
		ScenarioAggregate(con);
		ScenarioInnerJoin(con);
		ScenarioProjectionFilter(con);
	} catch (const std::exception &ex) {
		std::cerr << "\nDEMO FAILED: " << ex.what() << "\n";
		return 1;
	}

	std::cout << "\nAll scenarios passed (each MV is bag-equal to its base query).\n";
	return 0;
}
