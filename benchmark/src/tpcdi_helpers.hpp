#pragma once

#include "duckdb.hpp"
#include <string>
#include <vector>

namespace openivm_bench {

void CreateTPCDISchema(duckdb::Connection &con);
void InsertTPCDIData(duckdb::Connection &con, int scale_factor);
std::vector<std::string> GenerateTPCDIDeltaPool(int scale_factor);

} // namespace openivm_bench
