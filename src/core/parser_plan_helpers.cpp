#include "core/vector_utils.hpp"
#include "core/parser_plan_helpers.hpp"
#include "core/metadata_json.hpp"

#include "core/openivm_constants.hpp"
#include "core/openivm_debug.hpp"
#include "core/plan_rewrite.hpp"
#include "core/plan_rewrite_internal.hpp"
#include "core/refresh_metadata.hpp"
#include "core/sql_utils.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "lpts_expression_renderer.hpp"
#include "lpts_pipeline.hpp"
#include "lpts_helpers.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "rules/column_hider.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/bound_ref_wrapper.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/bind_context.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/optimizer/unnest_rewriter.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/cte_inlining.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_table_entry.hpp"

#include <set>
#include <cmath>
#include <limits>

namespace duckdb {

namespace {

struct SourceColumnsBindData : public TableFunctionData {
	explicit SourceColumnsBindData(TableCatalogEntry &table) : table(table) {
	}
	TableCatalogEntry &table;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SourceColumnsBindData>(table);
	}
	bool Equals(const FunctionData &other) const override {
		return &table == &other.Cast<SourceColumnsBindData>().table;
	}
};

static void RewriteSourceColumns(Binder &binder, SQLStatement &statement);

} // namespace

static Vector CanonicalizeNaNKeys(const Vector &input, idx_t count) {
	if (!TypeVisitor::Contains(input.GetType(), [](const LogicalType &type) { return type.IsFloating(); })) {
		return Vector::Ref(input);
	}
	Vector result(input.GetType(), count);
	result.Append(input, count);
	switch (input.GetType().InternalType()) {
	case PhysicalType::FLOAT: {
		auto values = FlatVector::GetDataMutable<float>(result);
		for (idx_t row = 0; row < count; row++) {
			if (FlatVector::Validity(result).RowIsValid(row) && std::isnan(values[row])) {
				values[row] = std::numeric_limits<float>::quiet_NaN();
			}
		}
		break;
	}
	case PhysicalType::DOUBLE: {
		auto values = FlatVector::GetDataMutable<double>(result);
		for (idx_t row = 0; row < count; row++) {
			if (FlatVector::Validity(result).RowIsValid(row) && std::isnan(values[row])) {
				values[row] = std::numeric_limits<double>::quiet_NaN();
			}
		}
		break;
	}
	case PhysicalType::STRUCT:
		for (auto &child : StructVector::GetEntries(result)) {
			child.Reference(CanonicalizeNaNKeys(child, count));
		}
		break;
	case PhysicalType::LIST: {
		auto &child = ListVector::GetChildMutable(result);
		child.Reference(CanonicalizeNaNKeys(child, child.size()));
		break;
	}
	case PhysicalType::ARRAY: {
		auto &child = ArrayVector::GetChildMutable(result);
		child.Reference(CanonicalizeNaNKeys(child, child.size()));
		break;
	}
	default:
		throw InternalException("Unexpected nested floating candidate key type");
	}
	return result;
}

static void CandidateValueKey(DataChunk &input, ExpressionState &, Vector &result) {
	// SQL equality can hide payload types, interval components, and the sign of zero.
	auto canonical = TypeContainsVariant(input.data[0].GetType())
	                     ? CanonicalizeVariantVector(input.data[0], input.size())
	                     : Vector::Ref(input.data[0]);
	// NaN signs/payloads are unspecified; their state identity must survive
	// storage canonicalization. Signed zero and VARIANT payload types stay distinct.
	auto identity = CanonicalizeNaNKeys(canonical, input.size());
	auto output = FlatVector::GetDataMutable<string_t>(result);
	MemoryStream stream;
	for (idx_t row = 0; row < input.size(); row++) {
		stream.Rewind();
		BinarySerializer::Serialize(identity.GetValue(row), stream);
		output[row] =
		    StringVector::AddStringOrBlob(result, const_char_ptr_cast(stream.GetData()), stream.GetPosition());
	}
}

ScalarFunction CandidateValueKeyFunction() {
	return ScalarFunction("openivm_internal_value_key", {LogicalType::ANY}, LogicalType::BLOB, CandidateValueKey);
}

TableFunction SourceColumnsFunction() {
	TableFunction function("openivm_source_columns", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       [](ClientContext &, TableFunctionInput &, DataChunk &) {
		                       throw InvalidInputException("OpenIVM source-column IR cannot execute directly");
	                       });
	function.bind_replace = [](ClientContext &context, TableFunctionBindInput &input) -> unique_ptr<TableRef> {
		auto entry = Catalog::GetEntry(
		    context,
		    SqlUtils::QualifyLookup(
		        EntryLookupInfo(CatalogType::TABLE_ENTRY, QualifiedName(Identifier(input.inputs[2].ToString()))),
		        Identifier(input.inputs[0].ToString()), Identifier(input.inputs[1].ToString())),
		    OnEntryNotFound::THROW_EXCEPTION);
		if (entry->type == CatalogType::VIEW_ENTRY) {
			auto &view = entry->Cast<ViewCatalogEntry>();
			auto view_binder = Binder::CreateBinder(context, input.binder, BinderType::VIEW_BINDER);
			view_binder->SetCanContainNulls(true);
			view_binder->SetSearchPath(view.ParentCatalog(), view.ParentSchema().name);
			view_binder->AddBoundView(view);
			auto query = view.GetQuery().Copy();
			RewriteSourceColumns(*view_binder, *query);
			SubqueryRef subquery(unique_ptr_cast<SQLStatement, SelectStatement>(std::move(query)));
			subquery.alias = input.ref.alias;
			subquery.column_name_alias = view.aliases;
			for (idx_t i = 0; i < input.ref.column_name_alias.size(); i++) {
				if (i < subquery.column_name_alias.size()) {
					subquery.column_name_alias[i] = input.ref.column_name_alias[i];
				} else {
					subquery.column_name_alias.push_back(input.ref.column_name_alias[i]);
				}
			}
			auto bound = view_binder->Bind(static_cast<TableRef &>(subquery));
			if (!view_binder->correlated_columns.empty()) {
				throw BinderException("Contents of view were altered - view bound correlated columns");
			}
			view.UpdateBinding(bound.types, bound.names);
			auto binder = Binder::CreateBinder(context, input.binder);
			binder->bind_context.AddView(bound.plan->GetRootIndex(), input.ref.alias, subquery, bound, view);
			OPENIVM_DEBUG_PRINT("[SOURCE COLUMNS] Bound view %s in its own search path\n", view.name.c_str());
			return make_uniq<BoundRefWrapper>(std::move(bound), std::move(binder));
		}
		auto &table = entry->Cast<TableCatalogEntry>();
		auto binder = Binder::CreateBinder(context, input.binder);
		auto index = binder->GenerateTableIndex();
		BoundStatement bound;
		vector<ColumnIndex> columns;
		for (auto &column : table.GetColumns().Logical()) {
			bound.names.push_back(column.Name());
			bound.types.push_back(column.Type());
			columns.emplace_back(column.Oid());
		}
		auto function = SourceColumnsFunction();
		auto get = make_uniq<LogicalGet>(index, BoundTableFunction(function), make_uniq<SourceColumnsBindData>(table),
		                                 bound.types, bound.names);
		get->SetColumnIds(std::move(columns));
		bound.plan = std::move(get);
		auto binding_names = BindContext::AliasColumnNames(table.name, bound.names, input.ref.column_name_alias);

		binder->bind_context.AddEntryBinding(index, input.ref.alias, binding_names, bound.types, table);
		OPENIVM_DEBUG_PRINT("[SOURCE COLUMNS] Preserving generated identities in %s.%s.%s\n",
		                    table.catalog.GetName().c_str(), table.schema.name.GetIdentifierName().c_str(),
		                    table.name.GetIdentifierName().c_str());
		return make_uniq<BoundRefWrapper>(std::move(bound), std::move(binder));
	};
	function.projection_pushdown = true;
	function.get_table_entry = [](const optional_ptr<const FunctionData> data) -> optional_ptr<TableCatalogEntry> {
		return data->Cast<SourceColumnsBindData>().table;
	};
	function.serialize = [](Serializer &serializer, const optional_ptr<FunctionData> data, const BoundTableFunction &) {
		auto &table = data->Cast<SourceColumnsBindData>().table;
		serializer.WriteProperty(100, "catalog", table.catalog.GetName());
		serializer.WriteProperty(101, "schema", table.schema.name.GetIdentifierName());
		serializer.WriteProperty(102, "table", table.name.GetIdentifierName());
	};
	function.deserialize = [](Deserializer &deserializer, BoundTableFunction &) -> unique_ptr<FunctionData> {
		auto catalog = deserializer.ReadProperty<string>(100, "catalog");
		auto schema = deserializer.ReadProperty<string>(101, "schema");
		auto name = deserializer.ReadProperty<string>(102, "table");
		auto table = Catalog::GetEntry<TableCatalogEntry>(
		    deserializer.Get<ClientContext &>(),
		    QualifiedName(Identifier(catalog), Identifier(schema), Identifier(name)), OnEntryNotFound::THROW_EXCEPTION);
		return make_uniq<SourceColumnsBindData>(*table);
	};
	return function;
}

namespace {

static void RewriteSourceColumns(Binder &binder, SQLStatement &statement) {
	// Bind generated columns as source values, not their current definitions. This
	// plan is compiler IR only: LPTS emits ordinary source SQL, while delta scans
	// read the materialized generated values captured with each signed change.
	SqlUtils::RewriteSourceReferences(statement, [&](unique_ptr<TableRef> &ref) {
		auto &base = ref->Cast<BaseTableRef>();
		if (base.at_clause) {
			return;
		}
		auto catalog = base.GetQualifiedName().Catalog().GetIdentifierName();
		auto schema = base.GetQualifiedName().Schema().GetIdentifierName();
		// The context-only resolver uses the caller's path. A view must check
		// schema/catalog ambiguity against its own entry retriever instead.
		if (catalog.empty() && !schema.empty() &&
		    DatabaseManager::Get(binder.context).GetDatabase(binder.context, Identifier(schema))) {
			auto local = binder.EntryRetriever().GetSchema(
			    EntryLookupInfo(CatalogType::SCHEMA_ENTRY, QualifiedName(Identifier(schema))),
			    OnEntryNotFound::RETURN_NULL);
			if (local) {
				throw BinderException(
				    "Ambiguous reference to catalog or schema \"%s\" - use a fully qualified path like \"%s.%s\"",
				    schema, local->ParentCatalog().GetName(), schema);
			}
			catalog = schema;
			schema.clear();
		}
		auto entry = binder.EntryRetriever().GetEntry(
		    SqlUtils::QualifyLookup(
		        EntryLookupInfo(CatalogType::TABLE_ENTRY, QualifiedName(Identifier(base.Table().GetIdentifierName()))),
		        Identifier(catalog), Identifier(schema)),
		    OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			return;
		}
		if (entry->type != CatalogType::VIEW_ENTRY) {
			if (entry->type != CatalogType::TABLE_ENTRY || !entry->ParentCatalog().IsDuckCatalog()) {
				return;
			}
			auto &table = entry->Cast<TableCatalogEntry>();
			if (table.GetColumns().LogicalColumnCount() == table.GetColumns().PhysicalColumnCount()) {
				return;
			}
		}
		auto function = make_uniq<TableFunctionRef>();
		vector<unique_ptr<ParsedExpression>> arguments;
		for (auto &name : {entry->ParentCatalog().GetName(), entry->ParentSchema().name, entry->name}) {
			arguments.push_back(ConstantExpression::FromValue(Value(name)));
		}
		function->function = make_uniq<FunctionExpression>("openivm_source_columns", std::move(arguments));
		// CopyProperties requires equal ref types and omits column-name aliases.
		function->alias = std::move(ref->alias);
		function->column_name_alias = std::move(ref->column_name_alias);
		function->query_location = ref->query_location;
		function->sample = std::move(ref->sample);
		function->external_dependency = std::move(ref->external_dependency);
		ref = std::move(function);
	});
}

} // namespace

void PlanWithSourceColumns(Planner &planner, unique_ptr<SQLStatement> statement) {
	RewriteSourceColumns(*planner.binder, *statement);
	planner.CreatePlan(std::move(statement));
}

void StripPublicationModifiers(ClientContext &context, unique_ptr<LogicalOperator> &plan, vector<string> &output_names,
                               string &suffix, string &ordering) {
	vector<LogicalProjection *> projections;
	auto *slot = &plan;
	while (*slot && (*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*slot)->children.size() == 1) {
		projections.push_back(&(*slot)->Cast<LogicalProjection>());
		slot = &(*slot)->children[0];
	}
	if (!*slot) {
		return;
	}
	vector<BoundOrderByNode> *orders = nullptr;
	idx_t limit = DConstants::INVALID_INDEX, offset = 0;
	double limit_percentage = -1;
	unique_ptr<LogicalOperator> *order_slot = nullptr;
	if ((*slot)->type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &top = (*slot)->Cast<LogicalTopN>();
		orders = &top.orders;
		limit = top.limit;
		offset = top.offset;
	} else if ((*slot)->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &top = (*slot)->Cast<LogicalLimit>();
		if ((top.limit_val.Type() != LimitNodeType::UNSET && top.limit_val.Type() != LimitNodeType::CONSTANT_VALUE &&
		     top.limit_val.Type() != LimitNodeType::CONSTANT_PERCENTAGE) ||
		    (top.offset_val.Type() != LimitNodeType::UNSET && top.offset_val.Type() != LimitNodeType::CONSTANT_VALUE)) {
			return;
		}
		if (top.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
			limit = top.limit_val.GetConstantValue();
		} else if (top.limit_val.Type() == LimitNodeType::CONSTANT_PERCENTAGE) {
			limit_percentage = top.limit_val.GetConstantPercentage();
		}
		if (top.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
			offset = top.offset_val.GetConstantValue();
		}
		auto *candidate = &(*slot)->children[0];
		while ((*candidate)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*candidate)->children.size() == 1) {
			projections.push_back(&(*candidate)->Cast<LogicalProjection>());
			candidate = &(*candidate)->children[0];
		}
		if ((*candidate)->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
			order_slot = candidate;
			orders = &(*candidate)->Cast<LogicalOrder>().orders;
		}
	} else if ((*slot)->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		orders = &(*slot)->Cast<LogicalOrder>().orders;
	} else {
		return;
	}
	if (orders) {
		// Publication expressions only refer to the unary output path. Avoid
		// re-running the full classifier (including source/join analysis) here.
		CreateMVPlanFacts facts;
		for (auto *node = plan.get(); node; node = node->children.size() == 1 ? node->children[0].get() : nullptr) {
			if (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				facts.projections.push_back(&node->Cast<LogicalProjection>());
			} else if (node->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
				facts.aggregates.push_back(&node->Cast<LogicalAggregate>());
			}
		}
		vector<string> order_sql;
		for (idx_t i = 0; i < orders->size(); i++) {
			auto &order = (*orders)[i];
			string expression;
			if (!RenderPlanOutputExpression(context, *order.expression, *plan, facts, output_names, expression)) {
				// The outer projection hides a sort value (e.g. SELECT k ORDER BY SUM(v)).
				// Keep it in maintenance state; only its ordinal crosses the public boundary.
				D_ASSERT(!projections.empty());
				auto alias = string(openivm::SORT_VALUE_PREFIX) + to_string(i);
				auto &projection = *projections.back();
				auto value = order.expression->Copy();
				value->SetAlias(Identifier(alias));
				auto type = value->GetReturnType();
				ColumnBinding binding {projection.table_index, ProjectionIndex(projection.expressions.size())};
				projection.expressions.push_back(std::move(value));
				projection.ResolveOperatorTypes();
				vector<LogicalProjection *> ancestors(projections.begin(), projections.end() - 1);
				PropagateHiddenBindingThroughProjectionPath(ancestors, binding, type, alias);
				output_names.push_back(alias);
				expression = SqlUtils::QuoteIdentifier(alias);
			}
			order_sql.push_back(expression + " " + order.GetOrderModifier());
		}
		ordering = "ORDER BY " + StringUtil::Join(order_sql, ", ");
	}
	suffix = ordering;
	if (limit != DConstants::INVALID_INDEX) {
		suffix += " LIMIT " + to_string(limit);
	} else if (limit_percentage >= 0) {
		suffix += " LIMIT " + Value::DOUBLE(limit_percentage).ToSQLString() + " PERCENT";
	}
	if (offset > 0) {
		suffix += " OFFSET " + to_string(offset);
	}
	if (order_slot) {
		*order_slot = std::move((*order_slot)->children[0]);
	}
	*slot = std::move((*slot)->children[0]);
	plan->ResolveOperatorTypes();
	OPENIVM_DEBUG_PRINT("[CREATE MV] Extracted publication modifiers: %s\n", suffix.c_str());
}

bool IsDerivedAggregate(const BoundAggregateExpression &aggregate) {
	const auto &name = aggregate.Function().GetName().GetIdentifierName();
	return name == "avg" || name == "stddev" || name == "stddev_samp" || name == "stddev_pop" || name == "variance" ||
	       name == "var_samp" || name == "var_pop";
}

static bool PrepareCtesForInlining(LogicalOperator *op, PlanRewriteNeeds &needs) {
	if (!op) {
		return false;
	}
	bool found_cte = op->type == LogicalOperatorType::LOGICAL_CTE_REF;
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		needs.has_aggregate = true;
		auto &aggregate = op->Cast<LogicalAggregate>();
		for (auto &expression : aggregate.expressions) {
			if (expression->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				continue;
			}
			auto &bound_aggregate = expression->Cast<BoundAggregateExpression>();
			needs.aggregate_filters = needs.aggregate_filters || bound_aggregate.GetFilter();
			needs.derived_aggregates = needs.derived_aggregates || IsDerivedAggregate(bound_aggregate);
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_WINDOW) {
		for (auto &expression : op->expressions) {
			auto &window = expression->Cast<BoundWindowExpression>();
			needs.running_window_state = needs.running_window_state || IsRunningWindowCandidate(window);
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_DISTINCT) {
		needs.distinct = true;
	} else if (op->type == LogicalOperatorType::LOGICAL_INTERSECT || op->type == LogicalOperatorType::LOGICAL_EXCEPT) {
		// INTERSECT/EXCEPT are full-refresh boundaries. Their positional output
		// cannot carry branch-local hidden outer-join state, and full refresh does
		// not need that state in the first place.
		needs.blocking_set_operation = true;
	} else if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		needs.fold_constant_scalar_subqueries = true;
	} else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	           op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op->Cast<LogicalComparisonJoin>();
		needs.outer_join_support =
		    needs.outer_join_support || (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
		                                 (join.join_type == JoinType::LEFT || join.join_type == JoinType::RIGHT ||
		                                  join.join_type == JoinType::OUTER));
		needs.semi_anti_subqueries = needs.semi_anti_subqueries || join.join_type == JoinType::MARK ||
		                             join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI ||
		                             join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI;
	}
	if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		found_cte = true;
		needs.inline_cte_refs = true;
		auto &cte = op->Cast<LogicalMaterializedCTE>();
		if (cte.materialize == CTEMaterialize::CTE_MATERIALIZE_ALWAYS) {
			cte.materialize = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
		}
	}
	for (auto &child : op->children) {
		found_cte = PrepareCtesForInlining(child.get(), needs) || found_cte;
	}
	return found_cte;
}

// DuckDB 2.0 represents correlated UNNEST as a table-in/out GET whose input
// projection computes the list expression. Its native CTE UNNEST rewriter only
// converts that GET when the projection slot is a column reference. Convert the
// deterministic computed slot to the equivalent LogicalUnnest first, then let
// the native rewriter remove the domain/deduplication CTEs.
static void NormalizeComputedUnnestGet(ClientContext &context, LogicalOperator &root,
                                       unique_ptr<LogicalOperator> &node) {
	for (auto &child : node->children) {
		NormalizeComputedUnnestGet(context, root, child);
	}
	if (node->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = node->Cast<LogicalGet>();
	if (!ExpressionBinder::IsUnnestFunction(get.function.GetName()) || get.ordinality_idx.IsValid() ||
	    node->children.size() != 1 || node->children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	auto &projection = node->children[0]->Cast<LogicalProjection>();
	if (projection.children.size() != 1) {
		return;
	}
	auto bindings = node->GetColumnBindings();
	auto indexes = node->GetTableIndex();
	if (indexes.empty()) {
		return;
	}
	auto unnest_index = indexes[0];
	node->ResolveOperatorTypes();
	bool has_computed_input = false;
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (bindings[i].table_index != unnest_index) {
			continue;
		}
		auto column = bindings[i].column_index.GetIndex();
		if (column >= projection.expressions.size() || projection.expressions[column]->IsVolatile()) {
			return;
		}
		has_computed_input = has_computed_input ||
		                     projection.expressions[column]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF;
	}
	if (!has_computed_input) {
		return;
	}

	auto get_node = std::move(node);
	auto &input_projection = get_node->children[0]->Cast<LogicalProjection>();
	auto input = std::move(input_projection.children[0]);
	auto unnest = make_uniq<LogicalUnnest>(unnest_index);
	unnest->children.push_back(std::move(input));
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < bindings.size(); i++) {
		auto &binding = bindings[i];
		if (binding.table_index != unnest_index) {
			continue;
		}
		auto column = binding.column_index.GetIndex();
		auto &input_expression = input_projection.expressions[column];
		auto alias = input_expression->GetAlias();
		auto unnest_expression = make_uniq<BoundUnnestExpression>(get_node->types[i]);
		unnest_expression->ChildMutable() = BoundCastExpression::AddArrayCastToList(context, input_expression->Copy());
		auto output = ColumnBinding::PushExpression(unnest->expressions, std::move(unnest_expression));
		input_expression =
		    make_uniq<BoundColumnRefExpression>(alias, get_node->types[i], ColumnBinding(unnest_index, output));
		input_projection.types[column] = get_node->types[i];
		replacer.replacement_bindings.emplace_back(
		    binding, ColumnBinding(input_projection.table_index, ProjectionIndex(column)), get_node->types[i]);
	}
	input_projection.children[0] = std::move(unnest);
	node = std::move(get_node->children[0]);
	replacer.stop_operator = *node;
	replacer.VisitOperator(root);
	OPENIVM_DEBUG_PRINT("[PlanRewrite] Normalized computed table-in/out UNNEST input\n");
}

// Native UNNEST normalization accepts one correlation condition. Pack the
// generated null-safe key tuple into one equivalent comparison, retaining every
// key (including NULLs) so the native rewriter can remove the deduplication join.
static bool ContainsUnnestGet(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET &&
	    ExpressionBinder::IsUnnestFunction(op.Cast<LogicalGet>().function.GetName())) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsUnnestGet(*child)) {
			return true;
		}
	}
	return false;
}

static void PackUnnestCorrelationKeys(ClientContext &context, LogicalOperator &op, TableIndex domain = TableIndex()) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		auto &cte = op.Cast<LogicalMaterializedCTE>();
		if (StringUtil::StartsWith(cte.ctename.GetIdentifierName(), "__duckdb_delim_") &&
		    !StringUtil::StartsWith(cte.ctename.GetIdentifierName(), "__duckdb_delim_dedup_")) {
			domain = cte.table_index;
		}
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		bool domain_join = false;
		for (auto &child : join.children) {
			domain_join |=
			    child->type == LogicalOperatorType::LOGICAL_CTE_REF && child->Cast<LogicalCTERef>().cte_index == domain;
		}
		if (domain_join && ContainsUnnestGet(join) && join.join_type == JoinType::INNER && join.conditions.size() > 1 &&
		    std::all_of(join.conditions.begin(), join.conditions.end(), [](const JoinCondition &condition) {
			    return condition.IsComparison() &&
			           condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
		    })) {
			vector<unique_ptr<Expression>> left, right;
			for (idx_t i = 0; i < join.conditions.size(); i++) {
				auto &condition = join.conditions[i];
				left.push_back(condition.LeftReference()->Copy());
				right.push_back(condition.RightReference()->Copy());
				left.back()->SetAlias(Identifier("key_" + to_string(i)));
				right.back()->SetAlias(Identifier("key_" + to_string(i)));
			}
			FunctionBinder binder(context);
			auto lhs =
			    binder.BindScalarFunction(Identifier(DEFAULT_SCHEMA), Identifier("struct_pack"), std::move(left));
			auto rhs =
			    binder.BindScalarFunction(Identifier(DEFAULT_SCHEMA), Identifier("struct_pack"), std::move(right));
			join.conditions.clear();
			join.conditions.emplace_back(std::move(lhs), std::move(rhs), ExpressionType::COMPARE_NOT_DISTINCT_FROM);
			OPENIVM_DEBUG_PRINT("[PlanRewrite] Packed native UNNEST correlation keys\n");
		}
	}
	for (auto &child : op.children) {
		PackUnnestCorrelationKeys(context, *child, domain);
	}
}

// A deterministic correlated table function is row-local even though DuckDB
// 2.0 decorrelates it into domain CTE -> DISTINCT keys -> function -> join.
// Restore the row-local form so the source delta flows through the function
// directly, while carrying every source column needed above the removed join.
static bool NormalizeCorrelatedTableFunctionCTE(unique_ptr<LogicalOperator> &candidate) {
	for (auto &child : candidate->children) {
		if (NormalizeCorrelatedTableFunctionCTE(child)) {
			return true;
		}
	}
	if (candidate->type != LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return false;
	}
	auto &domain = candidate->Cast<LogicalMaterializedCTE>();
	if (domain.children.size() != 2) {
		return false;
	}

	auto join_owner = &domain.children[1];
	while ((*join_owner)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*join_owner)->children.size() == 1) {
		join_owner = &(*join_owner)->children[0];
	}
	if ((*join_owner)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*join_owner)->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER || join.children.size() != 2) {
		return false;
	}
	idx_t domain_side;
	if (join.children[0]->type == LogicalOperatorType::LOGICAL_CTE_REF &&
	    join.children[0]->Cast<LogicalCTERef>().cte_index == domain.table_index) {
		domain_side = 0;
	} else if (join.children[1]->type == LogicalOperatorType::LOGICAL_CTE_REF &&
	           join.children[1]->Cast<LogicalCTERef>().cte_index == domain.table_index) {
		domain_side = 1;
	} else {
		return false;
	}
	auto function_side = 1 - domain_side;
	vector<reference<LogicalProjection>> output_projections;
	auto function_owner = &join.children[function_side];
	while ((*function_owner)->type == LogicalOperatorType::LOGICAL_PROJECTION &&
	       (*function_owner)->children.size() == 1) {
		output_projections.push_back((*function_owner)->Cast<LogicalProjection>());
		function_owner = &(*function_owner)->children[0];
	}
	if ((*function_owner)->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = (*function_owner)->Cast<LogicalGet>();
	auto function_name = get.function.GetName().GetIdentifierName();
	auto repeatable = ClassifyLogicalOperatorRepeatability(get) == LogicalOperatorRepeatability::REPEATABLE ||
	                  StringUtil::CIEquals(function_name, "generate_series") ||
	                  StringUtil::CIEquals(function_name, "range");
	if (ExpressionBinder::IsUnnestFunction(get.function.GetName()) || get.GetTable() || !repeatable ||
	    get.children.size() != 1) {
		return false;
	}

	vector<reference<LogicalProjection>> projections;
	auto dedup_owner = &get.children[0];
	while ((*dedup_owner)->type == LogicalOperatorType::LOGICAL_PROJECTION && (*dedup_owner)->children.size() == 1) {
		projections.push_back((*dedup_owner)->Cast<LogicalProjection>());
		dedup_owner = &(*dedup_owner)->children[0];
	}
	if ((*dedup_owner)->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || projections.empty()) {
		return false;
	}
	auto &dedup = (*dedup_owner)->Cast<LogicalAggregate>();
	if (dedup.children.size() != 1 || dedup.children[0]->type != LogicalOperatorType::LOGICAL_CTE_REF ||
	    dedup.children[0]->Cast<LogicalCTERef>().cte_index != domain.table_index || dedup.groups.empty()) {
		return false;
	}

	domain.children[0]->ResolveOperatorTypes();
	auto source_bindings = domain.children[0]->GetColumnBindings();
	auto source_types = domain.children[0]->types;
	auto domain_ref_bindings = join.children[domain_side]->GetColumnBindings();
	auto dedup_ref_bindings = dedup.children[0]->GetColumnBindings();
	auto dedup_bindings = dedup.GetColumnBindings();
	if (source_bindings.size() != domain_ref_bindings.size()) {
		return false;
	}

	ColumnBindingReplacer dedup_replacer;
	for (idx_t i = 0; i < dedup.groups.size(); i++) {
		if (dedup.groups[i]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto input_binding = dedup.groups[i]->Cast<BoundColumnRefExpression>().Binding();
		auto entry = std::find(dedup_ref_bindings.begin(), dedup_ref_bindings.end(), input_binding);
		if (entry == dedup_ref_bindings.end()) {
			return false;
		}
		auto source_index = NumericCast<idx_t>(entry - dedup_ref_bindings.begin());
		dedup_replacer.replacement_bindings.emplace_back(dedup_bindings[i], source_bindings[source_index],
		                                                 source_types[source_index]);
	}
	*dedup_owner = std::move(domain.children[0]);
	dedup_replacer.VisitOperator(*get.children[0]);

	// Carry every domain column through the argument projections. The table-in/out
	// GET exposes selected child bindings through projected_input.
	auto carried_bindings = source_bindings;
	auto carry_through = [&](const vector<reference<LogicalProjection>> &path) {
		bool appended = false;
		for (auto it = path.rbegin(); it != path.rend(); ++it) {
			auto &projection = it->get();
			for (idx_t i = 0; i < carried_bindings.size(); i++) {
				carried_bindings[i] =
				    ExposeBindingInProjection(projection, carried_bindings[i], source_types[i], "", true, appended);
			}
			projection.ResolveOperatorTypes();
		}
	};
	carry_through(projections);
	auto function_input_bindings = get.children[0]->GetColumnBindings();
	for (auto &binding : carried_bindings) {
		auto entry = std::find(function_input_bindings.begin(), function_input_bindings.end(), binding);
		if (entry == function_input_bindings.end()) {
			return false;
		}
		auto input = NumericCast<idx_t>(entry - function_input_bindings.begin());
		if (std::find(get.projected_input.begin(), get.projected_input.end(), input) == get.projected_input.end()) {
			get.projected_input.push_back(input);
		}
	}
	get.ResolveOperatorTypes();
	carry_through(output_projections);

	ColumnBindingReplacer domain_replacer;
	for (idx_t i = 0; i < domain_ref_bindings.size(); i++) {
		domain_replacer.replacement_bindings.emplace_back(domain_ref_bindings[i], carried_bindings[i], source_types[i]);
	}
	*join_owner = std::move(join.children[function_side]);
	candidate = std::move(domain.children[1]);
	domain_replacer.VisitOperator(*candidate);
	OPENIVM_DEBUG_PRINT("[PlanRewrite] Restored row-local correlated table function\n");
	return true;
}

PlanRewriteNeeds InlineCtesIfPresent(ClientContext &context, Binder &binder, unique_ptr<LogicalOperator> &plan) {
	PlanRewriteNeeds needs;
	if (PrepareCtesForInlining(plan.get(), needs)) {
		PackUnnestCorrelationKeys(context, *plan);
		NormalizeComputedUnnestGet(context, *plan, plan);
		Optimizer cte_opt(binder, context);
		CTEInlining cte_inlining(cte_opt);
		plan = cte_inlining.Optimize(std::move(plan));
		while (NormalizeCorrelatedTableFunctionCTE(plan)) {
		}
		// Inline the single-use dedup CTE before native UNNEST normalization.
		// DuckDB 2.0's explicit nested-CTE branch temporarily leaves a null
		// domain child while updating bindings; the equivalent inline-dedup
		// branch replaces the domain node before walking the plan.
		UnnestRewriter unnest_rewriter(context);
		plan = unnest_rewriter.Optimize(std::move(plan));
		plan = cte_inlining.Optimize(std::move(plan));
	}
	return needs;
}

string QualifyCreateSourceTable(ClientContext &context, const string &table_name, const CreateMVPlanFacts &facts,
                                const string &current_catalog, const string &current_schema, const string &default_db,
                                const string &default_schema) {
	// The staged executor builds auxiliary state with the metadata schema selected. Name the
	// relation the view actually bound, which can come from any schema on the search path.
	vector<string> parts;
	try {
		parts = SqlUtils::ParseQualifiedIdentifier(table_name);
	} catch (const ParserException &) {
		// Table functions are SQL expressions rather than relation identifiers.
		return table_name;
	}
	if (parts.size() > 3) {
		return table_name;
	}
	const auto &source_name = parts.back();
	const string source_schema = parts.size() >= 2 ? parts[parts.size() - 2] : INVALID_SCHEMA;
	const string source_catalog = parts.size() == 3 ? parts[0] : INVALID_CATALOG;
	// Resolve public views before consulting physical scan facts: an MV binds to its
	// publication table, whose name differs from the caller's quoted SQL identifier.
	QueryErrorContext error_context;
	for (auto type : {CatalogType::TABLE_ENTRY, CatalogType::VIEW_ENTRY}) {
		auto entry = Catalog::GetEntry(
		    context,
		    SqlUtils::QualifyLookup(EntryLookupInfo(type, QualifiedName(Identifier(source_name)), error_context),
		                            Identifier(source_catalog), Identifier(source_schema)),
		    OnEntryNotFound::RETURN_NULL);
		if (entry) {
			auto qualified =
			    SqlUtils::FullName(entry->ParentCatalog().GetName(), entry->ParentSchema().name, entry->name);
			OPENIVM_DEBUG_PRINT("[CREATE MV] Auxiliary source '%s' resolves to %s\n", table_name.c_str(),
			                    qualified.c_str());
			return qualified;
		}
	}
	const SourceTableInfo *bound = nullptr;
	bool bound_in_current_schema = false;
	for (auto &entry : facts.source_table_info) {
		auto &source = entry.second;
		if (!StringUtil::CIEquals(source.table_name, source_name) ||
		    (parts.size() >= 2 && !StringUtil::CIEquals(source.schema_name, source_schema)) ||
		    (parts.size() == 3 && !StringUtil::CIEquals(source.catalog_name, source_catalog))) {
			continue;
		}
		bool in_current_schema = StringUtil::CIEquals(source.catalog_name, current_catalog) &&
		                         StringUtil::CIEquals(source.schema_name, current_schema);
		// Same-named sources prefer the caller's schema, then a deterministic order.
		if (!bound || (in_current_schema && !bound_in_current_schema) ||
		    (in_current_schema == bound_in_current_schema &&
		     SqlUtils::FullName(source.catalog_name, source.schema_name, source.table_name) <
		         SqlUtils::FullName(bound->catalog_name, bound->schema_name, bound->table_name))) {
			bound = &source;
			bound_in_current_schema = in_current_schema;
		}
	}
	if (bound) {
		OPENIVM_DEBUG_PRINT("[CREATE MV] Auxiliary source '%s' bound to %s\n", table_name.c_str(),
		                    SqlUtils::FullName(bound->catalog_name, bound->schema_name, bound->table_name).c_str());
		return SqlUtils::FullName(bound->catalog_name, bound->schema_name, bound->table_name);
	}
	if (parts.size() > 1) {
		return table_name;
	}
	if (current_catalog.empty() || (current_catalog == default_db && current_schema == default_schema)) {
		return table_name;
	}
	return SqlUtils::QuoteIdentifier(current_catalog) + "." + SqlUtils::QuoteIdentifier(current_schema) + "." +
	       table_name;
}

// LPTS rendering of a plan in DuckDB's dialect, without the trailing ';'.
static string RenderPlanSQL(ClientContext &context, unique_ptr<LogicalOperator> &plan, const vector<string> &columns) {
	auto ast =
	    LogicalPlanToAst(context, plan, SqlDialect::DUCKDB, {}, CommonSubplanReusePolicy::PRESERVE_SOURCE_OCCURRENCES);
	return SqlUtils::StripTrailingSemicolon(AstToCteList(*ast, SqlDialect::DUCKDB)->ToQuery(true, columns));
}

void AddJoinKeyColumn(const unique_ptr<Expression> &expr,
                      unordered_map<TableIndex, unordered_set<ProjectionIndex>> &join_key_cols) {
	if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return;
	}
	auto &bcr = expr->Cast<BoundColumnRefExpression>();
	join_key_cols[bcr.Binding().table_index].insert(bcr.Binding().column_index);
}

static string BindingKey(const BoundColumnRefExpression &bcr) {
	return to_string(bcr.Binding().table_index.index) + ":" + to_string(bcr.Binding().column_index);
}

static string FindBindingParent(unordered_map<string, string> &parents, const string &key) {
	auto it = parents.find(key);
	if (it == parents.end()) {
		parents[key] = key;
		return key;
	}
	if (it->second == key) {
		return key;
	}
	it->second = FindBindingParent(parents, it->second);
	return it->second;
}

static void UnionBindings(unordered_map<string, string> &parents, const string &left, const string &right) {
	string left_parent = FindBindingParent(parents, left);
	string right_parent = FindBindingParent(parents, right);
	if (left_parent != right_parent) {
		parents[right_parent] = left_parent;
	}
}

static bool IsPurePassThroughExpression(const Expression *expr) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		return true;
	}
	if (BoundCastExpression::IsCast(*expr)) {
		auto &cast = expr->Cast<BoundFunctionExpression>();
		return IsPurePassThroughExpression(&BoundCastExpression::Child(cast));
	}
	return false;
}

static bool ExpressionReferencesNonGroupBinding(Expression *expr, TableIndex group_index) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &bcr = expr->Cast<BoundColumnRefExpression>();
		return bcr.Binding().table_index != group_index;
	}
	bool refs_non_group = false;
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		if (!refs_non_group && ExpressionReferencesNonGroupBinding(child.get(), group_index)) {
			refs_non_group = true;
		}
	});
	return refs_non_group;
}

bool OuterJoinAggregateNeedsRecompute(const CreateMVPlanFacts &facts, TableIndex group_index) {
	for (auto *agg_ptr : facts.aggregates) {
		auto &agg = *agg_ptr;
		for (auto &expr : agg.expressions) {
			if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				continue;
			}
			auto &bound = expr->Cast<BoundAggregateExpression>();
			for (auto &child : bound.GetChildren()) {
				if (!IsPurePassThroughExpression(child.get())) {
					return true;
				}
			}
		}
	}
	for (auto *proj_ptr : facts.projections) {
		auto &proj = *proj_ptr;
		for (auto &expr : proj.expressions) {
			if (!IsPurePassThroughExpression(expr.get()) &&
			    ExpressionReferencesNonGroupBinding(expr.get(), group_index)) {
				return true;
			}
		}
	}
	return false;
}

static const CreateMVPlanNodeFacts *GetPlanNodeFacts(const CreateMVPlanFacts &facts, const LogicalOperator *node) {
	auto entry = facts.plan_node_ids.find(node);
	if (entry == facts.plan_node_ids.end()) {
		return nullptr;
	}
	D_ASSERT(entry->second < facts.plan_nodes_post_order.size());
	return &facts.plan_nodes_post_order[entry->second];
}

bool OuterJoinPreservedSideHasTableFunction(const CreateMVPlanFacts &facts) {
	for (auto *join : facts.comparison_joins) {
		idx_t preserved_child;
		if (join->join_type == JoinType::LEFT) {
			preserved_child = 0;
		} else if (join->join_type == JoinType::RIGHT) {
			preserved_child = 1;
		} else {
			continue;
		}
		if (join->children.size() <= preserved_child) {
			continue;
		}
		auto *subtree = GetPlanNodeFacts(facts, join->children[preserved_child].get());
		if (subtree && subtree->contains_table_function) {
			return true;
		}
	}
	return false;
}

static void AddGetFacts(LogicalGet &get, const string &current_catalog, CreateMVPlanFacts &facts,
                        unordered_map<string, idx_t> &next_occurrence) {
	facts.gets_by_index[get.table_index] = &get;
	auto table_ref = get.GetTable();
	if (table_ref.get()) {
		auto &table = *table_ref.get();
		string table_name = table.name.GetIdentifierName();
		if (!table_name.empty() && !SqlUtils::IsDelta(table_name)) {
			auto key = SqlUtils::FullName(table.ParentCatalog().GetName().GetIdentifierName(),
			                              table.schema.name.GetIdentifierName(), table_name);
			if (facts.source_table_info
			        .emplace(key, SourceTableInfo {table_name, table.ParentCatalog().GetName().GetIdentifierName(),
			                                       table.schema.name.GetIdentifierName()})
			        .second) {
				facts.source_name_counts[table_name]++;
			}
		}
		string table_lc = StringUtil::Lower(table_name);
		ProjectionSourceOccurrence source;
		source.table = table_name;
		source.occurrence = next_occurrence[table_lc]++;
		source.table_index = get.table_index;
		facts.occurrence_by_index[get.table_index] = source;
		facts.source_occurrences.push_back(std::move(source));
	}
	if (get.function.GetName().GetIdentifierName() != "ducklake_scan" || !get.function.function_info) {
		return;
	}
	auto &info = get.function.function_info->Cast<DuckLakeFunctionInfo>();
	string cat = info.table.ParentCatalog().GetName().GetIdentifierName();
	if (cat.empty()) {
		if (current_catalog.empty()) {
			throw CatalogException("Could not resolve DuckLake catalog for table '" + info.table_name + "'");
		}
		cat = current_catalog;
	}
	DuckLakeSourceTableInfo source_info;
	source_info.table_name = info.table_name.GetIdentifierName();
	source_info.catalog_name = cat;
	source_info.schema_name = info.table.schema.name.GetIdentifierName();
	source_info.table_id = static_cast<int64_t>(info.table_id.index);
	facts.ducklake_table_info[SqlUtils::FullName(cat, info.table.schema.name.GetIdentifierName(), info.table_name)] =
	    source_info;
}

static string NullableGetTableName(LogicalGet &get);
static string NormalizeNullableTableName(const string &table_name);

template <class T>
static void AppendUniqueValues(vector<T> &target, const vector<T> &source) {
	for (auto &value : source) {
		if (std::find(target.begin(), target.end(), value) == target.end()) {
			target.push_back(value);
		}
	}
}

static void CollectCreateMVPlanFacts(LogicalOperator *op, const string &current_catalog, CreateMVPlanFacts &facts,
                                     unordered_map<string, idx_t> &next_occurrence, bool seen_agg_above,
                                     const LogicalOperator *redundant_distinct, PlanAnalysis &analysis,
                                     TableIndex internal_delim_cte = TableIndex()) {
	if (!op) {
		return;
	}
	for (auto &binding : op->GetColumnBindings()) {
		facts.max_table_index = std::max(facts.max_table_index, binding.table_index.index);
	}
	bool internal_delim_dedup = false;
	if (internal_delim_cte.IsValid() && op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op->Cast<LogicalAggregate>();
		auto *input = op;
		while (input->children.size() == 1 && input->type != LogicalOperatorType::LOGICAL_CTE_REF) {
			input = input->children[0].get();
		}
		internal_delim_dedup = aggregate.expressions.empty() && input->type == LogicalOperatorType::LOGICAL_CTE_REF &&
		                       input->Cast<LogicalCTERef>().cte_index == internal_delim_cte;
	}
	if (!internal_delim_dedup) {
		AnalyzePlanOperator(*op, analysis);
	}
	auto *logical_join = dynamic_cast<LogicalJoin *>(op);
	if (logical_join && (logical_join->join_type == JoinType::LEFT || logical_join->join_type == JoinType::RIGHT ||
	                     logical_join->join_type == JoinType::OUTER)) {
		facts.outer_joins.push_back(logical_join);
	}
	if (redundant_distinct && op != redundant_distinct && op->type == LogicalOperatorType::LOGICAL_DISTINCT) {
		facts.has_descendant_distinct = true;
	}
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !internal_delim_dedup) {
		seen_agg_above = true;
		auto &agg = op->Cast<LogicalAggregate>();
		facts.aggregates.push_back(&agg);
	} else if (op->type == LogicalOperatorType::LOGICAL_UNION) {
		if (!seen_agg_above) {
			facts.has_union_before_aggregate = true;
		}
		auto &setop = op->Cast<LogicalSetOperation>();
		facts.setops_by_index[setop.table_index] = &setop;
	} else if (op->type == LogicalOperatorType::LOGICAL_INTERSECT || op->type == LogicalOperatorType::LOGICAL_EXCEPT) {
		facts.has_unsupported_set_operation = true;
	} else if (op->type == LogicalOperatorType::LOGICAL_PIVOT) {
		facts.has_pivot = true;
	}
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		facts.has_cardinality_changing_filter = true;
		if (!op->children.empty()) {
			auto *filter_input = op->children[0].get();
			while (filter_input && filter_input->type == LogicalOperatorType::LOGICAL_PROJECTION &&
			       filter_input->children.size() == 1) {
				filter_input = filter_input->children[0].get();
			}
			if (filter_input && filter_input->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
				facts.has_filter_above_aggregate = true;
			}
		}
	}
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = op->Cast<LogicalProjection>();
		if (!facts.first_projection) {
			facts.first_projection = &proj;
		}
		facts.projections.push_back(&proj);
		facts.projections_by_index[proj.table_index] = &proj;
	} else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	           op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN ||
	           op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op->Cast<LogicalComparisonJoin>();
		if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
		    op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			facts.comparison_joins.push_back(&join);
			for (auto &cond : join.conditions) {
				if (!cond.IsComparison()) {
					continue;
				}
				auto *left = cond.LeftReference().get();
				auto *right = cond.RightReference().get();
				if (left->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
				    right->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
					continue;
				}
				auto &left_ref = left->Cast<BoundColumnRefExpression>();
				auto &right_ref = right->Cast<BoundColumnRefExpression>();
				UnionBindings(facts.join_parents, BindingKey(left_ref), BindingKey(right_ref));
				facts.join_refs.push_back(&left_ref);
				facts.join_refs.push_back(&right_ref);
			}
		}
		if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN && join.join_type == JoinType::SINGLE) {
			for (auto &expr : join.duplicate_eliminated_columns) {
				if (expr && expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					facts.single_delim_key_bindings.insert(BindingKey(expr->Cast<BoundColumnRefExpression>()));
				}
			}
			for (auto &cond : join.conditions) {
				if (!cond.IsComparison()) {
					continue;
				}
				if (cond.LeftReference() &&
				    cond.LeftReference()->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					facts.single_delim_key_bindings.insert(
					    BindingKey(cond.LeftReference()->Cast<BoundColumnRefExpression>()));
				}
				if (cond.RightReference() &&
				    cond.RightReference()->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					facts.single_delim_key_bindings.insert(
					    BindingKey(cond.RightReference()->Cast<BoundColumnRefExpression>()));
				}
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op->Cast<LogicalGet>();
		facts.has_cardinality_changing_filter =
		    facts.has_cardinality_changing_filter || !!get.table_filters.HasFilters();
		AddGetFacts(get, current_catalog, facts, next_occurrence);
	} else if (op->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte_ref = op->Cast<LogicalCTERef>();
		facts.cte_refs_by_table_index[cte_ref.table_index] = &cte_ref;
	} else if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && op->children.size() >= 2) {
		auto &cte = op->Cast<LogicalMaterializedCTE>();
		facts.cte_defs_by_index[cte.table_index] = op->children[0].get();
	} else if (op->type == LogicalOperatorType::LOGICAL_WINDOW) {
		facts.windows.push_back(&op->Cast<LogicalWindow>());
	}
	vector<idx_t> child_ids;
	child_ids.reserve(op->children.size());
	auto collect_child = [&](LogicalOperator *child, PlanAnalysis &child_analysis, TableIndex child_delim_cte) {
		CollectCreateMVPlanFacts(child, current_catalog, facts, next_occurrence, seen_agg_above, redundant_distinct,
		                         child_analysis, child_delim_cte);
		D_ASSERT(!facts.plan_nodes_post_order.empty());
		child_ids.push_back(facts.plan_nodes_post_order.size() - 1);
	};
	if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		auto &cte = op->Cast<LogicalMaterializedCTE>();
		PlanAnalysis body_analysis;
		if (!op->children.empty()) {
			collect_child(op->children[0].get(), body_analysis, internal_delim_cte);
		}
		if (op->children.size() >= 2) {
			auto consumer_delim = StringUtil::StartsWith(cte.ctename.GetIdentifierName(), "__duckdb_delim_")
			                          ? cte.table_index
			                          : internal_delim_cte;
			collect_child(op->children[1].get(), analysis, consumer_delim);
			MergeMaterializedCteBodyAnalysis(analysis, std::move(body_analysis));
		}
		for (idx_t i = 2; i < op->children.size(); i++) {
			PlanAnalysis ignored_analysis;
			collect_child(op->children[i].get(), ignored_analysis, internal_delim_cte);
		}
	} else {
		for (auto &child : op->children) {
			collect_child(child.get(), analysis, internal_delim_cte);
		}
	}
	CreateMVPlanNodeFacts node_facts;
	node_facts.plan_node = op;
	node_facts.child_ids = std::move(child_ids);
	const idx_t node_id = facts.plan_nodes_post_order.size();
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION || op->type == LogicalOperatorType::LOGICAL_UNION) {
		node_facts.group_column_search_ids.push_back(node_id);
	} else if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && node_facts.child_ids.size() >= 2) {
		AppendUniqueValues(node_facts.group_column_search_ids,
		                   facts.plan_nodes_post_order[node_facts.child_ids[1]].group_column_search_ids);
		AppendUniqueValues(node_facts.group_column_search_ids,
		                   facts.plan_nodes_post_order[node_facts.child_ids[0]].group_column_search_ids);
	} else {
		for (auto child_id : node_facts.child_ids) {
			AppendUniqueValues(node_facts.group_column_search_ids,
			                   facts.plan_nodes_post_order[child_id].group_column_search_ids);
		}
	}
	for (auto child_id : node_facts.child_ids) {
		auto &child_facts = facts.plan_nodes_post_order[child_id];
		AppendUniqueValues(node_facts.subtree_table_indices, child_facts.subtree_table_indices);
		AppendUniqueValues(node_facts.subtree_base_tables, child_facts.subtree_base_tables);
		node_facts.subtree_base_tables_complete =
		    node_facts.subtree_base_tables_complete && child_facts.subtree_base_tables_complete;
		node_facts.contains_table_function = node_facts.contains_table_function || child_facts.contains_table_function;
	}
	if (op->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op->Cast<LogicalGet>();
		node_facts.subtree_table_indices.push_back(get.table_index);
		string table_name = NullableGetTableName(get);
		if (table_name.empty()) {
			node_facts.subtree_base_tables_complete = false;
		} else {
			node_facts.subtree_base_tables.push_back(NormalizeNullableTableName(table_name));
		}
		node_facts.contains_table_function = !get.GetTable().get();
	} else if (op->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		node_facts.subtree_base_tables_complete = false;
	}
	facts.plan_node_ids[op] = node_id;
	facts.plan_nodes_post_order.push_back(std::move(node_facts));
}

static bool IsHiddenHavingColumn(const string &name) {
	return StringUtil::StartsWith(name, "openivm_having_");
}

// The aggregate function a column reference reads from one of `aggregates`, if any.
static optional_ptr<BoundAggregateExpression>
ReferencedAggregate(const BoundColumnRefExpression &column_ref,
                    const unordered_map<TableIndex, LogicalAggregate *> &aggregates) {
	auto aggregate_it = aggregates.find(column_ref.Binding().table_index);
	if (aggregate_it == aggregates.end() ||
	    column_ref.Binding().column_index >= aggregate_it->second->expressions.size()) {
		return nullptr;
	}
	auto &aggregate_expr = *aggregate_it->second->expressions[column_ref.Binding().column_index];
	return aggregate_expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE
	           ? &aggregate_expr.Cast<BoundAggregateExpression>()
	           : nullptr;
}

static bool IsMinMaxAggregateColumn(const BoundColumnRefExpression &column_ref,
                                    const unordered_map<TableIndex, LogicalAggregate *> &aggregates) {
	auto aggregate = ReferencedAggregate(column_ref, aggregates);
	return aggregate && (aggregate->Function().GetName().GetIdentifierName() == "min" ||
	                     aggregate->Function().GetName().GetIdentifierName() == "max");
}

static bool ExpressionReferencesMinMaxAggregate(Expression &expr,
                                                const unordered_map<TableIndex, LogicalAggregate *> &aggregates) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column_ref = expr.Cast<BoundColumnRefExpression>();
		return IsMinMaxAggregateColumn(column_ref, aggregates);
	}

	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!found && ExpressionReferencesMinMaxAggregate(child, aggregates)) {
			found = true;
		}
	});
	return found;
}

static bool ExpressionReferencesUserSumAggregate(Expression &expr, const CreateMVPlanFacts &facts,
                                                 const unordered_map<TableIndex, LogicalAggregate *> &aggregates,
                                                 idx_t depth = 0) {
	if (depth > 16) {
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column_ref = expr.Cast<BoundColumnRefExpression>();
		if (aggregates.count(column_ref.Binding().table_index)) {
			auto aggregate = ReferencedAggregate(column_ref, aggregates);
			return aggregate && aggregate->Function().GetName().GetIdentifierName() == "sum" &&
			       !IncrementalTableNames::IsInternalColumn(aggregate->GetAlias().GetIdentifierName());
		}
		auto projection_it = facts.projections_by_index.find(column_ref.Binding().table_index);
		if (projection_it == facts.projections_by_index.end()) {
			return false;
		}
		auto &projection = *projection_it->second;
		if (column_ref.Binding().column_index >= projection.expressions.size()) {
			return false;
		}
		return ExpressionReferencesUserSumAggregate(*projection.expressions[column_ref.Binding().column_index], facts,
		                                            aggregates, depth + 1);
	}

	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		if (!found && ExpressionReferencesUserSumAggregate(child, facts, aggregates, depth + 1)) {
			found = true;
		}
	});
	return found;
}

static void FinalizeCreateMVPlanFacts(CreateMVPlanFacts &facts) {
	unordered_map<TableIndex, LogicalAggregate *> aggregates;
	for (auto *aggregate : facts.aggregates) {
		aggregates[aggregate->aggregate_index] = aggregate;
	}
	if (aggregates.empty()) {
		return;
	}
	for (auto *projection : facts.projections) {
		for (auto &expr : projection->expressions) {
			Expression *unwrapped = expr.get();
			while (BoundCastExpression::IsCast(*unwrapped)) {
				unwrapped = BoundCastExpression::ChildMutable(unwrapped->Cast<BoundFunctionExpression>()).get();
			}
			if (unwrapped->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF &&
			    ExpressionReferencesUserSumAggregate(*unwrapped, facts, aggregates)) {
				facts.has_computed_sum_aggregate_projection = true;
			}
			if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    IsHiddenHavingColumn(expr->GetAlias().GetIdentifierName())) {
				auto &column_ref = expr->Cast<BoundColumnRefExpression>();
				if (IsMinMaxAggregateColumn(column_ref, aggregates)) {
					facts.has_hidden_minmax_having_column = true;
				}
			} else if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF &&
			           ExpressionReferencesMinMaxAggregate(*expr, aggregates)) {
				facts.has_computed_minmax_aggregate_projection = true;
			}
		}
	}
}

bool ResolvesToOutputBinding(TableIndex table_index, ProjectionIndex column_index, TableIndex group_index,
                             size_t group_count, const CreateMVPlanFacts &facts, bool through_casts, int depth) {
	if (depth > 16) {
		return false;
	}
	if (table_index == group_index) {
		return column_index < static_cast<idx_t>(group_count);
	}
	auto projection_it = facts.projections_by_index.find(table_index);
	if (projection_it != facts.projections_by_index.end()) {
		auto &proj = *projection_it->second;
		if (column_index >= proj.expressions.size()) {
			return false;
		}
		auto *expr = proj.expressions[column_index].get();
		while (through_casts && BoundCastExpression::IsCast(*expr)) {
			expr = BoundCastExpression::ChildMutable(expr->Cast<BoundFunctionExpression>()).get();
		}
		if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto &bcr = expr->Cast<BoundColumnRefExpression>();
		return ResolvesToOutputBinding(bcr.Binding().table_index, bcr.Binding().column_index, group_index, group_count,
		                               facts, through_casts, depth + 1);
	}
	auto setop_it = facts.setops_by_index.find(table_index);
	if (setop_it != facts.setops_by_index.end()) {
		auto &setop = *setop_it->second;
		if (column_index >= setop.column_count) {
			return false;
		}
		for (auto &child : setop.children) {
			auto bindings = child->GetColumnBindings();
			if (column_index >= bindings.size()) {
				continue;
			}
			auto &binding = bindings[column_index];
			if (ResolvesToOutputBinding(binding.table_index, binding.column_index, group_index, group_count, facts,
			                            through_casts, depth + 1)) {
				return true;
			}
		}
		return false;
	}
	auto cte_ref_it = facts.cte_refs_by_table_index.find(table_index);
	if (cte_ref_it != facts.cte_refs_by_table_index.end()) {
		auto &cte_ref = *cte_ref_it->second;
		auto cte_def_it = facts.cte_defs_by_index.find(cte_ref.cte_index);
		if (cte_def_it == facts.cte_defs_by_index.end()) {
			return false;
		}
		auto bindings = cte_def_it->second->GetColumnBindings();
		if (column_index >= bindings.size()) {
			return false;
		}
		auto &binding = bindings[column_index];
		return ResolvesToOutputBinding(binding.table_index, binding.column_index, group_index, group_count, facts,
		                               through_casts, depth + 1);
	}
	return false;
}

static bool ResolvesToGroupBinding(TableIndex table_index, ProjectionIndex column_index, TableIndex group_index,
                                   size_t group_count, const CreateMVPlanFacts &facts) {
	return ResolvesToOutputBinding(table_index, column_index, group_index, group_count, facts, false);
}

LogicalAggregate *FindViewOutputAggregate(LogicalOperator *root) {
	return SpineAggregate(ViewOutputSpine(root));
}

static string ProjectionOutputName(const LogicalProjection &projection, idx_t expr_index,
                                   const CreateMVPlanFacts &facts, const vector<string> &output_names,
                                   const BoundColumnRefExpression &bcr) {
	auto spine = ViewOutputSpine(facts.root);
	// Stored names belong to the view's output projection, after sanitization and
	// deduplication. A CTE body visited first can have a different output order.
	if (!spine.projections.empty() && spine.projections.front() == &projection && expr_index < output_names.size() &&
	    !output_names[expr_index].empty() && !IncrementalTableNames::IsInternalColumn(output_names[expr_index])) {
		return output_names[expr_index];
	}
	auto &expr = projection.expressions[expr_index];
	if (!expr->GetAlias().GetIdentifierName().empty()) {
		return expr->GetAlias().GetIdentifierName();
	}
	return bcr.GetName().GetIdentifierName();
}

BoundColumnRefExpression *GetColumnRefThroughCasts(Expression *expr, string *cast_type) {
	if (cast_type) {
		cast_type->clear();
	}
	vector<string> cast_specs;
	while (expr && BoundCastExpression::IsCast(*expr)) {
		auto &cast = expr->Cast<BoundFunctionExpression>();
		if (cast_type) {
			cast_specs.push_back(
			    SqlUtils::BuildCastSpec(expr->GetReturnType().ToString(), BoundCastExpression::IsTryCast(cast)));
		}
		expr = BoundCastExpression::ChildMutable(cast).get();
	}
	if (!expr || expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}
	if (cast_type) {
		for (auto it = cast_specs.rbegin(); it != cast_specs.rend(); it++) {
			*cast_type = SqlUtils::ComposeCastSpecs(*it, *cast_type);
		}
	}
	return &expr->Cast<BoundColumnRefExpression>();
}

static bool AddGroupColumnsFromProjection(LogicalProjection &proj, const CreateMVPlanFacts &facts,
                                          TableIndex group_index, size_t group_count,
                                          const vector<string> &output_names, vector<string> &group_names) {
	bool matched = false;
	for (idx_t expr_i = 0; expr_i < proj.expressions.size(); expr_i++) {
		auto &expr = proj.expressions[expr_i];
		if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &bcr = expr->Cast<BoundColumnRefExpression>();
		if (!ResolvesToGroupBinding(bcr.Binding().table_index, bcr.Binding().column_index, group_index, group_count,
		                            facts)) {
			continue;
		}
		string col_name = ProjectionOutputName(proj, expr_i, facts, output_names, bcr);
		if (!IncrementalTableNames::IsInternalColumn(col_name) ||
		    StringUtil::StartsWith(col_name, "openivm_group_key_")) {
			group_names.push_back(col_name);
			matched = true;
		}
	}
	return matched;
}

static bool AddGroupColumnsFromBindings(LogicalOperator &op, const CreateMVPlanFacts &facts, TableIndex group_index,
                                        size_t group_count, const vector<string> &output_names,
                                        vector<string> &group_names) {
	bool matched = false;
	auto bindings = op.GetColumnBindings();
	for (idx_t col_idx = 0; col_idx < bindings.size() && col_idx < output_names.size(); col_idx++) {
		auto &binding = bindings[col_idx];
		if (!ResolvesToGroupBinding(binding.table_index, binding.column_index, group_index, group_count, facts)) {
			continue;
		}
		if (!output_names[col_idx].empty() && (!IncrementalTableNames::IsInternalColumn(output_names[col_idx]) ||
		                                       StringUtil::StartsWith(output_names[col_idx], "openivm_group_key_"))) {
			group_names.push_back(output_names[col_idx]);
			matched = true;
		}
	}
	return matched;
}

static bool FindGroupColumns(const CreateMVPlanFacts &facts, TableIndex group_index, size_t group_count,
                             const vector<string> &output_names, vector<string> &group_names) {
	auto *root_facts = GetPlanNodeFacts(facts, facts.root);
	if (!root_facts) {
		return false;
	}
	for (auto node_id : root_facts->group_column_search_ids) {
		D_ASSERT(node_id < facts.plan_nodes_post_order.size());
		auto *candidate = facts.plan_nodes_post_order[node_id].plan_node;
		if (candidate->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			if (AddGroupColumnsFromProjection(candidate->Cast<LogicalProjection>(), facts, group_index, group_count,
			                                  output_names, group_names)) {
				return true;
			}
		} else {
			D_ASSERT(candidate->type == LogicalOperatorType::LOGICAL_UNION);
			if (AddGroupColumnsFromBindings(*candidate, facts, group_index, group_count, output_names, group_names)) {
				return true;
			}
		}
	}
	return false;
}

vector<string> DeriveGroupColumnNames(const CreateMVPlanFacts &facts, TableIndex group_index, size_t group_count,
                                      const vector<string> &output_names) {
	// The CTAS operator exposes its inserted row count. Its SELECT child
	// carries the bindings that correspond to the backing table's output names.
	auto &root = *facts.root;
	auto &output = root.type == LogicalOperatorType::LOGICAL_CREATE_TABLE ? *root.children[0] : root;
	vector<string> group_names;
	if (!AddGroupColumnsFromBindings(output, facts, group_index, group_count, output_names, group_names)) {
		FindGroupColumns(facts, group_index, group_count, output_names, group_names);
	}
	return group_names;
}

string BuildDistinctSelectedViewSQL(AstNode &node, const vector<string> &input_columns,
                                    const vector<string> &output_columns) {
	std::function<bool(AstNode &)> replace_input = [&](AstNode &candidate) {
		if (candidate.NodeType() == "Aggregate") {
			D_ASSERT(candidate.children.size() == 1);
			auto names = candidate.children[0]->OutputColumnNames();
			// CREATE-time DISTINCT normalization appends its support count after
			// the original input columns; the parent aggregate does not consume it.
			D_ASSERT(names.size() >= input_columns.size());
			names.resize(input_columns.size());
			candidate.children[0] =
			    make_uniq<AstGetNode>("", vector<string>(), "openivm_distinct_selected", TableIndex(0), input_columns,
			                          std::move(names), vector<string>());
			return true;
		}
		for (auto &child : candidate.children) {
			if (replace_input(*child)) {
				return true;
			}
		}
		return false;
	};
	if (!replace_input(node)) {
		throw InternalException("Missing aggregate for maintained DISTINCT selection");
	}
	OPENIVM_DEBUG_PRINT("[CREATE MV] Reading DISTINCT aggregate input from maintained selection\n");
	return AstToCteList(node)->ToQuery(true, output_columns);
}

static bool GetLogicalGetColumnName(LogicalGet &get, ProjectionIndex column_index, string &column);

void PopulateDistinctAggregateInputs(ClientContext &context, Binder &binder, const CreateMVPlanFacts &facts,
                                     const vector<string> &output_names, RefreshMetadata::DistinctAuxMeta &meta) {
	auto *parent = FindViewOutputAggregate(facts.root);
	D_ASSERT(parent);
	auto &aggregate = *parent;
	// Only a DISTINCT on this unary input path can feed direct tuple transitions.
	// Producer CTE definitions can contain other DISTINCTs with different schemas.
	auto input_spine = FindOutputSpine(aggregate.children[0].get(), [](const LogicalOperator &op) {
		return op.type == LogicalOperatorType::LOGICAL_FILTER || op.type == LogicalOperatorType::LOGICAL_ORDER_BY ||
		       op.type == LogicalOperatorType::LOGICAL_LIMIT || op.type == LogicalOperatorType::LOGICAL_TOP_N;
	});
	auto *input = input_spine.bottom;
	auto distinct =
	    input && input->type == LogicalOperatorType::LOGICAL_DISTINCT ? &input->Cast<LogicalDistinct>() : nullptr;
	vector<ColumnBinding> bindings;
	// Resolve unary inputs directly against a physical scan. Other input shapes
	// reuse the maintained candidate bag, whose selection preserves joins/UNIONs.
	auto *distinct_source = distinct ? distinct->children[0].get() : nullptr;
	vector<LogicalOperator *> filters;
	while (distinct_source &&
	       (distinct_source->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	        distinct_source->type == LogicalOperatorType::LOGICAL_FILTER ||
	        distinct_source->type == LogicalOperatorType::LOGICAL_ORDER_BY) &&
	       distinct_source->children.size() == 1) {
		if (distinct_source->type == LogicalOperatorType::LOGICAL_FILTER) {
			filters.push_back(distinct_source);
		}
		distinct_source = distinct_source->children[0].get();
	}
	bool candidate_input =
	    !distinct_source || distinct_source->type != LogicalOperatorType::LOGICAL_GET ||
	    std::any_of(facts.plan_nodes_post_order.begin(), facts.plan_nodes_post_order.end(), [](const auto &node) {
		    return node.plan_node->type == LogicalOperatorType::LOGICAL_TOP_N ||
		           node.plan_node->type == LogicalOperatorType::LOGICAL_LIMIT;
	    });
	auto default_collation = Settings::Get<DefaultCollationSetting>(context);
	if (candidate_input) {
		// Keep the candidate bag, not just today's selected rows: deletes must refill the boundary.
		LogicalGet *source = nullptr;
		for (auto &node : facts.plan_nodes_post_order) {
			if (node.plan_node->type == LogicalOperatorType::LOGICAL_GET &&
			    node.plan_node->Cast<LogicalGet>().GetTable()) {
				source = &node.plan_node->Cast<LogicalGet>();
				break;
			}
		}
		D_ASSERT(source);
		auto table = source->GetTable();
		meta.source = SqlUtils::FullName(table->ParentCatalog().GetName(), table->schema.name, table->name);
		meta.cols.clear();
		meta.source_exprs.clear();
		meta.filter.clear();
		vector<LogicalType> source_types;
		for (auto &entry : facts.gets_by_index) {
			auto &get = *entry.second;
			if (get.GetTable().get() != table.get()) {
				continue;
			}
			for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
				string column;
				if (!GetLogicalGetColumnName(get, ProjectionIndex(i), column)) {
					throw InternalException("Could not resolve DISTINCT candidate column");
				}
				auto expression = SqlUtils::QuoteIdentifier(column);
				if (std::find(meta.source_exprs.begin(), meta.source_exprs.end(), expression) ==
				    meta.source_exprs.end()) {
					meta.cols.push_back("openivm_topk_source_" + to_string(meta.cols.size()));
					meta.source_exprs.push_back(expression);
					meta.selected_source_columns.push_back(column);
					source_types.push_back(get.GetColumnType(ColumnIndex(get.GetColumnIds()[i].GetPrimaryIndex())));
				}
			}
		}
		for (idx_t i = 0, source_count = source_types.size(); i < source_count; i++) {
			auto identity = SqlUtils::ValueIdentitySQL(meta.source_exprs[i], source_types[i], default_collation);
			if (!identity.empty()) {
				meta.cols.push_back("openivm_topk_identity_" + to_string(meta.cols.size()));
				meta.source_exprs.push_back(std::move(identity));
			}
		}
		bindings = aggregate.children[0]->GetColumnBindings();
		aggregate.children[0]->ResolveOperatorTypes();
		for (idx_t i = 0; i < bindings.size(); i++) {
			auto column = "openivm_topk_input_" + to_string(i);
			meta.selected_input_columns.push_back(column);
			auto identity = SqlUtils::ValueIdentitySQL(SqlUtils::QuoteIdentifier(column),
			                                           aggregate.children[0]->types[i], default_collation);
			if (!identity.empty()) {
				meta.selected_identity_exprs.push_back(std::move(identity));
			}
		}
		unique_ptr<LogicalOperator> selected_plan;
		if (facts.cte_defs_by_index.empty()) {
			selected_plan = aggregate.children[0]->Copy(context);
		} else {
			// CTE_REFs need their enclosing definitions before extracting the input.
			auto selection_context = facts.root->Copy(context);
			InlineMaterializedCteRefs(context, binder, selection_context);
			auto copied_aggregate = FindViewOutputAggregate(selection_context.get());
			D_ASSERT(copied_aggregate && copied_aggregate->aggregate_index == aggregate.aggregate_index);
			selected_plan = std::move(copied_aggregate->children[0]);
		}
		D_ASSERT(selected_plan->GetColumnBindings().size() == meta.selected_input_columns.size());
		meta.selected_input_sql = RenderPlanSQL(context, selected_plan, meta.selected_input_columns);
		// The selection keeps stable column aliases when the source is renamed.
		meta.selected_input_sql =
		    SqlUtils::ReplaceTableReferences(meta.selected_input_sql, meta.source, "openivm_topk_candidates");
		OPENIVM_DEBUG_PRINT("[CREATE MV] Retained %zu candidate columns for DISTINCT input\n", meta.cols.size());
	} else {
		D_ASSERT(distinct);
		bindings = distinct->GetColumnBindings();
		D_ASSERT(bindings.size() == meta.cols.size());
		distinct->ResolveOperatorTypes();
		const auto &distinct_types = distinct->types;
		// Resolve the input through bound projections and filters. Parsed CTE names
		// have no catalog identity, and their expressions must also apply to delta rows.
		auto &get = distinct_source->Cast<LogicalGet>();
		auto table = get.GetTable();
		D_ASSERT(table);
		meta.source = SqlUtils::FullName(table->ParentCatalog().GetName(), table->schema.name, table->name);
		LptsExpressionRenderer::BindingResolver resolve_input;
		LptsExpressionRenderer input_renderer(
		    context, SqlDialect::DUCKDB,
		    [&](const ColumnBinding &binding, const char *where) { return resolve_input(binding, where); });
		resolve_input = [&](const ColumnBinding &binding, const char *) -> string {
			if (binding.table_index == get.table_index) {
				string column;
				if (!GetLogicalGetColumnName(get, binding.column_index, column)) {
					throw InternalException("Could not resolve DISTINCT source column");
				}
				return SqlUtils::QuoteIdentifier(column);
			}
			auto projection = facts.projections_by_index.find(binding.table_index);
			D_ASSERT(projection != facts.projections_by_index.end());
			return input_renderer.ExpressionToAliasedString(*projection->second->expressions[binding.column_index]);
		};
		meta.source_exprs.clear();
		for (idx_t i = 0; i < bindings.size(); i++) {
			BoundColumnRefExpression input(LogicalType::SQLNULL, bindings[i]);
			meta.source_exprs.push_back(input_renderer.ExpressionToAliasedString(input));
		}
		auto predicates = input_renderer.ScanFiltersToSql(get);
		for (auto *filter : filters) {
			for (auto &expression : filter->expressions) {
				predicates.push_back("(" + input_renderer.ExpressionToAliasedString(*expression) + ")");
			}
		}
		meta.filter = StringUtil::Join(predicates, " AND ");
		OPENIVM_DEBUG_PRINT("[CREATE MV] Resolved DISTINCT input to bound source %s\n", meta.source.c_str());

		// Floating columns keep per-column sign counts; other identity-bearing types need representatives.
		vector<string> identities;
		bool needs_representative = false;
		for (idx_t i = 0; i < distinct_types.size(); i++) {
			auto &type = distinct_types[i];
			identities.push_back(
			    SqlUtils::ValueIdentitySQL(SqlUtils::QuoteIdentifier(meta.cols[i]), type, default_collation));
			if (type.IsFloating()) {
				meta.sign_columns.push_back(meta.cols[i]);
			} else if (!identities.back().empty()) {
				needs_representative = true;
			}
		}
		// Per-column counters cannot preserve correlations between floating signs.
		meta.representative_input = needs_representative || meta.sign_columns.size() > 1;
		if (meta.representative_input) {
			meta.sign_columns.clear();
			meta.selected_source_columns = meta.cols;
			meta.selected_input_columns = meta.cols;
			meta.selected_input_sql =
			    "SELECT " + SqlUtils::JoinQuotedColumns(meta.cols) + " FROM openivm_topk_candidates";
			for (idx_t i = 0; i < distinct_types.size(); i++) {
				if (identities[i].empty()) {
					continue;
				}
				meta.selected_identity_exprs.push_back(std::move(identities[i]));
				meta.cols.push_back(meta.InternalColumn("openivm_tuple_identity_" + to_string(i)));
				meta.source_exprs.push_back(
				    SqlUtils::ValueIdentitySQL(meta.source_exprs[i], distinct_types[i], default_collation));
			}
			OPENIVM_DEBUG_PRINT("[CREATE MV] Retaining representatives for %zu DISTINCT identity keys\n",
			                    meta.selected_identity_exprs.size());
		}
	}

	auto group_cols = DeriveGroupColumnNames(facts, aggregate.group_index, aggregate.groups.size(), output_names);
	// Declare recursive callbacks separately: MSVC cannot capture a renderer in its own initializer.
	LptsExpressionRenderer::BindingResolver resolve_group;
	LptsExpressionRenderer renderer(context, SqlDialect::DUCKDB, [&](const ColumnBinding &binding, const char *where) {
		return resolve_group(binding, where);
	});
	resolve_group = [&](const ColumnBinding &binding, const char *) -> string {
		auto match = std::find(bindings.begin(), bindings.end(), binding);
		if (match != bindings.end()) {
			auto &columns = meta.selected_input_sql.empty() ? meta.cols : meta.selected_input_columns;
			return SqlUtils::QuoteIdentifier(columns[match - bindings.begin()]);
		}
		if (binding.table_index == aggregate.group_index) {
			return renderer.ExpressionToAliasedString(*aggregate.groups[binding.column_index]);
		}
		auto projection = facts.projections_by_index.find(binding.table_index);
		D_ASSERT(projection != facts.projections_by_index.end());
		return renderer.ExpressionToAliasedString(*projection->second->expressions[binding.column_index]);
	};
	if (!candidate_input) {
		// Maintained selections already include this path. Ordinary DISTINCT must
		// filter its old/new representatives, rather than filtering source support.
		vector<string> predicates;
		for (auto *node = aggregate.children[0].get(); node != distinct && node->children.size() == 1;
		     node = node->children[0].get()) {
			if (node->type == LogicalOperatorType::LOGICAL_FILTER) {
				for (auto &expression : node->expressions) {
					predicates.push_back("(" + renderer.ExpressionToAliasedString(*expression) + ")");
				}
			}
		}
		meta.post_filter = StringUtil::Join(predicates, " AND ");
		OPENIVM_DEBUG_PRINT("[CREATE MV] DISTINCT representative filter: %s\n", meta.post_filter.c_str());
	}
	for (auto &expression : aggregate.expressions) {
		auto &input = expression->Cast<BoundAggregateExpression>();
		if (input.Function().GetName().GetIdentifierName() != "sum") {
			continue;
		}
		// Boolean SUM counts true values. Use a signed numeric input before weighting;
		// the other SUM inputs use their accumulator type to make negation safe.
		auto type = input.GetChildren()[0]->GetReturnType().id() == LogicalTypeId::BOOLEAN ? LogicalType::BIGINT
		                                                                                   : input.GetReturnType();
		meta.sum_input_expr =
		    "CAST(" + renderer.ExpressionToAliasedString(*input.GetChildren()[0]) + " AS " + type.ToString() + ")";
	}

	auto output_spine = ViewOutputSpine(facts.root);
	D_ASSERT(!output_spine.projections.empty());
	for (auto &column : group_cols) {
		auto name = std::find(output_names.begin(), output_names.end(), column);
		D_ASSERT(name != output_names.end());
		meta.group_inputs.push_back(
		    {column, renderer.ExpressionToAliasedString(
		                 *output_spine.projections.front()->expressions[name - output_names.begin()])});
	}
	OPENIVM_DEBUG_PRINT("[CREATE MV] Retained %zu DISTINCT aggregate group expressions\n", meta.group_inputs.size());
}

static void AddUniqueGroupNames(vector<string> &group_names, const vector<string> &new_names) {
	for (auto &name : new_names) {
		AddUniqueCI(group_names, name);
	}
}

vector<string> DeriveScalarDelimKeyColumnNames(const CreateMVPlanFacts &facts, const vector<string> &output_names) {
	vector<string> group_names;
	auto *top_projection = facts.first_projection;
	if (top_projection) {
		for (idx_t expr_idx = 0; expr_idx < top_projection->expressions.size() && expr_idx < output_names.size();
		     expr_idx++) {
			auto &expr = top_projection->expressions[expr_idx];
			if (!expr || expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				continue;
			}
			if (!facts.single_delim_key_bindings.count(BindingKey(expr->Cast<BoundColumnRefExpression>()))) {
				continue;
			}
			if (!output_names[expr_idx].empty() && !IncrementalTableNames::IsInternalColumn(output_names[expr_idx])) {
				AddUniqueCI(group_names, output_names[expr_idx]);
			}
		}
	}
	if (group_names.empty() && !output_names.empty() && !IncrementalTableNames::IsInternalColumn(output_names[0])) {
		// Scalar correlated subqueries normally project the duplicate-eliminated outer keys
		// first. Use that shape as a fallback if binding aliases were hidden by projection
		// rewrites.
		group_names.push_back(output_names[0]);
	}
	return group_names;
}

vector<string> DeriveAggregateGroupColumnNames(const CreateMVPlanFacts &facts, const vector<string> &output_names,
                                               bool include_first_aggregate) {
	vector<string> group_names;
	bool seen_aggregate = false;
	for (auto *agg_ptr : facts.aggregates) {
		auto &agg = *agg_ptr;
		bool include_this = include_first_aggregate || seen_aggregate;
		seen_aggregate = true;
		if (include_this && !agg.groups.empty()) {
			vector<string> nested_names;
			if (FindGroupColumns(facts, agg.group_index, agg.groups.size(), output_names, nested_names)) {
				AddUniqueGroupNames(group_names, nested_names);
			}
		}
	}
	return group_names;
}

static string ResolveBindingToBaseColumn(BoundColumnRefExpression *bcr, const CreateMVPlanFacts &facts) {
	TableIndex table_index = bcr->Binding().table_index;
	ProjectionIndex column_index = bcr->Binding().column_index;
	for (int depth = 0; depth < 16; depth++) {
		auto get_it = facts.gets_by_index.find(table_index);
		if (get_it != facts.gets_by_index.end()) {
			auto *get = get_it->second;
			string column;
			return GetLogicalGetColumnName(*get, column_index, column) ? column : "";
		}
		auto proj_it = facts.projections_by_index.find(table_index);
		if (proj_it == facts.projections_by_index.end()) {
			return "";
		}
		auto *proj = proj_it->second;
		if (column_index >= proj->expressions.size()) {
			return "";
		}
		auto &expr = proj->expressions[column_index];
		if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return expr->GetAlias().empty() ? expr->GetName().GetIdentifierName()
			                                : expr->GetAlias().GetIdentifierName();
		}
		auto &next = expr->Cast<BoundColumnRefExpression>();
		table_index = next.Binding().table_index;
		column_index = next.Binding().column_index;
	}
	return "";
}

using ProjectionLineageStep = RefreshMetadata::ProjectionKeyLineageStep;
using ProjectionLineageArm = RefreshMetadata::ProjectionKeyLineageArm;

static string OccurrenceKey(const string &table, idx_t occurrence) {
	return StringUtil::Lower(table) + "#" + to_string(occurrence);
}

static string OccurrenceKey(const OccurrenceColumnRef &ref) {
	return OccurrenceKey(ref.table, ref.occurrence);
}

static bool GetLogicalGetColumnName(LogicalGet &get, ProjectionIndex column_index, string &column) {
	if (get.GetTable().get()) {
		auto &ids = get.GetColumnIds();
		if (column_index < ids.size()) {
			auto base_idx = ids[column_index].GetPrimaryIndex();
			auto &cols = get.GetTable().get()->GetColumns();
			if (base_idx < cols.LogicalColumnCount()) {
				column = cols.GetColumn(LogicalIndex(base_idx)).Name().GetIdentifierName();
				return true;
			}
		}
	}
	if (column_index < get.names.size()) {
		column = get.names[column_index].GetIdentifierName();
		return true;
	}
	return false;
}

static bool GetLogicalGetColumnType(LogicalGet &get, ProjectionIndex column_index, LogicalType &type) {
	if (column_index < get.returned_types.size()) {
		type = get.returned_types[column_index];
		return true;
	}
	if (get.GetTable().get()) {
		auto &ids = get.GetColumnIds();
		if (column_index < ids.size()) {
			auto base_idx = ids[column_index].GetPrimaryIndex();
			auto &cols = get.GetTable().get()->GetColumns();
			if (base_idx < cols.LogicalColumnCount()) {
				type = cols.GetColumn(LogicalIndex(base_idx)).Type();
				return true;
			}
		}
	}
	return false;
}

static bool HasKeyBinding(const vector<ColumnBinding> &keys, const ColumnBinding &binding) {
	return std::find(keys.begin(), keys.end(), binding) != keys.end();
}

static bool IsKeyReference(const Expression &expr, const vector<ColumnBinding> &keys) {
	// Casts and computed expressions need a separate equivalence proof.
	return expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
	       HasKeyBinding(keys, expr.Cast<BoundColumnRefExpression>().Binding());
}

static vector<ColumnBinding> DirectSourceKeyBindings(LogicalOperator &op, const CreateMVPlanFacts &facts,
                                                     const string &key, TableCatalogEntry *&source, idx_t depth = 0) {
	if (depth > 64) {
		return {};
	}
	auto bindings = op.GetColumnBindings();
	vector<ColumnBinding> result;
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto *table = get.GetTable().get();
		if (!table || !get.children.empty() || (source && source != table)) {
			return {};
		}
		source = table;
		for (idx_t i = 0; i < bindings.size(); i++) {
			string column;
			if (GetLogicalGetColumnName(get, bindings[i].column_index, column) && StringUtil::CIEquals(column, key)) {
				result.push_back(bindings[i]);
			}
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		auto def = facts.cte_defs_by_index.find(ref.cte_index);
		if (def == facts.cte_defs_by_index.end()) {
			return {};
		}
		auto keys = DirectSourceKeyBindings(*def->second, facts, key, source, depth + 1);
		auto outputs = def->second->GetColumnBindings();
		for (idx_t i = 0; i < bindings.size() && i < outputs.size(); i++) {
			if (HasKeyBinding(keys, outputs[i])) {
				result.push_back(bindings[i]);
			}
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && op.children.size() == 2) {
		return DirectSourceKeyBindings(*op.children[1], facts, key, source, depth + 1);
	}
	if (op.children.empty()) {
		return {};
	}
	auto left = DirectSourceKeyBindings(*op.children[0], facts, key, source, depth + 1);
	if (left.empty()) {
		return {};
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.children.size() == 2) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER) {
			return {};
		}
		auto right = DirectSourceKeyBindings(*op.children[1], facts, key, source, depth + 1);
		bool same_key = false;
		for (auto &condition : join.conditions) {
			if (!condition.IsComparison()) {
				continue;
			}
			if ((condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL ||
			     condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) &&
			    IsKeyReference(*condition.LeftReference(), left) &&
			    IsKeyReference(*condition.RightReference(), right)) {
				same_key = true;
			}
		}
		if (!same_key) {
			return {};
		}
		for (auto &binding : bindings) {
			if (HasKeyBinding(left, binding) || HasKeyBinding(right, binding)) {
				result.push_back(binding);
			}
		}
		return result;
	}
	if (op.children.size() != 1) {
		return {};
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return left;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			if (IsKeyReference(*projection.expressions[i], left)) {
				result.emplace_back(projection.table_index, ProjectionIndex(i));
			}
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		if (aggregate.grouping_sets.size() > 1) {
			return {};
		}
		for (idx_t i = 0; i < aggregate.groups.size(); i++) {
			if (IsKeyReference(*aggregate.groups[i], left) &&
			    (aggregate.grouping_sets.empty() || aggregate.grouping_sets[0].count(ProjectionIndex(i)))) {
				result.emplace_back(aggregate.group_index, ProjectionIndex(i));
			}
		}
	}
	return result;
}

string DeriveDirectSourceGroupKey(const CreateMVPlanFacts &facts, const vector<string> &output_names) {
	if (!facts.root) {
		return "";
	}
	auto *root = facts.root;
	if (root->type == LogicalOperatorType::LOGICAL_CREATE_TABLE && root->children.size() == 1) {
		root = root->children[0].get();
	}
	// Inductive invariant: output partition k reads only source partition k.
	// Selection/projection preserve it; aggregation must group by k; joins must
	// equate k on both arms. Global aggregates, windows and cross-key joins fail.
	// Requiring the original source name at the output avoids new lineage
	// metadata.
	auto outputs = root->GetColumnBindings();
	for (idx_t i = 0; i < outputs.size() && i < output_names.size(); i++) {
		if (IncrementalTableNames::IsInternalColumn(output_names[i])) {
			continue;
		}
		TableCatalogEntry *source = nullptr;
		auto keys = DirectSourceKeyBindings(*root, facts, output_names[i], source);
		if (HasKeyBinding(keys, outputs[i])) {
			return output_names[i];
		}
	}
	return "";
}

static bool ResolveBindingToGetColumn(ColumnBinding binding, const CreateMVPlanFacts &facts, LogicalGet *&get,
                                      string &column, ProjectionIndex *base_column = nullptr,
                                      string *cast_type = nullptr) {
	for (int depth = 0; depth < 16; depth++) {
		auto get_it = facts.gets_by_index.find(binding.table_index);
		if (get_it != facts.gets_by_index.end()) {
			get = get_it->second;
			if (base_column) {
				*base_column = binding.column_index;
			}
			return GetLogicalGetColumnName(*get, binding.column_index, column);
		}
		auto proj_it = facts.projections_by_index.find(binding.table_index);
		if (proj_it == facts.projections_by_index.end() ||
		    binding.column_index >= proj_it->second->expressions.size()) {
			return false;
		}
		string projection_cast;
		auto *next = GetColumnRefThroughCasts(proj_it->second->expressions[binding.column_index].get(),
		                                      cast_type ? &projection_cast : nullptr);
		if (!next) {
			return false;
		}
		if (cast_type && !projection_cast.empty()) {
			*cast_type = SqlUtils::ComposeCastSpecs(*cast_type, projection_cast);
		}
		binding = next->Binding();
	}
	return false;
}

static bool SetOccurrenceRef(LogicalGet &get, const CreateMVPlanFacts &facts, OccurrenceColumnRef &out) {
	auto occurrence = facts.occurrence_by_index.find(get.table_index);
	if (occurrence == facts.occurrence_by_index.end()) {
		return false;
	}
	out.table = occurrence->second.table;
	out.occurrence = occurrence->second.occurrence;
	return true;
}

static bool ResolveBindingToOccurrenceRef(ColumnBinding binding, const CreateMVPlanFacts &facts,
                                          OccurrenceColumnRef &out) {
	LogicalGet *get = nullptr;
	return ResolveBindingToGetColumn(binding, facts, get, out.column) && SetOccurrenceRef(*get, facts, out);
}

static bool ResolveBindingToOccurrenceRefWithCast(ColumnBinding binding, const CreateMVPlanFacts &facts,
                                                  OccurrenceColumnRef &out, string &cast_type,
                                                  const LogicalType &expression_type) {
	LogicalGet *get = nullptr;
	ProjectionIndex base_column;
	if (!ResolveBindingToGetColumn(binding, facts, get, out.column, &base_column, &cast_type) ||
	    !SetOccurrenceRef(*get, facts, out)) {
		return false;
	}
	LogicalType base_type;
	if (cast_type.empty() && GetLogicalGetColumnType(*get, base_column, base_type) && !(base_type == expression_type)) {
		cast_type = expression_type.ToString();
	}
	return true;
}

static void AddJoinEdgesFromFacts(CreateMVPlanFacts &facts) {
	for (auto *join : facts.comparison_joins) {
		if (join->join_type != JoinType::INNER) {
			continue;
		}
		for (auto &cond : join->conditions) {
			if (!cond.IsComparison()) {
				continue;
			}
			auto *left = GetColumnRefThroughCasts(cond.LeftReference().get());
			auto *right = GetColumnRefThroughCasts(cond.RightReference().get());
			if (!left || !right) {
				continue;
			}
			if (cond.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			OccurrenceColumnRef lref, rref;
			if (ResolveBindingToOccurrenceRef(left->Binding(), facts, lref) &&
			    ResolveBindingToOccurrenceRef(right->Binding(), facts, rref)) {
				facts.projection_lineage_edges.push_back({lref, rref});
				facts.projection_lineage_edges.push_back({rref, lref});
			}
		}
	}
}

bool ProducesAtMostOneRow(LogicalOperator &node) {
	LogicalOperator *current = &node;
	while ((current->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	        current->type == LogicalOperatorType::LOGICAL_FILTER) &&
	       !current->children.empty()) {
		current = current->children[0].get();
	}
	if (current->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = current->Cast<LogicalAggregate>();
	return aggregate.groups.empty() && aggregate.grouping_sets.size() <= 1;
}

static bool ResolveBindingToGroupKey(const ColumnBinding &binding,
                                     const unordered_map<TableIndex, LogicalProjection *> &projections,
                                     const LogicalAggregate &aggregate, idx_t &group_index, idx_t depth = 0) {
	if (depth > 16) {
		return false;
	}
	if (binding.table_index == aggregate.group_index) {
		if (binding.column_index >= aggregate.groups.size()) {
			return false;
		}
		group_index = binding.column_index;
		return true;
	}
	auto projection = projections.find(binding.table_index);
	if (projection == projections.end() || binding.column_index >= projection->second->expressions.size()) {
		return false;
	}
	auto &expression = projection->second->expressions[binding.column_index];
	if (expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &column = expression->Cast<BoundColumnRefExpression>();
	return ResolveBindingToGroupKey(column.Binding(), projections, aggregate, group_index, depth + 1);
}

bool IsRedundantDistinctOverGroupKeys(LogicalOperator &node) {
	if (node.type != LogicalOperatorType::LOGICAL_DISTINCT || node.children.empty()) {
		return false;
	}
	auto &distinct = node.Cast<LogicalDistinct>();
	if (distinct.distinct_type != DistinctType::DISTINCT || distinct.order_by) {
		return false;
	}

	auto *output = node.children[0].get();
	auto *current = output;
	unordered_map<TableIndex, LogicalProjection *> projections;
	while (current && current->children.size() == 1 &&
	       (current->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	        current->type == LogicalOperatorType::LOGICAL_FILTER)) {
		if (current->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = current->Cast<LogicalProjection>();
			projections[projection.table_index] = &projection;
		}
		current = current->children[0].get();
	}
	if (!current || current->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = current->Cast<LogicalAggregate>();
	if (aggregate.groups.empty() || aggregate.grouping_sets.size() > 1) {
		return false;
	}
	if (!aggregate.grouping_sets.empty() && aggregate.grouping_sets[0].size() != aggregate.groups.size()) {
		return false;
	}

	auto output_bindings = output->GetColumnBindings();
	if (output_bindings.size() != aggregate.groups.size()) {
		return false;
	}
	vector<bool> seen_group(aggregate.groups.size(), false);
	for (auto &binding : output_bindings) {
		idx_t group_index;
		if (!ResolveBindingToGroupKey(binding, projections, aggregate, group_index) || seen_group[group_index]) {
			return false;
		}
		seen_group[group_index] = true;
	}
	return true;
}

CreateMVPlanFacts BuildCreateMVPlanFacts(LogicalOperator *plan, const string &current_catalog) {
	CreateMVPlanFacts facts;
	facts.root = plan;
	LogicalOperator *top = plan;
	while (top && top->children.size() == 1 &&
	       (top->type == LogicalOperatorType::LOGICAL_CREATE_TABLE ||
	        top->type == LogicalOperatorType::LOGICAL_PROJECTION || top->type == LogicalOperatorType::LOGICAL_FILTER ||
	        top->type == LogicalOperatorType::LOGICAL_ORDER_BY || top->type == LogicalOperatorType::LOGICAL_LIMIT ||
	        top->type == LogicalOperatorType::LOGICAL_TOP_N)) {
		top = top->children[0].get();
	}
	facts.has_top_level_redundant_distinct =
	    top && top->type == LogicalOperatorType::LOGICAL_DISTINCT && !top->children.empty() &&
	    (ProducesAtMostOneRow(*top->children[0]) || IsRedundantDistinctOverGroupKeys(*top));
	unordered_map<string, idx_t> next_occurrence;
	CollectCreateMVPlanFacts(plan, current_catalog, facts, next_occurrence, false,
	                         facts.has_top_level_redundant_distinct ? top : nullptr, facts.analysis);
	next_occurrence.clear();
	for (auto &source : facts.source_occurrences) {
		if (facts.source_name_counts[source.table] <= 1) {
			continue;
		}
		auto table = facts.gets_by_index.at(source.table_index)->GetTable();
		source.table = SqlUtils::FullName(table->ParentCatalog().GetName(), table->schema.name, table->name);
		source.occurrence = next_occurrence[source.table]++;
		facts.occurrence_by_index[source.table_index] = source;
	}
	FinalizeCreateMVPlanFacts(facts);
	AddJoinEdgesFromFacts(facts);
	return facts;
}

static bool FindProjectionPath(const ProjectionSourceOccurrence &source, const OccurrenceColumnRef &key_ref,
                               const unordered_map<string, vector<ProjectionLineageEdge>> &adjacency,
                               vector<ProjectionLineageEdge> &path) {
	string source_key = OccurrenceKey(source.table, source.occurrence);
	string target_key = OccurrenceKey(key_ref);
	if (source_key == target_key) {
		path.clear();
		return true;
	}

	vector<string> queue;
	unordered_map<string, bool> seen;
	unordered_map<string, ProjectionLineageEdge> prev_edge;
	unordered_map<string, string> prev_node;
	queue.push_back(source_key);
	seen[source_key] = true;
	for (idx_t pos = 0; pos < queue.size(); pos++) {
		string node = queue[pos];
		auto adjacency_it = adjacency.find(node);
		if (adjacency_it == adjacency.end()) {
			continue;
		}
		for (auto &edge : adjacency_it->second) {
			string next = OccurrenceKey(edge.to);
			if (seen[next]) {
				continue;
			}
			seen[next] = true;
			prev_edge[next] = edge;
			prev_node[next] = node;
			if (next == target_key) {
				vector<ProjectionLineageEdge> reversed;
				string walk = target_key;
				while (walk != source_key) {
					reversed.push_back(prev_edge[walk]);
					walk = prev_node[walk];
				}
				path.assign(reversed.rbegin(), reversed.rend());
				return true;
			}
			queue.push_back(next);
		}
	}
	return false;
}

static bool BuildProjectionLineageArm(const ProjectionSourceOccurrence &source, const OccurrenceColumnRef &key_ref,
                                      const vector<ProjectionLineageEdge> &path, ProjectionLineageArm &arm) {
	arm.source = source.table;
	arm.occurrence = source.occurrence;
	arm.steps.clear();
	if (path.empty()) {
		arm.source_col = key_ref.column;
		return StringUtil::CIEquals(source.table, key_ref.table) && source.occurrence == key_ref.occurrence;
	}
	arm.source_col = path[0].from.column;
	for (idx_t i = 0; i < path.size(); i++) {
		ProjectionLineageStep step;
		step.table = path[i].to.table;
		step.occurrence = path[i].to.occurrence;
		step.lookup_col = path[i].to.column;
		step.lookup_out = i + 1 < path.size() ? path[i + 1].from.column : key_ref.column;
		arm.steps.push_back(std::move(step));
	}
	return true;
}

string BuildRefreshLineageJson(const vector<string> &entries) {
	if (entries.empty()) {
		return "";
	}
	return "{\"v\":1,\"items\":[" +
	       StringUtil::Join(entries, entries.size(), ",", [](const string &entry) { return entry; }) + "]}";
}

bool BuildProjectionKeyLineage(const CreateMVPlanFacts &facts, const vector<string> &output_names,
                               RefreshMetadata::ProjectionKeyLineage &out) {
	if (!facts.root) {
		return false;
	}
	auto *top_projection = facts.first_projection;
	if (!top_projection) {
		return false;
	}
	if (facts.source_occurrences.size() < 3) {
		return false;
	}
	if (facts.projection_lineage_edges.empty()) {
		return false;
	}

	unordered_map<string, vector<ProjectionLineageEdge>> adjacency;
	for (auto &edge : facts.projection_lineage_edges) {
		adjacency[OccurrenceKey(edge.from)].push_back(edge);
	}

	for (idx_t expr_i = 0; expr_i < top_projection->expressions.size(); expr_i++) {
		auto &expr = top_projection->expressions[expr_i];
		auto *bcr = GetColumnRefThroughCasts(expr.get());
		if (!bcr) {
			continue;
		}
		OccurrenceColumnRef key_ref;
		if (!ResolveBindingToOccurrenceRef(bcr->Binding(), facts, key_ref)) {
			continue;
		}
		string output_col = ProjectionOutputName(*top_projection, expr_i, facts, output_names, *bcr);
		if (output_col.empty() || IncrementalTableNames::IsInternalColumn(output_col)) {
			continue;
		}

		vector<ProjectionLineageArm> arms;
		bool covered = true;
		for (auto &occurrence : facts.source_occurrences) {
			vector<ProjectionLineageEdge> path;
			if (!FindProjectionPath(occurrence, key_ref, adjacency, path)) {
				covered = false;
				break;
			}
			ProjectionLineageArm arm;
			if (!BuildProjectionLineageArm(occurrence, key_ref, path, arm)) {
				covered = false;
				break;
			}
			arms.push_back(std::move(arm));
		}
		if (!covered || arms.empty()) {
			continue;
		}

		RefreshMetadata::ProjectionKeyLineage lineage;
		lineage.output_col = output_col;
		lineage.key_source = key_ref.table;
		lineage.key_occurrence = key_ref.occurrence;
		lineage.key_col = key_ref.column;
		lineage.arms = std::move(arms);
		out = std::move(lineage);
		return true;
	}
	return false;
}

bool BuildLeftJoinKeySource(const CreateMVPlanFacts &facts, RefreshMetadata::LeftJoinKeySource &out) {
	auto *join = FindProjectionKeyJoin(*facts.root);
	if (join && join->join_type == JoinType::LEFT) {
		auto *preserved_key = GetColumnRefThroughCasts(join->conditions[0].LeftReference().get(), &out.cast_type);
		if (!preserved_key) {
			return false;
		}
		OccurrenceColumnRef source;
		if (!ResolveBindingToOccurrenceRefWithCast(preserved_key->Binding(), facts, source, out.cast_type,
		                                           preserved_key->GetReturnType())) {
			return false;
		}
		out.table = source.table;
		out.occurrence = source.occurrence;
		out.column = source.column;
		out.cardinality_transition_check_safe =
		    facts.outer_joins.size() == 1 && join->conditions.size() == 1 &&
		    join->conditions[0].GetComparisonType() == ExpressionType::COMPARE_EQUAL;
		return true;
	}
	return false;
}

static string NullableGetTableName(LogicalGet &get) {
	auto table = get.GetTable();
	if (table.get() && !table.get()->name.empty()) {
		return table.get()->name.GetIdentifierName();
	}
	if (get.function.GetName().GetIdentifierName() == "ducklake_scan" && get.function.function_info) {
		return get.function.function_info->Cast<DuckLakeFunctionInfo>().table_name.GetIdentifierName();
	}
	return "";
}

static string NormalizeNullableTableName(const string &table_name) {
	static const string data_prefix(openivm::DATA_TABLE_PREFIX);
	static const string delta_prefix(openivm::DELTA_PREFIX);
	string last = SqlUtils::LastIdentifierPart(table_name);
	if (last.size() > data_prefix.size() && last.rfind(data_prefix, 0) == 0) {
		last = last.substr(data_prefix.size());
	}
	if (last.size() > delta_prefix.size() && last.rfind(delta_prefix, 0) == 0) {
		last = last.substr(delta_prefix.size());
	}
	return StringUtil::Lower(last);
}

bool BuildLeftJoinNullableSources(const CreateMVPlanFacts &facts, RefreshMetadata::LeftJoinNullableSources &out) {
	out.tables.clear();
	out.complete = true;
	std::set<string> tables;
	bool found_any = false;
	for (auto *join : facts.outer_joins) {
		if (!join || join->children.size() < 2) {
			continue;
		}
		auto add_nullable_child = [&](idx_t child_idx) {
			auto *subtree = GetPlanNodeFacts(facts, join->children[child_idx].get());
			if (!subtree) {
				out.complete = false;
				return;
			}
			tables.insert(subtree->subtree_base_tables.begin(), subtree->subtree_base_tables.end());
			out.complete = out.complete && subtree->subtree_base_tables_complete;
		};
		found_any = true;
		if (join->join_type == JoinType::LEFT) {
			add_nullable_child(1);
		} else if (join->join_type == JoinType::RIGHT) {
			add_nullable_child(0);
		} else {
			D_ASSERT(join->join_type == JoinType::OUTER);
			add_nullable_child(0);
			add_nullable_child(1);
		}
	}
	out.tables.assign(tables.begin(), tables.end());
	return found_any;
}

struct WindowLineageOp {
	WindowLineageKind kind = WindowLineageKind::DIRECT;
	string output_col;
	string source_table;
	idx_t source_occurrence = 0;
	string source_col;
	string source_cast;
	string lookup_table;
	idx_t lookup_occurrence = 0;
	string lookup_col;
	string lookup_cast;
	string lookup_output_col;
	string lookup_output_cast;
};

struct WindowLookupEdge {
	OccurrenceColumnRef lookup_join;
	string lookup_cast;
	OccurrenceColumnRef changed_join;
	string changed_cast;
};

struct WindowEquivalenceEdge {
	OccurrenceColumnRef first;
	string first_cast;
	OccurrenceColumnRef second;
	string second_cast;

	bool HasCast() const {
		return !first_cast.empty() || !second_cast.empty();
	}
};

static void CollectLeafColumnBindings(Expression *expr, vector<ColumnBinding> &bindings) {
	if (!expr) {
		return;
	}
	if (auto *bcr = GetColumnRefThroughCasts(expr)) {
		bindings.push_back(bcr->Binding());
		return;
	}
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](Expression &child) { CollectLeafColumnBindings(&child, bindings); });
}

static void AddUniqueOccurrenceRef(vector<OccurrenceColumnRef> &refs, const OccurrenceColumnRef &ref) {
	for (auto &existing : refs) {
		if (StringUtil::CIEquals(existing.table, ref.table) && existing.occurrence == ref.occurrence &&
		    StringUtil::CIEquals(existing.column, ref.column)) {
			return;
		}
	}
	refs.push_back(ref);
}

static bool ResolveBindingToOccurrenceRefsInternal(ColumnBinding binding, const CreateMVPlanFacts &facts,
                                                   vector<OccurrenceColumnRef> &out, int depth) {
	if (depth >= 16) {
		return false;
	}
	OccurrenceColumnRef direct_ref;
	if (ResolveBindingToOccurrenceRef(binding, facts, direct_ref)) {
		AddUniqueOccurrenceRef(out, direct_ref);
		return true;
	}
	auto proj_it = facts.projections_by_index.find(binding.table_index);
	if (proj_it == facts.projections_by_index.end()) {
		return false;
	}
	auto *proj = proj_it->second;
	if (binding.column_index >= proj->expressions.size()) {
		return false;
	}
	vector<ColumnBinding> child_bindings;
	CollectLeafColumnBindings(proj->expressions[binding.column_index].get(), child_bindings);
	bool found = false;
	for (auto &child_binding : child_bindings) {
		if (child_binding.table_index == binding.table_index && child_binding.column_index == binding.column_index) {
			continue;
		}
		found |= ResolveBindingToOccurrenceRefsInternal(child_binding, facts, out, depth + 1);
	}
	return found;
}

static bool ResolveBindingToOccurrenceRefs(ColumnBinding binding, const CreateMVPlanFacts &facts,
                                           vector<OccurrenceColumnRef> &out) {
	out.clear();
	return ResolveBindingToOccurrenceRefsInternal(binding, facts, out, 0);
}

static void CollectWindowJoinEdges(LogicalComparisonJoin &join, const CreateMVPlanFacts &facts,
                                   vector<WindowEquivalenceEdge> &equivalence_edges,
                                   vector<WindowLookupEdge> &lookup_edges) {
	if (join.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    join.type != LogicalOperatorType::LOGICAL_ASOF_JOIN) {
		return;
	}
	auto add_lookup_edge = [&](OccurrenceColumnRef lookup_ref, string lookup_cast, OccurrenceColumnRef changed_ref,
	                           string changed_cast) {
		lookup_edges.push_back(
		    {std::move(lookup_ref), std::move(lookup_cast), std::move(changed_ref), std::move(changed_cast)});
	};
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			continue;
		}
		if (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		string left_cast;
		string right_cast;
		auto *left = GetColumnRefThroughCasts(condition.LeftReference().get(), &left_cast);
		auto *right = GetColumnRefThroughCasts(condition.RightReference().get(), &right_cast);
		if (!left || !right) {
			continue;
		}
		OccurrenceColumnRef left_ref, right_ref;
		if (!ResolveBindingToOccurrenceRefWithCast(left->Binding(), facts, left_ref, left_cast,
		                                           left->GetReturnType()) ||
		    !ResolveBindingToOccurrenceRefWithCast(right->Binding(), facts, right_ref, right_cast,
		                                           right->GetReturnType())) {
			continue;
		}
		switch (join.join_type) {
		case JoinType::INNER:
		case JoinType::LEFT:
			add_lookup_edge(left_ref, left_cast, right_ref, right_cast);
			add_lookup_edge(right_ref, right_cast, left_ref, left_cast);
			break;
		case JoinType::RIGHT:
			add_lookup_edge(right_ref, right_cast, left_ref, left_cast);
			add_lookup_edge(left_ref, left_cast, right_ref, right_cast);
			break;
		default:
			break;
		}
		if (join.join_type == JoinType::INNER) {
			equivalence_edges.push_back(
			    {std::move(left_ref), std::move(left_cast), std::move(right_ref), std::move(right_cast)});
		}
	}
}

static void CollectWindowPartitionRefs(LogicalWindow &window, const CreateMVPlanFacts &facts,
                                       const vector<string> &partition_columns, vector<WindowLineageOp> &direct_ops) {
	for (auto &expression : window.expressions) {
		if (expression->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
			continue;
		}
		auto &window_expression = expression->Cast<BoundWindowExpression>();
		for (auto &partition : window_expression.Partitions()) {
			string partition_cast;
			auto *column_ref = GetColumnRefThroughCasts(partition.get(), &partition_cast);
			if (!column_ref) {
				continue;
			}
			vector<OccurrenceColumnRef> refs;
			if (!ResolveBindingToOccurrenceRefs(column_ref->Binding(), facts, refs)) {
				continue;
			}
			for (auto &ref : refs) {
				for (auto &stored : partition_columns) {
					auto pos = stored.find('=');
					string output_col = pos == string::npos ? stored : stored.substr(0, pos);
					string source_col = pos == string::npos ? stored : stored.substr(pos + 1);
					if (!StringUtil::CIEquals(source_col, ref.column)) {
						continue;
					}
					WindowLineageOp op;
					op.kind = WindowLineageKind::DIRECT;
					op.output_col = output_col;
					op.source_table = ref.table;
					op.source_occurrence = ref.occurrence;
					op.source_col = ref.column;
					op.source_cast = partition_cast;
					direct_ops.push_back(std::move(op));
				}
			}
		}
	}
}

static bool SameRef(const OccurrenceColumnRef &a, const OccurrenceColumnRef &b) {
	return StringUtil::CIEquals(a.table, b.table) && a.occurrence == b.occurrence &&
	       StringUtil::CIEquals(a.column, b.column);
}

static bool SameOp(const WindowLineageOp &a, const WindowLineageOp &b) {
	return a.kind == b.kind && StringUtil::CIEquals(a.output_col, b.output_col) &&
	       StringUtil::CIEquals(a.source_table, b.source_table) && a.source_occurrence == b.source_occurrence &&
	       StringUtil::CIEquals(a.source_col, b.source_col) && StringUtil::CIEquals(a.source_cast, b.source_cast) &&
	       StringUtil::CIEquals(a.lookup_table, b.lookup_table) && a.lookup_occurrence == b.lookup_occurrence &&
	       StringUtil::CIEquals(a.lookup_col, b.lookup_col) && StringUtil::CIEquals(a.lookup_cast, b.lookup_cast) &&
	       StringUtil::CIEquals(a.lookup_output_col, b.lookup_output_col) &&
	       StringUtil::CIEquals(a.lookup_output_cast, b.lookup_output_cast);
}

static void AddUniqueLineageOp(vector<WindowLineageOp> &ops, WindowLineageOp op) {
	for (auto &existing : ops) {
		if (SameOp(existing, op)) {
			return;
		}
	}
	ops.push_back(std::move(op));
}

static bool HasEquivalentPartitionRef(const vector<WindowLineageOp> &ops, const OccurrenceColumnRef &ref,
                                      const string &output_col) {
	for (auto &op : ops) {
		if (op.kind == WindowLineageKind::DIRECT && StringUtil::CIEquals(op.output_col, output_col) &&
		    StringUtil::CIEquals(op.source_table, ref.table) && op.source_occurrence == ref.occurrence &&
		    StringUtil::CIEquals(op.source_col, ref.column)) {
			return true;
		}
	}
	return false;
}

static RefreshMetadata::WindowPartitionLineageOp ToMetadataWindowLineageOp(const WindowLineageOp &op) {
	RefreshMetadata::WindowPartitionLineageOp metadata_op;
	metadata_op.kind = op.kind;
	metadata_op.output_col = op.output_col;
	metadata_op.source = op.source_table;
	metadata_op.source_col = op.source_col;
	metadata_op.source_cast = op.source_cast;
	metadata_op.lookup = op.lookup_table;
	metadata_op.lookup_col = op.lookup_col;
	metadata_op.lookup_cast = op.lookup_cast;
	metadata_op.lookup_out = op.lookup_output_col;
	metadata_op.lookup_out_cast = op.lookup_output_cast;
	return metadata_op;
}

bool BuildWindowPartitionLineageOps(const CreateMVPlanFacts &facts, const vector<string> &partition_columns,
                                    vector<RefreshMetadata::WindowPartitionLineageOp> &out,
                                    vector<RefreshMetadata::WindowPartitionLineageOp> *direct_out) {
	out.clear();
	if (direct_out) {
		direct_out->clear();
	}
	if (!facts.root || partition_columns.empty()) {
		return false;
	}
	if (facts.source_occurrences.empty()) {
		return false;
	}

	vector<WindowLineageOp> direct_ops;
	for (auto *window : facts.windows) {
		CollectWindowPartitionRefs(*window, facts, partition_columns, direct_ops);
	}
	if (direct_ops.empty()) {
		return false;
	}
	if (direct_out) {
		for (auto &op : direct_ops) {
			direct_out->push_back(ToMetadataWindowLineageOp(op));
		}
	}

	vector<WindowEquivalenceEdge> edges;
	vector<WindowLookupEdge> lookup_edges;
	for (auto *join : facts.comparison_joins) {
		CollectWindowJoinEdges(*join, facts, edges, lookup_edges);
	}

	vector<WindowLineageOp> partition_ops;
	for (auto &op : direct_ops) {
		AddUniqueLineageOp(partition_ops, op);
	}

	bool changed = true;
	while (changed) {
		changed = false;
		vector<WindowLineageOp> next_ops = partition_ops;
		for (auto &edge : edges) {
			if (edge.HasCast()) {
				continue;
			}
			for (auto &direct : partition_ops) {
				OccurrenceColumnRef direct_ref;
				direct_ref.table = direct.source_table;
				direct_ref.occurrence = direct.source_occurrence;
				direct_ref.column = direct.source_col;
				OccurrenceColumnRef other;
				bool found = false;
				if (SameRef(edge.first, direct_ref)) {
					other = edge.second;
					found = true;
				} else if (SameRef(edge.second, direct_ref)) {
					other = edge.first;
					found = true;
				}
				if (!found || HasEquivalentPartitionRef(next_ops, other, direct.output_col)) {
					continue;
				}
				WindowLineageOp equivalent;
				equivalent.kind = WindowLineageKind::DIRECT;
				equivalent.output_col = direct.output_col;
				equivalent.source_table = other.table;
				equivalent.source_occurrence = other.occurrence;
				equivalent.source_col = other.column;
				equivalent.source_cast = direct.source_cast;
				AddUniqueLineageOp(next_ops, std::move(equivalent));
				changed = true;
			}
		}
		partition_ops = std::move(next_ops);
	}

	vector<WindowLineageOp> ops;
	for (auto &op : partition_ops) {
		AddUniqueLineageOp(ops, op);
	}

	for (auto &edge : lookup_edges) {
		for (auto &direct : partition_ops) {
			if (!StringUtil::CIEquals(edge.lookup_join.table, direct.source_table) ||
			    edge.lookup_join.occurrence != direct.source_occurrence) {
				continue;
			}
			if (StringUtil::CIEquals(edge.lookup_join.column, direct.source_col) &&
			    StringUtil::CIEquals(edge.changed_join.table, direct.source_table) &&
			    edge.changed_join.occurrence == direct.source_occurrence) {
				continue;
			}
			WindowLineageOp lookup;
			lookup.kind = WindowLineageKind::LOOKUP;
			lookup.output_col = direct.output_col;
			lookup.source_table = edge.changed_join.table;
			lookup.source_occurrence = edge.changed_join.occurrence;
			lookup.source_col = edge.changed_join.column;
			lookup.source_cast = edge.changed_cast;
			lookup.lookup_table = direct.source_table;
			lookup.lookup_occurrence = direct.source_occurrence;
			lookup.lookup_col = edge.lookup_join.column;
			lookup.lookup_cast = edge.lookup_cast;
			lookup.lookup_output_col = direct.source_col;
			lookup.lookup_output_cast = direct.source_cast;
			AddUniqueLineageOp(ops, std::move(lookup));
		}
	}

	for (auto &op : ops) {
		out.push_back(ToMetadataWindowLineageOp(op));
	}
	return !out.empty();
}

void ResolveWindowPartitionOutputNames(const CreateMVPlanFacts &facts, vector<string> &partition_columns,
                                       const vector<string> &output_names) {
	if (partition_columns.empty()) {
		return;
	}
	auto *top_projection = facts.first_projection;
	if (!top_projection) {
		return;
	}

	for (auto &partition_column : partition_columns) {
		if (partition_column.find('=') != string::npos) {
			continue;
		}
		for (idx_t expr_i = 0; expr_i < top_projection->expressions.size(); expr_i++) {
			auto &expr = top_projection->expressions[expr_i];
			auto *bcr = GetColumnRefThroughCasts(expr.get());
			if (!bcr) {
				continue;
			}
			string base_col = ResolveBindingToBaseColumn(bcr, facts);
			if (!StringUtil::CIEquals(base_col, partition_column) &&
			    !StringUtil::CIEquals(bcr->GetName().GetIdentifierName(), partition_column)) {
				continue;
			}
			string output_col = ProjectionOutputName(*top_projection, expr_i, facts, output_names, *bcr);
			if (!output_col.empty() && !IncrementalTableNames::IsInternalColumn(output_col)) {
				partition_column = output_col + "=" + partition_column;
			}
			break;
		}
	}
}

void ResolveAggregateGroupColumnsThroughJoinKeys(const CreateMVPlanFacts &facts, vector<string> &aggregate_columns,
                                                 const vector<string> &output_names) {
	if (aggregate_columns.empty()) {
		return;
	}
	auto *top_projection = facts.first_projection;
	if (!top_projection) {
		return;
	}
	auto parents = facts.join_parents;
	if (facts.join_refs.empty()) {
		return;
	}

	unordered_map<string, string> output_by_parent;
	for (idx_t expr_i = 0; expr_i < top_projection->expressions.size(); expr_i++) {
		auto &expr = top_projection->expressions[expr_i];
		auto *bcr = GetColumnRefThroughCasts(expr.get());
		if (!bcr) {
			continue;
		}
		string output_col = ProjectionOutputName(*top_projection, expr_i, facts, output_names, *bcr);
		if (output_col.empty() || IncrementalTableNames::IsInternalColumn(output_col)) {
			continue;
		}
		string parent = FindBindingParent(parents, BindingKey(*bcr));
		if (!output_by_parent.count(parent)) {
			output_by_parent[parent] = output_col;
		}
	}

	auto is_output = [&](const string &column) {
		return std::any_of(output_names.begin(), output_names.end(),
		                   [&](const string &output_col) { return StringUtil::CIEquals(column, output_col); });
	};
	for (auto &group_col : aggregate_columns) {
		if (is_output(group_col)) {
			continue;
		}
		for (auto *ref : facts.join_refs) {
			string ref_col = ResolveBindingToBaseColumn(ref, facts);
			if (ref_col.empty()) {
				ref_col = ref->GetName().GetIdentifierName();
			}
			if (!StringUtil::CIEquals(ref_col, group_col) &&
			    !StringUtil::CIEquals(ref->GetName().GetIdentifierName(), group_col)) {
				continue;
			}
			string parent = FindBindingParent(parents, BindingKey(*ref));
			auto out_it = output_by_parent.find(parent);
			if (out_it != output_by_parent.end()) {
				group_col = out_it->second;
				break;
			}
		}
		if (is_output(group_col)) {
			continue;
		}
		string suffix = group_col;
		while (true) {
			auto underscore = suffix.find('_');
			if (underscore == string::npos || underscore + 1 >= suffix.size()) {
				break;
			}
			suffix = suffix.substr(underscore + 1);
			string matched_output;
			for (auto &output_col : output_names) {
				if (StringUtil::CIEquals(output_col, suffix)) {
					if (!matched_output.empty()) {
						matched_output.clear();
						break;
					}
					matched_output = output_col;
				}
			}
			if (!matched_output.empty()) {
				group_col = matched_output;
				break;
			}
		}
	}
}

string ExtractFullOuterJoinMetadata(const CreateMVPlanFacts &facts) {
	struct FullOuterEdge {
		OccurrenceColumnRef left;
		OccurrenceColumnRef right;
	};
	vector<FullOuterEdge> edges;
	auto add_edge = [&](const OccurrenceColumnRef &left, const OccurrenceColumnRef &right) {
		for (const auto &edge : edges) {
			if (StringUtil::CIEquals(edge.left.table, left.table) &&
			    StringUtil::CIEquals(edge.left.column, left.column) &&
			    StringUtil::CIEquals(edge.right.table, right.table) &&
			    StringUtil::CIEquals(edge.right.column, right.column)) {
				return;
			}
		}
		edges.push_back({left, right});
	};
	auto *projection_join = facts.analysis.found_aggregation ? nullptr : FindProjectionKeyJoin(*facts.root);
	for (auto *join : facts.comparison_joins) {
		if (join->join_type != JoinType::OUTER) {
			continue;
		}
		if (projection_join && join != projection_join) {
			continue;
		}
		for (auto &condition : join->conditions) {
			if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			auto left_ref = GetColumnRefThroughCasts(condition.LeftReference().get());
			auto right_ref = GetColumnRefThroughCasts(condition.RightReference().get());
			if (!left_ref || !right_ref) {
				continue;
			}
			vector<OccurrenceColumnRef> left_sources;
			vector<OccurrenceColumnRef> right_sources;
			if (!ResolveBindingToOccurrenceRefs(left_ref->Binding(), facts, left_sources) ||
			    !ResolveBindingToOccurrenceRefs(right_ref->Binding(), facts, right_sources)) {
				continue;
			}
			for (const auto &left : left_sources) {
				for (const auto &right : right_sources) {
					add_edge(left, right);
				}
			}
		}
	}
	if (edges.empty()) {
		return "";
	}

	vector<string> serialized_edges;
	serialized_edges.reserve(edges.size());
	for (const auto &edge : edges) {
		MetadataJson::Writer writer;
		writer.Str("left_table", edge.left.table);
		writer.Str("left_column", edge.left.column);
		writer.Str("right_table", edge.right.table);
		writer.Str("right_column", edge.right.column);
		serialized_edges.push_back(writer.Finish());
	}
	MetadataJson::Writer writer;
	writer.Objects("edges", serialized_edges);
	OPENIVM_DEBUG_PRINT("[CREATE MV] Recorded %zu physical FULL OUTER key edges\n", edges.size());
	return writer.Finish();
}

static bool ScopePresenceMatches(ColumnBinding binding, const string &name, const CreateMVPlanFacts &facts) {
	auto projection = facts.projections_by_index.find(binding.table_index);
	if (projection != facts.projections_by_index.end()) {
		auto &expression = *projection->second->expressions[binding.column_index];
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &value = expression.Cast<BoundConstantExpression>().GetValue();
			return expression.GetAlias().GetIdentifierName() == name && !value.IsNull() && value.GetValue<bool>();
		}
		if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			return ScopePresenceMatches(expression.Cast<BoundColumnRefExpression>().Binding(), name, facts);
		}
	}
	for (auto *aggregate : facts.aggregates) {
		if (binding.table_index != aggregate->aggregate_index) {
			continue;
		}
		auto &expression = aggregate->expressions[binding.column_index]->Cast<BoundAggregateExpression>();
		if (expression.Function().GetName().GetIdentifierName() == "bool_or" && expression.GetChildren().size() == 1 &&
		    expression.GetChildren()[0]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			return ScopePresenceMatches(expression.GetChildren()[0]->Cast<BoundColumnRefExpression>().Binding(), name,
			                            facts);
		}
	}
	auto setop = facts.setops_by_index.find(binding.table_index);
	if (setop != facts.setops_by_index.end()) {
		for (auto &child : setop->second->children) {
			if (ScopePresenceMatches(child->GetColumnBindings()[binding.column_index], name, facts)) {
				return true;
			}
		}
	}
	return false;
}

static bool CollectFullOuterScopeSources(LogicalOperator &node, const LogicalProjection &input_projection,
                                         const CreateMVPlanFacts &facts, const string &presence,
                                         vector<RefreshMetadata::FullOuterScopeInput> &sources) {
	string input_presence = presence;
	if (node.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		for (auto &expression : node.Cast<LogicalProjection>().expressions) {
			if (expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			    StringUtil::StartsWith(expression->GetAlias().GetIdentifierName(), openivm::FULL_SCOPE_PREFIX)) {
				input_presence = expression->GetAlias().GetIdentifierName();
				break;
			}
		}
	}
	if (node.type == LogicalOperatorType::LOGICAL_GET) {
		auto entry = facts.occurrence_by_index.find(node.Cast<LogicalGet>().table_index);
		if (entry == facts.occurrence_by_index.end()) {
			return false;
		}
		RefreshMetadata::FullOuterScopeInput input;
		input.table = entry->second.table;
		input.occurrence = entry->second.occurrence;
		bool found = false;
		for (idx_t i = 0; i < input_projection.expressions.size(); i++) {
			if (ScopePresenceMatches(ColumnBinding(TableIndex(input_projection.table_index), ProjectionIndex(i)),
			                         input_presence, facts)) {
				input.presence_index = i;
				found = true;
				break;
			}
		}
		D_ASSERT(found);
		if (!found) {
			throw InternalException("Missing source presence in FULL JOIN input");
		}
		sources.push_back(std::move(input));
	}
	for (auto &child : node.children) {
		if (!CollectFullOuterScopeSources(*child, input_projection, facts, input_presence, sources)) {
			return false;
		}
	}
	return true;
}

vector<RefreshMetadata::FullOuterScope> BuildFullOuterScopes(const CreateMVPlanFacts &facts) {
	vector<RefreshMetadata::FullOuterScope> scopes;
	for (auto *join : facts.comparison_joins) {
		if ((join->join_type != JoinType::OUTER && join->join_type != JoinType::LEFT &&
		     join->join_type != JoinType::RIGHT) ||
		    join->conditions.empty()) {
			continue;
		}
		RefreshMetadata::FullOuterScope scope;
		bool complete = true;
		for (idx_t side = 0; side < 2; side++) {
			auto &child = *join->children[side];
			if (child.type != LogicalOperatorType::LOGICAL_PROJECTION) {
				complete = false;
				break;
			}
			auto &projection = child.Cast<LogicalProjection>();
			auto &source = side == 0 ? scope.left : scope.right;
			for (auto &expression : projection.expressions) {
				if (expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
				    StringUtil::StartsWith(expression->GetAlias().GetIdentifierName(), openivm::FULL_SCOPE_PREFIX)) {
					source.presence_column = expression->GetAlias().GetIdentifierName();
					break;
				}
			}
			if (source.presence_column.empty()) {
				complete = false;
				break;
			}
			source.key_column = source.presence_column + "_key";
			if (!CollectFullOuterScopeSources(child, projection, facts, source.presence_column, source.inputs) ||
			    source.inputs.empty()) {
				complete = false;
				break;
			}
		}
		if (complete) {
			scopes.push_back(std::move(scope));
		}
	}
	return scopes;
}

static string SanitizeOutputName(const string &name) {
	if (IncrementalTableNames::IsInternalColumn(name)) {
		return name;
	}
	string clean;
	bool last_was_underscore = false;
	for (auto c : name) {
		if (isalnum(c)) {
			clean += c;
			last_was_underscore = false;
		} else if (!last_was_underscore && !clean.empty()) {
			clean += '_';
			last_was_underscore = true;
		}
	}
	if (!clean.empty() && clean.back() == '_') {
		clean.pop_back();
	}
	return clean.empty() ? name : clean;
}

static bool IsReservedMaterializedViewOutputName(const string &name) {
	return StringUtil::CIEquals(name, openivm::MULTIPLICITY_COL) ||
	       StringUtil::CIEquals(name, openivm::TIMESTAMP_COL) ||
	       StringUtil::CIEquals(name, openivm::PUBLISHED_ORDINAL_COL) ||
	       StringUtil::CIStartsWith(name, openivm::ROWS_POSITION_PREFIX) ||
	       StringUtil::CIStartsWith(name, openivm::RUNNING_INPUT_PREFIX);
}

void ValidateMaterializedViewOutputNames(const vector<string> &original_names, const vector<string> &output_names,
                                         idx_t visible_count) {
	case_insensitive_set_t helper_names;
	for (idx_t i = visible_count; i < output_names.size(); i++) {
		helper_names.insert(output_names[i]);
	}
	for (idx_t i = 0; i < visible_count; i++) {
		// Sanitization can turn a visible alias such as "openivm.multiplicity" into a helper name.
		auto &original = original_names[i];
		auto &output = output_names[i];
		if (IsReservedMaterializedViewOutputName(original) || IsReservedMaterializedViewOutputName(output) ||
		    helper_names.count(output)) {
			throw BinderException("Materialized-view output uses reserved OpenIVM column '%s'", original);
		}
	}
}

static LogicalOperator *FindTopNameSource(LogicalOperator *plan) {
	for (auto *walk = plan; walk;) {
		if (walk->type == LogicalOperatorType::LOGICAL_PROJECTION ||
		    walk->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			return walk;
		}
		if ((walk->type == LogicalOperatorType::LOGICAL_ORDER_BY || walk->type == LogicalOperatorType::LOGICAL_LIMIT ||
		     walk->type == LogicalOperatorType::LOGICAL_TOP_N || walk->type == LogicalOperatorType::LOGICAL_DISTINCT ||
		     walk->type == LogicalOperatorType::LOGICAL_FILTER) &&
		    !walk->children.empty()) {
			walk = walk->children[0].get();
			continue;
		}
		break;
	}
	return nullptr;
}

static void AppendProjectionOutputNames(LogicalProjection &projection, idx_t output_count,
                                        vector<string> &output_names) {
	while (output_names.size() < output_count) {
		idx_t idx = output_names.size();
		if (idx < projection.expressions.size() &&
		    !projection.expressions[idx]->GetAlias().GetIdentifierName().empty()) {
			output_names.push_back(projection.expressions[idx]->GetAlias().GetIdentifierName());
		} else {
			output_names.push_back("openivm_col_" + to_string(idx));
		}
	}
}

static void AppendAggregateOutputNames(LogicalAggregate &aggregate, idx_t output_count, vector<string> &output_names) {
	idx_t group_count = aggregate.groups.size();
	while (output_names.size() < output_count) {
		idx_t idx = output_names.size();
		if (idx >= group_count) {
			idx_t expr_idx = idx - group_count;
			if (expr_idx < aggregate.expressions.size() &&
			    !aggregate.expressions[expr_idx]->GetAlias().GetIdentifierName().empty()) {
				output_names.push_back(aggregate.expressions[expr_idx]->GetAlias().GetIdentifierName());
				continue;
			}
		}
		output_names.push_back("openivm_col_" + to_string(idx));
	}
}

static void AppendHiddenOutputNames(LogicalOperator *plan, vector<string> &output_names) {
	auto output_count = plan->GetColumnBindings().size();
	auto *name_source = FindTopNameSource(plan);
	if (!name_source) {
		return;
	}
	if (name_source->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		AppendProjectionOutputNames(name_source->Cast<LogicalProjection>(), output_count, output_names);
	} else {
		AppendAggregateOutputNames(name_source->Cast<LogicalAggregate>(), output_count, output_names);
	}
}

static void DeduplicateOutputNames(vector<string> &output_names) {
	case_insensitive_set_t seen;
	for (auto &name : output_names) {
		if (IncrementalTableNames::IsInternalColumn(name)) {
			continue;
		}
		string candidate = name;
		idx_t suffix = 1;
		while (seen.count(candidate)) {
			candidate = name + "_" + to_string(suffix++);
		}
		seen.insert(candidate);
		name = candidate;
	}
}

vector<string> PrepareOutputNames(LogicalOperator *select_plan, const vector<string> &planner_names) {
	auto output_names = planner_names;
	for (auto &name : output_names) {
		name = SanitizeOutputName(name);
	}
	AppendHiddenOutputNames(select_plan, output_names);
	DeduplicateOutputNames(output_names);
	return output_names;
}

static bool IsAggregateFunctionUnsupportedByLpts(const string &fn_name) {
	return fn_name == "quantile_cont" || fn_name == "quantile_disc" || fn_name == "percentile_cont" ||
	       fn_name == "percentile_disc" || fn_name == "approx_quantile" || fn_name == "mad" || fn_name == "median" ||
	       fn_name == "mode" ||
	       // Statistical aggregates whose children LPTS re-aliases to internal
	       // `tN_col` names; the serialized SQL refers to those names against the
	       // original FROM clause and fails binding at CREATE-table time.
	       fn_name == "corr" || fn_name == "covar_pop" || fn_name == "covar_samp" || fn_name == "regr_avgx" ||
	       fn_name == "regr_avgy" || fn_name == "regr_count" || fn_name == "regr_intercept" || fn_name == "regr_r2" ||
	       fn_name == "regr_slope" || fn_name == "regr_sxx" || fn_name == "regr_sxy" || fn_name == "regr_syy";
}

bool PlanNeedsOriginalSqlForLpts(const CreateMVPlanFacts &facts) {
	if (facts.has_pivot) {
		return true;
	}
	for (auto *aggregate : facts.aggregates) {
		for (auto &expr : aggregate->expressions) {
			if (expr->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
				continue;
			}
			auto &bound_agg = expr->Cast<BoundAggregateExpression>();
			if (IsAggregateFunctionUnsupportedByLpts(bound_agg.Function().GetName().GetIdentifierName())) {
				return true;
			}
		}
	}
	return false;
}

bool IsPacLoaded(ClientContext &context) {
	Value pac_check_val;
	return context.TryGetCurrentSetting(Identifier("pac_check"), pac_check_val);
}

void ForwardPacSettingsIfLoaded(ClientContext &context, Connection &con) {
	if (!IsPacLoaded(context)) {
		return;
	}
	for (auto &name : {"pac_mi", "pac_seed", "pac_m", "pac_noise", "pac_hash_repair", "pac_check", "pac_rewrite",
	                   "pac_conservative_mode"}) {
		Value val;
		if (context.TryGetCurrentSetting(Identifier(name), val) && !val.IsNull()) {
			con.Query("SET " + string(name) + " = " + val.ToString());
		}
	}
}

using LeftJoinInnerTableMap = unordered_map<const LogicalComparisonJoin *, std::set<TableIndex>>;

// Which base tables read as NULL in a null-padded row of join `oj`.
//
// It is not just oj's own inner side: in a chain c ⟕ o ⟕ l ⟕ s, when a line disappears the supplier
// joined to that line's key disappears too, and anything joined to the supplier after it, and so on.
// So start from oj's inner subtree and close transitively over any join whose condition touches an
// already-NULL table, adding that join's inner side. Aggregates over these tables contribute 0 to the
// null-padded row; aggregates over the preserved side still contribute.
static std::set<TableIndex> ComputeNullTablesForLevel(const CreateMVPlanFacts &facts, LogicalComparisonJoin *oj,
                                                      const LeftJoinInnerTableMap &inner_tables) {
	// The caller passes only binary LEFT joins, all of which are in inner_tables.
	auto null_tables = inner_tables.at(oj);
	auto touches_null = [&](Expression *side) {
		auto *cref = GetColumnRefThroughCasts(side);
		LogicalGet *g = nullptr;
		string c;
		return cref && ResolveBindingToGetColumn(cref->Binding(), facts, g, c) && g &&
		       null_tables.count(g->table_index);
	};
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto *other : facts.comparison_joins) {
			if (other == oj || other->join_type != JoinType::LEFT || other->children.size() != 2) {
				continue;
			}
			auto &other_inner = inner_tables.at(other);
			if (std::all_of(other_inner.begin(), other_inner.end(),
			                [&](TableIndex idx) { return null_tables.count(idx) > 0; })) {
				continue;
			}
			if (std::any_of(other->conditions.begin(), other->conditions.end(), [&](const JoinCondition &cond) {
				    return cond.IsComparison() &&
				           (touches_null(cond.LeftReference().get()) || touches_null(cond.RightReference().get()));
			    })) {
				null_tables.insert(other_inner.begin(), other_inner.end());
				changed = true;
			}
		}
	}
	return null_tables;
}

// Secondary delta for ONE LEFT JOIN level. `level` indexes the emitted placeholders, and
// `null_tables` is the set of base tables that read NULL in this level's null-padded row (see
// ComputeNullTablesForLevel) which decides each aggregate's contribution.
static bool IsUnfilteredGetSubtree(LogicalOperator *op) {
	while (op && op->type == LogicalOperatorType::LOGICAL_PROJECTION && op->children.size() == 1) {
		op = op->children[0].get();
	}
	return op && op->type == LogicalOperatorType::LOGICAL_GET && !op->Cast<LogicalGet>().table_filters.HasFilters();
}

static bool ColumnIsNotNull(LogicalGet &get, const string &column_name) {
	auto table = get.GetTable();
	if (!table.get() || !table.get()->ColumnExists(Identifier(column_name))) {
		return false;
	}
	auto column_index = table.get()->GetColumn(Identifier(column_name)).Logical();
	for (auto &constraint : table.get()->GetConstraints()) {
		if (constraint->type == ConstraintType::NOT_NULL &&
		    constraint->Cast<NotNullConstraint>().index == column_index) {
			return true;
		}
	}
	return false;
}

// The qualified identity of the view source `get` scans, which also names its base table; empty otherwise.
static string ResolveSourceBase(const CreateMVPlanFacts &facts, LogicalGet &get) {
	auto table = get.GetTable();
	if (!table.get()) {
		return "";
	}
	auto identity = SqlUtils::FullName(table->ParentCatalog().GetName(), table->schema.name, table->name);
	return facts.source_table_info.count(identity) ? identity : string();
}

// A match-key filter can travel down joins and direct projections to its source.
static bool CanFilterPreviousSourceKey(LogicalOperator &node, ColumnBinding key, const LogicalGet &source) {
	switch (node.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		if (&node != &source || key.table_index != source.table_index) {
			return false;
		}
		auto &ids = source.GetColumnIds();
		return key.column_index < ids.size() && !ids[key.column_index].IsVirtualColumn() &&
		       !ids[key.column_index].HasChildren();
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = node.Cast<LogicalProjection>();
		if (key.table_index != projection.table_index || key.column_index >= projection.expressions.size()) {
			return false;
		}
		auto &expr = *projection.expressions[key.column_index];
		return expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		       CanFilterPreviousSourceKey(*node.children[0], expr.Cast<BoundColumnRefExpression>().Binding(), source);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_FILTER:
		for (auto &child : node.children) {
			auto bindings = child->GetColumnBindings();
			if (std::find(bindings.begin(), bindings.end(), key) != bindings.end()) {
				return CanFilterPreviousSourceKey(*child, key, source);
			}
		}
		return false;
	default:
		return false;
	}
}

static string BuildLeftJoinPreviousPreservedSQL(const string &sql, const CreateMVPlanFacts &facts,
                                                LogicalOperator &plan, const string &key_source,
                                                const string &filter_key) {
	struct PreviousSource {
		std::set<string> columns;
		idx_t occurrences = 0;
	};
	unordered_map<string, PreviousSource> sources;
	vector<LogicalOperator *> pending {&plan};
	while (!pending.empty()) {
		auto &node = *pending.back();
		pending.pop_back();
		for (auto &child : node.children) {
			pending.push_back(child.get());
		}
		if (node.type != LogicalOperatorType::LOGICAL_GET) {
			continue;
		}
		auto &get = node.Cast<LogicalGet>();
		auto identity = ResolveSourceBase(facts, get);
		if (identity.empty()) {
			continue;
		}
		auto &source = sources[identity];
		source.occurrences++;
		for (auto &column : get.GetColumnIds()) {
			auto index = column.GetPrimaryIndex();
			if (!column.IsVirtualColumn() && index < get.names.size()) {
				source.columns.insert(get.names[index].GetIdentifierName());
			}
		}
	}

	return SqlUtils::RewriteTableReferences(sql, [&](const vector<string> &parts) -> string {
		if (parts.size() != 3) {
			return "";
		}
		auto identity = SqlUtils::FullName(parts[0], parts[1], parts[2]);
		if (facts.source_table_info.count(identity) == 0) {
			return "";
		}
		auto previous = SqlUtils::QuoteIdentifier(string(openivm::LJSEC_OLD_SOURCE_PREFIX) + identity);
		if (facts.ducklake_table_info.count(identity) != 0) {
			return previous;
		}
		auto source = sources.find(identity);
		if (source == sources.end() || source->second.columns.empty()) {
			return previous;
		}
		// A repeated table can play several roles. Restrict it by key only when the
		// key originates from a single occurrence through direct bindings.
		const bool keyed = !filter_key.empty() && identity == key_source && source->second.occurrences == 1;
		string candidates =
		    keyed ? " SEMI JOIN (SELECT __k FROM __transitions WHERE __newc > 0) openivm_first_match ON "
		            "openivm_scope_source." +
		                SqlUtils::QuoteIdentifier(filter_key) + " = openivm_first_match.__k"
		          : " SEMI JOIN (SELECT 1 FROM __transitions WHERE __newc > 0 LIMIT 1) openivm_first_match "
		            "ON TRUE";
		auto delta = SqlUtils::QuoteIdentifier(string(openivm::LJSEC_OLD_NATIVE_DELTA_PREFIX) + identity);
		vector<string> columns(source->second.columns.begin(), source->second.columns.end());
		return SqlUtils::BuildPreviousSourceRowsSQL(identity, delta, "", columns, candidates);
	});
}

// Appends this level's SQL and source identities to `meta`; false when the level is unsupported.
static bool BuildLeftJoinSecondaryForLevel(Binder &binder, const CreateMVPlanFacts &facts,
                                           const vector<string> &output_names, const string &view_name,
                                           LogicalComparisonJoin *oj, size_t level,
                                           const std::set<TableIndex> &null_tables,
                                           const string &delta_view_catalog_prefix,
                                           RefreshMetadata::LeftJoinSecondaryMeta &meta) {
	vector<string> preserved_cols;
	if (facts.aggregates.size() != 1) {
		return false;
	}
	if (!oj || oj->join_type != JoinType::LEFT || oj->conditions.size() != 1 || oj->children.size() != 2) {
		return false;
	}
	// Binder can flatten a filtered preserved-side subquery by lifting its
	// LogicalFilter above the join. Inspect the complete view plan as well as
	// the local children; secondary transition counts are valid only when no
	// additional cardinality predicate exists anywhere in this aggregate view.
	if (facts.has_cardinality_changing_filter) {
		return false;
	}
	// __newc below counts rows directly in the inner base table. A filter or any other
	// cardinality-changing inner subtree (including a right-only ON predicate pushed below
	// the join) would make that count differ from the actual join matches.
	if (!IsUnfilteredGetSubtree(oj->children[1].get())) {
		return false;
	}
	auto &condition = oj->conditions[0];
	if (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
	    condition.LeftReference()->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    condition.RightReference()->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	// Every LEFT JOIN level needs the secondary, including a SINGLE left join whose preserved side is a
	// bare table. The old comment here claimed the primary delta already emits the null-padded
	// reappearance for that case; it does not. The primary emits only a retraction of the previously
	// matched row, and the MERGE's gating then synthesises the user-visible NULL/0 values. That leaves
	// openivm_count_star at 0 for a group that still has a NULL-padded output row, which makes it
	// impossible to tell "this group still exists, unmatched" from "this group is gone" -- and so
	// emptied groups could never be cleaned up. Emitting the reappearance restores that distinction.
	auto &agg = *facts.aggregates[0];
	size_t G = agg.groups.size();
	if (G == 0 || output_names.size() < G + agg.expressions.size()) {
		return false;
	}
	// Deepest-join keys: condition.left = preserved side, condition.right = inner (null) side.
	auto &pres_ref = condition.LeftReference()->Cast<BoundColumnRefExpression>();
	auto &inner_ref = condition.RightReference()->Cast<BoundColumnRefExpression>();
	ColumnBinding key_pres = pres_ref.Binding();
	auto resolve_side = [&](ColumnBinding binding, LogicalGet *&get, string &col, string &identity) {
		if (!ResolveBindingToGetColumn(binding, facts, get, col) || !get) {
			return false;
		}
		identity = ResolveSourceBase(facts, *get);
		return !identity.empty();
	};
	// Losing the last inner match creates a NULL-padded row for every current preserved row,
	// including rows inserted in this batch. Gaining the first match must retract a NULL pad
	// for old preserved rows; new rows never materialized that pad.
	LogicalGet *inner_get = nullptr, *pres_get = nullptr;
	string inner_col, inner_identity, pres_col, pres_identity;
	if (!resolve_side(inner_ref.Binding(), inner_get, inner_col, inner_identity) ||
	    !resolve_side(key_pres, pres_get, pres_col, pres_identity)) {
		return false;
	}
	string qcol = SqlUtils::QuoteIdentifier(inner_col);
	if (pres_get->table_index == inner_get->table_index || StringUtil::CIEquals(pres_identity, inner_identity)) {
		// This transition program assumes independently tracked preserved and nullable inputs.
		return false;
	}

	// Classify each aggregate output column's contribution to a null-padded row of the deepest join.
	// output_names layout: [group cols (G)] [visible aggregates (agg.expressions, in order)] [hidden helpers].
	// Visible: resolve the arg's base table — inner-side count/sum -> 0, preserved-side count -> 1
	// (preserved-side sum needs the value: unsupported for now -> bail). count_star (no arg) -> 1.
	// Hidden: openivm_count_star -> 1, openivm_match_count / openivm_nonnull_sum_count* -> 0 (all inner-derived).
	size_t num_agg = output_names.size() - G;
	vector<string> contribs;
	for (size_t j = 0; j < num_agg; j++) {
		if (j < agg.expressions.size()) {
			auto &e = agg.expressions[j];
			if (e->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
				return false;
			}
			auto &ba = e->Cast<BoundAggregateExpression>();
			string fn = StringUtil::Lower(ba.Function().GetName().GetIdentifierName());
			if (ba.GetChildren().empty()) {
				contribs.push_back("1"); // count_star: null-padded row still counts
				continue;
			}
			auto *cref = GetColumnRefThroughCasts(ba.GetChildren()[0].get());
			if (!cref) {
				return false;
			}
			LogicalGet *g = nullptr;
			string c;
			bool resolved = ResolveBindingToGetColumn(cref->Binding(), facts, g, c);
			bool is_inner = resolved && g && null_tables.count(g->table_index) > 0;
			if (fn == "count") {
				if (!is_inner && (!resolved || !g || !ColumnIsNotNull(*g, c))) {
					return false;
				}
				contribs.push_back(is_inner ? "0" : "1");
				if (!is_inner) {
					// Preserved-side COUNT: the MERGE must NOT gate it by the (inner-side) match_count.
					preserved_cols.push_back(output_names[G + j]);
				}
			} else if (fn == "sum" && is_inner) {
				contribs.push_back("0"); // preserved-side SUM needs the value; unsupported
			} else {
				return false;
			}
		} else {
			// Hidden helper columns are internally generated with fixed openivm_* names (never
			// user-chosen), so an exact/prefix match is safe and tighter than a bare substring search.
			const string &col = output_names[G + j];
			if (StringUtil::CIEquals(col, "openivm_count_star")) {
				contribs.push_back("1");
			} else if (StringUtil::CIEquals(col, "openivm_match_count") ||
			           StringUtil::StartsWith(col, "openivm_nonnull_sum_count")) {
				contribs.push_back("0");
			} else {
				return false;
			}
		}
	}

	// The secondary delta consumes only group keys and the join key. Prune the
	// copied subtree so unused source columns do not enter old-row matching.
	auto &context = binder.context;
	auto x_plan = oj->children[0]->Copy(context);
	x_plan->ResolveOperatorTypes();
	auto x_bindings = x_plan->GetColumnBindings();
	vector<unique_ptr<Expression>> expressions;
	vector<string> x_names;
	auto add_output = [&](ColumnBinding binding, const string &name) {
		auto pos = std::find(x_bindings.begin(), x_bindings.end(), binding);
		if (pos == x_bindings.end()) {
			return false;
		}
		expressions.push_back(make_uniq<BoundColumnRefExpression>(x_plan->types[pos - x_bindings.begin()], binding));
		x_names.push_back(name);
		return true;
	};
	for (idx_t i = 0; i < G; i++) {
		auto *group = GetColumnRefThroughCasts(agg.groups[i].get());
		if (!group || !add_output(group->Binding(), "__g" + to_string(i))) {
			return false;
		}
	}
	if (!add_output(key_pres, "__k")) {
		return false;
	}
	auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
	projection->children.push_back(std::move(x_plan));
	x_plan = std::move(projection);
	Optimizer optimizer(binder, context);
	RemoveUnusedColumns(optimizer).VisitOperator(x_plan);
	x_plan->ResolveOperatorTypes();

	auto x_sql = RenderPlanSQL(context, x_plan, x_names);
	if (x_sql.empty()) {
		return false;
	}

	// Project the scan inputs before restoring their bags. Unused columns can be
	// volatile generated values and must not participate in row matching.
	auto filter_key = CanFilterPreviousSourceKey(*oj->children[0], key_pres, *pres_get) ? pres_col : string();
	auto old_x_sql = BuildLeftJoinPreviousPreservedSQL(x_sql, facts, *x_plan, pres_identity, filter_key);

	// delta_mv columns: group cols + aggregate cols + openivm_multiplicity (matches the primary INSERT).
	string col_list;
	string sel;
	for (size_t j = 0; j < G + contribs.size(); j++) {
		col_list += SqlUtils::QuoteIdentifier(output_names[j]) + ", ";
		sel += (j < G ? "X.\"__g" + std::to_string(j) + "\"" : contribs[j - G]) + ", ";
	}
	col_list += "openivm_multiplicity";

	// Must carry the same catalog prefix the delta table was created with. Unqualified works only when
	// the MV's state lives in the default catalog; a DuckLake-backed view's delta table is
	// dl.main.openivm_delta_<view>, and an unqualified INSERT there fails the whole refresh.
	string dmv = delta_view_catalog_prefix + SqlUtils::QuoteIdentifier(SqlUtils::DeltaName(view_name));
	// The inner delta row source is resolved at refresh: a regular table reads its
	// openivm_delta_<table>, a DuckLake table reads ducklake_table_insertions/deletions between
	// snapshots (whose IDs only exist at refresh time). It yields columns (__k, __m).
	string inner_delta_src =
	    string(openivm::LJSEC_INNER_DELTA_PREFIX) + std::to_string(level) + string(openivm::LJSEC_PLACEHOLDER_SUFFIX);
	// Share right-side transition counts between the current and old preserved inputs.
	// Keep the correlated count: DuckDB decorrelates it efficiently without an
	// additional full GROUP BY over the right table.
	string transitions = "SELECT __t.__k, __newc FROM (SELECT __k, SUM(__m) AS dsum FROM " + inner_delta_src +
	                     " GROUP BY __k) __t, LATERAL (SELECT (SELECT COUNT(*) FROM " + inner_identity +
	                     " ib WHERE ib." + qcol +
	                     " = __t.__k) AS __newc) __n WHERE (__newc > 0) <> ((__newc - __t.dsum) > 0)";
	string sql = "INSERT INTO " + dmv + " (" + col_list + ") WITH __transitions AS (" + transitions + ") SELECT " +
	             sel + "1 FROM __transitions __t JOIN (" + x_sql +
	             ") X ON X.__k = __t.__k WHERE __t.__newc = 0 UNION ALL SELECT " + sel +
	             "-1 FROM __transitions __t JOIN (" + old_x_sql + ") X ON X.__k = __t.__k WHERE __t.__newc > 0;";
	OPENIVM_DEBUG_PRINT("[CREATE] LEFT JOIN secondary level %zu uses current/previous preserved rows\n", level);

	meta.sql += (meta.sql.empty() ? "" : "\n") + sql;
	meta.inner_tables.push_back(inner_identity);
	meta.inner_keys.push_back(inner_col);
	for (auto &col : preserved_cols) {
		if (std::find(meta.preserved_cols.begin(), meta.preserved_cols.end(), col) == meta.preserved_cols.end()) {
			meta.preserved_cols.push_back(col);
		}
	}
	return true;
}

// Emit a secondary delta for EVERY LEFT JOIN level whose preserved side is itself a join.
//
// A chain of N tables has N-1 LEFT JOIN levels, and a disappearing match at ANY of them can strand a
// preserved-side count. Correcting only the outermost level (the previous behaviour) left
// COUNT(intermediate_key) undercounted for 4+ table chains: with c ⟕ o ⟕ l ⟕ s, deleting an order's
// last line dropped that order from COUNT(o.oid) because the `⟕ l` level had no correction.
//
// The per-level SQL fragments are concatenated and each level's inner source identity is stored in
// index-aligned arrays, so refresh can resolve each level's placeholder independently (a regular
// table reads its delta table, a DuckLake table reads snapshot insertions/deletions).
RefreshMetadata::LeftJoinSecondaryMeta BuildLeftJoinSecondaryDeltaSQL(Binder &binder, const CreateMVPlanFacts &facts,
                                                                      const vector<string> &output_names,
                                                                      const string &view_name,
                                                                      const string &delta_view_catalog_prefix) {
	RefreshMetadata::LeftJoinSecondaryMeta meta;
	LeftJoinInnerTableMap join_inner_tables;
	for (auto *join : facts.comparison_joins) {
		if (!join || join->join_type != JoinType::LEFT || join->children.size() != 2) {
			continue;
		}
		auto *subtree = GetPlanNodeFacts(facts, join->children[1].get());
		if (!subtree) {
			return {};
		}
		join_inner_tables.emplace(
		    join, std::set<TableIndex>(subtree->subtree_table_indices.begin(), subtree->subtree_table_indices.end()));
	}
	size_t level = 0;
	for (auto *oj : facts.comparison_joins) {
		if (!oj || oj->join_type != JoinType::LEFT || oj->conditions.empty() || oj->children.size() != 2) {
			continue;
		}
		// Every LEFT JOIN level gets a secondary, including the innermost one whose preserved side is a
		// bare table -- see the note in BuildLeftJoinSecondaryForLevel for why the primary delta does
		// NOT cover that case.
		auto null_tables = ComputeNullTablesForLevel(facts, oj, join_inner_tables);
		if (!BuildLeftJoinSecondaryForLevel(binder, facts, output_names, view_name, oj, level, null_tables,
		                                    delta_view_catalog_prefix, meta)) {
			// An unsupported level makes the whole correction incomplete, and a partial correction is
			// worse than none (it would double-count the levels it does cover). Bail entirely.
			return {};
		}
		level++;
	}
	return meta;
}

} // namespace duckdb
