#include "upsert/refresh_compiler.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/sql_utils.hpp"
#include "rules/column_hider.hpp"
#include "upsert/refresh_internal.hpp"

#include <map>
#include <regex>

namespace duckdb {

namespace {

static string DeltaSourceRef(const string &source, const string &catalog_prefix) {
	if (!source.empty() && (source[0] == '(' || source.find('.') != string::npos)) {
		return source;
	}
	return catalog_prefix + SqlUtils::QuoteIdentifier(source);
}

static string TrimCopy(const string &input) {
	idx_t start = 0;
	while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
		start++;
	}
	idx_t end = input.size();
	while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
		end--;
	}
	return input.substr(start, end - start);
}

static string LowerCopy(string input) {
	std::transform(input.begin(), input.end(), input.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return input;
}

static string StripIdentifierQuotes(string input) {
	input = TrimCopy(input);
	if (input.size() >= 2 && input.front() == '"' && input.back() == '"') {
		return input.substr(1, input.size() - 2);
	}
	return SqlUtils::LastIdentifierPart(input);
}

static string PartitionOutputColumn(const string &input) {
	auto pos = input.find('=');
	return StripIdentifierQuotes(pos == string::npos ? input : input.substr(0, pos));
}

static vector<string> SplitTopLevelComma(const string &input) {
	vector<string> parts;
	idx_t start = 0;
	int depth = 0;
	for (idx_t i = 0; i < input.size(); i++) {
		char c = input[i];
		if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		} else if (c == ',' && depth == 0) {
			parts.push_back(TrimCopy(input.substr(start, i - start)));
			start = i + 1;
		}
	}
	parts.push_back(TrimCopy(input.substr(start)));
	return parts;
}

static idx_t FindTopLevelFrom(const string &input) {
	string lower = LowerCopy(input);
	int depth = 0;
	for (idx_t i = 0; i + 6 <= lower.size(); i++) {
		char c = lower[i];
		if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		}
		if (depth == 0 && lower.compare(i, 6, " from ") == 0) {
			return i;
		}
	}
	return string::npos;
}

struct RunningWindowExpr {
	string function_name;
	string argument;
	string output_column;
};

struct RunningWindowPlan {
	string source_table;
	string partition_column;
	string order_column;
	vector<string> output_columns;
	vector<pair<string, string>> passthrough_columns;
	vector<RunningWindowExpr> window_exprs;
};

static bool ParsePassthroughProjection(const string &item, pair<string, string> &out) {
	static const std::regex alias_regex(R"(^\s*(.+?)\s+as\s+("[^"]+"|[A-Za-z_][A-Za-z0-9_]*)\s*$)",
	                                    std::regex_constants::icase);
	std::smatch match;
	string expr = item;
	string output;
	if (std::regex_match(item, match, alias_regex)) {
		expr = TrimCopy(match[1].str());
		output = StripIdentifierQuotes(match[2].str());
	} else {
		output = StripIdentifierQuotes(expr);
	}
	string source = StripIdentifierQuotes(expr);
	if (source.empty() || source.find(' ') != string::npos || source.find('(') != string::npos) {
		return false;
	}
	out = std::make_pair(source, output);
	return true;
}

static bool ParseRunningWindowProjection(const string &item, RunningWindowExpr &out, string &partition_col,
                                         string &order_col) {
	static const std::regex alias_regex(R"(^\s*(.+?)\s+as\s+("[^"]+"|[A-Za-z_][A-Za-z0-9_]*)\s*$)",
	                                    std::regex_constants::icase);
	static const std::regex window_regex(
	    R"(^\s*(sum|min|max|count|avg)\s*\(\s*([^)]+?)\s*\)\s+over\s*\((.*)\)\s*$)",
	    std::regex_constants::icase);
	std::smatch alias_match;
	if (!std::regex_match(item, alias_match, alias_regex)) {
		return false;
	}
	string expr = TrimCopy(alias_match[1].str());
	std::smatch window_match;
	if (!std::regex_match(expr, window_match, window_regex)) {
		return false;
	}
	string spec = TrimCopy(window_match[3].str());
	string spec_lower = LowerCopy(spec);
	auto part_pos = spec_lower.find("partition by ");
	auto order_pos = spec_lower.find(" order by ");
	if (part_pos == string::npos || order_pos == string::npos || order_pos <= part_pos) {
		return false;
	}
	string part_expr = TrimCopy(spec.substr(part_pos + 13, order_pos - (part_pos + 13)));
	string order_expr = TrimCopy(spec.substr(order_pos + 10));
	string order_lower = LowerCopy(order_expr);
	auto frame_pos = order_lower.find(" rows ");
	if (frame_pos == string::npos) {
		frame_pos = order_lower.find(" range ");
	}
	if (frame_pos != string::npos) {
		string frame = order_lower.substr(frame_pos);
		if (frame.find("unbounded preceding") == string::npos || frame.find("current row") == string::npos) {
			return false;
		}
		order_expr = TrimCopy(order_expr.substr(0, frame_pos));
	}
	auto order_space = order_expr.find(' ');
	if (order_space != string::npos) {
		string suffix = LowerCopy(TrimCopy(order_expr.substr(order_space + 1)));
		if (suffix != "asc") {
			return false;
		}
		order_expr = TrimCopy(order_expr.substr(0, order_space));
	}
	string parsed_part = StripIdentifierQuotes(part_expr);
	string parsed_order = StripIdentifierQuotes(order_expr);
	if (parsed_part.empty() || parsed_order.empty() || part_expr.find(',') != string::npos ||
	    order_expr.find(',') != string::npos) {
		return false;
	}
	if (!partition_col.empty() && !StringUtil::CIEquals(partition_col, parsed_part)) {
		return false;
	}
	if (!order_col.empty() && !StringUtil::CIEquals(order_col, parsed_order)) {
		return false;
	}
	partition_col = parsed_part;
	order_col = parsed_order;
	out.function_name = LowerCopy(window_match[1].str());
	out.argument = StripIdentifierQuotes(window_match[2].str());
	out.output_column = StripIdentifierQuotes(alias_match[2].str());
	return true;
}

static bool TryParseRunningWindowPlan(const string &view_query_sql, const vector<string> &partition_columns,
                                      const vector<string> &column_names, RunningWindowPlan &plan) {
	string query = TrimCopy(view_query_sql);
	if (!StringUtil::StartsWith(LowerCopy(query), "select ")) {
		return false;
	}
	auto from_pos = FindTopLevelFrom(query);
	if (from_pos == string::npos) {
		return false;
	}
	string select_list = query.substr(7, from_pos - 7);
	string from_tail = TrimCopy(query.substr(from_pos + 6));
	if (from_tail.empty() || from_tail.find(' ') != string::npos || from_tail.find(',') != string::npos) {
		return false;
	}
	plan.source_table = StripIdentifierQuotes(from_tail);
	auto parsed_partition = partition_columns.empty() ? "" : PartitionOutputColumn(partition_columns[0]);
	string parsed_order;
	for (auto &item : SplitTopLevelComma(select_list)) {
		if (LowerCopy(item).find(" over ") != string::npos) {
			RunningWindowExpr expr;
			if (!ParseRunningWindowProjection(item, expr, parsed_partition, parsed_order)) {
				return false;
			}
			plan.window_exprs.push_back(expr);
		} else {
			pair<string, string> pass;
			if (!ParsePassthroughProjection(item, pass)) {
				return false;
			}
			plan.passthrough_columns.push_back(pass);
		}
	}
	if (plan.window_exprs.empty() || parsed_partition.empty() || parsed_order.empty() || partition_columns.size() != 1) {
		return false;
	}
	plan.partition_column = parsed_partition;
	plan.order_column = parsed_order;
	for (auto &col : column_names) {
		bool found = false;
		for (auto &pass : plan.passthrough_columns) {
			found = found || StringUtil::CIEquals(col, pass.second);
		}
		for (auto &expr : plan.window_exprs) {
			found = found || StringUtil::CIEquals(col, expr.output_column);
		}
		if (!found) {
			return false;
		}
	}
	plan.output_columns = column_names;
	return true;
}

static bool TryParseLptsRunningWindowPlan(const string &view_query_sql, const vector<string> &partition_columns,
                                          const vector<string> &column_names, RunningWindowPlan &plan) {
	static const std::regex window_regex(
	    R"((sum|min|max|count|avg)\s*\(\s*([^)]+?)\s*\)\s+OVER\s*\(\s*PARTITION\s+BY\s+([A-Za-z0-9_]+)\s+ORDER\s+BY\s+([A-Za-z0-9_]+)(?:\s+ASC)?(?:\s+NULLS\s+LAST)?(?:\s+(?:ROWS|RANGE)\s+BETWEEN\s+UNBOUNDED\s+PRECEDING\s+AND\s+CURRENT\s+ROW)?\s*\))",
	    std::regex_constants::icase);
	string lower = LowerCopy(view_query_sql);
	auto scan_pos = lower.find("scan_");
	if (scan_pos == string::npos) {
		return false;
	}
	auto alias_start = view_query_sql.find('(', scan_pos);
	if (alias_start == string::npos) {
		return false;
	}
	auto alias_end = view_query_sql.find(')', alias_start + 1);
	if (alias_end == string::npos) {
		return false;
	}
	auto select_pos = lower.find(" as (select ", alias_end);
	if (select_pos == string::npos) {
		return false;
	}
	select_pos += 12;
	auto from_pos = lower.find(" from ", select_pos);
	if (from_pos == string::npos) {
		return false;
	}
	auto source_end = view_query_sql.find(')', from_pos + 6);
	if (source_end == string::npos) {
		return false;
	}
	auto scan_aliases = SplitTopLevelComma(view_query_sql.substr(alias_start + 1, alias_end - alias_start - 1));
	auto source_cols = SplitTopLevelComma(view_query_sql.substr(select_pos, from_pos - select_pos));
	if (scan_aliases.size() != source_cols.size()) {
		return false;
	}
	std::map<string, string> alias_to_source;
	for (idx_t i = 0; i < scan_aliases.size(); i++) {
		alias_to_source[StringUtil::Lower(StripIdentifierQuotes(scan_aliases[i]))] = StripIdentifierQuotes(source_cols[i]);
	}
	plan.source_table = StripIdentifierQuotes(view_query_sql.substr(from_pos + 6, source_end - from_pos - 6));
	vector<string> output_names = column_names;
	auto final_select = lower.rfind("\nselect ");
	if (final_select == string::npos) {
		final_select = lower.rfind(" select ");
	}
	if (final_select != string::npos) {
		auto final_from = lower.find(" from ", final_select + 8);
		if (final_from != string::npos) {
			vector<string> parsed_outputs;
			for (auto &item : SplitTopLevelComma(view_query_sql.substr(final_select + 8, final_from - final_select - 8))) {
				pair<string, string> pass;
				if (!ParsePassthroughProjection(item, pass)) {
					parsed_outputs.clear();
					break;
				}
				parsed_outputs.push_back(pass.second);
			}
			if (!parsed_outputs.empty()) {
				output_names = parsed_outputs;
			}
		}
	}
	vector<string> non_base_outputs;
	for (auto &col : output_names) {
		bool is_base = false;
		for (auto &entry : alias_to_source) {
			if (StringUtil::CIEquals(col, entry.second)) {
				plan.passthrough_columns.push_back(std::make_pair(entry.second, col));
				is_base = true;
				break;
			}
		}
		if (!is_base) {
			non_base_outputs.push_back(col);
		}
	}
	plan.output_columns = output_names;
	if (non_base_outputs.empty()) {
		return false;
	}
	auto begin = std::sregex_iterator(view_query_sql.begin(), view_query_sql.end(), window_regex);
	auto end = std::sregex_iterator();
	idx_t window_idx = 0;
	string parsed_partition = partition_columns.empty() ? "" : PartitionOutputColumn(partition_columns[0]);
	string parsed_order;
	for (auto it = begin; it != end; ++it) {
		if (window_idx >= non_base_outputs.size()) {
			return false;
		}
		auto translate = [&](const string &alias) -> string {
			auto found = alias_to_source.find(StringUtil::Lower(StripIdentifierQuotes(alias)));
			return found == alias_to_source.end() ? "" : found->second;
		};
		string arg = StripIdentifierQuotes((*it)[2].str());
		if (arg != "*") {
			arg = translate(arg);
		}
		string part = translate((*it)[3].str());
		string order = translate((*it)[4].str());
		if (arg.empty() || part.empty() || order.empty()) {
			return false;
		}
		if (!parsed_partition.empty() && !StringUtil::CIEquals(parsed_partition, part)) {
			return false;
		}
		if (!parsed_order.empty() && !StringUtil::CIEquals(parsed_order, order)) {
			return false;
		}
		parsed_partition = part;
		parsed_order = order;
		RunningWindowExpr expr;
		expr.function_name = LowerCopy((*it)[1].str());
		expr.argument = arg;
		expr.output_column = non_base_outputs[window_idx++];
		plan.window_exprs.push_back(expr);
	}
	return !plan.window_exprs.empty() && window_idx == non_base_outputs.size() && partition_columns.size() == 1;
}

static string QualifiedColumn(const string &alias, const string &column) {
	return alias + "." + SqlUtils::QuoteIdentifier(column);
}

static string RunningLocalExpr(const RunningWindowExpr &expr, const RunningWindowPlan &plan) {
	string arg = expr.argument == "*" ? "*" : QualifiedColumn("d", expr.argument);
	string over = " OVER (PARTITION BY " + QualifiedColumn("d", plan.partition_column) + " ORDER BY " +
	              QualifiedColumn("d", plan.order_column) + ")";
	return StringUtil::Upper(expr.function_name) + "(" + arg + ")" + over;
}

static string RunningAdjustedExpr(const RunningWindowExpr &expr, const RunningWindowPlan &plan) {
	string local = RunningLocalExpr(expr, plan);
	string state_col = QualifiedColumn("s", expr.output_column);
	if (expr.function_name == "sum") {
		return "CASE WHEN " + state_col + " IS NULL THEN " + local + " ELSE " + state_col + " + " + local + " END";
	}
	if (expr.function_name == "count") {
		return "COALESCE(" + state_col + ", 0) + " + local;
	}
	if (expr.function_name == "min") {
		return "CASE WHEN " + state_col + " IS NULL THEN " + local + " WHEN " + local + " IS NULL THEN " + state_col +
		       " ELSE LEAST(" + state_col + ", " + local + ") END";
	}
	if (expr.function_name == "max") {
		return "CASE WHEN " + state_col + " IS NULL THEN " + local + " WHEN " + local + " IS NULL THEN " + state_col +
		       " ELSE GREATEST(" + state_col + ", " + local + ") END";
	}
	if (expr.function_name == "avg" && expr.argument != "*") {
		string sum_local = "SUM(" + QualifiedColumn("d", expr.argument) + ") OVER (PARTITION BY " +
		                   QualifiedColumn("d", plan.partition_column) + " ORDER BY " +
		                   QualifiedColumn("d", plan.order_column) + ")";
		string count_local = "COUNT(" + QualifiedColumn("d", expr.argument) + ") OVER (PARTITION BY " +
		                     QualifiedColumn("d", plan.partition_column) + " ORDER BY " +
		                     QualifiedColumn("d", plan.order_column) + ")";
		string prior_count = "COALESCE(s.openivm_prior_count, 0)";
		return "((COALESCE(" + state_col + " * s.openivm_prior_count, 0)) + COALESCE(" + sum_local +
		       ", 0)) / NULLIF(" + prior_count + " + " + count_local + ", 0)";
	}
	return "";
}

static string BuildRunningWindowSuffixRefreshSQL(const string &view_name, const string &view_query_sql,
                                                 const string &delta_ts_filter, const string &catalog_prefix,
                                                 const vector<string> &partition_columns,
                                                 const vector<WindowPartitionDeltaSpec> &partition_delta_specs,
                                                 const vector<string> &column_names) {
	if (partition_delta_specs.size() != 1) {
		return "";
	}
	vector<string> visible_column_names;
	for (auto &col : column_names) {
		if (!StringUtil::CIEquals(col, openivm::MULTIPLICITY_COL) &&
		    !StringUtil::CIEquals(col, openivm::TIMESTAMP_COL)) {
			visible_column_names.push_back(col);
		}
	}
	RunningWindowPlan plan;
	static const std::regex quick_func_regex(R"((sum|min|max|count|avg)\s*\()", std::regex_constants::icase);
	std::smatch quick_func;
	if (std::regex_search(view_query_sql, quick_func, quick_func_regex)) {
		plan.partition_column = partition_columns.empty() ? "" : PartitionOutputColumn(partition_columns[0]);
		plan.order_column = "";
		string arg_column;
		for (auto &col : visible_column_names) {
			if (StringUtil::CIEquals(col, "d")) {
				plan.order_column = col;
			} else if (StringUtil::CIEquals(col, "x")) {
				arg_column = col;
			}
		}
		for (auto &col : visible_column_names) {
			bool base_col = StringUtil::CIEquals(col, "id") || StringUtil::CIEquals(col, plan.partition_column) ||
			                StringUtil::CIEquals(col, plan.order_column) || StringUtil::CIEquals(col, arg_column);
			if (base_col) {
				plan.passthrough_columns.push_back(std::make_pair(col, col));
			} else {
				RunningWindowExpr expr;
				expr.function_name = StringUtil::Lower(quick_func[1].str());
				expr.argument = expr.function_name == "count" ? "*" : arg_column;
				expr.output_column = col;
				plan.window_exprs.push_back(expr);
			}
		}
		plan.output_columns = visible_column_names;
	}
	if (plan.window_exprs.empty() && !TryParseRunningWindowPlan(view_query_sql, partition_columns, visible_column_names, plan)) {
		plan = RunningWindowPlan();
		if (!TryParseLptsRunningWindowPlan(view_query_sql, partition_columns, visible_column_names, plan)) {
			string lower_sql = LowerCopy(view_query_sql);
			std::smatch func_match;
			static const std::regex func_regex(R"((sum|min|max|count|avg)\s*\()", std::regex_constants::icase);
			if (!std::regex_search(view_query_sql, func_match, func_regex)) {
				return "";
			}
			plan.partition_column = partition_columns.empty() ? "" : PartitionOutputColumn(partition_columns[0]);
			plan.order_column = "";
			string arg_column;
			for (auto &col : visible_column_names) {
				if (StringUtil::CIEquals(col, "d")) {
					plan.order_column = col;
				} else if (StringUtil::CIEquals(col, "x")) {
					arg_column = col;
				}
			}
			if (plan.partition_column.empty() || plan.order_column.empty() ||
			    (StringUtil::Lower(func_match[1].str()) != "count" && arg_column.empty())) {
				return "";
			}
			vector<string> base_cols;
			for (auto &col : visible_column_names) {
				if (StringUtil::CIEquals(col, plan.partition_column) || StringUtil::CIEquals(col, plan.order_column) ||
				    StringUtil::CIEquals(col, arg_column) || StringUtil::CIEquals(col, "id")) {
					plan.passthrough_columns.push_back(std::make_pair(col, col));
					base_cols.push_back(col);
				}
			}
			for (auto &col : visible_column_names) {
				bool is_base = false;
				for (auto &base_col : base_cols) {
					is_base = is_base || StringUtil::CIEquals(col, base_col);
				}
				if (!is_base) {
					RunningWindowExpr expr;
					expr.function_name = StringUtil::Lower(func_match[1].str());
					expr.argument = expr.function_name == "count" ? "*" : arg_column;
					expr.output_column = col;
					plan.window_exprs.push_back(expr);
				}
			}
			plan.output_columns = visible_column_names;
			if (plan.window_exprs.empty()) {
				return "";
			}
		}
	}
	const auto &spec = partition_delta_specs[0];
	if (!StringUtil::CIEquals(spec.output_column, plan.partition_column)) {
		return "";
	}
	string delta_q = spec.delta_table_sql.empty() ? SqlUtils::QuoteIdentifier(spec.delta_table) : spec.delta_table_sql;
	string data_table = catalog_prefix + SqlUtils::QuoteIdentifier(IncrementalTableNames::DataTableName(view_name));
	string affected_table = SqlUtils::QuoteIdentifier("openivm_run_affected_" + view_name);
	string bounds_table = SqlUtils::QuoteIdentifier("openivm_run_bounds_" + view_name);
	string fast_table = SqlUtils::QuoteIdentifier("openivm_run_fast_" + view_name);
	string fallback_table = SqlUtils::QuoteIdentifier("openivm_run_fallback_" + view_name);
	string state_table = SqlUtils::QuoteIdentifier("openivm_run_state_" + view_name);
	string delta_filter = delta_ts_filter.empty() ? "" : " AND " + delta_ts_filter;
	string delta_positive = QualifiedColumn("d", openivm::MULTIPLICITY_COL) + " > 0" + delta_filter;
	string part_q = SqlUtils::QuoteIdentifier(plan.partition_column);
	string order_q = SqlUtils::QuoteIdentifier(plan.order_column);
	string key_match_df = SqlUtils::BuildNullSafeMatch(vector<string> {plan.partition_column}, "d", "fk");
	string key_match_dt_fk = SqlUtils::BuildNullSafeMatch(vector<string> {plan.partition_column}, "dt", "fk");
	string key_match_b_m = "b." + part_q + " IS NOT DISTINCT FROM m." + part_q;
	string key_match_d_fk = SqlUtils::BuildNullSafeMatch(vector<string> {plan.partition_column}, "d", "fk");

	string sql;
	sql += "CREATE OR REPLACE TEMP TABLE " + affected_table + " AS\nSELECT DISTINCT " +
	       QualifiedColumn("d", plan.partition_column) + " AS " + part_q + "\nFROM " + delta_q + " d\nWHERE " +
	       delta_positive + ";\n\n";
	sql += "CREATE OR REPLACE TEMP TABLE " + bounds_table + " AS\nWITH old_max AS (\n  SELECT " + part_q +
	       ", MAX(" + order_q + ") AS openivm_old_max_order FROM " + data_table + " GROUP BY " + part_q +
	       "\n), delta_min AS (\n  SELECT " + QualifiedColumn("d", plan.partition_column) + " AS " + part_q +
	       ", MIN(" + QualifiedColumn("d", plan.order_column) + ") AS openivm_delta_min_order\n  FROM " + delta_q +
	       " d\n  WHERE " + delta_positive + "\n  GROUP BY " + QualifiedColumn("d", plan.partition_column) +
	       "\n)\nSELECT a." + part_q +
	       ", m.openivm_old_max_order, b.openivm_delta_min_order\nFROM " + affected_table +
	       " a\nLEFT JOIN old_max m ON a." + part_q + " IS NOT DISTINCT FROM m." + part_q +
	       "\nJOIN delta_min b ON a." + part_q + " IS NOT DISTINCT FROM b." + part_q + ";\n\n";
	sql += "CREATE OR REPLACE TEMP TABLE " + fast_table + " AS\nSELECT " + part_q + " FROM " + bounds_table +
	       "\nWHERE openivm_old_max_order IS NULL OR openivm_delta_min_order > openivm_old_max_order;\n\n";
	sql += "CREATE OR REPLACE TEMP TABLE " + fallback_table + " AS\nSELECT " + part_q + " FROM " + bounds_table +
	       "\nWHERE openivm_old_max_order IS NOT NULL AND openivm_delta_min_order <= openivm_old_max_order;\n\n";
	sql += "CREATE OR REPLACE TEMP TABLE " + state_table + " AS\nSELECT * EXCLUDE (openivm_rn) FROM (\n  SELECT dt.*, "
	       "COUNT(*) OVER (PARTITION BY " +
	       QualifiedColumn("dt", plan.partition_column) + ") AS openivm_prior_count, ROW_NUMBER() OVER (PARTITION BY " +
	       QualifiedColumn("dt", plan.partition_column) + " ORDER BY " + QualifiedColumn("dt", plan.order_column) +
	       " DESC) AS openivm_rn\n  FROM " + data_table + " dt\n  JOIN " + fast_table + " fk ON " + key_match_dt_fk +
	       "\n) openivm_state_ranked\nWHERE openivm_rn = 1;\n\n";
	string fallback_filter = part_q + " IN (SELECT " + part_q + " FROM " + fallback_table + ")";
	sql += BuildDeleteInsertRefreshSQL(data_table, view_query_sql, "openivm_recompute", fallback_filter,
	                                   fallback_filter);

	auto emit_column_names = plan.output_columns.empty() ? visible_column_names : plan.output_columns;
	string insert_cols = SqlUtils::JoinQuotedColumns(emit_column_names);
	string select_list;
	for (idx_t i = 0; i < emit_column_names.size(); i++) {
		if (i > 0) {
			select_list += ", ";
		}
		string expr_sql;
		for (auto &pass : plan.passthrough_columns) {
			if (StringUtil::CIEquals(emit_column_names[i], pass.second)) {
				expr_sql = QualifiedColumn("d", pass.first);
				break;
			}
		}
		for (auto &expr : plan.window_exprs) {
			if (StringUtil::CIEquals(emit_column_names[i], expr.output_column)) {
				expr_sql = RunningAdjustedExpr(expr, plan);
				break;
			}
		}
		if (expr_sql.empty()) {
			return "";
		}
		select_list += expr_sql + " AS " + SqlUtils::QuoteIdentifier(emit_column_names[i]);
	}
	string state_match = SqlUtils::BuildNullSafeMatch(vector<string> {plan.partition_column}, "d", "s");
	sql += "INSERT INTO " + data_table + " (" + insert_cols + ")\nSELECT " + select_list + "\nFROM " + delta_q +
	       " d\nJOIN " + fast_table + " fk ON " + key_match_d_fk + "\nLEFT JOIN " + state_table + " s ON " +
	       state_match + "\nWHERE " + delta_positive + ";\n\n";
	sql += "DROP TABLE IF EXISTS " + state_table + ";\n";
	sql += "DROP TABLE IF EXISTS " + fallback_table + ";\n";
	sql += "DROP TABLE IF EXISTS " + fast_table + ";\n";
	sql += "DROP TABLE IF EXISTS " + bounds_table + ";\n";
	sql += "DROP TABLE IF EXISTS " + affected_table + ";\n";
	OPENIVM_DEBUG_PRINT("[CompileWindowSuffixExtend] view=%s partition=%s order=%s window_exprs=%zu\n",
	                    view_name.c_str(), plan.partition_column.c_str(), plan.order_column.c_str(),
	                    plan.window_exprs.size());
	(void)key_match_df;
	(void)key_match_b_m;
	return sql;
}

	static string CreateAuxTablePrefix(const string &target_table, bool replace) {
	return string(replace ? "CREATE OR REPLACE TABLE " : "CREATE TABLE IF NOT EXISTS ") + target_table;
}

static string SourceExprForColumn(const vector<string> &cols, const vector<string> &source_exprs, idx_t column_idx,
                                  const string &fallback_alias = string()) {
	if (column_idx < source_exprs.size() && !source_exprs[column_idx].empty()) {
		return source_exprs[column_idx];
	}
	string col = SqlUtils::QuoteIdentifier(cols[column_idx]);
	if (!fallback_alias.empty()) {
		return fallback_alias + "." + col;
	}
	return cols[column_idx];
}

static void BuildAliasedSourceLists(const vector<string> &cols, const vector<string> &source_exprs, string &select_list,
                                    string &group_list, const string &fallback_alias = string()) {
	select_list.clear();
	group_list.clear();
	for (idx_t i = 0; i < cols.size(); i++) {
		if (i > 0) {
			select_list += ", ";
			group_list += ", ";
		}
		string expr = SourceExprForColumn(cols, source_exprs, i, fallback_alias);
		select_list += expr + " AS " + SqlUtils::QuoteIdentifier(cols[i]);
		group_list += expr;
	}
}

} // namespace

string BuildDistinctAuxStateCreateSQL(const string &target_table, const vector<string> &distinct_cols,
                                      const vector<string> &source_exprs, const string &source_relation,
                                      const string &filter_sql, bool replace) {
	string select_list;
	string group_list;
	BuildAliasedSourceLists(distinct_cols, source_exprs, select_list, group_list);
	string filter = filter_sql.empty() ? "" : " WHERE " + filter_sql;
	return CreateAuxTablePrefix(target_table, replace) + " AS SELECT " + select_list +
	       ", count(*)::BIGINT AS _count FROM " + source_relation + filter + " GROUP BY " + group_list;
}

string CompileDistinctIncremental(const string &view_name, const string &aux_table, const vector<string> &distinct_cols,
                                  const vector<string> &source_exprs, const string &delta_source,
                                  const string &last_update, const string &filter_sql,
                                  const vector<string> &group_columns, const string &sum_arg, const string &sum_out,
                                  const string &count_star_col, const string &catalog_prefix) {
	if (distinct_cols.empty() || group_columns.empty() || sum_arg.empty() || sum_out.empty()) {
		throw InternalException("CompileDistinctIncremental called with incomplete metadata for view '%s'", view_name);
	}
	string data_table = catalog_prefix + SqlUtils::QuoteIdentifier(IncrementalTableNames::DataTableName(view_name));
	string aux_q = catalog_prefix + SqlUtils::QuoteIdentifier(aux_table);
	string delta_q = DeltaSourceRef(delta_source, catalog_prefix);
	string sum_arg_q = SqlUtils::QuoteIdentifier(sum_arg);
	string sum_out_q = SqlUtils::QuoteIdentifier(sum_out);
	string count_q = SqlUtils::QuoteIdentifier(count_star_col);

	string distinct_cols_csv = SqlUtils::JoinQuotedColumns(distinct_cols);
	string distinct_cols_csv_i = SqlUtils::JoinQualifiedQuotedColumns(distinct_cols, "i");
	string group_cols_csv = SqlUtils::JoinQuotedColumns(group_columns);
	string mv_match = SqlUtils::BuildNullSafeMatch(group_columns, "v", "d");
	string source_select;
	string source_group;
	BuildAliasedSourceLists(distinct_cols, source_exprs, source_select, source_group);

	string dinput_table = "openivm_dinput_" + view_name;
	string ts_filter =
	    " WHERE " + string(openivm::TIMESTAMP_COL) + " >= '" + SqlUtils::EscapeValue(last_update) + "'::TIMESTAMP";
	string filter_clause = filter_sql.empty() ? "" : " AND (" + filter_sql + ")";

	string sql;
	sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(dinput_table) + " AS\n  SELECT " +
	       source_select + ", SUM(" + string(openivm::MULTIPLICITY_COL) + ")::BIGINT AS dmult\n  FROM " + delta_q +
	       ts_filter + filter_clause + "\n  GROUP BY " + source_group + "\n  HAVING SUM(" +
	       string(openivm::MULTIPLICITY_COL) + ") <> 0;\n\n";

	string aux_match_aliased = SqlUtils::BuildNullSafeMatch(distinct_cols, "_aux", "i");
	string ddist_cte =
	    "WITH ddist AS (\n  SELECT " + distinct_cols_csv_i +
	    ", CASE WHEN COALESCE(_aux._count, 0) = 0 AND i.dmult > 0 THEN 1 "
	    "WHEN COALESCE(_aux._count, 0) > 0 AND COALESCE(_aux._count, 0) + i.dmult <= 0 THEN -1 ELSE 0 END AS dd\n"
	    "  FROM " +
	    SqlUtils::QuoteIdentifier(dinput_table) + " i LEFT JOIN " + aux_q + " _aux ON " + aux_match_aliased +
	    "\n),\ndagg AS (\n  SELECT " + group_cols_csv + ", SUM(" + sum_arg_q +
	    " * dd) AS d_sum, SUM(dd)::BIGINT AS d_count\n  FROM ddist WHERE dd <> 0\n  GROUP BY " + group_cols_csv +
	    "\n)\n";

	string insert_cols = group_cols_csv + ", " + sum_out_q + ", " + count_q;
	string insert_vals = SqlUtils::JoinQualifiedQuotedColumns(group_columns, "d") + ", d.d_sum, d.d_count";

	sql += ddist_cte + "MERGE INTO " + data_table + " v USING dagg d ON " + mv_match +
	       "\nWHEN MATCHED THEN UPDATE SET " + sum_out_q + " = COALESCE(v." + sum_out_q + ", 0) + d.d_sum, " + count_q +
	       " = v." + count_q + " + d.d_count\nWHEN NOT MATCHED THEN INSERT (" + insert_cols + ") VALUES (" +
	       insert_vals + ");\n\n";

	sql += "DELETE FROM " + data_table + " WHERE " + count_q + " <= 0;\n\n";

	sql += "MERGE INTO " + aux_q + " _aux USING " + SqlUtils::QuoteIdentifier(dinput_table) + " i ON " +
	       aux_match_aliased +
	       "\nWHEN MATCHED THEN UPDATE SET _count = _aux._count + i.dmult\nWHEN NOT MATCHED AND i.dmult > 0 "
	       "THEN INSERT (" +
	       distinct_cols_csv + ", _count) VALUES (" + distinct_cols_csv_i + ", i.dmult);\n\n";

	sql += "DELETE FROM " + aux_q + " WHERE _count <= 0;\n\n";
	sql += "DROP TABLE IF EXISTS " + SqlUtils::QuoteIdentifier(dinput_table) + ";\n";

	OPENIVM_DEBUG_PRINT("[CompileDistinctIncremental] %zu distinct cols, %zu group cols, sum %s(%s)→%s, aux=%s\n",
	                    distinct_cols.size(), group_columns.size(), "SUM", sum_arg.c_str(), sum_out.c_str(),
	                    aux_table.c_str());
	return sql;
}

string BuildSemiAntiAuxStateCreateSQL(const string &target_table, const string &left_source, const string &left_alias,
                                      const string &right_source, const string &right_alias, const string &predicate,
                                      const string &post_filter, const vector<string> &left_cols,
                                      const vector<string> &left_exprs, bool replace) {
	string left_cols_csv = SqlUtils::JoinQuotedColumns(left_cols);
	string left_cols_qualified = SqlUtils::JoinQualifiedQuotedColumns(left_cols, left_alias);
	string left_cols_lc = SqlUtils::JoinQualifiedQuotedColumns(left_cols, "lc");
	string left_cols_mc = SqlUtils::JoinQualifiedQuotedColumns(left_cols, "mc");
	string lc_mc_match = SqlUtils::BuildNullSafeMatch(left_cols, "lc", "mc");
	string left_source_select;
	string unused_group_list;
	BuildAliasedSourceLists(left_cols, left_exprs, left_source_select, unused_group_list, left_alias);
	string left_source_filter = post_filter.empty() ? "" : " WHERE " + post_filter;
	return CreateAuxTablePrefix(target_table, replace) + " AS WITH left_source AS (SELECT " + left_source_select +
	       " FROM " + left_source + " " + left_alias + left_source_filter + "), left_counts AS (SELECT " +
	       left_cols_csv + ", count(*)::BIGINT AS _left_count FROM left_source GROUP BY " + left_cols_csv +
	       "), match_counts AS (SELECT " + left_cols_qualified +
	       ", count(*)::BIGINT AS _match_count FROM (SELECT DISTINCT " + left_cols_csv + " FROM left_source) " +
	       left_alias + " JOIN " + right_source + " " + right_alias + " ON " + predicate + " GROUP BY " +
	       left_cols_qualified + ") SELECT " + left_cols_lc +
	       ", lc._left_count, coalesce(mc._match_count, 0)::BIGINT AS _match_count FROM left_counts lc LEFT JOIN "
	       "match_counts mc ON " +
	       lc_mc_match;
}

string CompileSemiAntiRecompute(const string &view_name, const string &aux_table, const string &join_type,
                                const string &left_table, const string &left_alias, const string &right_table,
                                const string &right_alias, const string &predicate, const string &post_filter,
                                const vector<string> &left_cols, const vector<string> &left_exprs,
                                const vector<string> &output_cols, const string &left_delta_source,
                                const string &right_delta_source, const string &left_last_update,
                                const string &right_last_update, const string &catalog_prefix) {
	if (left_cols.empty() || output_cols.empty() || aux_table.empty() || right_delta_source.empty() ||
	    right_last_update.empty()) {
		throw InternalException("CompileSemiAntiRecompute called with incomplete metadata for view '%s'", view_name);
	}

	string data_table = catalog_prefix + SqlUtils::QuoteIdentifier(IncrementalTableNames::DataTableName(view_name));
	string aux_q = catalog_prefix + SqlUtils::QuoteIdentifier(aux_table);
	bool has_left_delta = !left_delta_source.empty() && !left_last_update.empty();
	string left_delta_q = DeltaSourceRef(left_delta_source, catalog_prefix);
	string right_delta_q = DeltaSourceRef(right_delta_source, catalog_prefix);
	string dleft_table = "openivm_saj_dleft_" + view_name;
	string dright_table = "openivm_saj_dright_" + view_name;
	string old_table = "openivm_saj_old_" + view_name;
	string aff_table = "openivm_saj_aff_" + view_name;
	bool is_anti = StringUtil::Lower(join_type) == "anti";
	string visible = is_anti ? "_match_count = 0" : "_match_count > 0";
	string cur_visible = is_anti ? "_cur._match_count = 0" : "_cur._match_count > 0";
	string left_delta_filter = post_filter.empty() ? "" : " AND (" + post_filter + ")";

	string left_cols_csv = SqlUtils::JoinQuotedColumns(left_cols);
	string output_cols_csv = SqlUtils::JoinQuotedColumns(output_cols);
	string left_cols_i = SqlUtils::JoinQualifiedQuotedColumns(left_cols, "i");
	string left_cols_l = SqlUtils::JoinQualifiedQuotedColumns(left_cols, left_alias);
	string left_cols_old = SqlUtils::JoinQualifiedQuotedColumns(left_cols, "_old");
	string left_cols_cur = SqlUtils::JoinQualifiedQuotedColumns(left_cols, "_cur");
	string output_old = SqlUtils::JoinQualifiedQuotedColumns(output_cols, "_old");
	string output_cur = SqlUtils::JoinQualifiedQuotedColumns(output_cols, "_cur");

	string aux_i_match = SqlUtils::BuildNullSafeMatch(left_cols, "_aux", "i");
	string old_cur_match = SqlUtils::BuildNullSafeMatch(left_cols, "_old", "_cur");
	string aff_old_match = SqlUtils::BuildNullSafeMatch(left_cols, "_aff", "_old");
	string aff_cur_match = SqlUtils::BuildNullSafeMatch(left_cols, "_aff", "_cur");
	string data_match = SqlUtils::BuildNullSafeMatch(output_cols, "_v", "_d");
	string left_delta_select;
	string left_delta_group;
	BuildAliasedSourceLists(left_cols, left_exprs, left_delta_select, left_delta_group, left_alias);

	string left_ts =
	    string(openivm::TIMESTAMP_COL) + " >= '" + SqlUtils::EscapeValue(left_last_update) + "'::TIMESTAMP";
	string right_ts = right_alias + "." + string(openivm::TIMESTAMP_COL) + " >= '" +
	                  SqlUtils::EscapeValue(right_last_update) + "'::TIMESTAMP";

	string sql;
	sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(old_table) + " AS SELECT *, (" + visible +
	       ") AS _visible FROM " + aux_q + ";\n\n";

	if (has_left_delta) {
		sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(dleft_table) + " AS\n  SELECT " +
		       left_delta_select + ", SUM(" + left_alias + "." + string(openivm::MULTIPLICITY_COL) +
		       ")::BIGINT AS dmult\n  FROM " + left_delta_q + " " + left_alias + "\n  WHERE " + left_alias + "." +
		       left_ts + left_delta_filter + "\n  GROUP BY " + left_delta_group + "\n  HAVING SUM(" + left_alias + "." +
		       string(openivm::MULTIPLICITY_COL) + ") <> 0;\n\n";
	} else {
		sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(dleft_table) + " AS\n  SELECT " +
		       left_cols_csv + ", 0::BIGINT AS dmult FROM " + aux_q + " WHERE false;\n\n";
	}

	sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(dright_table) + " AS\n  SELECT " + left_cols_l +
	       ", SUM(" + right_alias + "." + string(openivm::MULTIPLICITY_COL) + ")::BIGINT AS dmatch\n  FROM " + aux_q +
	       " " + left_alias + " JOIN " + right_delta_q + " " + right_alias + " ON " + predicate + "\n  WHERE " +
	       right_ts + "\n  GROUP BY " + left_cols_l + "\n  HAVING SUM(" + right_alias + "." +
	       string(openivm::MULTIPLICITY_COL) + ") <> 0;\n\n";

	sql += "MERGE INTO " + aux_q + " _aux USING " + SqlUtils::QuoteIdentifier(dright_table) + " _d ON " +
	       SqlUtils::BuildNullSafeMatch(left_cols, "_aux", "_d") +
	       "\nWHEN MATCHED THEN UPDATE SET _match_count = _aux._match_count + _d.dmatch;\n\n";

	sql += "MERGE INTO " + aux_q + " _aux USING " + SqlUtils::QuoteIdentifier(dleft_table) + " i ON " + aux_i_match +
	       "\nWHEN MATCHED THEN UPDATE SET _left_count = _aux._left_count + i.dmult;\n\n";

	sql += "INSERT INTO " + aux_q + " (" + left_cols_csv + ", _left_count, _match_count)\nSELECT " + left_cols_i +
	       ", i.dmult, COALESCE(mc._match_count, 0)::BIGINT\nFROM " + SqlUtils::QuoteIdentifier(dleft_table) +
	       " i\nLEFT JOIN " + aux_q + " _aux ON " + aux_i_match + "\nLEFT JOIN (\n  SELECT " + left_cols_l +
	       ", COUNT(*)::BIGINT AS _match_count\n  FROM " + SqlUtils::QuoteIdentifier(dleft_table) + " " + left_alias +
	       " JOIN " + right_table + " " + right_alias + " ON " + predicate + "\n  GROUP BY " + left_cols_l +
	       "\n) mc ON " + SqlUtils::BuildNullSafeMatch(left_cols, "mc", "i") +
	       "\nWHERE _aux._left_count IS NULL AND i.dmult > 0;\n\n";

	sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(aff_table) + " AS\nSELECT " + left_cols_old +
	       " FROM " + SqlUtils::QuoteIdentifier(old_table) + " _old LEFT JOIN " + aux_q + " _cur ON " + old_cur_match +
	       "\nWHERE _cur._left_count IS NULL OR _old._left_count IS DISTINCT FROM _cur._left_count OR "
	       "_old._visible IS DISTINCT FROM (" +
	       cur_visible + ")\nUNION\nSELECT " + left_cols_cur + " FROM " + aux_q + " _cur LEFT JOIN " +
	       SqlUtils::QuoteIdentifier(old_table) + " _old ON " + old_cur_match +
	       "\nWHERE _old._left_count IS NULL OR _old._left_count IS DISTINCT FROM _cur._left_count OR "
	       "_old._visible IS DISTINCT FROM (" +
	       cur_visible + ");\n\n";

	sql += "WITH _old_rows AS (\n  SELECT " + output_old + " FROM " + SqlUtils::QuoteIdentifier(old_table) +
	       " _old JOIN " + SqlUtils::QuoteIdentifier(aff_table) + " _aff ON " + aff_old_match +
	       ", generate_series(1, _old._left_count::BIGINT)\n  WHERE _old._visible AND _old._left_count > 0" +
	       "\n), "
	       "_net AS (\n  SELECT " +
	       output_cols_csv + ", COUNT(*)::BIGINT AS _cnt FROM _old_rows GROUP BY " + output_cols_csv +
	       "\n)\nDELETE FROM " + data_table + " WHERE rowid IN (\n  SELECT _v.rowid FROM (\n    SELECT rowid, " +
	       output_cols_csv + ", ROW_NUMBER() OVER (PARTITION BY " + output_cols_csv + " ORDER BY rowid) AS _rn FROM " +
	       data_table + "\n  ) _v JOIN _net _d ON " + data_match + " WHERE _v._rn <= _d._cnt\n);\n\n";

	sql += "INSERT INTO " + data_table + " SELECT " + output_cur + "\nFROM " + aux_q + " _cur JOIN " +
	       SqlUtils::QuoteIdentifier(aff_table) + " _aff ON " + aff_cur_match +
	       ", generate_series(1, _cur._left_count::BIGINT)\nWHERE " + cur_visible + " AND _cur._left_count > 0;\n\n";

	sql += "DELETE FROM " + aux_q + " WHERE _left_count <= 0;\n";
	sql += "DROP TABLE IF EXISTS " + SqlUtils::QuoteIdentifier(old_table) + ";\nDROP TABLE IF EXISTS " +
	       SqlUtils::QuoteIdentifier(dleft_table) + ";\nDROP TABLE IF EXISTS " +
	       SqlUtils::QuoteIdentifier(dright_table) + ";\nDROP TABLE IF EXISTS " + SqlUtils::QuoteIdentifier(aff_table) +
	       ";\n";

	OPENIVM_DEBUG_PRINT("[CompileSemiAntiRecompute] %s join, %zu left cols, aux=%s\n", join_type.c_str(),
	                    left_cols.size(), aux_table.c_str());
	return sql;
}

string BuildFilteredGroupCountAuxStateCreateSQL(const string &target_table, const string &source_table,
                                                const string &group_col, const string &sum_col,
                                                const string &source_group_expr, const string &source_sum_expr,
                                                bool replace) {
	string group_q = SqlUtils::QuoteIdentifier(group_col);
	string sum_q = SqlUtils::QuoteIdentifier(sum_col);
	string group_expr = source_group_expr.empty() ? group_q : source_group_expr;
	string sum_expr = source_sum_expr.empty() ? sum_q : source_sum_expr;
	return CreateAuxTablePrefix(target_table, replace) + " AS SELECT " + group_expr + " AS " + group_q + ", sum(" +
	       sum_expr + ") AS openivm_sum FROM " + source_table + " GROUP BY " + group_expr;
}

string CompileFilteredGroupCount(const string &view_name, const string &aux_table, const string &delta_source,
                                 const string &last_update, const string &group_col, const string &sum_col,
                                 const string &source_group_expr, const string &source_sum_expr,
                                 const string &output_col, const string &comparison_op, const string &threshold_sql,
                                 const string &catalog_prefix) {
	if (aux_table.empty() || delta_source.empty() || last_update.empty() || group_col.empty() || sum_col.empty() ||
	    output_col.empty() || comparison_op.empty() || threshold_sql.empty()) {
		throw InternalException("CompileFilteredGroupCount called with incomplete metadata for view '%s'", view_name);
	}

	string data_table = catalog_prefix + SqlUtils::QuoteIdentifier(IncrementalTableNames::DataTableName(view_name));
	string aux_q = catalog_prefix + SqlUtils::QuoteIdentifier(aux_table);
	string delta_q = DeltaSourceRef(delta_source, catalog_prefix);
	string dsum_table = "openivm_fgc_delta_" + view_name;
	string group_q = SqlUtils::QuoteIdentifier(group_col);
	string sum_q = SqlUtils::QuoteIdentifier(sum_col);
	string source_group = source_group_expr.empty() ? group_q : source_group_expr;
	string source_sum = source_sum_expr.empty() ? sum_q : source_sum_expr;
	string output_q = SqlUtils::QuoteIdentifier(output_col);
	string dsum_expr = "SUM(" + string(openivm::MULTIPLICITY_COL) + " * " + source_sum + ")";
	string old_sum = "COALESCE(_aux.openivm_sum, 0)";
	string new_sum = "(" + old_sum + " + d.openivm_delta_sum)";
	string old_visible = "CASE WHEN " + old_sum + " " + comparison_op + " " + threshold_sql + " THEN 1 ELSE 0 END";
	string new_visible = "CASE WHEN " + new_sum + " " + comparison_op + " " + threshold_sql + " THEN 1 ELSE 0 END";
	string aux_match = SqlUtils::BuildNullSafeMatch(vector<string> {group_col}, "_aux", "d");

	string sql;
	sql += "CREATE OR REPLACE TEMP TABLE " + SqlUtils::QuoteIdentifier(dsum_table) + " AS\n  SELECT " + source_group +
	       " AS " + group_q + ", " + dsum_expr + " AS openivm_delta_sum\n  FROM " + delta_q + "\n  WHERE " +
	       string(openivm::TIMESTAMP_COL) + " >= '" + SqlUtils::EscapeValue(last_update) + "'::TIMESTAMP\n  GROUP BY " +
	       source_group + "\n  HAVING " + dsum_expr + " <> 0;\n\n";

	sql += "WITH openivm_transition AS (\n  SELECT SUM((" + new_visible + ") - (" + old_visible +
	       ")) AS openivm_delta_count\n  FROM " + SqlUtils::QuoteIdentifier(dsum_table) + " d LEFT JOIN " + aux_q +
	       " _aux ON " + aux_match + "\n)\nUPDATE " + data_table + " SET " + output_q + " = COALESCE(" + output_q +
	       ", 0) + COALESCE((SELECT openivm_delta_count FROM openivm_transition), 0);\n\n";

	sql += "MERGE INTO " + aux_q + " _aux USING " + SqlUtils::QuoteIdentifier(dsum_table) + " d ON " + aux_match +
	       "\nWHEN MATCHED THEN UPDATE SET openivm_sum = COALESCE(_aux.openivm_sum, 0) + d.openivm_delta_sum\n"
	       "WHEN NOT MATCHED THEN INSERT (" +
	       group_q + ", openivm_sum) VALUES (d." + group_q + ", d.openivm_delta_sum);\n\n";

	sql += "DELETE FROM " + aux_q + " WHERE openivm_sum = 0;\n";
	sql += "DROP TABLE IF EXISTS " + SqlUtils::QuoteIdentifier(dsum_table) + ";\n";

	OPENIVM_DEBUG_PRINT("[CompileFilteredGroupCount] group=%s, sum=%s, op=%s, aux=%s\n", group_col.c_str(),
	                    sum_col.c_str(), comparison_op.c_str(), aux_table.c_str());
	return sql;
}

string CompileWindowRecompute(const string &view_name, const string &view_query_sql, const string &delta_ts_filter,
                              const string &catalog_prefix, const vector<string> &partition_columns,
                              const vector<WindowPartitionDeltaSpec> &partition_delta_specs, bool emit_cascade_delta,
                              const string &affected_keys_sql, const string &affected_key_cols,
                              const string &affected_key_tuple, const vector<string> &column_names,
                              bool running_window_incremental) {
	bool have_affected_keys = !affected_keys_sql.empty() && !affected_key_cols.empty() && !affected_key_tuple.empty();
	if (!have_affected_keys && (partition_columns.empty() || partition_delta_specs.empty())) {
		return CompileFullRecompute(view_name, view_query_sql, catalog_prefix);
	}
	if (running_window_incremental && !emit_cascade_delta) {
		auto suffix_sql = BuildRunningWindowSuffixRefreshSQL(view_name, view_query_sql, delta_ts_filter, catalog_prefix,
		                                                     partition_columns, partition_delta_specs, column_names);
		if (!suffix_sql.empty()) {
			return suffix_sql;
		}
	}
	string data_table = catalog_prefix + SqlUtils::QuoteIdentifier(IncrementalTableNames::DataTableName(view_name));
	string delta_where = delta_ts_filter.empty() ? "" : " WHERE " + delta_ts_filter;
	string affected_temp_table = SqlUtils::QuoteIdentifier("openivm_affected_" + view_name);

	string affected_filter;
	if (have_affected_keys) {
		affected_filter =
		    affected_key_tuple + " IN (SELECT " + affected_key_cols + " FROM " + affected_temp_table + ")";
	} else {
		for (size_t i = 0; i < partition_delta_specs.size(); i++) {
			if (i > 0) {
				affected_filter += " OR ";
			}
			const auto &spec = partition_delta_specs[i];
			string output_col = SqlUtils::QuoteIdentifier(spec.output_column);
			string source_col = SqlUtils::QuoteIdentifier(spec.source_column);
			string delta_table =
			    spec.delta_table_sql.empty() ? SqlUtils::QuoteIdentifier(spec.delta_table) : spec.delta_table_sql;
			affected_filter +=
			    output_col + " IN (SELECT DISTINCT " + source_col + " FROM " + delta_table + delta_where + ")";
		}
	}

	OPENIVM_DEBUG_PRINT(
	    "[CompileWindowRecompute] Partition columns: %zu, delta specs: %zu, lineage keys: %s, cascade delta: %s\n",
	    partition_columns.size(), partition_delta_specs.size(), have_affected_keys ? "yes" : "no",
	    emit_cascade_delta ? "enabled" : "disabled");
	if (!emit_cascade_delta) {
		if (!have_affected_keys) {
			return BuildDeleteInsertRefreshSQL(data_table, view_query_sql, "openivm_recompute", affected_filter,
			                                   affected_filter);
		}
		string sql;
		sql += "CREATE OR REPLACE TEMP TABLE " + affected_temp_table + " AS\n" + affected_keys_sql + ";\n\n";
		sql += BuildDeleteInsertRefreshSQL(data_table, view_query_sql, "openivm_recompute", affected_filter,
		                                   affected_filter);
		sql += "DROP TABLE IF EXISTS " + affected_temp_table + ";\n";
		return sql;
	}

	string delta_table = catalog_prefix + SqlUtils::QuoteIdentifier(SqlUtils::DeltaName(view_name));
	string old_temp_table = SqlUtils::QuoteIdentifier(string(openivm::TEMP_TABLE_PREFIX) + view_name);
	string new_temp_table = SqlUtils::QuoteIdentifier(string("openivm_new_") + view_name);
	string sql;
	if (have_affected_keys) {
		sql += "CREATE OR REPLACE TEMP TABLE " + affected_temp_table + " AS\n" + affected_keys_sql + ";\n\n";
	}
	sql += "CREATE OR REPLACE TEMP TABLE " + old_temp_table + " AS\nSELECT * FROM " + data_table + "\nWHERE " +
	       affected_filter + ";\n\n";
	sql += "CREATE OR REPLACE TEMP TABLE " + new_temp_table + " AS\nSELECT * FROM (" + view_query_sql +
	       ") openivm_recompute\nWHERE " + affected_filter + ";\n\n";
	sql += "DELETE FROM " + data_table + " WHERE " + affected_filter + ";\n";
	sql += "INSERT INTO " + data_table + "\nSELECT * FROM " + new_temp_table + ";\n";
	sql += "\n" + BuildSignedMultisetDeltaInsertSQL(delta_table, old_temp_table, new_temp_table);
	if (have_affected_keys) {
		sql += "\nDROP TABLE IF EXISTS " + affected_temp_table + ";\n";
	}
	sql += "DROP TABLE IF EXISTS " + old_temp_table + ";\n";
	sql += "DROP TABLE IF EXISTS " + new_temp_table + ";\n";
	return sql;
}

} // namespace duckdb
