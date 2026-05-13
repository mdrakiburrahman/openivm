# Metadata columns

OpenIVM adds hidden columns to materialized views and delta tables. These columns are internal — the user never sees them directly. The physical MV table is named `openivm_data_<view_name>`, and a SQL view named `<view_name>` is created on top using `EXCLUDE` to hide them.

## Delta table columns

Delta tables have two metadata columns appended to the base table schema. See [Delta Tables](delta-tables.md) for the full lifecycle.

| Column | Type | Description |
|---|---|---|
| `openivm_multiplicity` | `INTEGER` | Signed Z-set weight: `+1` = inserted row, `-1` = deleted row. Multiplicity > 1 is encoded by repeated rows. |
| `openivm_timestamp` | `TIMESTAMP` | When the change was recorded. Defaults to `now()`. |

**Multiplicity** encodes the sign and count of each delta row as a signed integer (Z-set weight). INSERT writes `+1`, DELETE writes `-1`, UPDATE writes two rows (old=`-1`, new=`+1`). During upsert, aggregates fold insertions and deletions into a net change with `SUM(openivm_multiplicity * value)`; projection views compute `SUM(openivm_multiplicity)` per distinct tuple to determine the net inserts/deletes. Joins multiply weights across leaves and apply a Möbius inclusion-exclusion sign — see [`inner-join.md`](../operators/inner-join.md).

**Timestamp** is set to `now()::timestamp` when the delta row is written. During refresh, only rows where `openivm_timestamp >= last_update` are read (where `last_update` is the per-(view,table) cursor in `openivm_delta_tables` — see [delta-tables.md](delta-tables.md)). This lets multiple MVs share a single delta table and each consume only the changes it hasn't processed yet.

## MV hidden columns

These columns live on the physical data table (`openivm_data_<view_name>`) and are hidden from the user via the view.

### `openivm_left_key`

Added when the view contains a LEFT or RIGHT JOIN. Stores the preserved-side join key so the upsert can do partial recompute — delete all MV rows for affected keys and re-insert from the original query filtered to those keys. See [Left Join](../operators/left-join.md) for the upsert strategy.

```sql
-- Original
SELECT c.name, o.amount
FROM customers c LEFT JOIN orders o ON c.id = o.customer_id;

-- Internal (stored in openivm_data_customer_orders)
SELECT c.name, o.amount, c.id AS openivm_left_key
FROM customers c LEFT JOIN orders o ON c.id = o.customer_id;
```

The type matches the join key's type.

### `openivm_sum_*` and `openivm_count_*`

Added when the view uses `AVG()`. The parser decomposes each `AVG(x) AS alias` into two hidden columns that can be maintained incrementally:

| Hidden column | Source | Updated via |
|---|---|---|
| `openivm_sum_<alias>` | `SUM(x)` | MERGE adds delta sum to existing value |
| `openivm_count_<alias>` | `COUNT(x)` | MERGE adds delta count to existing value |

The user-visible `AVG` column is recomputed after each MERGE as `openivm_sum / NULLIF(openivm_count, 0)`.

```sql
-- Original
SELECT region, AVG(amount) AS avg_amount FROM sales GROUP BY region;

-- Internal (stored in openivm_data_sales_summary)
SELECT region,
    SUM(amount) AS openivm_sum_avg_amount,
    COUNT(amount) AS openivm_count_avg_amount,
    AVG(amount) AS avg_amount
FROM sales GROUP BY region;
```

### `openivm_distinct_count`

Added when the view uses `SELECT DISTINCT`, which the parser rewrites to `GROUP BY` + a hidden `COUNT(*)`. See [Distinct](../operators/distinct.md).

## The user-facing view

The physical table holding MV data is named `openivm_data_<view_name>`. Users interact with a SQL view that hides internal columns:

```sql
-- No internal columns
CREATE VIEW mv_name AS SELECT * FROM openivm_data_mv_name;

-- With internal columns (e.g. AVG decomposition + left join key)
CREATE VIEW mv_name AS
    SELECT * EXCLUDE (openivm_sum_avg_amount, openivm_count_avg_amount, openivm_left_key)
    FROM openivm_data_mv_name;
```

All IVM operations (upsert, delta tracking, MERGE) target the data table, never the view. The view is purely cosmetic.
