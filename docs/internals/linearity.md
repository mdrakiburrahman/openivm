# Operator linearity

Each IVM rewrite rule carries a **linearity classification** that determines how its
delta-rule is derived from the operator's algebra. The classification is exposed on the
`IvmRule` base class via `GetLinearity()` (see `src/include/rules/rule.hpp`):

```cpp
enum class Linearity { LINEAR, BILINEAR, NON_LINEAR };
```

This taxonomy is the same one DBSP §6 uses (Budiu et al., VLDB 2023) and determines the
shape of `ΔQ` for an operator `Q`.

## The three classes

### LINEAR

`Δ(Q(R)) = Q(ΔR)`. The rule applies the operator to the delta unchanged; no auxiliary
state is needed and cost is proportional to `|delta|`.

| Operator | Rule class |
|---|---|
| Table scan | `IvmScanRule` |
| Projection | `IvmProjectionRule` |
| Filter | `IvmFilterRule` |
| UNION ALL (bag union) | `IvmUnionRule` |
| `SUM`, `COUNT` (over linear inputs) | propagated through `IvmAggregateRule` |

`IvmAggregateRule` is structurally LINEAR — it just passes the multiplicity column
through as a group-by key. AVG and STDDEV/VARIANCE are decomposed into linear helper
columns before upsert compilation. Non-linear aggregate forms such as MIN/MAX deletes,
LIST filters, and non-summable output columns are detected at compile time and routed
to **group recompute** in `CompileAggregateGroups`, so the per-rule classification stays
clean.

### BILINEAR

Linear in each input separately. The delta rule expands to multiple terms, each weighted
by the **Z-set bilinear product** of leaf multiplicities times a **Möbius
inclusion-exclusion sign**. For an N-table inner join, this is `2^N − 1` terms; for a
DuckLake N-term telescoping join, it's exactly N.

| Operator | Rule class |
|---|---|
| INNER JOIN, CROSS JOIN, arbitrary-predicate joins | `IvmJoinRule` |
| LEFT JOIN, RIGHT JOIN, FULL OUTER JOIN | `IvmJoinRule` plus outer-join upsert paths |
| DuckLake telescoping join | `BuildDuckLakeJoinTerms` |

See [`operators/inner-join.md`](../operators/inner-join.md) for the algebraic derivation
of the combined-multiplicity formula.

### NON_LINEAR

Neither linear nor bilinear. The delta requires the *accumulated* state of one or more
inputs — there is no closed-form per-row rule. OpenIVM falls back to:

- **Auxiliary state** for threshold operators such as SEMI/ANTI
- **Group recompute** for affected groups (DISTINCT, MIN/MAX with deletes, LIST filters)
- **Partition recompute** for affected partitions (window functions)
- **Full refresh** when neither fits

| Operator | Rule class | Fallback |
|---|---|---|
| `DISTINCT` (δ in DBSP) | `IvmDistinctRule` | Group recompute via COUNT(*) sentinel |
| `SEMI JOIN`, `ANTI JOIN` | `IvmJoinRule` + aux-state upsert | Match-count threshold state |
| Window functions | `IvmWindowRule` | Partition recompute |

DISTINCT is non-linear *even on positive Z-sets* — it drops duplicates, which can't be
expressed as a sum over deltas. SEMI and ANTI joins are threshold operators over
right-side match counts. Window functions (ROW_NUMBER, RANK, NTILE, LAG, LEAD,
running aggregates) depend on partition order; a single insert/delete can re-rank
every row in the partition.

## Why this matters

The linearity class is a **document-time invariant**: it tells you what cost to expect
and what state OpenIVM has to maintain to keep the MV correct. It also gates the
`append-only` optimisation (see [`optimizations/append-only.md`](../optimizations/append-only.md)) —
LINEAR operators preserve insert-only semantics through the delta pipeline; BILINEAR
joins do not (cross-terms produce negative weights via the Möbius sign), which is why
the optimisation only fires for single-delta-table joins.

Adding a new operator should start with: pick the linearity class, then derive the
delta rule that the class permits.
