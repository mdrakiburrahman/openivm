#ifndef SQL_UTILS_HPP
#define SQL_UTILS_HPP

#include "duckdb.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "lpts_pipeline.hpp"

#include <string>
#include <unordered_set>

namespace duckdb {

// Utility functions for SQL string manipulation.
// Originally from the compiler extension; inlined here to remove the external dependency.

class BaseTableRef;
class SelectStatement;
class TableCatalogEntry;

struct SessionLocation {
	string catalog;
	string schema;
};

class SqlUtils {
public:
	// SQL equality can merge observably different values, including nested ones.
	static bool EqualityCanHideValueIdentity(const LogicalType &type);
	static string NegativeZeroSQL(const string &value);
	// DuckDB tests the sign bit directly; other dialects keep the portable string form.
	static string NegativeZeroSQL(const string &value, SqlDialect dialect);
	static string FloatingRepresentativeSQL(const string &value, const string &negative_count,
	                                        const string &total_count);
	static string ValueIdentitySQL(const string &value, const LogicalType &type, const string &default_collation = "");
	static EntryLookupInfo QualifyLookup(const EntryLookupInfo &lookup, const Identifier &catalog,
	                                     const Identifier &schema) {
		return EntryLookupInfo(lookup, QualifiedName(catalog, schema, lookup.GetEntryIdentifier()));
	}
	// Visit real relation references, excluding CTE names in their lexical scope.
	static void VisitSourceReferences(SQLStatement &statement, const std::function<void(BaseTableRef &)> &callback);
	static void RewriteSourceReferences(SQLStatement &statement,
	                                    const std::function<void(unique_ptr<TableRef> &)> &callback);
	static void WriteFile(const string &filename, bool append, const string &compiled_query);
	static string EscapeSingleQuotes(const string &input);
	// Null for a parse error, multiple statements, or a statement other than SELECT.
	static unique_ptr<SelectStatement> TryParseSelect(const string &sql);
	static vector<string> SplitSQLStatements(const string &sql);
	static string SQLStatementPreview(const string &statement);
	static string DeltaName(const string &name);
	static string LastIdentifierPart(string name);
	static vector<string> ParseQualifiedIdentifier(const string &name);
	// Read one identifier at start without scanning the statement body.
	static vector<string> ReadQualifiedIdentifier(const string &sql, idx_t start, idx_t &end);
	// Empty replacement keeps a reference unchanged; visit all references in one scan.
	static string RewriteTableReferences(const string &sql,
	                                     const std::function<string(const vector<string> &)> &rewrite);
	template <class CATALOG, class SCHEMA, class TABLE>
	static string FullName(const CATALOG &catalog, const SCHEMA &schema, const TABLE &table) {
		return QuoteIdentifier(catalog) + "." + QuoteIdentifier(schema) + "." + QuoteIdentifier(table);
	}
	/// Compare SQL expressions holding catalog identifiers using DuckDB's ASCII-only case folding.
	static string IdentifierEqualsSQL(const string &left, const string &right);
	static string QualifiedPrefix(const string &catalog, const string &schema);
	static string JsonQuote(const string &value);
	static string TrimSQLFragment(const string &input);
	static vector<string> ParseColumnList(const string &raw);
	static string JsonArray(const vector<string> &values);
	static string DuckLakeTableFunction(const string &function_name, const string &catalog, const string &schema,
	                                    const string &table, int64_t last_snapshot_id, int64_t current_snapshot_id);
	/// Rows inserted into or deleted from a DuckLake table between two snapshots, projected to select_list.
	static string DuckLakeChangesSQL(const string &select_list, const string &catalog, const string &schema,
	                                 const string &table, int64_t last_snapshot_id, int64_t current_snapshot_id);
	/// Run sql and throw EXCEPTION(format, args..., error) when it fails; format's last %s receives the error.
	template <class EXCEPTION = CatalogException, class... ARGS>
	static unique_ptr<QueryResult> QueryOrThrow(Connection &con, const string &sql, const string &format,
	                                            ARGS... args) {
		auto result = con.Query(sql);
		if (result->HasError()) {
			throw EXCEPTION(format, args..., result->GetError());
		}
		return result;
	}
	static bool GetBoolSetting(ClientContext &context, const string &setting_name, bool default_value) {
		Value setting_value;
		if (context.TryGetCurrentSetting(Identifier(setting_name), setting_value) && !setting_value.IsNull()) {
			return setting_value.GetValue<bool>();
		}
		return default_value;
	}
	/// The setting's text, or default_value when it is unset or NULL.
	static string GetStringSetting(ClientContext &context, const string &setting_name,
	                               const string &default_value = "") {
		Value setting_value;
		if (context.TryGetCurrentSetting(Identifier(setting_name), setting_value) && !setting_value.IsNull()) {
			return setting_value.ToString();
		}
		return default_value;
	}
	// Copy a registered extension setting to a helper connection without changing the database default.
	static void CopySessionSetting(ClientContext &from, ClientContext &to, const string &name);
	/// The connection's current_database(), or empty when it cannot be read.
	static string CurrentDatabase(Connection &con);
	/// The connection's current_database(), read without a statement; throws CatalogException(format, error)
	/// when there is none (for example, the default database was detached).
	static string RequireCurrentDatabase(Connection &con, const string &format);
	/// The caller's default catalog and schema; an unset session catalog is the default database.
	static SessionLocation SessionDefaultLocation(ClientContext &context);
	/// Rows in a native table's storage, without a COUNT(*) statement. An estimate: it includes deleted
	/// rows not yet vacuumed. Invalid for tables of other catalogs (DuckLake, attached SQL databases).
	static optional_idx StorageRowCount(TableCatalogEntry &table);
	/// `DELETE FROM <table> WHERE <column> = '<view_name>'` for per-view metadata rows.
	static string DeleteViewRowsSQL(const string &table, const string &view_name, const string &column = "view_name");
	static bool IsDelta(const string &name);
	/// The columns other than the multiplicity column, in their original order.
	static vector<string> WithoutMultiplicity(const vector<string> &columns);
	static string JoinQuotedColumns(const vector<string> &columns);
	static string JoinQualifiedQuotedColumns(const vector<string> &columns, const string &alias);
	/// `alias.col AS col, ...`: qualified columns that keep their unqualified output names.
	static string JoinQualifiedAliasedColumns(const vector<string> &columns, const string &alias);
	static string BuildAllNullPredicate(const vector<string> &columns);
	static string BuildAnyNullPredicate(const vector<string> &columns, const string &prefix = "");
	static string BuildNullSafeMatch(const vector<string> &columns, const string &lhs_alias, const string &rhs_alias);
	// Repeat each input row count times, preserving an empty result for nonpositive counts.
	static string BuildRowRepetitionSQL(const string &count, const string &alias, SqlDialect dialect);
	// Reconstruct a previous native bag, optionally projecting and restricting all inputs before subtraction.
	static string BuildPreviousSourceRowsSQL(const string &current, const string &delta, const string &delta_ts_filter,
	                                         const vector<string> &columns = {}, const string &candidate_join = "");
	static string BuildNullSafeKeyPredicate(const vector<string> &columns, const string &left_prefix,
	                                        const string &right_prefix);
	/// Cast specs retain the legacy single-target-type representation while encoding nested/TRY_CAST expressions.
	static string BuildCastSpec(const string &target_type, bool try_cast);
	static string ComposeCastSpecs(const string &outer_cast_spec, const string &inner_cast_spec);
	static string ApplyCastSpec(const string &column_expression, const string &cast_spec);
	// Pass unique_keys + temp_table + the data table's columns and types when it carries a UNIQUE
	// index, to get a form that never deletes and re-inserts the same key in one transaction.
	static string BuildFullRecomputeSQL(const string &data_table, const string &view_query_sql,
	                                    const vector<string> &unique_keys = {}, const string &temp_table = "",
	                                    const vector<string> &columns = {},
	                                    const vector<LogicalType> &column_types = {});
	// Upserts `source` rows into a data table keyed by a UNIQUE index. Callers delete NULL-containing
	// keys first. Matched rows are updated in the target's storage order (see the definition).
	static string BuildKeyedUpsertSQL(const string &data_table, const string &source, const vector<string> &keys,
	                                  const vector<string> &columns);
	static string ReplaceAllOccurrences(string haystack, const string &needle, const string &replacement);
	static vector<string> ReplaceEachPlainOccurrence(const string &haystack, const string &needle,
	                                                 const string &replacement);
	// Table-reference helpers accept SQL identifier spellings. Use QuoteIdentifier for
	// raw table names from the catalog so embedded dots and quotes retain their meaning.
	static string ReplaceTableReferences(const string &sql, const string &table_name, const string &replacement);
	static string ReplaceTableReferenceOccurrence(const string &sql, const string &table_name, idx_t occurrence,
	                                              const string &replacement, bool &replaced);
	static vector<string> ReplaceEachTableReference(const string &sql, const string &table_name,
	                                                const string &replacement);
	static bool RewriteColumnReferences(string &sql, const string &old_name, const string &new_name,
	                                    const unordered_set<string> &qualifiers, bool allow_unqualified);
	static string FindTableReference(const string &sql, const string &table_name);
	static string FindTableReferenceOccurrence(const string &sql, const string &table_name, idx_t occurrence);

	/// Parse a REFRESH EVERY interval string (e.g. "5 minutes", "2 hours") into seconds.
	/// Returns -1 if no interval clause found. Throws on invalid format or < 60 seconds.
	static int64_t ParseRefreshInterval(const string &interval_str);

	/// Quote an identifier for safe use in generated SQL (handles reserved words and special chars).
	template <class T>
	static string QuoteIdentifier(const T &name) {
		return SQLIdentifier::ToString(name);
	}

	/// Escape a string value for use inside single-quoted SQL literals.
	/// Use for WHERE view_name = '<escaped>' clauses.
	static string EscapeValue(const string &val) {
		return EscapeSingleQuotes(val);
	}
};

} // namespace duckdb

#endif // SQL_UTILS_HPP
