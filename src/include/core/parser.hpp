#ifndef OPENIVM_PARSER_HPP
#define OPENIVM_PARSER_HPP

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "upsert/refresh.hpp"

#include <utility>

namespace duckdb {

class MaterializedViewParserExtension : public ParserExtension {
public:
	explicit MaterializedViewParserExtension() {
		parse_function = ParseFunction;
		plan_function = PlanFunction;
	}

	static ParserExtensionParseResult ParseFunction(ParserExtensionInfo *info, const string &query);
	static ParserExtensionPlanResult PlanFunction(ParserExtensionInfo *info, ClientContext &context,
	                                              unique_ptr<ParserExtensionParseData> parse_data);
};

BoundStatement DDLExecutorBind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                               SQLStatement &statement);

struct MaterializedViewOperatorExtension : public OperatorExtension {
	MaterializedViewOperatorExtension() {
		Bind = DDLExecutorBind;
	}

	std::string GetName() override {
		return "MaterializedView";
	}

	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &) override {
		throw NotImplementedException("MaterializedViewOperatorExtension::Deserialize not implemented");
	}
};

struct MaterializedViewParseData : ParserExtensionParseData {
	MaterializedViewParseData() {
	}

	unique_ptr<SQLStatement> statement;
	bool plan = false;
	int64_t refresh_interval = -1; // seconds, -1 = not specified (manual only)
	bool is_replace = false;       // CREATE OR REPLACE: drop old MV before creating
	string alter_sql;              // non-empty for ALTER MATERIALIZED VIEW (executed directly in plan function)

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq_base<ParserExtensionParseData, MaterializedViewParseData>(statement->Copy(), false);
		auto &data = dynamic_cast<MaterializedViewParseData &>(*copy);
		data.refresh_interval = refresh_interval;
		data.is_replace = is_replace;
		data.alter_sql = alter_sql;
		return copy;
	}

	string ToString() const override {
		return statement->ToString();
	}

	explicit MaterializedViewParseData(unique_ptr<SQLStatement> statement, bool plan, int64_t refresh_interval = -1)
	    : statement(std::move(statement)), plan(plan), refresh_interval(refresh_interval) {
	}
};

class MaterializedViewState : public ClientContextState {
public:
	explicit MaterializedViewState(unique_ptr<ParserExtensionParseData> parse_data)
	    : parse_data(std::move(parse_data)) {
	}

	void QueryEnd() override {
		parse_data.reset();
	}

	unique_ptr<ParserExtensionParseData> parse_data;
};

class DDLExecutorFunction : public TableFunction {
public:
	DDLExecutorFunction() {
		name = "DDL executor function";
		arguments.push_back(LogicalType::BOOLEAN);
		bind = DDLExecutorBind;
		init_global = DDLExecutorInit;
		function = DDLExecutorExecute;
	}

	struct DDLExecutorBindData : public TableFunctionData {
		explicit DDLExecutorBindData(bool result) : result(result) {
		}
		bool result;
		vector<string> ddl;
	};

	struct DDLExecutorGlobalData : public GlobalTableFunctionState {
		DDLExecutorGlobalData() : offset(0) {
		}
		idx_t offset;
	};

	static unique_ptr<FunctionData> DDLExecutorBind(ClientContext &context, TableFunctionBindInput &input,
	                                                vector<LogicalType> &return_types, vector<string> &names) {
		names.emplace_back("MATERIALIZED VIEW CREATION");
		return_types.emplace_back(LogicalType::BOOLEAN);
		bool result = false;
		if (IntegerValue::Get(input.inputs[0]) == 1) {
			result = true;
		}
		return make_uniq<DDLExecutorBindData>(result);
	}

	static unique_ptr<GlobalTableFunctionState> DDLExecutorInit(ClientContext &context, TableFunctionInitInput &input) {
		return make_uniq<DDLExecutorGlobalData>();
	}

	static void DDLExecutorExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		auto &bind_data = data_p.bind_data->Cast<DDLExecutorBindData>();
		auto &data = dynamic_cast<DDLExecutorGlobalData &>(*data_p.global_state);
		if (data.offset >= 1) {
			return;
		}
		auto result = Value::BOOLEAN(bind_data.result);
		data.offset++;
		output.SetValue(0, 0, result);
		output.SetCardinality(1);
	}
};

} // namespace duckdb

#endif // OPENIVM_PARSER_HPP
