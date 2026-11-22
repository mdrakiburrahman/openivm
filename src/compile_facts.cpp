#include "core/metadata_json.hpp"
#include "compile_facts.hpp"

#include "core/openivm_constants.hpp"
#include "core/refresh_metadata.hpp"
#include "core/sql_utils.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "upsert/refresh_internal.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace duckdb {
namespace openivm {

CompileFacts CompileFacts::Default(SqlDialect dialect) {
	CompileFacts out;
	out.target_dialect = dialect;
	return out;
}

namespace {

SqlDialect ParseDialectString(const string &s) {
	// Reuse the lpts helper so dialect names stay in lockstep with the
	// LPTS pipeline. LPTS throws on unrecognised values which is exactly
	// the contract we want.
	try {
		return ParseSqlDialect(s);
	} catch (const std::exception &e) {
		throw InvalidInputException("openivm_compile_with_facts: target_dialect='%s' is not a recognised SQL dialect "
		                            "(expected 'spark' | 'duckdb' | 'ducklake')",
		                            s.c_str());
	}
}

// Text of the first `"<key>": { ... }` member. The lenient MetadataJson readers are not nesting-aware, so the clock
// members are read from this substring instead of the whole document.
bool ExtractJsonObject(const string &json, const string &key, string &object) {
	const string needle = "\"" + key + "\"";
	for (size_t pos = json.find(needle); pos != string::npos; pos = json.find(needle, pos + 1)) {
		size_t value = pos + needle.size();
		MetadataJson::SkipSpace(json, value);
		if (value >= json.size() || json[value] != ':') {
			continue;
		}
		MetadataJson::SkipSpace(json, ++value);
		if (value >= json.size() || json[value] != '{') {
			continue;
		}
		int depth = 0;
		bool in_string = false;
		for (size_t i = value; i < json.size(); i++) {
			char c = json[i];
			if (in_string) {
				if (c == '\\') {
					i++;
				} else if (c == '"') {
					in_string = false;
				}
			} else if (c == '"') {
				in_string = true;
			} else if (c == '{') {
				depth++;
			} else if (c == '}' && --depth == 0) {
				object = json.substr(value, i - value + 1);
				return true;
			}
		}
		return false;
	}
	return false;
}

} // namespace

CompileFacts ParseFactsJson(const string &json) {
	CompileFacts out;

	string dialect_str;
	if (!MetadataJson::ExtractJsonString(json, "target_dialect", dialect_str)) {
		throw InvalidInputException(
		    "openivm_compile_with_facts: required field 'target_dialect' missing or not a string");
	}
	out.target_dialect = ParseDialectString(dialect_str);

	MetadataJson::ExtractJsonInt(json, "schema_version", out.schema_version);
	MetadataJson::ExtractJsonBool(json, "compile_only", out.compile_only);
	MetadataJson::ExtractJsonBool(json, "force_view_delta_cascade", out.force_view_delta_cascade);
	MetadataJson::ExtractJsonBool(json, "assume_insert_only", out.assume_insert_only);
	MetadataJson::ExtractJsonStringMap(json, "delta_shape", out.delta_shape);
	MetadataJson::ExtractJsonBool(json, "running_window_incremental", out.running_window_incremental);
	MetadataJson::ExtractJsonBool(json, "emit_spark_hints", out.emit_spark_hints);

	for (auto &object : MetadataJson::ExtractJsonObjectsFromArray(json, "fk_relations")) {
		CompileFactsFkRelation fk;
		MetadataJson::ExtractJsonString(object, "child_table", fk.child_table);
		MetadataJson::ExtractJsonStringArray(object, "child_columns", fk.child_columns);
		MetadataJson::ExtractJsonString(object, "parent_table", fk.parent_table);
		MetadataJson::ExtractJsonStringArray(object, "parent_columns", fk.parent_columns);
		// Also accept the roadmap draft spelling; Spark emits child_/parent_.
		if (fk.child_table.empty()) {
			MetadataJson::ExtractJsonString(object, "fk_table", fk.child_table);
		}
		if (fk.child_columns.empty()) {
			MetadataJson::ExtractJsonStringArray(object, "fk_cols", fk.child_columns);
		}
		if (fk.parent_table.empty()) {
			MetadataJson::ExtractJsonString(object, "pk_table", fk.parent_table);
		}
		if (fk.parent_columns.empty()) {
			MetadataJson::ExtractJsonStringArray(object, "pk_cols", fk.parent_columns);
		}
		MetadataJson::ExtractJsonBool(object, "rely", fk.rely);
		if (fk.rely && !fk.child_table.empty() && !fk.parent_table.empty() && !fk.child_columns.empty() &&
		    fk.child_columns.size() == fk.parent_columns.size()) {
			out.fk_relations.push_back(std::move(fk));
		}
	}

	string clock_object;
	if (ExtractJsonObject(json, "clock", clock_object)) {
		out.clock.present = true;
		MetadataJson::ExtractJsonString(clock_object, "as_of", out.clock.as_of);
		MetadataJson::ExtractJsonString(clock_object, "time_zone", out.clock.time_zone);
		MetadataJson::ExtractJsonString(clock_object, "previous_as_of", out.clock.previous_as_of);
		MetadataJson::ExtractJsonString(clock_object, "previous_time_zone", out.clock.previous_time_zone);
		out.clock_outcome = make_shared_ptr<ClockOutcome>();
		out.clock_outcome->reason = "not_evaluated";
	}

	return out;
}

//------------------------------------------------------------------------------
// CompileFactsContextSlot
//
// The slot is stored in `ClientContext::registered_state` via a
// `ClientContextState` subclass that simply owns a `shared_ptr<CompileFacts>`.
// The RAII helper inserts the slot in its ctor and removes it in its dtor,
// even on exception paths from `GenerateRefreshSQL`. This guarantees that
// downstream PRAGMA-refresh calls in the same connection never see leaked
// facts.
//------------------------------------------------------------------------------

namespace {

class CompileFactsState : public ClientContextState {
public:
	explicit CompileFactsState(shared_ptr<CompileFacts> facts_p) : facts(std::move(facts_p)) {
	}
	shared_ptr<CompileFacts> facts;
};

} // namespace

CompileFactsContextSlot::CompileFactsContextSlot(ClientContext &ctx_p, shared_ptr<CompileFacts> facts)
    : ctx(ctx_p), installed(false) {
	if (!facts) {
		return;
	}
	auto state = make_shared_ptr<CompileFactsState>(std::move(facts));
	ctx.registered_state->Insert(SLOT_KEY, std::move(state));
	installed = true;
}

CompileFactsContextSlot::~CompileFactsContextSlot() {
	if (!installed) {
		return;
	}
	try {
		ctx.registered_state->Remove(SLOT_KEY);
	} catch (...) {
		// swallow — destructor must not throw
	}
}

CompileFacts CompileFactsContextSlot::Get(ClientContext &ctx) {
	auto state = ctx.registered_state->Get<CompileFactsState>(SLOT_KEY);
	if (!state || !state->facts) {
		return CompileFacts::Default();
	}
	return *state->facts;
}

//------------------------------------------------------------------------------
// openivm_compile_with_facts(view_name VARCHAR, facts_json VARCHAR) table function
//
// Schema: (refresh_type INTEGER, refresh_type_name VARCHAR,
//          stmt_order INTEGER, stmt_kind VARCHAR, sql VARCHAR)
//
// The bind step parses the JSON into a `CompileFacts`, installs it onto
// `ClientContext::registered_state` via `CompileFactsContextSlot`, then
// invokes `GenerateRefreshSQL`. The resulting SQL is split into logical buckets
// (meta_pre / data / meta_post) and each top-level statement becomes its own
// output row. The slot is removed before bind returns so optimizer rules in
// later refresh calls don't see stale facts.
//------------------------------------------------------------------------------

namespace {

struct CompileWithFactsBindData : public TableFunctionData {
	int32_t refresh_type_int = 0;
	string refresh_type_name;
	vector<string> stmt_kinds;
	vector<string> stmt_sqls;
	bool has_clock = false;
	string clock_mode;
	string clock_reason;
};

struct CompileWithFactsGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

// Pushes each top-level statement of a SQL fragment, tagged with `kind`, into the bind result.
static void PushBucket(CompileWithFactsBindData &bd, const string &kind, const string &sql_block) {
	for (auto &statement : SqlUtils::SplitSQLStatements(sql_block)) {
		bd.stmt_kinds.push_back(kind);
		bd.stmt_sqls.push_back(statement + ";");
	}
}

} // namespace

unique_ptr<FunctionData> OpenIvmCompileWithFactsBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() < 2) {
		throw InvalidInputException("openivm_compile_with_facts(view_name, facts_json) requires exactly two arguments");
	}
	string view_name = StringValue::Get(input.inputs[0]);
	string facts_json = StringValue::Get(input.inputs[1]);

	auto facts_owned = make_shared_ptr<CompileFacts>(ParseFactsJson(facts_json));

	Connection con(*context.db.get());
	auto resolved = ResolveViewCatalogFromContext(context, con, view_name);
	view_name = resolved.view_name;
	RefreshMetadata metadata(con);
	RefreshType type = metadata.GetViewType(view_name);

	string sql;
	string out_pre_meta;
	string out_post_meta;
	{
		CompileFactsContextSlot slot(context, facts_owned);
		sql = GenerateRefreshSQL(context, resolved.view_catalog_name, resolved.view_schema_name, view_name,
		                         resolved.cross_system, "", "", resolved.cross_system ? &out_pre_meta : nullptr,
		                         resolved.cross_system ? &out_post_meta : nullptr,
		                         /*compile_profile=*/nullptr, /*precomputed_delta_activity=*/nullptr,
		                         /*out_adaptive_estimate=*/nullptr, /*facts=*/facts_owned.get());
	}

	auto result = make_uniq<CompileWithFactsBindData>();
	if (facts_owned->clock_outcome) {
		auto &outcome = *facts_owned->clock_outcome;
		result->has_clock = true;
		result->clock_mode = outcome.mode;
		result->clock_reason = outcome.reason;
		if (outcome.effective_refresh_type >= 0) {
			type = static_cast<RefreshType>(outcome.effective_refresh_type);
		}
	}
	result->refresh_type_int = static_cast<int32_t>(type);
	result->refresh_type_name = string(RefreshTypeName(type));

	PushBucket(*result, "meta_pre", out_pre_meta);
	PushBucket(*result, "data", sql);
	PushBucket(*result, "meta_post", out_post_meta);

	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("refresh_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("refresh_type_name");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("stmt_order");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("stmt_kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("sql");
	if (result->has_clock) {
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("clock_mode");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("clock_reason");
	}

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> OpenIvmCompileWithFactsInit(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	return make_uniq<CompileWithFactsGlobalState>();
}

void OpenIvmCompileWithFactsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bd = data_p.bind_data->Cast<CompileWithFactsBindData>();
	auto &state = data_p.global_state->Cast<CompileWithFactsGlobalState>();

	idx_t total = bd.stmt_sqls.size();
	idx_t emitted = 0;
	while (state.offset < total && emitted < STANDARD_VECTOR_SIZE) {
		output.data[0].SetValue(emitted, Value::INTEGER(bd.refresh_type_int));
		output.data[1].SetValue(emitted, Value(bd.refresh_type_name));
		output.data[2].SetValue(emitted, Value::INTEGER(static_cast<int32_t>(state.offset)));
		output.data[3].SetValue(emitted, Value(bd.stmt_kinds[state.offset]));
		output.data[4].SetValue(emitted, Value(bd.stmt_sqls[state.offset]));
		if (bd.has_clock) {
			output.data[5].SetValue(emitted, Value(bd.clock_mode));
			output.data[6].SetValue(emitted, Value(bd.clock_reason));
		}
		state.offset++;
		emitted++;
	}
	output.SetCardinalityUnsafe(emitted);
}

} // namespace openivm
} // namespace duckdb
