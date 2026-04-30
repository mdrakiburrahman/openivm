# CTEs and Subqueries

## Example

```sql
CREATE TABLE employees (id INT, dept VARCHAR, salary INT);
INSERT INTO employees VALUES (1, 'Eng', 100), (2, 'Eng', 200), (3, 'Sales', 150);

CREATE MATERIALIZED VIEW dept_stats AS
    WITH dept_agg AS (
        SELECT dept, SUM(salary) AS total, COUNT(*) AS cnt
        FROM employees GROUP BY dept
    )
    SELECT dept, total, total / cnt AS avg_sal FROM dept_agg;

INSERT INTO employees VALUES (4, 'Eng', 300);
PRAGMA ivm('dept_stats');
```

## How IVM handles it

CTEs and subqueries are handled transparently. DuckDB's planner inlines CTEs and
decorrelates correlated subqueries during query planning, before OpenIVM's optimizer
rules run. By the time the IVM rewrite rules see the logical plan, CTEs have been
expanded into their equivalent join/aggregate/projection operators.

This means:

- **CTEs** work with any supported operator inside them (aggregates, joins, filters, etc.)
- **Scalar subqueries** are decorrelated to left joins with aggregation
- **EXISTS** subqueries can be maintained through the [semi-join aux-state path](semi-anti-join.md)
- **NOT EXISTS** subqueries can be maintained through the [anti-join aux-state path](semi-anti-join.md)

The IVM delta rules apply to the decorrelated plan as usual. If DuckDB decorrelates a subquery into a plan shape OpenIVM does not support, the view falls back to full refresh.

## Limitations

- **Recursive CTEs** are not supported. Views with recursive CTEs fall back to full refresh.
- **Lateral joins** that cannot be decorrelated are not supported.
- **IN** and **NOT IN** are not documented as aux-state semi/anti support because their NULL semantics may require MARK joins. Those views are incremental only when the final decorrelated plan uses supported operators.
