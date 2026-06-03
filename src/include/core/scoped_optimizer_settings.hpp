#pragma once

#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

class ScopedDisabledOptimizers {
public:
	ScopedDisabledOptimizers(ClientContext &context, const string &optimizer_list)
	    : config(DBConfig::GetConfig(context)), saved(config.options.disabled_optimizers) {
		auto list = StringUtil::Split(optimizer_list, ",");
		for (auto &entry : list) {
			auto param = StringUtil::Lower(entry);
			StringUtil::Trim(param);
			if (param.empty()) {
				continue;
			}
			config.options.disabled_optimizers.insert(OptimizerTypeFromString(param));
		}
	}

	~ScopedDisabledOptimizers() {
		config.options.disabled_optimizers = std::move(saved);
	}

	ScopedDisabledOptimizers(const ScopedDisabledOptimizers &) = delete;
	ScopedDisabledOptimizers &operator=(const ScopedDisabledOptimizers &) = delete;

private:
	DBConfig &config;
	set<OptimizerType> saved;
};

} // namespace duckdb
