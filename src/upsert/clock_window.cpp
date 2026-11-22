#include "upsert/clock_window.hpp"

#include "core/openivm_debug.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

#include <cctype>
#include <functional>

namespace duckdb {
namespace openivm {

const char *ClockModeName(ClockMode mode) {
	switch (mode) {
	case ClockMode::NOT_APPLICABLE:
		return "not_applicable";
	case ClockMode::FULL_DEFAULT:
		return "full_default";
	case ClockMode::FULL_AT_AS_OF:
		return "full_at_as_of";
	case ClockMode::INCREMENTAL:
		return "incremental";
	}
	return "full_default";
}

namespace {

constexpr idx_t MAX_ZONE_LENGTH = 64;

struct Instant {
	string canonical; // YYYY-MM-DD HH:MM:SS.ffffff, ordered like the instant
};

bool ReadDigits(const string &text, idx_t pos, idx_t count, int &value) {
	if (pos + count > text.size()) {
		return false;
	}
	value = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!isdigit(static_cast<unsigned char>(text[pos + i]))) {
			return false;
		}
		value = value * 10 + (text[pos + i] - '0');
	}
	return true;
}

bool IsLeapYear(int year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

// Accepts only RFC 3339 UTC: YYYY-MM-DDTHH:MM:SS[.f{1,6}]Z. Offsets and leap seconds are rejected.
bool ParseInstant(const string &text, Instant &out) {
	int year, month, day, hour, minute, second;
	if (text.size() < 20 || !ReadDigits(text, 0, 4, year) || text[4] != '-' || !ReadDigits(text, 5, 2, month) ||
	    text[7] != '-' || !ReadDigits(text, 8, 2, day) || text[10] != 'T' || !ReadDigits(text, 11, 2, hour) ||
	    text[13] != ':' || !ReadDigits(text, 14, 2, minute) || text[16] != ':' || !ReadDigits(text, 17, 2, second)) {
		return false;
	}
	static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (year < 1 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) {
		return false;
	}
	int max_day = DAYS[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
	if (day < 1 || day > max_day) {
		return false;
	}
	string fraction;
	idx_t pos = 19;
	if (text[pos] == '.') {
		pos++;
		while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) {
			fraction += text[pos++];
		}
		if (fraction.empty() || fraction.size() > 6) {
			return false;
		}
	}
	if (pos + 1 != text.size() || text[pos] != 'Z') {
		return false;
	}
	fraction.append(6 - fraction.size(), '0');
	out.canonical = text.substr(0, 10) + " " + text.substr(11, 8) + "." + fraction;
	return true;
}

bool IsValidZoneName(const string &zone) {
	if (zone.empty() || zone.size() > MAX_ZONE_LENGTH) {
		return false;
	}
	for (char c : zone) {
		if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '/' && c != '+' && c != '-') {
			return false;
		}
	}
	return true;
}

// The date the zone observes at the instant; also validates the zone name against the ICU database.
bool LocalDate(Connection &con, const Instant &instant, const string &zone, string &local_date) {
	auto result = con.Query("SELECT CAST(timezone('" + zone + "', TIMESTAMPTZ '" + instant.canonical +
	                        "+00:00') AS DATE)::VARCHAR");
	if (result->HasError() || result->RowCount() != 1 || result->Collection().GetValue(0, 0).IsNull()) {
		return false;
	}
	local_date = result->Collection().GetValue(0, 0).ToString();
	return true;
}

enum class ClockKind : uint8_t { TIMESTAMP, DATE };

// A TIMESTAMPTZ literal when DuckDB binds the query. Spark has no such type: its TIMESTAMP reads the offset, so the
// raw group recompute statements, which are not bound by DuckDB, use it.
string LiteralSql(ClockKind kind, const Instant &instant, const string &local_date, SqlDialect dialect, bool bound) {
	if (kind == ClockKind::DATE) {
		return "CAST('" + local_date + "' AS DATE)";
	}
	string type = (dialect == SqlDialect::SPARK && !bound) ? "TIMESTAMP" : "TIMESTAMPTZ";
	return "CAST('" + instant.canonical + "+00:00' AS " + type + ")";
}

struct ClockUse {
	unique_ptr<ParsedExpression> comparison; // an unmodified copy of the comparison
	idx_t clock_operand;                     // 0 = left, 1 = right
	ClockKind kind;
	unique_ptr<ParsedExpression> *slot; // the clock operand within the working query
};

bool IsClockKindName(const string &lower, ClockKind &kind) {
	if (lower == "current_timestamp") {
		kind = ClockKind::TIMESTAMP;
		return true;
	}
	if (lower == "current_date") {
		kind = ClockKind::DATE;
		return true;
	}
	return false;
}

bool IsUnqualifiedColumn(const ParsedExpression &expression, string &lower_name) {
	if (expression.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &column = expression.Cast<ColumnRefExpression>();
	if (column.IsQualified()) {
		return false;
	}
	lower_name = StringUtil::Lower(column.GetColumnName().GetIdentifierName());
	return true;
}

bool IsAggregateName(const string &lower) {
	return lower == "sum" || lower == "count" || lower == "count_star" || lower == "min" || lower == "max" ||
	       lower == "avg";
}

bool IsScalarFunctionName(const string &lower) {
	return IsAggregateName(lower) || lower == "coalesce";
}

bool IsOrderedComparison(ExpressionType type) {
	return type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	       type == ExpressionType::COMPARE_GREATERTHAN || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
}

// Validates an aggregate argument and records every clock comparison. Returns the reason the shape is unsupported, or
// an empty string.
class AggregateArgumentWalker {
public:
	AggregateArgumentWalker(const case_insensitive_set_t &columns_p, vector<ClockUse> &uses_p)
	    : columns(columns_p), uses(uses_p) {
	}

	string Walk(unique_ptr<ParsedExpression> &slot) {
		auto &expression = *slot;
		switch (expression.GetExpressionClass()) {
		case ExpressionClass::CONSTANT:
			return "";
		case ExpressionClass::COLUMN_REF: {
			string name;
			ClockKind kind = ClockKind::TIMESTAMP;
			if (IsUnqualifiedColumn(expression, name) && IsClockKindName(name, kind)) {
				return "unsupported_clock_use";
			}
			return IsSourceColumn(expression) ? "" : "unsupported_view_shape";
		}
		case ExpressionClass::COMPARISON:
			return WalkComparison(slot);
		case ExpressionClass::FUNCTION: {
			auto &function = expression.Cast<FunctionExpression>();
			if (function.Filter() || (function.OrderBy() && !function.OrderBy()->orders.empty()) ||
			    function.Distinct() ||
			    !IsScalarFunctionName(StringUtil::Lower(function.FunctionName().GetIdentifierName()))) {
				return "unsupported_view_shape";
			}
			return WalkChildren(expression);
		}
		case ExpressionClass::CASE:
		case ExpressionClass::CAST:
		case ExpressionClass::CONJUNCTION:
		case ExpressionClass::OPERATOR:
		case ExpressionClass::BETWEEN:
			return WalkChildren(expression);
		default:
			return "unsupported_view_shape";
		}
	}

private:
	bool IsSourceColumn(const ParsedExpression &expression) const {
		auto &column = expression.Cast<ColumnRefExpression>();
		return column.ColumnNames().size() == 1 && columns.count(column.GetColumnName().GetIdentifierName()) > 0;
	}

	string WalkChildren(ParsedExpression &expression) {
		string reason;
		ParsedExpressionIterator::EnumerateChildren(expression, [&](unique_ptr<ParsedExpression> &child) {
			if (reason.empty()) {
				reason = Walk(child);
			}
		});
		return reason;
	}

	string WalkComparison(unique_ptr<ParsedExpression> &slot) {
		auto &comparison = slot->Cast<ComparisonExpression>();
		ClockKind kinds[2] = {ClockKind::TIMESTAMP, ClockKind::TIMESTAMP};
		bool is_clock[2] = {false, false};
		unique_ptr<ParsedExpression> *operands[2] = {&comparison.LeftMutable(), &comparison.RightMutable()};
		for (idx_t i = 0; i < 2; i++) {
			string name;
			is_clock[i] = IsUnqualifiedColumn(**operands[i], name) && IsClockKindName(name, kinds[i]);
		}
		if (!is_clock[0] && !is_clock[1]) {
			return WalkChildren(*slot);
		}
		if (is_clock[0] && is_clock[1]) {
			return "unsupported_clock_use";
		}
		idx_t clock_operand = is_clock[0] ? 0 : 1;
		auto &other = *operands[1 - clock_operand];
		string other_name;
		ClockKind other_kind = ClockKind::TIMESTAMP;
		if (!IsOrderedComparison(slot->GetExpressionType()) ||
		    other->GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		    (IsUnqualifiedColumn(*other, other_name) && IsClockKindName(other_name, other_kind))) {
			return "unsupported_clock_use";
		}
		if (!IsSourceColumn(*other)) {
			return "unsupported_view_shape";
		}
		ClockUse use;
		use.comparison = slot->Copy();
		use.clock_operand = clock_operand;
		use.kind = kinds[clock_operand];
		use.slot = operands[clock_operand];
		uses.push_back(std::move(use));
		return "";
	}

	const case_insensitive_set_t &columns;
	vector<ClockUse> &uses;
};

unique_ptr<ParsedExpression> ParseLiteral(Connection &con, const string &sql) {
	Parser parser(*con.context);
	auto expressions = parser.ParseExpressionList(sql);
	D_ASSERT(expressions.size() == 1);
	return std::move(expressions[0]);
}

// A comparison with its clock operand replaced by the literal, rendered as SQL.
string ComparisonAt(Connection &con, const ClockUse &use, const string &literal) {
	auto copy = use.comparison->Copy();
	auto &comparison = copy->Cast<ComparisonExpression>();
	auto &operand = use.clock_operand == 0 ? comparison.LeftMutable() : comparison.RightMutable();
	operand = ParseLiteral(con, literal);
	return copy->ToString();
}

} // namespace

ClockPlan PlanClockWindow(Connection &con, const ClockFacts &clock, const string &view_query_sql,
                          const vector<string> &group_columns, bool has_delta_sources, SqlDialect dialect) {
	ClockPlan plan;
	auto fail = [&](ClockMode mode, const string &reason) {
		plan.mode = mode;
		plan.reason = reason;
		plan.query.clear();
		plan.full_query.clear();
		plan.crossing_groups.clear();
		OPENIVM_DEBUG_PRINT("[ClockWindow] mode=%s reason=%s\n", ClockModeName(mode), reason.c_str());
		return plan;
	};

	Instant now, previous;
	string now_date, previous_date;
	if (!ParseInstant(clock.as_of, now) || !IsValidZoneName(clock.time_zone) ||
	    !LocalDate(con, now, clock.time_zone, now_date)) {
		return fail(ClockMode::FULL_DEFAULT, "invalid_clock");
	}
	bool has_previous = !clock.previous_as_of.empty() && !clock.previous_time_zone.empty();
	if ((!clock.previous_as_of.empty() && !ParseInstant(clock.previous_as_of, previous)) ||
	    (!clock.previous_time_zone.empty() && !IsValidZoneName(clock.previous_time_zone))) {
		return fail(ClockMode::FULL_DEFAULT, "invalid_clock");
	}

	Parser parser(*con.context);
	try {
		parser.ParseQuery(view_query_sql);
	} catch (std::exception &) {
		return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
	}
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
	}
	auto &node = *parser.statements[0]->Cast<SelectStatement>().node;
	if (node.type != QueryNodeType::SELECT_NODE || !node.modifiers.empty() || !node.cte_map.map.empty()) {
		return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
	}
	auto &select = node.Cast<SelectNode>();
	if (select.where_clause || select.having || select.qualify || select.sample ||
	    select.aggregate_handling != AggregateHandling::STANDARD_HANDLING || select.groups.grouping_sets.size() > 1 ||
	    select.groups.group_expressions.empty() || group_columns.empty() || !select.from_table ||
	    select.from_table->type != TableReferenceType::BASE_TABLE || select.from_table->sample ||
	    !select.from_table->column_name_alias.empty() || select.from_table->Cast<BaseTableRef>().at_clause) {
		return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
	}

	// Every column the query reads must be a real source column, so no SQL value function such as
	// `localtimestamp` or `current_user` can hide behind a column reference.
	case_insensitive_set_t columns;
	auto described = con.Query("SELECT * FROM " + select.from_table->ToString() + " LIMIT 0");
	if (described->HasError()) {
		return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
	}
	for (idx_t i = 0; i < described->ColumnCount(); i++) {
		columns.insert(described->ColumnName(i).GetIdentifierName());
	}
	ClockKind unused = ClockKind::TIMESTAMP;
	for (auto &name : columns) {
		if (IsClockKindName(StringUtil::Lower(name), unused)) {
			return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
		}
	}

	vector<ClockUse> uses;
	AggregateArgumentWalker walker(columns, uses);
	vector<string> group_key_sql(group_columns.size());
	vector<bool> group_seen(group_columns.size(), false);
	for (auto &item : select.select_list) {
		if (item->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &column = item->Cast<ColumnRefExpression>();
			if (column.ColumnNames().size() != 1 || columns.count(column.GetColumnName().GetIdentifierName()) == 0) {
				return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
			}
			string output =
			    item->HasAlias() ? item->GetAlias().GetIdentifierName() : column.GetColumnName().GetIdentifierName();
			bool grouped = false;
			for (auto &group : select.groups.group_expressions) {
				grouped = grouped || (group->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
				                      StringUtil::CIEquals(group->ToString(), column.ToString()));
			}
			idx_t index = 0;
			while (index < group_columns.size() && !StringUtil::CIEquals(group_columns[index], output)) {
				index++;
			}
			if (!grouped || index == group_columns.size() || group_seen[index]) {
				return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
			}
			group_seen[index] = true;
			group_key_sql[index] = column.ToString();
			continue;
		}
		if (item->GetExpressionClass() != ExpressionClass::FUNCTION) {
			return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
		}
		auto &function = item->Cast<FunctionExpression>();
		if (function.Filter() || (function.OrderBy() && !function.OrderBy()->orders.empty()) || function.Distinct() ||
		    !IsAggregateName(StringUtil::Lower(function.FunctionName().GetIdentifierName()))) {
			return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
		}
		string reason;
		ParsedExpressionIterator::EnumerateChildren(*item, [&](unique_ptr<ParsedExpression> &child) {
			if (reason.empty()) {
				reason = walker.Walk(child);
			}
		});
		if (!reason.empty()) {
			return fail(ClockMode::FULL_DEFAULT, reason);
		}
	}
	for (auto seen : group_seen) {
		if (!seen) {
			return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
		}
	}
	for (auto &group : select.groups.group_expressions) {
		if (group->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return fail(ClockMode::FULL_DEFAULT, "unsupported_view_shape");
		}
	}
	if (uses.empty()) {
		return fail(ClockMode::FULL_DEFAULT, "no_clock_predicate");
	}

	bool same_zone = has_previous && clock.previous_time_zone == clock.time_zone;
	if (same_zone && !LocalDate(con, previous, clock.time_zone, previous_date)) {
		return fail(ClockMode::FULL_DEFAULT, "invalid_clock");
	}

	// The as-of replaces each clock reference in the working query; the comparisons kept in `uses` stay untouched.
	for (auto &use : uses) {
		*use.slot = ParseLiteral(con, LiteralSql(use.kind, now, now_date, dialect, false));
	}
	plan.query = parser.statements[0]->ToString();
	for (auto &use : uses) {
		*use.slot = ParseLiteral(con, LiteralSql(use.kind, now, now_date, dialect, true));
	}
	plan.full_query = parser.statements[0]->ToString();

	if (!has_delta_sources) {
		plan.mode = ClockMode::FULL_AT_AS_OF;
		plan.reason = "no_delta_sources";
	} else if (!has_previous) {
		plan.mode = ClockMode::FULL_AT_AS_OF;
		plan.reason = "missing_previous";
	} else if (!same_zone) {
		plan.mode = ClockMode::FULL_AT_AS_OF;
		plan.reason = "zone_changed";
	} else if (previous.canonical > now.canonical) {
		plan.mode = ClockMode::FULL_AT_AS_OF;
		plan.reason = "clock_regressed";
	}
	if (plan.mode == ClockMode::FULL_AT_AS_OF) {
		plan.query = plan.full_query;
		OPENIVM_DEBUG_PRINT("[ClockWindow] mode=full_at_as_of reason=%s\n", plan.reason.c_str());
		return plan;
	}

	// A group's result can change without a source change only when a row's clock predicate flips between the two
	// instants; every other term of the aggregate is a function of the row alone.
	string from = select.from_table->ToString();
	string keys;
	for (idx_t i = 0; i < group_key_sql.size(); i++) {
		keys += (i == 0 ? "" : ", ") + group_key_sql[i];
	}
	for (idx_t i = 0; i < uses.size(); i++) {
		string before = ComparisonAt(con, uses[i], LiteralSql(uses[i].kind, previous, previous_date, dialect, false));
		string after = ComparisonAt(con, uses[i], LiteralSql(uses[i].kind, now, now_date, dialect, false));
		plan.crossing_groups += (i == 0 ? "" : "\n  UNION\n  ") + string("SELECT DISTINCT ") + keys + " FROM " + from +
		                        " WHERE (" + before + ") IS DISTINCT FROM (" + after + ")";
	}
	plan.mode = ClockMode::INCREMENTAL;
	plan.reason = "ok";
	OPENIVM_DEBUG_PRINT("[ClockWindow] mode=incremental clock_uses=%zu\n", uses.size());
	return plan;
}

} // namespace openivm
} // namespace duckdb
