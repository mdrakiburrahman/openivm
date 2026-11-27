#pragma once

#include "core/refresh_locks.hpp"
#include "duckdb/common/string_util.hpp"

#include <chrono>
#include <thread>

namespace duckdb {

// Observe the actual gate wait, rather than assuming a launched worker reached
// an ownership lookup. The deadline only bounds a broken test's wait.
class MutationGateTestAccess {
public:
	static void WaitForBlockedOwner(MutationGate &gate, const string &source = "", bool publication = false) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			{
				std::lock_guard<mutex> guard(gate.lock);
				if (source.empty() && gate.waiting_owners != 0) {
					return;
				}
				for (auto &catalog : gate.sources) {
					auto entry = catalog.second.find(StringUtil::Lower(source));
					if (entry != catalog.second.end() &&
					    (publication ? entry->second->waiting_publishers : entry->second->waiting_readers)) {
						return;
					}
				}
			}
			std::this_thread::yield();
		}
		throw InvalidInputException("Competing mutation did not wait on its admission gate: %s", source);
	}

	static idx_t StartedWaits(MutationGate &gate) {
		std::lock_guard<mutex> guard(gate.lock);
		return gate.started_waits;
	}

	// Observe that a blocking wait started after `baseline`, even one that a deadline
	// already ended, so a slow caller cannot pass its deadline unobserved.
	static void WaitForStartedWait(MutationGate &gate, idx_t baseline) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (StartedWaits(gate) > baseline) {
				return;
			}
			std::this_thread::yield();
		}
		throw InvalidInputException("Competing mutation did not start a blocking wait");
	}
};

} // namespace duckdb
