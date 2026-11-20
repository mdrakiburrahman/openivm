#include "upsert/refresh_index_regen.hpp"
#include "core/openivm_debug.hpp"
#include "delta/delta_helpers.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <duckdb/planner/operator/logical_aggregate.hpp>
#include <duckdb/planner/operator/logical_empty_result.hpp>
#include <duckdb/planner/operator/logical_get.hpp>
#include <duckdb/planner/operator/logical_projection.hpp>
#include <duckdb/planner/operator/logical_comparison_join.hpp>
#include <duckdb/planner/operator/logical_delim_get.hpp>
#include <duckdb/planner/operator/logical_dependent_join.hpp>
#include <duckdb/planner/operator/logical_filter.hpp>
#include <duckdb/planner/operator/logical_set_operation.hpp>
#include <duckdb/planner/operator/logical_unnest.hpp>

namespace duckdb {

static void CollectExpressionBindings(Expression &expression, std::vector<ColumnBinding> &bindings) {
	if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		bindings.push_back(expression.Cast<BoundColumnRefExpression>().Binding());
	}
	ExpressionIterator::EnumerateChildren(expression, [&](unique_ptr<Expression> &child) {
		if (child) {
			CollectExpressionBindings(*child, bindings);
		}
	});
}

static void CollectOperatorExpressionBindings(LogicalOperator &op, std::vector<ColumnBinding> &bindings) {
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
		if (expression && *expression) {
			CollectExpressionBindings(**expression, bindings);
		}
	});
	if (op.type != LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		return;
	}
	auto &join = op.Cast<LogicalDependentJoin>();
	if (join.condition) {
		CollectExpressionBindings(*join.condition, bindings);
	}
	for (auto &column : join.correlated_columns) {
		bindings.push_back(column.binding);
	}
}

static new_idx GetOrCreateTableIndex(old_idx old_index, Binder &binder,
                                     std::unordered_map<old_idx, new_idx> &table_reassign) {
	auto entry = table_reassign.find(old_index);
	if (entry != table_reassign.end()) {
		OPENIVM_DEBUG_PRINT("Index regen: Reuse %llu -> %llu\n", static_cast<unsigned long long>(old_index.index),
		                    static_cast<unsigned long long>(entry->second.index));
		return entry->second;
	}
	auto fresh_index = binder.GenerateTableIndex();
	table_reassign.emplace(old_index, fresh_index);
	OPENIVM_DEBUG_PRINT("Index regen: Remap %llu -> %llu\n", static_cast<unsigned long long>(old_index.index),
	                    static_cast<unsigned long long>(fresh_index.index));
	return fresh_index;
}

static void RenumberTableIndices(LogicalOperator &plan, Binder &binder, RenumberWrapper &result) {
	auto bindings = plan.GetColumnBindings();
	result.column_bindings.insert(result.column_bindings.end(), bindings.begin(), bindings.end());
	CollectOperatorExpressionBindings(plan, result.column_bindings);
	result.contains_delim_operator |= plan.type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
	                                  plan.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN ||
	                                  plan.type == LogicalOperatorType::LOGICAL_DELIM_GET;
	for (auto &child : plan.children) {
		RenumberTableIndices(*child, binder, result);
	}
	auto remap = [&](TableIndex &index) {
		index = GetOrCreateTableIndex(index, binder, result.idx_map);
	};
	switch (plan.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = plan.Cast<LogicalAggregate>();
		remap(aggregate.group_index);
		remap(aggregate.aggregate_index);
		if (aggregate.groupings_index.IsValid()) {
			remap(aggregate.groupings_index);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = plan.Cast<LogicalGet>();
		// Copy serializes DuckLake's bound scan but drops its SQL parameter cache.
		PopulateDuckLakeChangeScanParameters(get);
		remap(get.table_index);
		break;
	}
	case LogicalOperatorType::LOGICAL_DELIM_GET:
		remap(plan.Cast<LogicalDelimGet>().table_index);
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION:
		remap(plan.Cast<LogicalProjection>().table_index);
		break;
	case LogicalOperatorType::LOGICAL_UNNEST:
		remap(plan.Cast<LogicalUnnest>().unnest_index);
		break;
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		remap(plan.Cast<LogicalSetOperation>().table_index);
		break;
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		// Each stored binding may name a different table; remap all of them.
		for (auto &binding : plan.Cast<LogicalEmptyResult>().bindings) {
			remap(binding.table_index);
		}
		break;
	default:
		break;
	}
}

static void RebindExpressionsNullSafe(LogicalOperator &op, ColumnBindingReplacer &replacer,
                                      const std::unordered_map<old_idx, new_idx> &table_mapping) {
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
		if (expression && *expression) {
			replacer.VisitExpression(expression);
		}
	});
	if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &join = op.Cast<LogicalDependentJoin>();
		if (join.condition) {
			replacer.VisitExpression(&join.condition);
		}
		for (auto &column : join.correlated_columns) {
			auto entry = table_mapping.find(column.binding.table_index);
			if (entry != table_mapping.end()) {
				column.binding.table_index = entry->second;
			}
		}
	}
	for (auto &child : op.children) {
		if (child) {
			RebindExpressionsNullSafe(*child, replacer, table_mapping);
		}
	}
}

static ColumnBindingReplacer vec_to_replacer(const std::vector<ColumnBinding> &bindings,
                                             const std::unordered_map<old_idx, new_idx> &table_mapping) {
	ColumnBindingReplacer replacer;
	column_binding_set_t seen;
	for (const ColumnBinding &cb : bindings) {
		if (table_mapping.find(cb.table_index) == table_mapping.end()) {
			continue;
		}
		if (!seen.insert(cb).second) {
			continue;
		}
		new_idx new_t = table_mapping.at(cb.table_index);
		replacer.replacement_bindings.emplace_back(cb,
		                                           ColumnBinding(TableIndex(new_t), ProjectionIndex(cb.column_index)));
	}
	return replacer;
}

RenumberWrapper renumber_and_rebind_subtree(unique_ptr<LogicalOperator> plan, Binder &binder) {
	RenumberWrapper res {std::move(plan), {}, {}, false};
	RenumberTableIndices(*res.op, binder, res);
	ColumnBindingReplacer replacer = vec_to_replacer(res.column_bindings, res.idx_map);
	if (!res.contains_delim_operator) {
		replacer.VisitOperator(*res.op);
	} else {
		// Some DELIM/DEPENDENT join expression slots are nullable until delimiter
		// rewriting. DuckDB's generic visitor dereferences them unconditionally.
		RebindExpressionsNullSafe(*res.op, replacer, res.idx_map);
	}
	return res;
}

} // namespace duckdb
