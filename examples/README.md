# OpenIVM debug demo

A tiny, self-contained C++ program (`openivm_demo.cpp`) for **stepping through an
end-to-end OpenIVM SQL flow** in the VS Code debugger — the fastest way to learn
how incremental view maintenance actually works inside this codebase.

## Prerequisites

Run the bootstrapper once (idempotent). It installs the C++ toolchain + gdb,
checks out submodules, builds **release + debug**, and installs the VS Code C++
extensions:

```bash
contrib/bootstrap-dev-env.sh
```

## Debug it (3 steps)

1. Open this repo folder in VS Code (`code .`).
2. Open `examples/openivm_demo.cpp` and click in the gutter to set a breakpoint
   (try the first `Run(con, "PRAGMA refresh(...)")` line).
3. Press **F5** and pick **“Debug OpenIVM Demo”**. It builds the debug binary,
   launches under gdb, and stops at your breakpoint. **Step Into (F11)** to
   descend from the SQL call into OpenIVM's C++.

## What the demo exercises

Each scenario is an independent function — set a breakpoint in just one and run.

| Scenario                   | SQL shape              | Core path you'll step into                           |
| -------------------------- | ---------------------- | ---------------------------------------------------- |
| `ScenarioAggregate`        | `SUM/COUNT … GROUP BY` | `CompileAggregateGroups` (MERGE upsert)              |
| `ScenarioInnerJoin`        | `JOIN … ON`            | `IncrementalJoinRule` (inclusion-exclusion)          |
| `ScenarioProjectionFilter` | `SELECT expr … WHERE`  | `CompileProjectionsFilters` (counting consolidation) |

Every scenario does: `CREATE MATERIALIZED VIEW` → batch `INSERT/DELETE/UPDATE`
→ `PRAGMA refresh` → `EXCEPT ALL` bag-equality check against a full recompute.

## Highest-value breakpoints

Set these in the C++ source (`src/…`) to watch the pipeline end to end:

| File                                     | Why                                            |
| ---------------------------------------- | ---------------------------------------------- |
| `src/core/parser.cpp`                    | `CREATE MATERIALIZED VIEW` is intercepted here |
| `src/core/incremental_checker.cpp`       | how the view is classified (`RefreshType`)     |
| `src/upsert/refresh.cpp`                 | `PRAGMA refresh` entry point                   |
| `src/rules/incremental_rewrite_rule.cpp` | dispatch to the per-operator delta rule        |
| `src/rules/join.cpp`                     | join delta terms                               |
| `src/upsert/refresh_compiler.cpp`        | the generated upsert SQL                       |

## Bonus: debug any SQL test

Pick **“Debug a single SQL test (unittest)”** in the Run panel and enter a path
(default `test/sql/aggregate.test`) to step through any SQLLogicTest end to end.

## Run without the debugger

```bash
GEN=ninja make            # or: make debug
./build/release/extension/openivm/openivm_demo
```

> The **debug** binary links AddressSanitizer. Under a debugger (gdb/ptrace) the
> launch configs set `ASAN_OPTIONS=detect_leaks=0` so the end-of-run leak check
> doesn't abort the session. If you run the debug binary by hand under gdb, set
> the same env var: `ASAN_OPTIONS=detect_leaks=0 gdb ./build/debug/.../openivm_demo`.

