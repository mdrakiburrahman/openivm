#pragma once

// Set to 1 to enable verbose OpenIVM debug output, 0 to disable.
// This is separate from DuckDB's DEBUG macro to avoid cluttering test output.
#define OPENIVM_DEBUG 1

#if OPENIVM_DEBUG
#include <cstdio>
// Basename of __FILE__ (the build compiles with absolute paths). The leading "/"
// guarantees strrchr finds a slash, so the result is never null.
#define OPENIVM_DEBUG_FILE (__builtin_strrchr("/" __FILE__, '/') + 1)
#define OPENIVM_DEBUG_PRINT(...)                                                                                       \
	do {                                                                                                               \
		fprintf(stderr, "[%s:%d %s] ", OPENIVM_DEBUG_FILE, __LINE__, __func__);                                        \
		fprintf(stderr, __VA_ARGS__);                                                                                  \
		fflush(stderr);                                                                                                \
	} while (0)
#else
#define OPENIVM_DEBUG_PRINT(...) ((void)0)
#endif
