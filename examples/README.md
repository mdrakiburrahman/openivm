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
2. Open `examples/openivm_demo.cpp` and click in the gutter to set a breakpoint.
   For the **compile-only** tour (Scenario 1) the best first breakpoint is in
   `src/compile_facts.cpp` at `OpenIvmCompileWithFactsBind`; for the refresh
   scenarios (2–4), try the first `Run(con, "PRAGMA refresh(...)")` line.
3. Press **F5** and pick **“Debug OpenIVM Demo”**. It builds the debug binary,
   launches under gdb, and stops at your breakpoint. **Step Into (F11)** to
   descend from the SQL call into OpenIVM's C++.

## DuckDB ↔ OpenIVM interop — every entry & exit

OpenIVM is a DuckDB **extension**: it never runs on its own. At `LOAD 'openivm'`
time it registers four kinds of hooks, and from then on DuckDB calls _into_
OpenIVM through them and OpenIVM hands control _back_ after each. This is why a
breakpoint in one hook (e.g. the compile bind) stays quiet during `CREATE
MATERIALIZED VIEW` — that statement crosses the boundary through a _different_
hook.

The boxes on the **left are DuckDB core**; the boxes on the **right are OpenIVM
extension classes** (registered at `LOAD 'openivm'`). `(1)`–`(7)` mark where
DuckDB calls **into** OpenIVM (`──>`, set a breakpoint) and where OpenIVM
**returns** control (`<──`). The numbers match the table below.

```
                  DUCKDB CORE                   ┃              OPENIVM EXTENSION
                                                ┃  (classes registered at LOAD 'openivm')
════════════════════════════════════════════════╋══════════════════════════════════════════════
                                                ┃
  ┌──────────────────────────┐                  ┃    ┌────────────────────────────────────────┐
  │                          │                  ┃    │                                        │
  │  Parser                  │ (1) raw SQL text ┃    │  MaterializedViewParserExtension       │
  │  (sees every statement)  │                  ┃    │                                        │
  │                          │ ─────────────────╋──> │    ParseFunction()  parser_parse.cpp:12│
  │                          │ (2) parse result ┃    │    PlanFunction()   parser.cpp:75      │
  │                          │ <────────────────╋─── │    -> build delta_* + data table       │
  │                          │                  ┃    │                                        │
  └──────────────────────────┘                  ┃    └────────────────────────────────────────┘
                                                ┃
  ┌──────────────────────────┐                  ┃    ┌────────────────────────────────────────┐
  │                          │                  ┃    │                                        │
  │  Optimizer               │ (3) base DML     ┃    │  RefreshInsertRule                     │
  │  (sees every plan)       │                  ┃    │                                        │
  │                          │ ─────────────────╋──> │    RefreshInsertRuleFunction()         │
  │                          │                  ┃    │    refresh_insert_rule.cpp:181         │
  │                          │                  ┃    │    -> tee rows into delta_*            │
  │                          │                  ┃    ├────────────────────────────────────────┤
  │                          │ (7) delta plan   ┃    │  IncrementalRewriteRule                │
  │                          │ ─────────────────╋──> │    IncrementalRewriteRuleFunction()    │
  │                          │ <────────────────╋─── │    incremental_rewrite_rule.cpp:61     │
  │                          │                  ┃    │    -> read delta_* not base            │
  └──────────────────────────┘                  ┃    └────────────────────────────────────────┘
                                                ┃
  ┌──────────────────────────┐                  ┃    ┌────────────────────────────────────────┐
  │                          │                  ┃    │                                        │
  │  Binder                  │ (4) bind call    ┃    │  openivm_compile_with_facts()          │
  │  SELECT ... FROM         │ ─────────────────╋──> │    OpenIvmCompileWithFactsBind         │
  │  openivm_compile_with_   │                  ┃    │    compile_facts.cpp:288  <enter>      │
  │  facts(...)              │                  ┃    │    -> GenerateRefreshSQL               │
  │                          │                  ┃    │      refresh_sql.cpp:307 (emit SQL)    │
  │                          │ (5) one row/stmt ┃    │    compile_facts.cpp:340  <exit>       │
  │                          │ <────────────────╋─── │    (MV is never modified)              │
  │                          │                  ┃    │                                        │
  └──────────────────────────┘                  ┃    └────────────────────────────────────────┘
                                                ┃
  ┌──────────────────────────┐                  ┃    ┌────────────────────────────────────────┐
  │                          │                  ┃    │                                        │
  │  Pragma executor         │ (6) refresh      ┃    │  UpsertDeltaQueriesLocked              │
  │  PRAGMA refresh('v')     │ ─────────────────╋──> │    refresh.cpp:427 ... 554             │
  │  (Scenarios 2-4)         │ <────────────────╋─── │    drives (7), then upsert             │
  │                          │                  ┃    │                                        │
  └──────────────────────────┘                  ┃    └────────────────────────────────────────┘
```

| #   | DuckDB → OpenIVM (set breakpoint here) | File:line                                   | Fires when                                                                     | Releases control by                              |
| --- | -------------------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------ |
| (1) | `ParseFunction`                        | `src/core/parser_parse.cpp:12`              | **every** SQL statement (sniffs the text for MV DDL)                           | returning a parse result (`:53` / `:58` / `:88`) |
| (2) | `PlanFunction`                         | `src/core/parser.cpp:75`                    | a `CREATE`/`ALTER`/`DROP`/`REFRESH MATERIALIZED VIEW` matched                  | returning the plan result                        |
| (3) | `RefreshInsertRuleFunction`            | `src/rules/refresh_insert_rule.cpp:181`     | optimizing any `INSERT`/`UPDATE`/`DELETE`/`DROP` (acts only on tracked tables) | returning the augmented plan                     |
| (4) | `OpenIvmCompileWithFactsBind` (enter)  | `src/compile_facts.cpp:288`                 | a query binds `openivm_compile_with_facts(…)` — **Scenario 1**                 | continues to (5)                                 |
| (5) | `OpenIvmCompileWithFactsBind` (exit)   | `src/compile_facts.cpp:340`                 | the refresh program has been emitted                                           | returning the table-function result              |
| (6) | `UpsertDeltaQueriesLocked`             | `src/upsert/refresh.cpp:427`                | `PRAGMA refresh('v')` — **Scenarios 2–4**                                      | returning at `src/upsert/refresh.cpp:554`        |
| (7) | `IncrementalRewriteRuleFunction`       | `src/rules/incremental_rewrite_rule.cpp:61` | optimizing the `ComputeDelta` plan built inside (6)                            | returning the rewritten plan                     |

> **Scenario 1 round trip** is just (4) → (5) (the `CREATE MATERIALIZED VIEW` you
> run first crosses at (1) and (2), _not_ the compile bind — that's why the bind
> breakpoint only hits later).
>
> **(3) and (7) are optimizer hooks** DuckDB calls for _every_ plan; they
> early-out unless they recognize their shape (tracked-table DML for (3), a
> `COMPUTEDELTA` marker for (7)). If such a breakpoint hits too often, make it
> conditional.

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
compiles the _same_ view twice — `target_dialect` `spark` then `duckdb` — so you
can watch the one knob that changes the emitted SQL. The full program is also
written to `$TMPDIR/openivm_demo_compiled/openivm_upsert_queries_mv_compile_region.sql`.

**Scenarios 2–4** each do: `CREATE MATERIALIZED VIEW` → batch
`INSERT/DELETE/UPDATE` → `PRAGMA refresh` → `EXCEPT ALL` bag-equality check
against a full recompute.

### Scenario 1 — compile-only

Set these and **Step Into (F11)** to watch a SQL statement become a refresh
program, without anything being executed:

| File / line                      | Why                                                                                               |
| -------------------------------- | ------------------------------------------------------------------------------------------------- |
| `src/compile_facts.cpp:302`      | `OpenIvmCompileWithFactsBind` — table-function entry; inspect `view_name` + `facts_json`          |
| `src/compile_facts.cpp:159`      | `ParseFactsJson` — the JSON becomes a `CompileFacts` struct (`target_dialect`, `compile_only`, …) |
| `src/upsert/refresh_sql.cpp:307` | `GenerateRefreshSQL` — **the heart of compilation**; everything below fans out from here          |
| `src/compile_facts.cpp:243`      | `PushBucket` — the program is split into one output row per statement                             |

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
