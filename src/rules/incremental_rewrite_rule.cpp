#include "rules/incremental_rewrite_rule.hpp"

#include "compile_facts.hpp"
#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/parser_ddl.hpp"
#include "core/published_view.hpp"
#include "core/refresh_metadata.hpp"
#include "core/parser_plan_helpers.hpp"
#include "core/plan_rewrite.hpp"
#include "core/scoped_optimizer_settings.hpp"
#include "core/sql_utils.hpp"
#include "core/time_travel_pins.hpp"
#include "delta/delta_compiler.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/planner.hpp"

namespace duckdb {

void IncrementalRewriteRule::AddInsertNode(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan,
                                           const string &view_name, const string &view_catalog_name,
                                           const string &view_schema_name) {
#if OPENIVM_DEBUG
	OPENIVM_DEBUG_PRINT("\nAdd the insert node to the plan...\n");
	OPENIVM_DEBUG_PRINT("Plan:\n%s\nParameters:", plan->ToString().c_str());
	for (const auto &i_param : plan->ParamsToString()) {
		OPENIVM_DEBUG_PRINT("%s", i_param.second.c_str());
	}
	OPENIVM_DEBUG_PRINT("\n---end of insert node output---\n");
#endif

	auto table =
	    Catalog::GetEntry<TableCatalogEntry>(context,
	                                         QualifiedName(Identifier(view_catalog_name), Identifier(view_schema_name),
	                                                       Identifier(SqlUtils::DeltaName(view_name))),
	                                         OnEntryNotFound::THROW_EXCEPTION, QueryErrorContext());
	auto insert_node = make_uniq<LogicalInsert>(*table, binder.GenerateTableIndex());

	Value value;
	unique_ptr<BoundConstantExpression> exp;
	for (size_t i = 0; i < plan->expressions.size(); i++) {
		insert_node->expected_types.emplace_back(plan->expressions[i]->GetReturnType());
		value = Value(plan->expressions[i]->GetReturnType());
		exp = make_uniq<BoundConstantExpression>(std::move(value));
		insert_node->bound_defaults.emplace_back(std::move(exp));
	}

	insert_node->children.emplace_back(std::move(plan));
	plan = std::move(insert_node);
}

unique_ptr<SQLStatement> IncrementalRewriteRule::LoadStoredViewStatement(ClientContext &context, Connection &con,
                                                                         const string &view_catalog, const string &view,
                                                                         const string &missing_error) {
	RefreshMetadata::UseCatalog(context, con, view_catalog);
	if (!context.transaction.IsAutoCommit()) {
		RefreshMetadata(con).SnapshotTransaction(context);
	}
	RefreshMetadata metadata(con);
	metadata.UseView(RefreshMetadata::PlanningView(context, view));
	auto view_query = metadata.GetViewQuery(view);
	if (view_query.empty()) {
		throw Exception(ExceptionType::CATALOG, missing_error);
	}
	OPENIVM_DEBUG_PRINT("[IVM] View: %s, Query: %s\n", view.c_str(), view_query.c_str());
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(view_query);
	if (parser.statements.empty()) {
		throw Exception(ExceptionType::PARSER, "IVM: empty view definition for '" + view + "'");
	}
	openivm::TimeTravelPins::Peel(context, *parser.statements[0]);
	return std::move(parser.statements[0]);
}

void IncrementalRewriteRule::IncrementalRewriteRuleFunction(OptimizerExtensionInput &input,
                                                            duckdb::unique_ptr<LogicalOperator> &plan) {
	WarnSnapshotHistoryDeletion(input.context, *plan);
	if (plan->children.empty()) {
		return;
	}

	auto child = plan.get();
	while (!child->children.empty()) {
		child = child->children[0].get();
	}
	auto marker = dynamic_cast<LogicalGet *>(child);
	if (!marker || !StringUtil::CIEquals(marker->function.GetName().GetIdentifierName(), "ComputeDelta")) {
		return;
	}

	OPENIVM_DEBUG_PRINT("Activating the rewrite rule\n");

	// ComputeDelta(view_catalog_name, view_schema_name, view_name)
	auto view_catalog = StringValue::Get(marker->parameters[0]);
	auto view_schema = StringValue::Get(marker->parameters[1]);
	auto view = StringValue::Get(marker->parameters[2]);

	Connection con(*input.context.db);
	Planner planner(input.context);
	OPENIVM_DEBUG_PRINT("[REWRITE] About to CreatePlan for view query\n");
	PlanWithSourceColumns(planner, LoadStoredViewStatement(input.context, con, view_catalog, view,
	                                                       "IVM: missing view definition for '" + view + "'"));
	RefreshMetadata metadata(con);
	metadata.UseView(RefreshMetadata::PlanningView(input.context, view));
	auto derived_outputs = metadata.GetDerivedAggregateOutputs(view);
	if (derived_outputs.complete && !derived_outputs.outputs.empty()) {
		auto group_columns = metadata.GetGroupColumns(view);
		// The upsert evaluates f(state + delta). Evaluating f(delta) here can
		// be invalid (e.g. an unsigned cast of COUNT(*) - 2 on a one-row delta).
		// Keep raw aggregate inputs and let the optimizer prune consumed outputs.
		planner.plan->ResolveOperatorTypes();
		// Foreign INSERT SQL can use bare NULL, including for complex outputs
		// whose target dialect has no verified cast type mapping.
		bool native = openivm::CompileFactsContextSlot::Get(input.context).target_dialect == SqlDialect::DUCKDB;
		auto bindings = planner.plan->GetColumnBindings();
		vector<unique_ptr<Expression>> expressions;
		for (idx_t i = 0; i < bindings.size(); i++) {
			bool derived = std::any_of(derived_outputs.outputs.begin(), derived_outputs.outputs.end(),
			                           [&](const DerivedAggregateOutput &output) {
				                           return output.output_column == planner.names[i].GetIdentifierName() &&
				                                  std::find(group_columns.begin(), group_columns.end(),
				                                            output.output_column) == group_columns.end();
			                           });
			unique_ptr<Expression> expression =
			    derived ? unique_ptr<Expression>(
			                  make_uniq<BoundConstantExpression>(native ? Value(planner.plan->types[i]) : Value()))
			            : make_uniq<BoundColumnRefExpression>(planner.plan->types[i], bindings[i]);
			expression->SetAlias(planner.names[i]);
			expressions.push_back(std::move(expression));
		}
		auto projection = make_uniq<LogicalProjection>(planner.binder->GenerateTableIndex(), std::move(expressions));
		projection->children.push_back(std::move(planner.plan));
		planner.plan = std::move(projection);
		OPENIVM_DEBUG_PRINT("[IVM Rewrite] Retained raw state inputs for derived aggregate deltas\n");
	}
	OPENIVM_DEBUG_PRINT("[REWRITE] CreatePlan done\n");
	OPENIVM_DEBUG_PRINT("Unoptimized plan: \n%s\n", planner.plan->ToString().c_str());
	// This is still the stored base-view plan. Its scans have not been replaced with delta scans yet,
	// so current base-table facts must never be folded into it. The user setting only controls the
	// subsequent optimization of the finished incremental plan in GenerateRefreshSQL().
	ScopedDisabledOptimizers disabled_optimizers(input.context, openivm::TEMPLATE_DATA_DEPENDENT_OPTIMIZERS);
	Optimizer optimizer(*planner.binder, input.context);
	auto optimized_plan = optimizer.Optimize(std::move(planner.plan));
	// LPTS stores repeated logical subplans once in a compact CTE graph so DuckDB
	// can bind and optimize large generated SQL without recursive-plan blow-up.
	// Delta joins, however, need one independently renumbered leaf per reference
	// to generate all inclusion-exclusion terms (including delta/delta terms).
	InlineMaterializedCteRefs(input.context, *planner.binder, optimized_plan);

	OPENIVM_DEBUG_PRINT("Optimized plan: \n%s\n", optimized_plan->ToString().c_str());

	if (optimized_plan->children.empty()) {
		throw NotImplementedException("Plan contains single node, this is not supported");
	}

	auto output_names = PrepareOutputNames(optimized_plan.get(), IdentifiersToStrings(planner.names));
	auto delta_model = BuildRefreshDeltaViewModel(input, optimized_plan.get(), output_names);
	LogDeltaModelSummary(delta_model);

	// Join delta compilation uses input.optimizer.binder, which may not have been advanced by the local optimizer.
	while (input.optimizer.binder.GenerateTableIndex().index <= delta_model.max_table_index) {
	}

	DeltaCompiler compiler(input, con, view, delta_model);

	OPENIVM_DEBUG_PRINT("[IVM Rewrite] === Starting IR delta compilation ===\n");
	auto root = optimized_plan.get();
	DeltaPlanFragment fragment = compiler.Compile(optimized_plan, root);
	OPENIVM_DEBUG_PRINT("[IVM Rewrite] === Delta compilation done, running AddInsertNode ===\n");
	AddInsertNode(input.context, input.optimizer.binder, fragment.op, view, view_catalog, view_schema);
	OPENIVM_DEBUG_PRINT("[IVM Rewrite] === FINAL PLAN ===\n%s\n", fragment.op->ToString().c_str());
	plan = std::move(fragment.op);
	return;
}

} // namespace duckdb
