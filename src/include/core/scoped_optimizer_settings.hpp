#pragma once

#include "core/openivm_constants.hpp"
#include "core/sql_utils.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

// Optimizers to disable when optimizing the finished incremental plan. Native refresh executes the
// generated SQL immediately, so callers may opt in to planning against the current delta statistics.
// Empty string means "disable nothing". The earlier base-view template stage is always conservative.
inline string FinalPlanDisabledOptimizers(ClientContext &context, bool cross_system) {
	if (!cross_system && SqlUtils::GetBoolSetting(context, "openivm_enable_data_dependent_optimizers", false)) {
		return "";
	}
	return openivm::TEMPLATE_DATA_DEPENDENT_OPTIMIZERS;
}

// Disables optimizers for the lifetime of the object. The option is database-wide, so concurrent
// scopes share it: each optimizer is added when the first scope needing it starts and removed when
// the last one ends, and only if a scope added it. Changes go through DBConfig's locked accessors,
// which other connections read while planning.
class ScopedDisabledOptimizers {
public:
	ScopedDisabledOptimizers(ClientContext &context, const string &optimizer_list)
	    : config(DBConfig::GetConfig(context)) {
		for (auto &entry : StringUtil::Split(optimizer_list, ",")) {
			auto param = StringUtil::Lower(entry);
			StringUtil::Trim(param);
			if (!param.empty()) {
				types.insert(OptimizerTypeFromString(param));
			}
		}
		auto &registry = Registry();
		lock_guard<mutex> guard(registry.lock);
		auto disabled = config.GetDisabledOptimizers();
		bool changed = false;
		for (auto type : types) {
			auto &scope = registry.scopes[&config][type];
			if (scope.count++ == 0) {
				scope.added = disabled.insert(type).second;
				changed |= scope.added;
			}
		}
		if (changed) {
			config.SetDisabledOptimizers(std::move(disabled));
		}
	}

	~ScopedDisabledOptimizers() {
		auto &registry = Registry();
		lock_guard<mutex> guard(registry.lock);
		auto &scopes = registry.scopes[&config];
		auto disabled = config.GetDisabledOptimizers();
		bool changed = false;
		for (auto type : types) {
			auto scope = scopes.find(type);
			if (--scope->second.count == 0) {
				changed |= scope->second.added && disabled.erase(type) > 0;
				scopes.erase(scope);
			}
		}
		if (scopes.empty()) {
			registry.scopes.erase(&config);
		}
		if (changed) {
			config.SetDisabledOptimizers(std::move(disabled));
		}
	}

	ScopedDisabledOptimizers(const ScopedDisabledOptimizers &) = delete;
	ScopedDisabledOptimizers &operator=(const ScopedDisabledOptimizers &) = delete;

private:
	struct Scope {
		idx_t count = 0;
		bool added = false;
	};
	struct ScopeRegistry {
		mutex lock;
		unordered_map<DBConfig *, map<OptimizerType, Scope>> scopes;
	};
	static ScopeRegistry &Registry() {
		static ScopeRegistry registry;
		return registry;
	}

	DBConfig &config;
	set<OptimizerType> types;
};

} // namespace duckdb
