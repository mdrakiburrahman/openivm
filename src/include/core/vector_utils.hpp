#ifndef OPENIVM_VECTOR_UTILS_HPP
#define OPENIVM_VECTOR_UTILS_HPP

#include "duckdb.hpp"

namespace duckdb {

template <class T>
void AddUnique(vector<T> &entries, T entry) {
	for (auto &existing : entries) {
		if (existing == entry) {
			return;
		}
	}
	entries.push_back(std::move(entry));
}

inline bool ContainsCI(const vector<string> &values, const string &candidate) {
	for (auto &value : values) {
		if (StringUtil::CIEquals(value, candidate)) {
			return true;
		}
	}
	return false;
}

// Append a non-empty candidate unless an entry already matches it case-insensitively.
inline void AddUniqueCI(vector<string> &values, const string &candidate) {
	if (!candidate.empty() && !ContainsCI(values, candidate)) {
		values.push_back(candidate);
	}
}

} // namespace duckdb

#endif // OPENIVM_VECTOR_UTILS_HPP
