# Spark support

**OpenIVM Spark (in development): <https://github.com/mdrakiburrahman/openivm-spark>**

OpenIVM runs inside DuckDB. Spark support means three things: it can read Spark-dialect `CREATE MATERIALIZED VIEW` bodies, it can compile a view's refresh program as Spark SQL without executing it, and it registers a Spark-compatible `add_months` scalar. OpenIVM does not run Spark, connect to a Spark cluster, or apply the compiled statements for you.

## Input dialect

`openivm_input_dialect` (default `duckdb`) sets the dialect of incoming `CREATE MATERIALIZED VIEW` bodies. The documented values are `duckdb` and `spark`. An unrecognised value (for example `klingon`) is rejected by `SET` (`test/sql/time_travel.test`); the validator lives in LPTS, so this page does not list any further accepted values.

With `spark`, the body is translated to DuckDB syntax (via LPTS `NormalizeInputSqlToDuckDB`) before parsing. This covers backtick identifiers and Spark time-travel clauses. See [parser](internals/parser.md#input-dialect).

```sql
SET openivm_input_dialect='spark';

CREATE MATERIALIZED VIEW sales_by_region AS
    SELECT region, SUM(amount) AS total
    FROM `sales` VERSION AS OF 366
    GROUP BY region;
```

### `VERSION AS OF` and `TIMESTAMP AS OF`

Both pins are accepted, each on its own relation, with or without an alias, in joins and in CTEs:

```sql
SELECT c.region, SUM(o.amount) AS total
FROM orders VERSION AS OF 366 o
JOIN customers VERSION AS OF 12 c ON o.c_id = c.c_id
GROUP BY c.region;

SELECT c_id, SUM(amount) FROM orders TIMESTAMP AS OF '2024-01-01 00:00:00' GROUP BY c_id;
```

(Tested in `test/sql/time_travel.test`.) Behaviour and limits:

- The pin is stored in the view SQL in DuckDB spelling (`orders AT (VERSION => 366)`), so it survives restarts. When output is compiled for Spark it is rendered back as `VERSION AS OF n`.
- Under the default `duckdb` input dialect, `VERSION AS OF` is a syntax error. It is not reinterpreted.
- A view that pins the same relation ambiguously is refused with `NotImplementedException` instead of guessing a snapshot. This covers the same relation at two different snapshots, and the same relation scanned both pinned and unpinned. Repeated scans that share one pin are fine.
- If a pin cannot be attached to the same relation and alias after translation, creation fails rather than reading a different snapshot.
- For a target dialect with no verified time-travel syntax, compilation raises `LPTS_UNSUPPORTED_TIME_TRAVEL`.
- Sources of a compiled view are plain in-memory stand-in tables, so the local copy holds no snapshots. The pin is carried in the compiled SQL for the target engine. Native DuckLake time travel is described in [DuckLake](ducklake.md).

Details are in [parser: time-travel pins](internals/parser.md#time-travel-pins).

## Compile-only output

`openivm_compile_with_facts(view_name, facts_json)` compiles a view's refresh program and returns it as rows without executing it. For Spark, set `target_dialect` to `spark`:

```sql
SELECT stmt_order, stmt_kind, sql
FROM openivm_compile_with_facts(
    'sales_by_region',
    '{"target_dialect":"spark","compile_only":true}'
)
ORDER BY stmt_order;
```

Result columns: `refresh_type` (INTEGER), `refresh_type_name` (VARCHAR), `stmt_order` (INTEGER), `stmt_kind` (VARCHAR), `sql` (VARCHAR). There is one row per top-level statement. `stmt_kind` is `meta_pre`, `data` or `meta_post`. The `data` rows hold the refresh statements. `refresh_type` and `refresh_type_name` repeat the view's refresh type on every row (for example `2` and `SIMPLE_PROJECTION`).

Facts JSON fields, as parsed in `src/compile_facts.cpp`:

| Field | Meaning |
|---|---|
| `target_dialect` | **Required.** `spark`, `duckdb` or `ducklake`; anything else is an error. |
| `schema_version` | Integer; the current version is 2. |
| `compile_only` | Boolean, default `false`. |
| `force_view_delta_cascade` | Boolean, default `false`. |
| `assume_insert_only` | Boolean, default `false`. Set only if the caller has proven the batch is append-only. With `compile_only` it re-enables the insert-only fast paths. |
| `delta_shape` | Object mapping source table to a shape string. |
| `running_window_incremental` | Boolean, default `false`. |
| `emit_spark_hints` | Boolean, default `false`; see below. |
| `fk_relations` | Array of `{child_table, child_columns, parent_table, parent_columns, rely}`. Only entries with `rely: true` and matching column counts are used. |
| `clock` | Object `{as_of, time_zone, previous_as_of, previous_time_zone}`; opt-in clock-as-input maintenance, see [Clock-dependent grouped aggregates](#clock-dependent-grouped-aggregates). |

Unknown fields are ignored. The facts apply to that one call only.

Spark output observed in `test/sql/compile_spark_dialect_hardening.test`: no DuckDB `::` casts (`CAST(...)` instead), `current_timestamp()`, `<=>` for null-safe equality, backtick-quoted identifiers, and `DECIMAL(38,0)` where a provably bounded `HUGEINT` appears. Those tests check particular view shapes. They are not a promise that every view or SQL construct compiles to Spark, and the compiled SQL is not executed against Spark by OpenIVM's tests.

## Clock-dependent grouped aggregates

A view that reads `current_timestamp` or `current_date` is stored as `FULL_REFRESH`, because its rows can change with no source change. Without the `clock` fact that is all that happens: the program is a full recompute and `openivm_compile_with_facts` returns its five columns unchanged.

With the `clock` fact, two grouped shapes can be maintained as a `GROUP_RECOMPUTE` instead:

```sql
SELECT k, SUM(CASE WHEN ts <= current_timestamp THEN v ELSE 0 END) AS s FROM src GROUP BY k
SELECT k, SUM(CASE WHEN d >= current_date THEN v ELSE 0 END) AS s FROM src GROUP BY k
```

The view must read one base table, group by plain columns that are all selected, and use only `sum`, `count`, `min`, `max` and `avg` at the top level. Inside the aggregates, `CASE`, `CAST`, arithmetic, comparisons, `AND`/`OR` and `coalesce` are allowed. A clock reference is accepted only as one operand of `<`, `<=`, `>` or `>=` whose other operand is a source column. Any other clock use, `now()`, `random()`, a filter, join, subquery or window leaves the view on the full-recompute path.

### Clock-named columns

DuckDB binds an unqualified `current_date` or `current_timestamp` to a source column or select-list alias of that name before the clock, and quoting does not survive parsing. With a column named `current_date`, `"current_date" >= current_date` is a comparison of the column with itself. A caller that has already resolved which reference is the clock writes it as one of two functions, which a column or alias cannot shadow:

| Function | Value |
|---|---|
| `openivm_clock_timestamp()` | `get_current_timestamp()`, a `TIMESTAMP WITH TIME ZONE` |
| `openivm_clock_date()` | `CAST(get_current_timestamp() AS DATE)`, the date in the session time zone |

```sql
SELECT k, SUM(CASE WHEN "current_date" >= openivm_clock_date() THEN v ELSE 0 END) AS s FROM src GROUP BY k
```

- Only the unqualified, zero-argument form counts as the clock. A qualified name, an argument, `DISTINCT`, `FILTER` or `ORDER BY` leaves the view on the full-recompute path.
- If a user function or macro named `openivm_clock_timestamp` or `openivm_clock_date` exists outside the `system` catalog, `clock_reason` is `unsupported_clock_use`.
- If a view reads the clock without either function and has a source column named `current_date` or `current_timestamp`, `clock_reason` is `unsupported_view_shape`, because the bare keyword means the column there. A select-list alias named after a clock keyword that is also used as the clock is `unsupported_view_shape` too.
- Without the `clock` fact these views are `FULL_REFRESH`, like any clock-reading view. The view's stored SQL keeps the function names.

The Spark text depends on how the program was built. With the fact, the `GROUP_RECOMPUTE` and `full_at_as_of` programs contain only `as_of` literals. A default full recompute of a view with no downstream views is the stored DuckDB text, so it contains `openivm_clock_date()` or `openivm_clock_timestamp()` verbatim and the caller maps them to `current_date()` and `current_timestamp()`. A cascaded full recompute renders `openivm_clock_timestamp()` as `current_timestamp()` and `openivm_clock_date()` as `CAST(current_timestamp() AS DATE)`. Columns named `current_date` or `current_timestamp` are rendered as bare identifiers in every case. Spark resolves them to the columns unless both `spark.sql.ansi.enabled` and `spark.sql.ansi.enforceReservedKeywords` are true, in which case the caller must quote them.

```json
{"target_dialect":"spark","compile_only":true,
 "clock":{"as_of":"2026-01-02T08:00:00Z","time_zone":"America/Los_Angeles",
          "previous_as_of":"2026-01-02T07:30:00Z","previous_time_zone":"America/Los_Angeles"}}
```

- `as_of` is an RFC 3339 UTC instant (`YYYY-MM-DDTHH:MM:SS[.ffffff]Z`). `time_zone` is an IANA name; it decides the date that `current_date` stands for.
- `previous_as_of` and `previous_time_zone` are the values the caller supplied with its last successful refresh. OpenIVM stores no clock state; the caller persists them. Only one of the pair, or none, is `missing_previous`; a `previous_as_of` that is not a valid instant is `invalid_clock`.
- The compiled program embeds `as_of` in place of every clock reference, so it never reads the clock itself.

With `clock` present, the result gains two trailing VARCHAR columns, `clock_mode` and `clock_reason`, and `refresh_type`/`refresh_type_name` report the type that was compiled.

| `clock_mode` | Meaning |
|---|---|
| `incremental` | `GROUP_RECOMPUTE`. The affected groups are the groups with source-delta rows plus the groups with a row whose clock predicate has a different value at `previous_as_of` and at `as_of`. The program must run even when every source delta is empty. |
| `full_at_as_of` | Full recompute with `as_of` embedded. `clock_reason` is `no_delta_sources`, `missing_previous`, `zone_changed`, `clock_regressed`, `unsupported_affected_mode`, `refresh_recovery`, `forced_full_refresh`, `source_requires_full_refresh` or `adaptive_full_refresh`. |
| `full_default` | The clock was not applied; the program is the ordinary full recompute and still reads the clock. `clock_reason` is `invalid_clock`, `unsupported_view_shape`, `unsupported_clock_use` or `no_clock_predicate`. |
| `not_applicable` | The view is not a `FULL_REFRESH` view, so the fact is ignored. |

Caller obligations:

- Persist `as_of` and `time_zone` as the next call's `previous_*` only when `clock_mode` is `incremental` or `full_at_as_of`, and only after the program has committed. Treat an unknown mode as a full recompute.
- Never skip the program because the source delta is empty when `clock_mode` is `incremental`.
- Run the program in a session whose time zone equals `time_zone`. A naive `TIMESTAMP` column is compared in the session zone.
- A clock that moves backwards or a changed zone is never maintained incrementally; the next program is a full recompute.

OpenIVM native `PRAGMA refresh` does not take this fact and is unchanged. Pinned (`VERSION AS OF`) sources and DuckLake sources stay on the full-recompute path.

## `openivm_emit_spark_hints`

Spark optimizer hints are off by default. With hints off, Spark output is byte-identical to output with `"emit_spark_hints":false`.

Enable them for one compile call:

```sql
SELECT sql
FROM openivm_compile_with_facts(
    'sph_hint_mv',
    '{"target_dialect":"spark","compile_only":true,"emit_spark_hints":true}'
)
WHERE stmt_kind = 'data';
```

or for the session:

```sql
SET openivm_emit_spark_hints = true;
```

Semantics, from `src/upsert/refresh_sql.cpp`:

- Hints are emitted only when `target_dialect` is `spark`. They are ignored for `duckdb` and `ducklake`.
- Hints are emitted if either the `emit_spark_hints` fact or the `openivm_emit_spark_hints` setting is true.
- The only hint verified by tests is `/*+ BROADCAST(...) */`, for a two-table inner-join view whose refresh SQL reads a delta table (`openivm_delta_<table>`). The test checks that the hint and the delta table both appear. It does not pin down which relation is broadcast. Which other hints LPTS emits is decided inside LPTS and is not covered here or by an OpenIVM test, so do not rely on any others.
- Hints are optimizer advice for Spark. They do not change query results.

## `add_months(DATE, INTEGER)`

Loading the extension registers the scalar `add_months(DATE, INTEGER)`, implemented in LPTS (`spark_scalar_functions`). It lets Spark-style view bodies that call `add_months` resolve in DuckDB and be maintained incrementally.

```sql
SELECT add_months(DATE '2015-01-31', 1);   -- 2015-02-28
SELECT add_months(DATE '2015-02-28', 1);   -- 2015-03-28

CREATE MATERIALIZED VIEW am_mv AS
    SELECT id, d, n, add_months(d, n) AS shifted FROM am_item;
```

(Both literals are checked in `test/sql/spark_add_months.test`. That test also shows a view using `add_months` compiled as a `SIMPLE_PROJECTION` delta rather than demoted to a full refresh, and equal to a full recomputation after inserts, updates and deletes.)

Limits:

- Only the `(DATE, INTEGER)` signature is registered. No other overloads (for example `TIMESTAMP` or `VARCHAR` inputs) are documented or tested here.
- Month-end results are shown only by the examples above, and the same test also covers `2016-02-29 + 12` months = `2017-02-28` and `2015-04-30 + 1` = `2015-05-30`. This page does not describe other edge cases (such as `NULL` or negative months). Exhaustive scalar tests live in the LPTS repository, not in OpenIVM.
- The test above runs on DuckDB. It does not show how the call is rendered in Spark output.
