# Delta Tables

Delta tables record row-level changes (inserts, deletes, updates) to base tables and materialized views. OpenIVM uses them to compute incremental refreshes without scanning the full base data.

A shared delta table lets each view read only the changes it has not yet processed, without re-scanning the base table or coordinating with other views. Further, an UPDATE replaces a row in-place in the base table. The delta table stores both the old row (multiplicity `-1`) and the new row (multiplicity `+1`), preserving the full before-and-after picture.

Delta tables follow the convention `openivm_delta_<table_name>`. For a base table named `sales`, the delta table is `openivm_delta_sales`. For a materialized view named `region_totals`, the delta view table is `openivm_delta_region_totals`.

## Schema

A delta table has the same columns as its source table, plus two metadata columns:

| Column | Type | Description |
|---|---|---|
| `openivm_multiplicity` | `INTEGER` | Signed Z-set weight: `+1` = inserted row, `-1` = deleted row. Joins multiply weights and apply a Möbius inclusion-exclusion sign — see [`inner-join.md`](../operators/inner-join.md). |
| `openivm_timestamp` | `TIMESTAMP` | When the change was recorded. Defaults to `now()`. |

OpenIVM also creates a delta table for each MV. When a refresh computes the incremental change to an MV, the resulting delta rows are written into the MV's delta table so that downstream views can consume them.

## Change tracking

OpenIVM implements an optimizer insert rule that intercepts DML statements on tracked tables and writes corresponding rows to the delta table automatically.

- **INSERT**: Each inserted row is copied to the delta table with `openivm_multiplicity = 1`.
- **DELETE**: Each deleted row is copied to the delta table with `openivm_multiplicity = -1`.
- **UPDATE**: Decomposed into a retraction of the old row (`-1`) followed by an insertion of the new row (`+1`). Both rows are written to the delta table in a **single atomic `UNION ALL` insert** so that a concurrent refresh cannot snapshot a partial state — either both rows are visible or neither.

All delta rows receive a `openivm_timestamp` of `now()` at the time of the DML operation, in a transparent way which does not require user interaction.

## Timestamp-based cleanup

Delta rows are not removed immediately after a refresh: a base table's deltas can only be cleaned up once **all** materialized views that depend on that table have been refreshed past the delta's timestamp. 

## Refresh flow

When refreshing a MV, OpenIVM follows this sequence:

1. **Scan delta tables.** Read rows from each `openivm_delta_<base_table>` where `openivm_timestamp >= last_refresh_timestamp`.
2. **Compute the delta query.** Apply the view's incremental operator tree (filter, project, join, aggregate) to the delta rows. This produces the change to the materialized view — the "delta of the MV."
3. **Write to delta view.** INSERT the computed delta rows into `openivm_delta_<view_name>` so that downstream chained views can consume them.
4. **Upsert into the MV.** Apply the delta to the materialized view table using MERGE (grouped aggregates), counting-based INSERT/DELETE (projections), or single-row UPDATE (ungrouped aggregates). See the [operator docs](../operators/) for details on each strategy.
5. **Clean up.** Advance the cursor in `openivm_delta_tables` and delete consumed delta rows. Two timestamps are bumped per `(view, table)`:
    - `last_update` is set to `MAX(openivm_timestamp) + 1µs` over rows visible in this transaction's snapshot — *not* `now()`. Using `now()` was unsafe because a row committed between transaction begin and snapshot read could end up with a timestamp ≤ `now()` while still being processed by the next refresh, double-applying it. Anchoring to `MAX(base_ts)+1µs` guarantees the next refresh's `ts ≥ last_update` filter both excludes everything we just processed and includes anything newer than our snapshot.
    - `last_refresh_ts` is set to `now()` (transaction-start wall clock). This is used to filter `openivm_delta_<view>` companion rows produced by chained refreshes — companion rows carry timestamps near `now()` rather than base-row timestamps, so they need a separate cursor.

For chained views, [companion rows](../optimizations/companion-rows.md) ensure downstream consumers see correct old-to-new state transitions. See [Pipelines](../refresh/pipelines.md) for cascade mode details.
