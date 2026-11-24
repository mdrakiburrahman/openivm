#include "upsert/refresh_index_regen.hpp"
#include "core/openivm_debug.hpp"
#include "delta/delta_helpers.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <duckdb/planner/operator/logical_aggregate.hpp>
#include <duckdb/planner/operator/logical_empty_result.hpp>
#include <duckdb/planner/operator/logical_get.hpp>
#include <duckdb/planner/operator/logical_projection.hpp>
#include <duckdb/planner/operator/logical_secure_view.hpp>
#include <duckdb/planner/operator/logical_comparison_join.hpp>
#include <duckdb/planner/operator/logical_delim_get.hpp>
#include <duckdb/planner/operator/logical_dependent_join.hpp>
#include <duckdb/planner/operator/logical_filter.hpp>
#include <duckdb/planner/operator/logical_set_operation.hpp>
#include <duckdb/planner/operator/logical_unnest.hpp>

namespace duckdb {

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

static void RemapTableIndex(TableIndex &index, const std::unordered_map<old_idx, new_idx> &idx_map) {
	auto entry = idx_map.find(index);
	if (entry != idx_map.end()) {
		index = entry->second;
	}
}

static void RemapExpressionBindings(Expression &expression, const std::unordered_map<old_idx, new_idx> &idx_map) {
	if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		RemapTableIndex(expression.Cast<BoundColumnRefExpression>().BindingMutable().table_index, idx_map);
	}
	ExpressionIterator::EnumerateChildren(expression, [&](unique_ptr<Expression> &child) {
		if (child) {
			RemapExpressionBindings(*child, idx_map);
		}
	});
}

// Rewrite every column reference into the renumbered tables. This walks expression slots directly
// because some DELIM/DEPENDENT join slots stay null until delimiter rewriting, which DuckDB's
// generic visitor dereferences unconditionally.
static void RemapOperatorBindings(LogicalOperator &op, const std::unordered_map<old_idx, new_idx> &idx_map) {
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
		if (expression && *expression) {
			RemapExpressionBindings(**expression, idx_map);
		}
	});
	if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &join = op.Cast<LogicalDependentJoin>();
		if (join.condition) {
			RemapExpressionBindings(*join.condition, idx_map);
		}
		for (auto &column : join.correlated_columns) {
			RemapTableIndex(column.binding.table_index, idx_map);
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_SECURE_VIEW) {
		for (auto &binding : op.Cast<LogicalSecureView>().output_bindings) {
			RemapTableIndex(binding.table_index, idx_map);
		}
	}
	for (auto &child : op.children) {
		if (child) {
			RemapOperatorBindings(*child, idx_map);
		}
	}
}

RenumberWrapper renumber_and_rebind_subtree(unique_ptr<LogicalOperator> plan, Binder &binder) {
	RenumberWrapper res {std::move(plan), {}};
	RenumberTableIndices(*res.op, binder, res);
	RemapOperatorBindings(*res.op, res.idx_map);
	return res;
}

} // namespace duckdb
