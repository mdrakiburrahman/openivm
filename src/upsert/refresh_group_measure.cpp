#include "upsert/refresh_internal.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/sql_utils.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace duckdb {

namespace {

struct DeltaColumnInfo {
	string name;
	string type;
};

static bool IsNumericTypeName(const string &type_name) {
	auto lower = StringUtil::Lower(type_name);
	return lower.find("int") != string::npos || lower.find("decimal") != string::npos ||
	       lower.find("float") != string::npos || lower.find("double") != string::npos ||
	       lower.find("real") != string::npos || lower.find("numeric") != string::npos;
}

static bool ValuesNotDistinct(const Value &left, const Value &right) {
	if (left.IsNull() || right.IsNull()) {
		return left.IsNull() && right.IsNull();
	}
	return left.ToString() == right.ToString();
}

static vector<DeltaColumnInfo> LoadVisibleDeltaColumns(Connection &con, const string &delta_table_sql) {
	vector<DeltaColumnInfo> columns;
	auto result = con.Query("DESCRIBE SELECT * FROM " + delta_table_sql);
	if (result->HasError()) {
		return columns;
	}
	for (idx_t i = 0; i < result->RowCount(); i++) {
		string name = result->GetValue(0, i).ToString();
		if (name == string(openivm::MULTIPLICITY_COL) || name == string(openivm::TIMESTAMP_COL)) {
			continue;
		}
		columns.push_back({name, result->GetValue(1, i).ToString()});
	}
	return columns;
}

static bool IsIdentifierChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool MatchesColumnIdentifier(const string &token, const string &column_name) {
	if (token == column_name) {
		return true;
	}
	if (token.size() <= column_name.size() + 2 || token[0] != 't') {
		return false;
	}
	idx_t pos = 1;
	while (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos]))) {
		pos++;
	}
	return pos > 1 && pos + 1 + column_name.size() == token.size() && token[pos] == '_' &&
	       token.substr(pos + 1) == column_name;
}

static bool ContainsColumnIdentifier(const string &text, const string &column_name) {
	if (column_name.empty()) {
		return false;
	}
	for (idx_t i = 0; i < text.size();) {
		if (!IsIdentifierChar(text[i])) {
			i++;
			continue;
		}
		idx_t start = i++;
		while (i < text.size() && IsIdentifierChar(text[i])) {
			i++;
		}
		if (MatchesColumnIdentifier(text.substr(start, i - start), column_name)) {
			return true;
		}
	}
	return false;
}

static bool ColumnLooksMeasureOnly(const string &view_query_sql, const string &column_name) {
	auto lower = StringUtil::Lower(view_query_sql);
	auto col = StringUtil::Lower(column_name);
	auto clause_contains_col = [&](const string &keyword, bool stop_at_paren) {
		size_t pos = 0;
		while ((pos = lower.find(keyword, pos)) != string::npos) {
			size_t end = stop_at_paren ? lower.find(')', pos) : lower.find('\n', pos);
			if (end == string::npos) {
				end = lower.size();
			}
			if (ContainsColumnIdentifier(lower.substr(pos, end - pos), col)) {
				return true;
			}
			pos += keyword.size();
		}
		return false;
	};
	for (auto &keyword :
	     {" where ", " on ", " group by ", " partition by ", " order by ", "count(distinct", "count (distinct", "min(",
	      "min (", "max(", "max (", "avg(", "avg (", "list(", "list (", "arg_min", "arg_max"}) {
		string kw(keyword);
		bool aggregate_function = kw.find('(') != string::npos || kw.find("arg_") != string::npos;
		if (clause_contains_col(keyword, aggregate_function)) {
			return false;
		}
	}
	bool saw_simple_sum = false;
	for (auto &keyword : {"sum(", "sum ("}) {
		string kw(keyword);
		size_t pos = 0;
		while ((pos = lower.find(kw, pos)) != string::npos) {
			size_t start = pos + kw.size();
			size_t end = lower.find(')', start);
			if (end == string::npos) {
				return false;
			}
			auto arg = lower.substr(start, end - start);
			if (ContainsColumnIdentifier(arg, col)) {
				if (arg.find('+') != string::npos || arg.find('-') != string::npos || arg.find('*') != string::npos ||
				    arg.find('/') != string::npos || arg.find("case") != string::npos ||
				    arg.find("coalesce") != string::npos) {
					return false;
				}
				saw_simple_sum = true;
			}
			pos = end + 1;
		}
	}
	return saw_simple_sum;
}

static vector<string> FindMeasureOutputColumns(const vector<string> &column_names,
                                               const vector<LogicalType> &column_types, const string &changed_column) {
	vector<string> outputs;
	auto changed_norm = NormalizeColumnNameForMatch(changed_column);
	if (changed_norm.empty()) {
		return outputs;
	}
	for (idx_t i = 0; i < column_names.size(); i++) {
		if (column_names[i] == "primary_key" || column_names[i] == string(openivm::MULTIPLICITY_COL) ||
		    column_names[i] == string(openivm::TIMESTAMP_COL) || i >= column_types.size() ||
		    !IsSummableLogicalType(column_types[i])) {
			continue;
		}
		auto output_norm = NormalizeColumnNameForMatch(column_names[i]);
		if (output_norm.find(changed_norm) != string::npos) {
			outputs.push_back(column_names[i]);
		}
	}
	return outputs;
}

static vector<string> SplitTopLevelCommaList(const string &text) {
	vector<string> items;
	idx_t start = 0;
	idx_t depth = 0;
	bool in_single_quote = false;
	for (idx_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == '\'') {
			if (in_single_quote && i + 1 < text.size() && text[i + 1] == '\'') {
				i++;
				continue;
			}
			in_single_quote = !in_single_quote;
			continue;
		}
		if (in_single_quote) {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		} else if (c == ',' && depth == 0) {
			auto item = text.substr(start, i - start);
			StringUtil::Trim(item);
			items.push_back(std::move(item));
			start = i + 1;
		}
	}
	auto item = text.substr(start);
	StringUtil::Trim(item);
	if (!item.empty()) {
		items.push_back(std::move(item));
	}
	return items;
}

static bool SelectItemHasAlias(const string &item, const string &alias) {
	auto lower = StringUtil::Lower(item);
	auto alias_lower = StringUtil::Lower(alias);
	auto suffix = " as " + alias_lower;
	if (lower.size() >= suffix.size() && lower.substr(lower.size() - suffix.size()) == suffix) {
		return true;
	}
	auto quoted_suffix = " as \"" + alias_lower + "\"";
	return lower.size() >= quoted_suffix.size() && lower.substr(lower.size() - quoted_suffix.size()) == quoted_suffix;
}

static size_t FindTopLevelFrom(const string &text, size_t start) {
	idx_t depth = 0;
	bool in_single_quote = false;
	for (idx_t i = start; i + 6 <= text.size(); i++) {
		char c = text[i];
		if (c == '\'') {
			if (in_single_quote && i + 1 < text.size() && text[i + 1] == '\'') {
				i++;
				continue;
			}
			in_single_quote = !in_single_quote;
			continue;
		}
		if (in_single_quote) {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		} else if (depth == 0 && text.substr(i, 6) == " FROM ") {
			return i;
		}
	}
	return string::npos;
}

static string SelectAliasExpression(const string &item, const string &alias) {
	if (!SelectItemHasAlias(item, alias)) {
		return "";
	}
	auto lower = StringUtil::Lower(item);
	auto pos = lower.rfind(" as ");
	if (pos == string::npos) {
		return "";
	}
	auto expr = item.substr(0, pos);
	StringUtil::Trim(expr);
	return expr;
}

static bool ParseCteLine(const string &line, string &cte_name, vector<string> &columns, vector<string> &select_items) {
	size_t name_end = line.find(" (");
	size_t cols_end = line.find(") AS (SELECT ", name_end == string::npos ? 0 : name_end);
	if (name_end == string::npos || cols_end == string::npos) {
		return false;
	}
	cte_name = line.substr(0, name_end);
	auto cols = line.substr(name_end + 2, cols_end - name_end - 2);
	columns = SplitTopLevelCommaList(cols);
	size_t select_start = cols_end + string(") AS (SELECT ").size();
	size_t from_pos = FindTopLevelFrom(line, select_start);
	if (from_pos == string::npos) {
		return false;
	}
	select_items = SplitTopLevelCommaList(line.substr(select_start, from_pos - select_start));
	return columns.size() == select_items.size();
}

static vector<string> ExtractAliasTokens(const string &expr) {
	vector<string> tokens;
	for (idx_t i = 0; i < expr.size();) {
		if (!(std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_')) {
			i++;
			continue;
		}
		idx_t start = i++;
		while (i < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_')) {
			i++;
		}
		auto token = expr.substr(start, i - start);
		if (token.size() > 2 && token[0] == 't' && std::isdigit(static_cast<unsigned char>(token[1])) &&
		    token.find('_') != string::npos) {
			tokens.push_back(std::move(token));
		}
	}
	std::sort(tokens.begin(), tokens.end());
	tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
	return tokens;
}

static bool ContainsAllTokens(const vector<string> &columns, const vector<string> &tokens) {
	unordered_set<string> column_set(columns.begin(), columns.end());
	for (auto &token : tokens) {
		if (!column_set.count(token)) {
			return false;
		}
	}
	return true;
}

static string ProjectFinalSelectToPrimaryKey(const string &query_sql, bool trace_primary_expr) {
	size_t select_pos = query_sql.rfind("\nSELECT ");
	if (select_pos == string::npos) {
		select_pos = query_sql.rfind("SELECT ");
	} else {
		select_pos++;
	}
	if (select_pos == string::npos) {
		return "";
	}
	size_t list_start = select_pos + string("SELECT ").size();
	size_t from_pos = query_sql.find(" FROM ", list_start);
	if (from_pos == string::npos) {
		return "";
	}
	auto select_list = query_sql.substr(list_start, from_pos - list_start);
	for (auto &item : SplitTopLevelCommaList(select_list)) {
		if (SelectItemHasAlias(item, "primary_key")) {
			if (trace_primary_expr) {
				auto source_expr = SelectAliasExpression(item, "primary_key");
				auto source_tokens = ExtractAliasTokens(source_expr);
				if (source_tokens.size() == 1) {
					string traced_expr;
					vector<string> traced_tokens;
					size_t line_start = 0;
					while (line_start < select_pos) {
						size_t line_end = query_sql.find('\n', line_start);
						if (line_end == string::npos || line_end > select_pos) {
							break;
						}
						auto line = query_sql.substr(line_start, line_end - line_start);
						string cte_name;
						vector<string> columns;
						vector<string> select_items;
						if (ParseCteLine(line, cte_name, columns, select_items)) {
							for (idx_t i = 0; i < columns.size(); i++) {
								if (columns[i] == source_tokens[0]) {
									traced_expr = select_items[i] + " AS primary_key";
									traced_tokens = ExtractAliasTokens(traced_expr);
									break;
								}
							}
						}
						if (!traced_expr.empty()) {
							break;
						}
						line_start = line_end + 1;
					}
					if (!traced_expr.empty() && !traced_tokens.empty()) {
						line_start = 0;
						while (line_start < select_pos) {
							size_t line_end = query_sql.find('\n', line_start);
							if (line_end == string::npos || line_end > select_pos) {
								break;
							}
							auto line = query_sql.substr(line_start, line_end - line_start);
							string cte_name;
							vector<string> columns;
							vector<string> select_items;
							if (ParseCteLine(line, cte_name, columns, select_items) &&
							    ContainsAllTokens(columns, traced_tokens)) {
								size_t prefix_end = line_end;
								while (prefix_end > 0 &&
								       std::isspace(static_cast<unsigned char>(query_sql[prefix_end - 1]))) {
									prefix_end--;
								}
								if (prefix_end > 0 && query_sql[prefix_end - 1] == ',') {
									prefix_end--;
								}
								return query_sql.substr(0, prefix_end) + "\nSELECT " + traced_expr + " FROM " +
								       cte_name;
							}
							line_start = line_end + 1;
						}
					}
				}
			}
			return query_sql.substr(0, select_pos) + "SELECT " + item + query_sql.substr(from_pos);
		}
	}
	return "";
}

static bool TryDetectSingleMeasureUpdate(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                         const string &delta_table_name, const string &delta_table_sql,
                                         const vector<DeltaColumnInfo> &columns, string &changed_column) {
	auto last_update = metadata.GetLastUpdate(view_name, delta_table_name);
	if (last_update.empty() || columns.empty()) {
		return false;
	}
	string select_columns;
	for (auto &column : columns) {
		if (!select_columns.empty()) {
			select_columns += ", ";
		}
		select_columns += SqlUtils::QuoteIdentifier(column.name);
	}
	string delta_sql = "SELECT " + select_columns + ", " + string(openivm::MULTIPLICITY_COL) + " FROM " +
	                   delta_table_sql + " WHERE " + string(openivm::TIMESTAMP_COL) + " >= '" +
	                   SqlUtils::EscapeValue(last_update) + "'::TIMESTAMP ORDER BY " +
	                   string(openivm::MULTIPLICITY_COL) + " LIMIT 3";
	auto rows = con.Query(delta_sql);
	if (rows->HasError() || rows->RowCount() != 2) {
		return false;
	}
	auto neg_mult = rows->GetValue(columns.size(), 0).GetValue<int32_t>();
	auto pos_mult = rows->GetValue(columns.size(), 1).GetValue<int32_t>();
	if (neg_mult != -1 || pos_mult != 1) {
		return false;
	}

	idx_t changed_idx = columns.size();
	for (idx_t i = 0; i < columns.size(); i++) {
		if (ValuesNotDistinct(rows->GetValue(i, 0), rows->GetValue(i, 1))) {
			continue;
		}
		if (changed_idx != columns.size()) {
			return false;
		}
		changed_idx = i;
	}
	if (changed_idx == columns.size() || !IsNumericTypeName(columns[changed_idx].type)) {
		return false;
	}
	changed_column = columns[changed_idx].name;
	return true;
}

} // namespace

bool TryBuildGroupMeasureUpdateRefresh(RefreshMetadata &metadata, Connection &con, const string &view_name,
                                       const string &view_query_sql, const vector<string> &active_delta_table_names,
                                       const vector<string> &column_names, const vector<LogicalType> &column_types,
                                       const string &data_table, const string &view_catalog_name,
                                       const string &view_schema_name, string &upsert_query) {
	if (active_delta_table_names.size() != 1 ||
	    std::find(column_names.begin(), column_names.end(), "primary_key") == column_names.end()) {
		return false;
	}
	const auto &delta_table_name = active_delta_table_names[0];
	if (metadata.IsDuckLakeTable(view_name, delta_table_name)) {
		return false;
	}
	string delta_table_sql =
	    metadata.ResolveDeltaQualifiedName(view_name, delta_table_name, view_catalog_name, view_schema_name);
	auto columns = LoadVisibleDeltaColumns(con, delta_table_sql);
	string changed_column;
	if (!TryDetectSingleMeasureUpdate(metadata, con, view_name, delta_table_name, delta_table_sql, columns,
	                                  changed_column) ||
	    !ColumnLooksMeasureOnly(view_query_sql, changed_column)) {
		return false;
	}

	string base_table = BaseTableNameFromDeltaKey(delta_table_name);
	string last_update = metadata.GetLastUpdate(view_name, delta_table_name);
	auto measure_outputs = FindMeasureOutputColumns(column_names, column_types, changed_column);
	if (measure_outputs.empty()) {
		return false;
	}

	string current_replacement =
	    BuildStandardDeltaRowsSQL(delta_table_sql, last_update, string(openivm::MULTIPLICITY_COL) + " > 0");
	string current_query = SqlUtils::ReplaceTableReferences(view_query_sql, base_table, current_replacement);
	if (current_query == view_query_sql) {
		return false;
	}
	auto primary_key_query = ProjectFinalSelectToPrimaryKey(current_query, true);
	if (primary_key_query.empty()) {
		primary_key_query = current_query;
	}

	string affected_temp = SqlUtils::QuoteIdentifier(string(openivm::TEMP_TABLE_PREFIX) + "measure_" + view_name);
	string delta_temp = SqlUtils::QuoteIdentifier(string(openivm::TEMP_TABLE_PREFIX) + "measure_delta_" + view_name);
	string update_set;
	for (auto &column : measure_outputs) {
		if (!update_set.empty()) {
			update_set += ", ";
		}
		auto qcol = SqlUtils::QuoteIdentifier(column);
		update_set +=
		    qcol + " = COALESCE(openivm_tgt." + qcol + ", 0) + COALESCE(openivm_delta.openivm_measure_delta, 0)";
	}
	if (update_set.empty()) {
		return false;
	}

	upsert_query = "CREATE OR REPLACE TEMP TABLE " + affected_temp + " AS\nSELECT DISTINCT primary_key FROM (" +
	               primary_key_query + ") openivm_measure_current;\n\n";
	upsert_query += "CREATE OR REPLACE TEMP TABLE " + delta_temp + " AS\nSELECT SUM(" +
	                string(openivm::MULTIPLICITY_COL) + " * " + SqlUtils::QuoteIdentifier(changed_column) +
	                ") AS openivm_measure_delta FROM " + delta_table_sql + " WHERE " + string(openivm::TIMESTAMP_COL) +
	                " >= '" + SqlUtils::EscapeValue(last_update) + "'::TIMESTAMP;\n\n";
	upsert_query += "UPDATE " + data_table + " AS openivm_tgt\nSET " + update_set + "\nFROM " + affected_temp +
	                " AS openivm_aff, " + delta_temp +
	                " AS openivm_delta\nWHERE openivm_tgt.primary_key IS NOT DISTINCT FROM "
	                "openivm_aff.primary_key AND COALESCE(openivm_delta.openivm_measure_delta, 0) <> 0;\n\n";
	upsert_query += "DROP TABLE IF EXISTS " + affected_temp + ";\nDROP TABLE IF EXISTS " + delta_temp + ";\n";
	OPENIVM_DEBUG_PRINT("[UPSERT] GROUP_RECOMPUTE measure-update fast path on %s.%s\n", delta_table_name.c_str(),
	                    changed_column.c_str());
	return true;
}

} // namespace duckdb
