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

## Debug it

**Fastest start (no clicking):** open the repo in VS Code (`code .`), open the
Run panel, pick **“Debug OpenIVM Demo (auto-stop at the compile path)”**, and
press **F5**. It builds the debug binary and stops automatically at
`OpenIvmCompileWithFactsBind` (Scenario 1). **Step Over (F10)** a few lines to
see `view_name` / `facts_json`, then **Continue (F5)** to land in
`ParseFactsJson` (watch the JSON become a `CompileFacts` struct) and
`GenerateRefreshSQL` (the heart of compilation). These are debugger-side
breakpoints, so they show **no red dot** in the gutter — execution still stops
and the editor jumps to the source line with the call stack + locals.

**Set your own breakpoints:** pick **“Debug OpenIVM Demo”** instead, click in the
gutter of `examples/openivm_demo.cpp` or any `src/…` file (try a
`Run(con, "PRAGMA refresh(...)")` line for Scenarios 2–4), then press **F5**.
**Step Into (F11)** descends from a SQL call into OpenIVM's C++.

## What the demo exercises

Each scenario is an independent function — set a breakpoint in just one and run.

| Scenario                   | SQL shape                    | Core path you'll step into                           |
| -------------------------- | ---------------------------- | ---------------------------------------------------- |
| `ScenarioCompileWithFacts` | `openivm_compile_with_facts` | `OpenIvmCompileWithFactsBind` → `GenerateRefreshSQL` |
| `ScenarioAggregate`        | `SUM/COUNT … GROUP BY`       | `CompileAggregateGroups` (MERGE upsert)              |
| `ScenarioInnerJoin`        | `JOIN … ON`                  | `IncrementalJoinRule` (inclusion-exclusion)          |
| `ScenarioProjectionFilter` | `SELECT expr … WHERE`        | `CompileProjectionsFilters` (counting consolidation) |

**Scenario 1 (compile-only)** is the openivm-spark path. It calls
`openivm_compile_with_facts('<view>', '<CompileFacts JSON>')`, which **emits the
refresh-SQL program and executes nothing** — the materialized view is provably
untouched (the demo asserts it is bag-equal to a pre-compile snapshot). It
compiles the *same* view twice — `target_dialect` `spark` then `duckdb` — so you
can watch the one knob that changes the emitted SQL. The full program is also
written to `$TMPDIR/openivm_demo_compiled/openivm_upsert_queries_mv_compile_region.sql`.

**Scenarios 2–4** each do: `CREATE MATERIALIZED VIEW` → batch
`INSERT/DELETE/UPDATE` → `PRAGMA refresh` → `EXCEPT ALL` bag-equality check
against a full recompute.

## Highest-value breakpoints

### Scenario 1 — compile-only (start here)

Set these and **Step Into (F11)** to watch a SQL statement become a refresh
program, without anything being executed:

| File / line                      | Why                                                                       |
| -------------------------------- | ------------------------------------------------------------------------- |
| `src/compile_facts.cpp:302`      | `OpenIvmCompileWithFactsBind` — table-function entry; inspect `view_name` + `facts_json` |
| `src/compile_facts.cpp:159`      | `ParseFactsJson` — the JSON becomes a `CompileFacts` struct (`target_dialect`, `compile_only`, …) |
| `src/upsert/refresh_sql.cpp:307` | `GenerateRefreshSQL` — **the heart of compilation**; everything below fans out from here |
| `src/compile_facts.cpp:243`      | `PushBucket` — the program is split into one output row per statement      |

### Scenarios 2–4 — full incremental refresh

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

