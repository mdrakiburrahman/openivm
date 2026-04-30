---
name: zsets
description: Reference for Z-sets — the integer-weighted multiset algebra at the heart of DBSP, differential dataflow, and OpenIVM's delta tables. Focused on the algebra itself (definition, group structure, operators) and on its lineage (Green/Tannen K-relations, Koch's Z-relations, Naiad/differential dataflow). Mirrors the structure of Feldera's `01-zsets-and-dbsp.md` worked example. For DBSP system details see `explain-related-systems`; for the broader IVM operator theory see `explain-ivm`; for compilation/cost work see `incrementalization-strategies`.
---

## Scope and cross-references

This skill covers **the Z-set algebra itself** and prior work that produced it. It does **not** re-cover material already in sibling skills:

- **`explain-related-systems`** — DBSP system, DBToaster, pg_ivm, Snowflake Dynamic Tables, Materialize. Look there for the DBSP operator algebra (D, I, lift), the four fundamental operators, the linear/bilinear/non-linear classification of incremental forms, and the system-level comparison.
- **`explain-ivm`** — operator-by-operator delta rules, multiplicity tracking, correctness considerations. Look there for the delta-rule cookbook.
- **`incrementalization-strategies`** — higher-order IVM, factorisation, F-IVM, compilation-based approaches, cost models. Look there for "how to make Z-set IVM faster".
- **`merge-ivm-reference`** — how the Z-set delta gets turned into a SQL MERGE in OpenIVM's compile path.

What is unique to this skill: the algebraic object as a standalone topic, the worked Feldera medallion-pipeline trace that motivates it, and the literature lineage (semirings → K-relations → Z-relations → differential dataflow → DBSP). When the user asks "what is a Z-set really", or "where does this idea come from", land here.

---

## 1. What is a Z-set?

A **Z-set** over a domain *A* is a function `Z : A → ℤ` with **finite support** (only finitely many elements have non-zero weight). Equivalently, a finitely-supported function `A → ℤ` — the free abelian group on *A*, written `ℤ[A]` in the algebra literature.

| Element | Weight | Meaning |
|---|---|---|
| `row_A` | `+1` | present (inserted) |
| `row_B` | `+2` | present with multiplicity 2 |
| `row_C` | `-1` | retracted (deleted) |
| `row_D` | `0`  | absent (not materialised) |

### 1.1 Why integers? — the group property

Pointwise addition gives Z-sets a **commutative group** structure. `0` is identity; `−Z` is the inverse:

```
{(A, +1), (B, +1)}            -- batch 1
+ {(B, -1), (C, +1)}          -- batch 2
= {(A, +1), (B,  0), (C, +1)} -- B cancels
```

The inverse is the *whole point*. Set semantics ({0,1}) and bag/multiset semantics (ℕ) lack inverses, so classical IVM frameworks (Gupta-Mumick-Subrahmanian's counting algorithm, Blakeley-Larson-Tompa deltas) need explicit delete-handling logic. Z-sets get deletes for free from the group structure: a deletion is just an addition of a negative weight.

### 1.2 Z-sets generalise sets and bags

| Semantics | Weight domain | Inverse? | Captures |
|---|---|---|---|
| Set | `{0, 1}` | No | classical RA |
| Bag / multiset | `ℕ` | No | SQL with duplicates |
| **Z-set** | `ℤ` | **Yes** | sets + bags + deltas in one object |
| K-relation | any commutative semiring K | depends | provenance, probability, security |

A Z-set with only positive weights *is* a multiset; restricted to {0,1} it *is* a set. The novel structure relative to bag semantics is the negative weights, which let one object represent both *a relation and a delta to it*.

### 1.3 Operators on Z-sets

The relational operators are defined on Z-sets in the natural algebraic way; for the per-operator *incremental* forms see `explain-ivm`.

- **Addition** `Z₁ + Z₂`: pointwise sum. Used to apply a delta.
- **Negation** `−Z`: flip all weights. Used to retract.
- **Selection** `σ_p(Z)`: keep weights for elements satisfying *p*. Linear.
- **Projection** `π_X(Z)`: sum weights of elements that agree on the projected columns. Linear.
- **Join** `Z₁ ⋈ Z₂`: weight of `(a,b)` is `Z₁(a) · Z₂(b)`, filtered by predicate. **Bilinear**.
- **Union (bag)** = addition. **Difference** = `Z₁ + (−Z₂)`.
- **Distinct** `δ(Z)`: project to {0,1} weights — drops duplicates **and** drops negatives. Non-linear, the source of most IVM cost.

The point: positive RA + aggregation can be expressed as polynomial expressions on Z-sets, and each operator's incremental form follows mechanically from its algebraic shape (linear / bilinear / non-linear). This is what `explain-related-systems` covers under the DBSP heading.

---

## 2. The medallion-pipeline worked example (Feldera `01-zsets-and-dbsp.md`)

This is the concrete trace that the source document hangs everything on. Including it here because it makes the Z-set semantics tangible.

### Setup

A bronze→silver→gold pipeline:

```
bronze_supplier ─Δ─┐
bronze_partsupp Δ─┐│
                  ││
                  ▼▼
              ┌──────┐    ┌──────┐    ┌────────────┐
              │filter│───►│ join │───►│ aggregate  │───► gold_agg_supply_cost
              └──────┘    └──┬───┘    │ (SUM cost) │
                              │        └────────────┘
                          ┌───┴────┐
                          │  Z1    │   one-tick delay; holds trace of partsupp
                          │ trace  │
                          └────────┘
```

A new row arrives: `Δ(bronze_supplier) = {(supplier_42, +1)}`.

### Trace through each operator

| Stage | Z-set transformation | Cost |
|---|---|---|
| Δ input | `{(supplier_42, +1)}` | — |
| Filter (`s_acctbal > 0`) | unchanged (predicate true) | O(1), stateless |
| Join vs. partsupp trace | probes Z1-stored state, emits `{((supplier_42, ps_100), +1), ((supplier_42, ps_101), +1)}` | O(matches), stateful |
| SUM aggregate | reads previous group total, emits `{(group_key, +$550)}` | O(1) per group |
| Output | retract old, insert new: `{(g, old, −1), (g, new, +1)}` | — |

**Total: 4 rows touched** vs. millions in batch mode. The Z-set framing is what lets every operator speak the same language: Δ in, Δ out, weights composing under +.

For *why* `Q(i + Δi) = Q(i) + ΔQ(Δi, state)` and the operator algebra behind it (`Q^Δ = D ∘ lift(Q) ∘ I`), see `explain-related-systems` §DBSP.

---

## 3. Z-sets in code — Feldera Rust pointers

For grounding in the reference implementation:

| Concept | Type / location |
|---|---|
| Weight type | `pub type ZWeight = i64;` — `crates/dbsp/src/algebra/zset.rs:39` |
| Flat Z-set (no value) | `OrdZSet<K>` |
| Indexed Z-set ((key, value, weight)) | `IndexedZSet`; backings `OrdIndexedWSet`, `FileIndexedWSet`, `FallbackIndexedWSet` (auto-spill) |
| Trace trait (state for stateful ops) | `crates/dbsp/src/trace.rs` |
| Spine (default trace, 9-level LSM-style) | `crates/dbsp/src/trace/spine_async.rs`, `MAX_LEVELS = 9` |
| Z1 one-tick delay | `Z1<T>` in `crates/dbsp/src/operator/z1.rs` |
| Circuit driver | `DBSPHandle` in `crates/dbsp/src/circuit/dbsp_handle.rs` |

These are the same data structures Naiad/differential-dataflow used (under different names) — see §4.3.

---

## 4. Prior and adjacent work — Z-sets are a unification, not a fresh invention

The Feldera/DBSP write-up presents Z-sets cleanly, but the algebraic object has a long pedigree. This is the part most worth tracking — it's where the literature search adds something the source document doesn't.

### 4.1 Provenance semirings — Green, Karvounarakis & Tannen (PODS 2007)

*"Provenance Semirings"*. The seminal paper that established **K-relations**: relations where each tuple is annotated with a value from a commutative semiring `K`, and positive RA is interpreted via `+` and `·` of *K*. Specialising K recovers many models:

| K | Model recovered |
|---|---|
| `({0,1}, ∨, ∧)` | classical set semantics |
| `(ℕ, +, ·)` | bag/multiset semantics |
| **`(ℤ, +, ·)`** | **Z-relations / DBSP Z-sets** |
| `(ℕ[X], +, ·)` | provenance polynomials (most informative) |
| confidentiality lattice | access-control |
| `([0,1], +, ·)` truncated | probabilistic databases |

Tannen is explicitly an author on the DBSP paper — the connection is direct, not analogical.

### 4.2 Z-relations and IVM — Koch (PODS 2010)

*"Incremental Query Evaluation in a Ring of Databases"*, Christoph Koch.

The paper that made the IVM case for Z-relations explicit:

- Relations annotated with positive **or negative** integers, viewed as elements of a commutative ring.
- The difference operator of RA *is* the ring's additive inverse — uniform handling of inserts and deletes.
- Equivalence of RA queries over Z-relations is **decidable** (in contrast to set/bag semantics, where it is undecidable). A real optimisation lever — rewrite rules become algebraically checkable.
- Updates and base relations live in the same algebraic universe; a batch can be reordered freely.

The direct algebraic ancestor of DBSP's Z-sets. Koch's later **DBToaster** (VLDB Journal 2014) operationalises the same view as a compiler producing higher-order delta rules — covered in `explain-related-systems` and `incrementalization-strategies`.

### 4.3 Differential dataflow / Naiad — McSherry et al. (CIDR 2013, SOSP 2013)

*"Differential Dataflow"*, Frank McSherry, Derek G. Murray, Rebecca Isaacs, Michael Isard. Built on Naiad's timely dataflow.

- Collections carry **multisets with ℤ weights** — the same object as Z-sets, predating the DBSP name by a decade.
- Innovation vs. plain incremental dataflow: differences are indexed by a **partial-order timestamp** (loop counter × epoch), enabling efficient incremental computation across iterations of a recursive computation, not just across input epochs.
- Operators maintain **arrangements** — sorted, indexed, time-versioned views of a collection. Conceptual ancestor of DBSP's traces. Feldera's Spine is essentially a re-implementation.

Frank McSherry is a DBSP co-author, so the lineage is direct: DBSP gives a denotational, paper-friendly semantics to what differential-dataflow already did operationally. This is worth knowing because differential-dataflow has *more* (recursion across nested time) than DBSP-as-deployed in many systems chooses to expose.

### 4.4 The classical IVM line (pre-Z-set)

For context — the non-Z-set IVM tradition:

- **Blakeley, Larson & Tompa (1986)** "Efficiently Updating Materialized Views". Original delta expressions for SPJ over sets.
- **Gupta, Mumick & Subrahmanian (SIGMOD 1993)** the counting algorithm — augments tuples with a derivation count. Anticipates Z-sets but stops at ℕ; deletes still need special-case logic.
- **Bancilhon (1986); Balbin & Ramamohanarao (1987)** semi-naïve evaluation for Datalog — recursive analogue, also implicitly counting.
- **Roussopoulos (1991)** ViewCache; **Quass & Widom (1997)** self-maintainable views.

The Z-set framing subsumes these: where the count algorithm uses ℕ-weighted bags + auxiliary delete logic, Z-sets use ℤ and get deletes for free from the group inverse. (Detail on per-operator deltas: `explain-ivm`.)

### 4.5 Adjacent algebraic frameworks

- **F-IVM (Olteanu et al.)** — *factorised* IVM over rings; extends Koch's ring-of-databases with factorisation of join results. Triple-lock factorisation (SIGMOD 2018) is the entry point. Complementary to DBSP. See `incrementalization-strategies`.
- **Provenance for FO / fixpoints** — Grädel & Tannen, extending semirings to first-order logic and Datalog. Relevant if you care about *why* a tuple is in the view, not just whether.
- **CRDTs** — different community, same instinct: commutative monoids/groups for conflict-free updates. PN-Counters and Add-Wins sets are essentially Z-set-shaped.

---

## 5. How OpenIVM realises the Z-set abstraction

OpenIVM compiles the DBSP view of IVM to standard SQL upserts. As of commit `fc6dab9` ("Z-set refactor"), the realisation mirrors the algebra natively:

| Z-set concept | OpenIVM realisation |
|---|---|
| Z-set element with weight ∈ ℤ | row in `delta_<table>` with `_duckdb_ivm_multiplicity: INTEGER` (signed) |
| Weight ±1 (single insert / delete) | column value `+1` or `-1` |
| Weight > 1 (batch) | encoded as repeated rows today; the column type can carry any signed weight |
| Z-set addition (consolidation) | `SUM(_duckdb_ivm_multiplicity)` per distinct tuple — no `CASE WHEN` round-trip |
| Z-set bilinear product (join) | leaves multiply weights; `IvmJoinRule` additionally applies a Möbius IE sign `(-1)^(k-1)` because OpenIVM's "current base" reads `R_now = R_old + ΔR` |
| Trace / Z1-stored state | the materialised view itself; DuckLake snapshots for time-travelled bases |
| One circuit step | one `PRAGMA ivm('view_name')` invocation |

**Pre-`fc6dab9` divergence (now closed)**: OpenIVM previously stored multiplicity as `BOOLEAN` (`true`=insert, `false`=delete) and used `SUM(CASE WHEN mul = false THEN -col ELSE col END)` as the canonical consolidation. Joins combined leaf multiplicities with XOR. The refactor replaced both with their native Z-set forms (signed integer + bilinear product); the Möbius sign is the one OpenIVM-specific addition on top of the textbook DBSP algebra.

For the operator-rule details (`IvmJoinRule` inclusion-exclusion, `IvmAggregateRule`, etc.), see `AGENTS.md` or `CLAUDE.md`, `explain-ivm`, and `docs/internals/linearity.md`.

---

## 6. Suggested reading order

1. **DBSP paper** — Budiu et al., VLDB 2023, `arXiv:2203.16684`. Cleanest single statement.
2. **Provenance semirings** — Green, Karvounarakis, Tannen, PODS 2007. Algebraic foundation.
3. **Koch, "Incremental Query Evaluation in a Ring of Databases"** — PODS 2010. The IVM case for Z-relations.
4. **Differential dataflow** — McSherry et al., CIDR 2013, plus the Naiad SOSP 2013 paper. The operational precursor.
5. **Feldera Rust code** — `crates/dbsp/src/algebra/zset.rs`, `operator/z1.rs`, `trace/spine_async.rs`. Reference implementation.
6. **OpenIVM paper** — Battiston, Kathuria, Boncz, SIGMOD-Companion 2024, `arXiv:2404.16486`. SQL-to-SQL translation.

## 7. Source

This skill consolidates Feldera's `01-zsets-and-dbsp.md` (`mdrakiburrahman/feldera`, branch `dev/mdrrahman/research`). The §1 algebra and §2 medallion-pipeline trace follow that document. The §4 prior-work lineage and §5 OpenIVM mapping are added here. DBSP system mechanics (operator algebra D/I/lift, trace/Spine internals beyond pointers) deliberately omitted — those live in `explain-related-systems`.
