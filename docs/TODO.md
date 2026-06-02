# OpenIVM Production Readiness TODO

## P0: Cost Model Audit

- [ ] **Reduce cardinality probe cost.** The current model (`src/upsert/refresh_cost_model.cpp`) still issues `COUNT(*)` queries for base tables, delta tables, and DuckLake insertion/deletion sets. Reuse refresh delta activity, cache recent counts, or use DuckDB statistics where the estimate is good enough.
- [ ] **Calibrate join refresh cost.** The model accounts for DuckLake N-term joins and FK pruning, but still prices joins mostly from input sizes and term counts. Validate selectivity, skew, join type, and CPU/write costs against measured refresh time.
- [ ] **Calibrate recompute cost.** Full recompute estimates still undercount join, aggregate, window, distinct, and write-side processing overhead.
- [ ] **Validate fanout estimates.** The model uses MV cardinality and table cardinality as a fanout proxy. Measure skewed joins and selective filters where average fanout is misleading.
- [ ] **Complete strategy-specific estimates.** Audit costs for counting-based upsert, group recompute, window partition recompute, outer-join MERGE, DISTINCT aux-state, and semi/anti aux-state refresh.
- [ ] **Test learned model calibration.** Track predicted vs actual refresh time per strategy, expose/use the refresh-history `strategy` column consistently, and verify cold-start thresholds.
- [ ] **Validate with real workloads.** Run the cost model on TPC-derived and synthetic stress views. Report decision accuracy, predicted/actual latency, and regret when the model chooses the slower strategy.

## P1: Correctness & Robustness

- [ ] **Transaction isolation during refresh.** The refresh opens a separate `Connection` — verify what snapshot it reads from and whether concurrent DML during refresh can produce inconsistent results.
- [ ] **Error recovery.** If refresh fails mid-way (e.g., out of memory during MERGE), is the MV left in a consistent state? Consider wrapping the full refresh in a single transaction.
- [ ] **Large delta handling.** When delta rows exceed the base table size, IVM is likely slower than full recompute. The cost model should catch this, but verify the threshold experimentally.
- [ ] **DROP MATERIALIZED VIEW syntax.** `DROP VIEW <mv>` cleans OpenIVM metadata and internal tables. The `DROP MATERIALIZED VIEW` syntax is still not routed through the extension parser.
- [ ] **NULL group keys edge cases.** `IS NOT DISTINCT FROM` in MERGE handles NULLs, but audit the full path (delta computation → consolidation → MERGE) for correctness with composite NULL keys.

## P2: Documentation

- [ ] **Performance evaluation methodology.** Write a doc explaining how to measure IVM refresh latency, what variables to control, and what metrics to report.
- [ ] **Refresh cost reference.** Document the current `PRAGMA refresh_cost` output columns, learned calibration fields, and interpretation of cost-model decisions.
- [ ] **Cross-system usage guide.** The paper describes cross-system IVM (DuckDB→PostgreSQL). Document how to set this up.

## P3: Performance Evaluation

- [ ] **Incremental refresh vs full recompute latency curves.** Measure refresh time at delta sizes 1%, 5%, 10%, 25%, 50% of base table for each operator type.
- [ ] **Operator breakdown.** Profile where time is spent: delta scan, LPTS compilation, SQL generation, upsert execution. Identify bottlenecks.
- [ ] **Chained MV overhead.** Measure the cost of companion row generation for 2-level and 3-level chains.
- [ ] **Comparison with pg_ivm.** Run equivalent queries on PostgreSQL with pg_ivm to establish a baseline.
- [ ] **Scalability.** Test with base tables at 1M, 10M, 100M rows. Identify at what scale IVM breaks even with full recompute.

## P4: Benchmarking Suite

The current suite covers projection, filter, grouped aggregate, and common join shapes at 1K–1M rows with 1%–50% delta ratios.

- [ ] **Audit benchmark operator coverage.** Add targeted cases for any missing FULL OUTER JOIN, SEMI/ANTI, window, LIST, STDDEV/VARIANCE, DISTINCT aux-state, and mixed operator stack behavior.
- [ ] **Cost-model accuracy benchmark.** Record predicted incremental/full time, actual refresh time, decision, calibration state, and regret ratio.
- [ ] **Mixed DML workload.** INSERT + DELETE + UPDATE interleaved in the same refresh cycle to stress delta consolidation.
- [ ] **TPC-H derived queries.** Q1 (grouped aggregates + filter), Q3 (3-way join + aggregate), Q5 (multi-join + aggregate), Q6 (filter + aggregate) as materialized views with incremental refresh after base table inserts.
- [ ] **Latency distribution.** Report p50/p95/p99 refresh times, not just median, to catch tail latency from large deltas or skewed groups.
- [ ] **CI integration.** Run a lightweight benchmark subset on every PR to catch performance regressions.

## P5: Features

- [ ] **Broader semi/anti support.** Extend aux-state maintenance to aggregates over SEMI/ANTI, join-chain inputs, and richer subquery shapes.
- [ ] **Window functions over joins.** Single-table windows use partition recompute. Investigate affected-partition extraction through joins.
- [ ] **Higher-order IVM.** For join-heavy queries (3+ tables), DBToaster-style auxiliary maps eliminate joins at update time. Evaluate whether the space-time tradeoff is worthwhile for OpenIVM's SQL-to-SQL model.
- [ ] **Automatic refresh.** Trigger-based immediate refresh on DML, similar to pg_ivm. Currently requires explicit `PRAGMA refresh()`.
- [ ] **DISTINCT aggregates.** `COUNT(DISTINCT x)` is not incrementally maintained. Explore approximate (HyperLogLog) or exact (auxiliary set per group) approaches.
