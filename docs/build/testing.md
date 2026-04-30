# Testing OpenIVM

## Run All Tests

```bash
make test
```

## Run a Single Test

```bash
build/release/test/unittest "test/sql/mv_inner_join.test"
```

## Test Files

All tests live in `test/sql/*.test` using DuckDB's SQLLogicTest format.

| Test file | Coverage |
|---|---|
| `mv_aggregate.test` | SUM, COUNT, AVG, MIN/MAX, STDDEV/VARIANCE aggregations |
| `mv_projection.test` | Column projection, expression projection |
| `mv_filter.test` | WHERE clause filtering |
| `mv_inner_join.test` | Inner joins, cross joins, arbitrary predicates, multi-table joins |
| `mv_left_join.test` | LEFT JOIN, RIGHT JOIN, mixed INNER+LEFT, NULL keys |
| `mv_full_outer_join.test` | FULL OUTER JOIN projection and aggregate maintenance |
| `mv_semi_anti_join.test` | SEMI JOIN, ANTI JOIN, EXISTS, NOT EXISTS aux-state maintenance |
| `mv_union.test` | UNION ALL views |
| `mv_chained.test` | Chained (multi-level) materialized views |
| `mv_window.test` | Partition-level window recompute |
| `ivm_checker.test` | Query constraint validation |
| `mv_pipeline.test` | End-to-end refresh pipeline |
| `ivm_metadata.test` | Catalog and metadata tables |
| `ivm_parser.test` | SQL parsing and rewriting |
| `ivm_insert_rule.test` | Insert rules and delta generation |
| `ivm_auto_refresh.test` | Automatic refresh triggers |
| `ivm_list.test` | LIST aggregate support |

## Verification Pattern

Every refresh must be cross-checked with `EXCEPT ALL` in both directions to confirm
the materialized view matches a full recomputation:

```sql
-- No rows should be returned by either query:
SELECT * FROM mv EXCEPT ALL SELECT <mv_query> FROM base_tables;
SELECT <mv_query> FROM base_tables EXCEPT ALL SELECT * FROM mv;
```

This catches both missing rows and extra rows, including duplicates under bag semantics.
