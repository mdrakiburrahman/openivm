#include "core/plan_rewrite.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/plan_rewrite_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <cctype>
#include <functional>
#include <map>
#include <limits>
#include <set>

namespace duckdb {
unique_ptr<BoundAggregateExpression> MakeAggregate(ClientContext &context, const string &name,
                                                   unique_ptr<Expression> child, const string &alias) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), DEFAULT_SCHEMA, Identifier(name)));
	FunctionBinder binder(context);
	ErrorData error;
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	arguments.emplace_back(Identifier(), std::move(child));
	auto result = binder.BindAggregateFunction(entry, std::move(arguments), error);
	if (!result) {
		error.Throw();
	}
	result->SetAlias(Identifier(alias));
	return result;
}

unique_ptr<BoundAggregateExpression> MakeCountStar(const string &alias) {
	auto result = make_uniq<BoundAggregateExpression>(BoundAggregateFunction(CountStarFun::GetFunction()),
	                                                  vector<unique_ptr<Expression>>(), nullptr, nullptr,
	                                                  AggregateType::NON_DISTINCT);
	result->SetAlias(Identifier(alias));
	return result;
}

unique_ptr<LogicalAggregate> MakeGroupedAggregate(Binder &binder, vector<unique_ptr<Expression>> groups,
                                                  vector<unique_ptr<Expression>> aggregates,
                                                  unique_ptr<LogicalOperator> child) {
	auto group_index = binder.GenerateTableIndex();
	auto aggregate_index = binder.GenerateTableIndex();
	auto result = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
	GroupingSet grouping_set;
	for (auto &group : groups) {
		grouping_set.insert(ProjectionIndex(result->groups.size()));
		result->group_stats.push_back(make_uniq<BaseStatistics>(BaseStatistics::CreateUnknown(group->GetReturnType())));
		result->groups.push_back(std::move(group));
	}
	result->grouping_sets.push_back(std::move(grouping_set));
	result->children.push_back(std::move(child));
	result->ResolveOperatorTypes();
	return result;
}

/// Keep [A-Za-z0-9_] and collapse other character runs into one '_' (no trailing '_'); `fallback` if nothing remains.
static string SanitizeAliasSuffix(const string &raw, const string &fallback) {
	string clean;
	bool last_under = false;
	for (unsigned char c : raw) {
		if (std::isalnum(c) || c == '_') {
			clean += static_cast<char>(c);
			last_under = (c == '_');
		} else if (!last_under && !clean.empty()) {
			clean += '_';
			last_under = true;
		}
	}
	if (!clean.empty() && clean.back() == '_') {
		clean.pop_back();
	}
	return clean.empty() ? fallback : clean;
}

static bool IsStddevOrVariance(const string &name) {
	return name == "stddev" || name == "stddev_samp" || name == "stddev_pop" || name == "variance" ||
	       name == "var_samp" || name == "var_pop";
}

static bool IsPopulationVariant(const string &name) {
	return name == "stddev_pop" || name == "var_pop";
}

static bool IsStddevVariant(const string &name) {
	return name == "stddev" || name == "stddev_samp" || name == "stddev_pop";
}
static const char *SumSqPrefix(const string &func_name) {
	if (func_name == "stddev_pop") {
		return openivm::SUM_SQP_COL_PREFIX;
	}
	if (func_name == "var_pop") {
		return openivm::VAR_SQP_COL_PREFIX;
	}
	if (func_name == "variance" || func_name == "var_samp") {
		return openivm::VAR_SQ_COL_PREFIX;
	}
	return openivm::SUM_SQ_COL_PREFIX; // stddev, stddev_samp
}

static bool CanMaskAggregateFilter(const BoundAggregateExpression &aggregate) {
	const auto &name = aggregate.Function().GetName().GetIdentifierName();
	if (aggregate.GetChildren().empty()) {
		return name == "count_star";
	}
	if (aggregate.GetChildren().size() != 1) {
		return false;
	}
	return name == "count" || name == "sum" || name == "min" || name == "max" || name == "avg" ||
	       IsStddevOrVariance(name) || name == "bool_and" || name == "bool_or" || name == "bit_and" ||
	       name == "bit_or" || name == "bit_xor" || name == "product" || name == "any_value";
}

void RewriteAggregateFilters(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	for (auto &child : plan->children) {
		RewriteAggregateFilters(context, child);
	}
	if (plan->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}
	auto &agg = plan->Cast<LogicalAggregate>();
	bool any_filter = false;
	for (auto &expr : agg.expressions) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    expr->Cast<BoundAggregateExpression>().GetFilter()) {
			any_filter = true;
			break;
		}
	}
	if (!any_filter) {
		return;
	}
	bool rewritten = false;
	for (auto &expr : agg.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			continue;
		}
		auto &bound = expr->Cast<BoundAggregateExpression>();
		if (!bound.GetFilter()) {
			continue;
		}
		if (!CanMaskAggregateFilter(bound)) {
			OPENIVM_DEBUG_PRINT("[PlanRewrite] Preserved native FILTER for aggregate %s\n",
			                    bound.Function().GetName().GetIdentifierName().c_str());
			continue;
		}
		auto filter_expr = std::move(bound.GetFilterMutable()); // nulls bound.filter
		if (bound.GetChildren().empty()) {
			auto then_expr = make_uniq<BoundConstantExpression>(Value::BIGINT(1));
			auto else_expr = make_uniq<BoundConstantExpression>(Value(LogicalType::BIGINT));
			auto case_expr =
			    make_uniq<BoundCaseExpression>(std::move(filter_expr), std::move(then_expr), std::move(else_expr));
			expr = MakeAggregate(context, "count", std::move(case_expr), bound.GetAlias().GetIdentifierName());
			rewritten = true;
		} else {
			auto arg_type = bound.GetChildren()[0]->GetReturnType();
			auto else_expr = make_uniq<BoundConstantExpression>(Value(arg_type)); // NULL of same type
			bound.GetChildrenMutable()[0] = make_uniq<BoundCaseExpression>(
			    std::move(filter_expr), std::move(bound.GetChildrenMutable()[0]), std::move(else_expr));
			rewritten = true;
		}
	}
	if (!rewritten) {
		return;
	}
	agg.ResolveOperatorTypes();
	OPENIVM_DEBUG_PRINT("[PlanRewrite] Rewrote FILTER aggregates to CASE expressions\n");
}

LogicalOperator *FindProjectionAggregateInput(unique_ptr<LogicalOperator> &plan, bool allow_having_filter) {
	if (plan->type != LogicalOperatorType::LOGICAL_PROJECTION || plan->children.empty()) {
		return nullptr;
	}
	auto *input = plan->children[0].get();
	if (input->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return input;
	}
	if (allow_having_filter && input->type == LogicalOperatorType::LOGICAL_FILTER && !input->children.empty() &&
	    input->children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return input->children[0].get();
	}
	return nullptr;
}

unique_ptr<Expression> MakeNegativeZeroExpression(ClientContext &context, Optimizer &optimizer,
                                                  unique_ptr<Expression> argument) {
	// Only zero needs a representation witness. Ordinary values avoid formatting;
	// ASCII is portable in exported SQL, unlike a DuckDB-only SIGNBIT call.
	auto zero = make_uniq<BoundConstantExpression>(Value::DOUBLE(0).DefaultCastAs(argument->GetReturnType()));
	auto is_zero = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, argument->Copy(), std::move(zero));
	auto text = BoundCastExpression::AddCastToType(context, std::move(argument), LogicalType::VARCHAR);
	auto negative = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL,
	                                                  optimizer.BindScalarFunction("ascii", std::move(text)),
	                                                  make_uniq<BoundConstantExpression>(Value::INTEGER(45)));
	return make_uniq<BoundCaseExpression>(std::move(is_zero), std::move(negative),
	                                      make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)));
}

static bool IsDecomposedSumAlias(const string &alias) {
	return StringUtil::StartsWith(alias, openivm::SUM_COL_PREFIX) ||
	       StringUtil::StartsWith(alias, openivm::VAR_SQ_COL_PREFIX) ||
	       StringUtil::StartsWith(alias, openivm::VAR_SQP_COL_PREFIX);
}

void RewriteNonFiniteSumInputs(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan) {
	auto spine = FindAggregateOutputSpine(plan.get());
	auto *aggregate = SpineAggregate(spine);
	if (!aggregate || spine.projections.empty()) {
		return;
	}
	unordered_map<TableIndex, LogicalProjection *> projections;
	for (auto *projection : spine.projections) {
		projections.emplace(projection->table_index, projection);
	}
	std::set<idx_t> computed_inputs;
	std::function<void(const Expression &, bool)> collect = [&](const Expression &expression, bool computed) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto binding = expression.Cast<BoundColumnRefExpression>().Binding();
			if (binding.table_index == aggregate->aggregate_index) {
				if (computed) {
					computed_inputs.insert(binding.column_index.GetIndex());
				}
			} else if (projections.count(binding.table_index)) {
				collect(*projections.at(binding.table_index)->expressions[binding.column_index], computed);
			}
			return;
		}
		ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) { collect(child, true); });
	};
	for (auto &expression : spine.projections.front()->expressions) {
		collect(*expression, false);
	}
	// Preserve the existing derived-only path; a computed aggregate needs recoverable
	// state for every floating SUM it shares the MERGE with, including AVG helpers.
	if (std::none_of(computed_inputs.begin(), computed_inputs.end(), [&](idx_t index) {
		    auto &sum = aggregate->expressions[index]->Cast<BoundAggregateExpression>();
		    auto alias = sum.GetAlias().GetIdentifierName();
		    return !IsDecomposedSumAlias(alias) && !StringUtil::StartsWith(alias, openivm::COUNT_COL_PREFIX);
	    })) {
		return;
	}
	Optimizer optimizer(binder, context);
	column_binding_map_t<unique_ptr<Expression>> replacements;
	const auto original_aggregate_count = aggregate->expressions.size();
	for (idx_t index = 0; index < original_aggregate_count; index++) {
		auto &sum = aggregate->expressions[index]->Cast<BoundAggregateExpression>();
		if (sum.Function().GetName().GetIdentifierName() != "sum" || sum.IsDistinct() || sum.GetFilter() ||
		    sum.GetChildren()[0]->IsVolatile() || sum.GetReturnType().id() != LogicalTypeId::DOUBLE) {
			continue;
		}
		auto argument = sum.GetChildren()[0]->Copy();
		auto argument_type = argument->GetReturnType();
		auto positive_infinity = Value::DOUBLE(std::numeric_limits<double>::infinity()).DefaultCastAs(argument_type);
		auto negative_infinity = Value::DOUBLE(-std::numeric_limits<double>::infinity()).DefaultCastAs(argument_type);
		auto finite = make_uniq<BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND,
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN, argument->Copy(),
		                                      make_uniq<BoundConstantExpression>(positive_infinity)),
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, argument->Copy(),
		                                      make_uniq<BoundConstantExpression>(negative_infinity)));
		auto large = BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO, optimizer.BindScalarFunction("abs", argument->Copy()),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(openivm::FINITE_SUM_THRESHOLD)));
		auto zero = [] {
			return make_uniq<BoundConstantExpression>(Value::DOUBLE(0));
		};
		auto scale = [] {
			return make_uniq<BoundConstantExpression>(Value::DOUBLE(openivm::FINITE_SUM_SCALE));
		};
		auto large_input = make_uniq<BoundCaseExpression>(
		    large->Copy(), optimizer.BindScalarFunction("/", argument->Copy(), scale()), zero());
		auto large_sum =
		    MakeAggregate(context, "sum",
		                  make_uniq<BoundCaseExpression>(finite->Copy(), std::move(large_input),
		                                                 make_uniq<BoundConstantExpression>(Value(argument_type))),
		                  string(openivm::LARGE_SUM_COL_PREFIX) + to_string(index));
		sum.GetChildrenMutable()[0] = make_uniq<BoundCaseExpression>(
		    std::move(finite), make_uniq<BoundCaseExpression>(std::move(large), zero(), argument->Copy()),
		    make_uniq<BoundConstantExpression>(Value(argument_type)));
		sum.SetAlias(Identifier(string(openivm::FINITE_SUM_COL_PREFIX) + to_string(index)));
		ColumnBinding large_binding(aggregate->aggregate_index, ProjectionIndex(aggregate->expressions.size()));
		aggregate->expressions.push_back(std::move(large_sum));
		vector<ColumnBinding> counts;
		for (auto prefix :
		     {openivm::NAN_COUNT_COL_PREFIX, openivm::POS_INF_COUNT_COL_PREFIX, openivm::NEG_INF_COUNT_COL_PREFIX}) {
			unique_ptr<Expression> predicate;
			if (prefix == openivm::NAN_COUNT_COL_PREFIX) {
				predicate = optimizer.BindScalarFunction("isnan", argument->Copy());
			} else {
				auto infinity = Value::DOUBLE(prefix == openivm::POS_INF_COUNT_COL_PREFIX
				                                  ? std::numeric_limits<double>::infinity()
				                                  : -std::numeric_limits<double>::infinity());
				predicate = BoundComparisonExpression::Create(
				    ExpressionType::COMPARE_EQUAL, argument->Copy(),
				    make_uniq<BoundConstantExpression>(infinity.DefaultCastAs(argument_type)));
			}
			auto counted = make_uniq<BoundCaseExpression>(
			    std::move(predicate), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
			    make_uniq<BoundConstantExpression>(Value(LogicalType::INTEGER)));
			counts.emplace_back(aggregate->aggregate_index, ProjectionIndex(aggregate->expressions.size()));
			aggregate->expressions.push_back(
			    MakeAggregate(context, "count", std::move(counted), string(prefix) + to_string(index)));
		}
		auto present = [&](idx_t i) {
			return BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_GREATERTHAN,
			    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, counts[i]),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		};
		auto constant = [](double value) {
			return make_uniq<BoundConstantExpression>(Value::DOUBLE(value));
		};
		ColumnBinding binding(aggregate->aggregate_index, ProjectionIndex(index));
		auto small_ref = make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, binding);
		auto large_ref = make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, large_binding);
		auto large_zero = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, large_ref->Copy(), zero());
		auto combined = optimizer.BindScalarFunction(
		    "*",
		    optimizer.BindScalarFunction("+", large_ref->Copy(),
		                                 optimizer.BindScalarFunction("/", small_ref->Copy(), scale())),
		    scale());
		// Keep tiny inputs unscaled when the large lane cancels to zero.
		unique_ptr<Expression> value =
		    make_uniq<BoundCaseExpression>(std::move(large_zero), std::move(small_ref), std::move(combined));
		value = make_uniq<BoundCaseExpression>(present(2), constant(-std::numeric_limits<double>::infinity()),
		                                       std::move(value));
		// Use native arithmetic so CAST to VARCHAR retains the platform's NaN sign.
		auto opposite_infinities = optimizer.BindScalarFunction("+", constant(std::numeric_limits<double>::infinity()),
		                                                        constant(-std::numeric_limits<double>::infinity()));
		auto positive = make_uniq<BoundCaseExpression>(present(2), std::move(opposite_infinities),
		                                               constant(std::numeric_limits<double>::infinity()));
		value = make_uniq<BoundCaseExpression>(present(1), std::move(positive), std::move(value));
		replacements.emplace(binding, make_uniq<BoundCaseExpression>(present(0),
		                                                             constant(std::numeric_limits<double>::quiet_NaN()),
		                                                             std::move(value)));
	}
	if (replacements.empty()) {
		return;
	}
	std::function<void(unique_ptr<Expression> &)> rewrite = [&](unique_ptr<Expression> &expression) {
		if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto found = replacements.find(expression->Cast<BoundColumnRefExpression>().Binding());
			if (found != replacements.end()) {
				auto alias = expression->GetAlias();
				expression = found->second->Copy();
				expression->SetAlias(alias);
				return;
			}
		}
		ExpressionIterator::EnumerateChildren(*expression, [&](unique_ptr<Expression> &child) { rewrite(child); });
	};
	for (auto *projection : spine.projections) {
		for (auto &expression : projection->expressions) {
			rewrite(expression);
		}
	}
	if (spine.bottom->type == LogicalOperatorType::LOGICAL_FILTER) {
		for (auto &expression : spine.bottom->expressions) {
			rewrite(expression);
		}
	}
	plan->ResolveOperatorTypes();
	OPENIVM_DEBUG_PRINT("[PlanRewrite] Retained finite sums and non-finite support counts for %zu SUM inputs\n",
	                    replacements.size());
}

void InjectSumNonNullCounts(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (!plan) {
		return;
	}

	// Find the linear output path down to the aggregate. ORDER BY/LIMIT/TOP_N/DISTINCT
	// and MATERIALIZED_CTE are pass-through wrappers; projections may rename or
	// cast aggregate outputs and therefore need to be followed explicitly.
	auto spine = FindOutputSpine(plan.get(), [](const LogicalOperator &op) {
		return op.type == LogicalOperatorType::LOGICAL_ORDER_BY || op.type == LogicalOperatorType::LOGICAL_LIMIT ||
		       op.type == LogicalOperatorType::LOGICAL_TOP_N || op.type == LogicalOperatorType::LOGICAL_DISTINCT ||
		       op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
	});
	auto *agg_search = SpineAggregate(spine);
	auto &projections = spine.projections;
	if (!agg_search || projections.empty()) {
		return;
	}

	auto &agg = agg_search->Cast<LogicalAggregate>();
	auto &output_projection = *projections.front();
	const idx_t original_aggregate_count = agg.expressions.size();

	struct SumCountOutput {
		idx_t projection_index;
		idx_t aggregate_index;
	};
	vector<SumCountOutput> outputs;
	column_binding_set_t exposed_sums;
	for (idx_t projection_index = 0; projection_index < output_projection.expressions.size(); projection_index++) {
		Expression *resolved = output_projection.expressions[projection_index].get();
		bool has_cast = false;
		while (resolved) {
			while (BoundCastExpression::IsCast(*resolved)) {
				has_cast = true;
				resolved = BoundCastExpression::ChildMutable(resolved->Cast<BoundFunctionExpression>()).get();
			}
			if (resolved->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				break;
			}
			auto &ref = resolved->Cast<BoundColumnRefExpression>();
			if (ref.Binding().table_index == agg.aggregate_index) {
				break;
			}
			LogicalProjection *referenced_projection = nullptr;
			for (idx_t projection_depth = 1; projection_depth < projections.size(); projection_depth++) {
				if (projections[projection_depth]->table_index == ref.Binding().table_index) {
					referenced_projection = projections[projection_depth];
					break;
				}
			}
			if (!referenced_projection || ref.Binding().column_index >= referenced_projection->expressions.size()) {
				resolved = nullptr;
				break;
			}
			resolved = referenced_projection->expressions[ref.Binding().column_index].get();
		}
		if (!resolved || resolved->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &ref = resolved->Cast<BoundColumnRefExpression>();
		if (ref.Binding().table_index != agg.aggregate_index ||
		    ref.Binding().column_index >= original_aggregate_count) {
			continue;
		}
		auto &aggregate_expr = agg.expressions[ref.Binding().column_index];
		if (aggregate_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			continue;
		}
		auto &sum = aggregate_expr->Cast<BoundAggregateExpression>();
		if (sum.Function().GetName().GetIdentifierName() != "sum" || sum.IsDistinct() ||
		    sum.GetChildren().size() != 1) {
			continue;
		}
		if (IsDecomposedSumAlias(sum.GetAlias().GetIdentifierName())) {
			continue;
		}

		if (has_cast && exposed_sums.insert(ref.Binding()).second) {
			auto alias = string(openivm::SUM_COL_PREFIX) + "state_" + to_string(projection_index);
			PropagateHiddenBindingThroughProjectionPath(projections, ref.Binding(), sum.GetReturnType(), alias, true);
			OPENIVM_DEBUG_PRINT("[PlanRewrite] Exposed raw SUM state for cast output %zu\n", projection_index);
		}
		outputs.push_back({projection_index, agg.expressions.size()});
		agg.expressions.push_back(MakeAggregate(context, "count", sum.GetChildren()[0]->Copy(),
		                                        string(openivm::SUM_COUNT_COL_PREFIX) + to_string(projection_index)));
	}
	if (outputs.empty()) {
		return;
	}

	agg.ResolveOperatorTypes();
	auto aggregate_bindings = agg_search->GetColumnBindings();
	auto aggregate_types = agg_search->types;
	for (auto &output : outputs) {
		idx_t output_index = agg.groups.size() + output.aggregate_index;
		auto alias = string(openivm::SUM_COUNT_COL_PREFIX) + to_string(output.projection_index);
		PropagateHiddenBindingThroughProjectionPath(projections, aggregate_bindings[output_index],
		                                            aggregate_types[output_index], alias);
	}
	OPENIVM_DEBUG_PRINT("[PlanRewrite] Injected %zu SUM non-NULL counts\n", outputs.size());
}

void RewriteDerivedAggregates(ClientContext &context, unique_ptr<LogicalOperator> &plan, Optimizer &opt, bool is_top) {
	for (auto &child : plan->children) {
		RewriteDerivedAggregates(context, child, opt, false);
	}
	auto *agg_search = FindProjectionAggregateInput(plan, is_top);
	if (!agg_search) {
		return;
	}
	auto &agg = agg_search->Cast<LogicalAggregate>();
	bool has_derived = false;
	for (auto &expr : agg.expressions) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &bound_agg = expr->Cast<BoundAggregateExpression>();
			auto &name = bound_agg.Function().GetName().GetIdentifierName();
			// Keep DISTINCT aggregates intact for the existing affected-group recompute path.
			if ((name == "avg" || IsStddevOrVariance(name)) && !bound_agg.IsDistinct()) {
				has_derived = true;
				break;
			}
		}
	}
	if (!has_derived) {
		return;
	}

	auto &proj = plan->Cast<LogicalProjection>();
	size_t group_count = agg.groups.size();
	enum class DecompKind { AVG, STDDEV };
	struct Decomp {
		DecompKind kind;
		string alias;      // internal alias for hidden column suffixes
		string user_alias; // user-facing alias (from projection expression), set in step 2
		string func_name;  // original function name
		idx_t sum_idx;
		idx_t sum_sq_idx; // only for STDDEV
		idx_t count_idx;
		idx_t old_idx; // index in ORIGINAL expressions array
	};
	vector<Decomp> decomps;
	vector<unique_ptr<Expression>> new_exprs;
	std::map<idx_t, idx_t> nondecomposed_remap;

	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		auto &expr = agg.expressions[i];
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			nondecomposed_remap[i] = new_exprs.size();
			new_exprs.push_back(std::move(expr));
			continue;
		}
		auto &bound = expr->Cast<BoundAggregateExpression>();
		if (bound.GetChildren().empty()) {
			nondecomposed_remap[i] = new_exprs.size();
			new_exprs.push_back(std::move(expr));
			continue;
		}

		bool is_avg = (bound.Function().GetName().GetIdentifierName() == "avg");
		bool is_stddev = IsStddevOrVariance(bound.Function().GetName().GetIdentifierName());
		if ((!is_avg && !is_stddev) || bound.IsDistinct()) {
			nondecomposed_remap[i] = new_exprs.size();
			new_exprs.push_back(std::move(expr));
			continue;
		}

		string alias = bound.GetAlias().GetIdentifierName().empty()
		                   ? (bound.Function().GetName().GetIdentifierName() + "_" + bound.GetChildren()[0]->GetName())
		                   : bound.GetAlias().GetIdentifierName();
		alias = SanitizeAliasSuffix(alias, alias);

		Decomp d;
		d.kind = is_avg ? DecompKind::AVG : DecompKind::STDDEV;
		d.alias = alias;
		d.func_name = bound.Function().GetName().GetIdentifierName();
		d.old_idx = i;
		d.sum_idx = new_exprs.size();
		new_exprs.push_back(
		    MakeAggregate(context, "sum", bound.GetChildren()[0]->Copy(), string(openivm::SUM_COL_PREFIX) + alias));
		if (is_stddev) {
			auto sq_arg = opt.BindScalarFunction("*", bound.GetChildren()[0]->Copy(), bound.GetChildren()[0]->Copy());
			d.sum_sq_idx = new_exprs.size();
			new_exprs.push_back(
			    MakeAggregate(context, "sum", std::move(sq_arg),
			                  string(SumSqPrefix(bound.Function().GetName().GetIdentifierName())) + alias));
		}
		d.count_idx = new_exprs.size();
		new_exprs.push_back(
		    MakeAggregate(context, "count", bound.GetChildren()[0]->Copy(), string(openivm::COUNT_COL_PREFIX) + alias));

		decomps.push_back(std::move(d));
	}
	agg.expressions = std::move(new_exprs);
	agg.ResolveOperatorTypes();
	auto agg_bindings = agg_search->GetColumnBindings();
	auto agg_types = agg_search->types;
	const idx_t original_proj_size = proj.expressions.size();
	auto build_replacement = [&](const Decomp &d) -> unique_ptr<Expression> {
		ColumnBinding sum_binding = agg_bindings[group_count + d.sum_idx];
		ColumnBinding count_binding = agg_bindings[group_count + d.count_idx];
		LogicalType sum_type = agg_types[group_count + d.sum_idx];
		LogicalType count_type = agg_types[group_count + d.count_idx];
		if (d.kind == DecompKind::AVG) {
			auto sum_ref = make_uniq<BoundColumnRefExpression>(sum_type, sum_binding);
			auto count_ref = make_uniq<BoundColumnRefExpression>(count_type, count_binding);
			auto formula = opt.BindScalarFunction("/", std::move(sum_ref), std::move(count_ref));
			auto present = BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_GREATERTHAN, make_uniq<BoundColumnRefExpression>(count_type, count_binding),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
			auto empty = make_uniq<BoundConstantExpression>(Value(formula->GetReturnType()));
			return make_uniq<BoundCaseExpression>(std::move(present), std::move(formula), std::move(empty));
		}
		ColumnBinding sum_sq_binding = agg_bindings[group_count + d.sum_sq_idx];
		LogicalType sum_sq_type = agg_types[group_count + d.sum_sq_idx];
		auto s1 = make_uniq<BoundColumnRefExpression>(sum_type, sum_binding);
		auto s2 = make_uniq<BoundColumnRefExpression>(sum_type, sum_binding);
		auto sq = make_uniq<BoundColumnRefExpression>(sum_sq_type, sum_sq_binding);
		auto n = make_uniq<BoundColumnRefExpression>(count_type, count_binding);
		auto sum_sq_over_n = opt.BindScalarFunction("/", opt.BindScalarFunction("*", std::move(s1), std::move(s2)),
		                                            make_uniq<BoundColumnRefExpression>(count_type, count_binding));
		auto numerator = opt.BindScalarFunction("-", std::move(sq), std::move(sum_sq_over_n));
		unique_ptr<Expression> denom;
		if (IsPopulationVariant(d.func_name)) {
			denom = std::move(n);
		} else {
			denom = opt.BindScalarFunction("-", std::move(n), make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
		}
		auto var_expr = opt.BindScalarFunction("/", std::move(numerator), std::move(denom));
		var_expr = opt.BindScalarFunction("greatest", std::move(var_expr),
		                                  make_uniq<BoundConstantExpression>(Value::DOUBLE(0)));
		auto formula =
		    IsStddevVariant(d.func_name) ? opt.BindScalarFunction("sqrt", std::move(var_expr)) : std::move(var_expr);
		int64_t threshold = IsPopulationVariant(d.func_name) ? 0 : 1;
		auto count_check = make_uniq<BoundColumnRefExpression>(count_type, count_binding);
		auto when_expr =
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, std::move(count_check),
		                                      make_uniq<BoundConstantExpression>(Value::BIGINT(threshold)));
		auto else_expr = make_uniq<BoundConstantExpression>(Value(formula->GetReturnType()));
		return make_uniq<BoundCaseExpression>(std::move(when_expr), std::move(formula), std::move(else_expr));
	};
	for (auto &d : decomps) {
		ColumnBinding old_binding {agg.aggregate_index, ProjectionIndex(d.old_idx)};
		d.user_alias = d.alias; // fallback if no direct ref
		for (idx_t pi = 0; pi < original_proj_size; pi++) {
			if (proj.expressions[pi]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				continue;
			}
			auto &ref = proj.expressions[pi]->Cast<BoundColumnRefExpression>();
			if (ref.Binding() == old_binding) {
				d.user_alias =
				    ref.GetAlias().GetIdentifierName().empty() ? d.alias : ref.GetAlias().GetIdentifierName();
				break;
			}
		}
	}
	column_binding_map_t<const Decomp *> old_binding_to_decomp;
	for (auto &d : decomps) {
		old_binding_to_decomp[ColumnBinding(TableIndex(agg.aggregate_index), ProjectionIndex(d.old_idx))] = &d;
	}
	std::function<void(unique_ptr<Expression> &)> walk = [&](unique_ptr<Expression> &expr) {
		if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &bcr = expr->Cast<BoundColumnRefExpression>();
			if (bcr.Binding().table_index == agg.aggregate_index) {
				auto dit = old_binding_to_decomp.find(bcr.Binding());
				if (dit != old_binding_to_decomp.end()) {
					auto replacement = build_replacement(*dit->second);
					replacement->SetAlias(Identifier(bcr.GetAlias().GetIdentifierName()));
					expr = std::move(replacement);
					return; // do NOT descend into the replacement
				}
				auto rit = nondecomposed_remap.find(bcr.Binding().column_index);
				if (rit != nondecomposed_remap.end() && rit->second != bcr.Binding().column_index) {
					bcr.BindingMutable().column_index = ProjectionIndex(rit->second);
				}
			}
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { walk(child); });
	};
	for (idx_t pi = 0; pi < original_proj_size; pi++) {
		walk(proj.expressions[pi]);
	}
	if (!plan->children.empty() && plan->children[0]->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &having = plan->children[0]->Cast<LogicalFilter>();
		for (auto &expression : having.expressions) {
			walk(expression);
		}
	}
	for (auto &d : decomps) {
		ColumnBinding sum_binding = agg_bindings[group_count + d.sum_idx];
		ColumnBinding count_binding = agg_bindings[group_count + d.count_idx];
		LogicalType sum_type = agg_types[group_count + d.sum_idx];
		LogicalType count_type = agg_types[group_count + d.count_idx];
		string col_suffix = SanitizeAliasSuffix(d.user_alias.empty() ? d.alias : d.user_alias, d.alias);
		auto sum_pt = make_uniq<BoundColumnRefExpression>(sum_type, sum_binding);
		sum_pt->SetAlias(Identifier(string(openivm::SUM_COL_PREFIX) + col_suffix));
		proj.expressions.push_back(std::move(sum_pt));

		if (d.kind == DecompKind::STDDEV) {
			ColumnBinding sum_sq_binding = agg_bindings[group_count + d.sum_sq_idx];
			LogicalType sum_sq_type = agg_types[group_count + d.sum_sq_idx];
			auto sum_sq_pt = make_uniq<BoundColumnRefExpression>(sum_sq_type, sum_sq_binding);
			sum_sq_pt->SetAlias(Identifier(string(SumSqPrefix(d.func_name)) + col_suffix));
			proj.expressions.push_back(std::move(sum_sq_pt));
		}

		auto count_pt = make_uniq<BoundColumnRefExpression>(count_type, count_binding);
		count_pt->SetAlias(Identifier(string(openivm::COUNT_COL_PREFIX) + col_suffix));
		proj.expressions.push_back(std::move(count_pt));
	}
	proj.ResolveOperatorTypes();

	OPENIVM_DEBUG_PRINT("[PlanRewrite] Derived aggregates → hidden columns, %zu decompositions\n", decomps.size());
}

} // namespace duckdb
