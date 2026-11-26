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
// scopes share it: a starting scope adds each optimizer it needs that is missing, and the last scope
// needing one removes it on release, only if a scope added it. A value set by the user since OpenIVM
// last wrote it is the user's: scopes then keep their hands off what it contains, while each new
// scope still adds back what it needs. All changes happen under one lock and go through DBConfig's
// locked accessors, which other connections read while planning.
// Limitations, as DuckDB has neither a change hook nor a compare-and-set for the option: a SET to
// exactly the value OpenIVM last wrote looks like no SET, so the last scope's release still removes
// the optimizers it added; and a SET landing between a scope's read and write of the option is lost.
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
		auto &database = registry.databases[&config];
		auto disabled = Reconcile(database);
		bool changed = false;
		for (auto type : types) {
			auto &scope = database.scopes[type];
			scope.count++;
			// Also when other scopes are live: a SET by the user since then may have dropped the
			// optimizer, and this scope must still run with it disabled.
			if (disabled.insert(type).second) {
				scope.added = true;
				changed = true;
			}
		}
		if (changed) {
			Write(database, std::move(disabled));
		}
	}

	~ScopedDisabledOptimizers() {
		auto &registry = Registry();
		lock_guard<mutex> guard(registry.lock);
		auto &database = registry.databases[&config];
		auto disabled = Reconcile(database);
		bool changed = false;
		for (auto type : types) {
			auto scope = database.scopes.find(type);
			if (--scope->second.count == 0) {
				changed |= scope->second.added && disabled.erase(type) > 0;
				database.scopes.erase(scope);
			}
		}
		if (changed) {
			Write(database, std::move(disabled));
		}
		if (database.scopes.empty()) {
			registry.databases.erase(&config);
		}
	}

	ScopedDisabledOptimizers(const ScopedDisabledOptimizers &) = delete;
	ScopedDisabledOptimizers &operator=(const ScopedDisabledOptimizers &) = delete;

private:
	struct Scope {
		idx_t count = 0;
		bool added = false;
	};
	struct DatabaseScopes {
		map<OptimizerType, Scope> scopes;
		//! The value OpenIVM last wrote, to recognize a SET by the user since then.
		bool has_written = false;
		set<OptimizerType> written;
	};
	struct ScopeRegistry {
		mutex lock;
		unordered_map<DBConfig *, DatabaseScopes> databases;
	};
	static ScopeRegistry &Registry() {
		static ScopeRegistry registry;
		return registry;
	}

	//! The current value. If the user changed it since OpenIVM's last write, no scope owns any of it.
	set<OptimizerType> Reconcile(DatabaseScopes &database) {
		auto disabled = config.GetDisabledOptimizers();
		if (database.has_written && disabled != database.written) {
			for (auto &scope : database.scopes) {
				scope.second.added = false;
			}
			database.has_written = false;
		}
		return disabled;
	}

	void Write(DatabaseScopes &database, set<OptimizerType> disabled) {
		database.written = disabled;
		database.has_written = true;
		config.SetDisabledOptimizers(std::move(disabled));
	}

	DBConfig &config;
	set<OptimizerType> types;
};

} // namespace duckdb
