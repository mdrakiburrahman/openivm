#pragma once

#include "core/metadata_location.hpp"
#include "duckdb.hpp"
#include "sql_dialect.hpp"

namespace duckdb {

// The public relation is a stable scan boundary. Its schema contains only user
// columns, independent of the maintenance plan's helper columns and filters.
string PublishedViewName(const string &view_name);
string PublishedSourceViewName(string source_name);
void WarnSnapshotHistoryDeletion(ClientContext &context, LogicalOperator &plan);
bool IsSnapshotPublication(ClientContext &context, const string &catalog, const string &schema,
                           const string &view_name);
string BuildSnapshotPublicationSQL(Connection &con, const string &catalog, const string &published,
                                   const string &data_table, const string &query);
string BuildPublishViewSQL(const string &view_name, const string &prefix, const string &query,
                           const vector<string> &columns, const vector<LogicalType> &column_types, bool ducklake,
                           const string &metadata_table, const vector<string> &scope_columns = {},
                           const string &timestamp_sql = "", SqlDialect dialect = SqlDialect::DUCKDB,
                           const string &appended_rows = "", const string &scope_rows = "",
                           const vector<string> &delta_metadata_tables = {}, bool consumers_absent = false,
                           bool private_connection = false);
// Whether any view consumes the visible delta, read with the predicate the publication SQL embeds.
bool PublishedDeltaHasConsumers(Connection &con, const string &view_name, const string &prefix,
                                const string &metadata_table, const vector<string> &delta_metadata_tables);

} // namespace duckdb
