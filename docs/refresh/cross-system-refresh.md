# Cross-system refresh

A materialized view can live in an attached DuckDB or DuckLake catalog. OpenIVM control
metadata can live in a separate native DuckDB database or in PostgreSQL attached through
DuckDB's `postgres` extension. The backing table and publication objects live with the
view. Native source deltas live with their source, and DuckLake source changes are read
from snapshots. See [metadata placement](../internals/metadata-placement.md).

## DuckLake view with PostgreSQL control metadata

Use a PostgreSQL database and role that can create schemas and write tables in that
database. Start DuckDB with OpenIVM available and run the following in a directory
where DuckDB can create the lake catalog and its data directory. Adjust the connection
string for your server and supply credentials through your PostgreSQL client configuration.

```sql
LOAD openivm;
INSTALL postgres;
LOAD postgres;
INSTALL ducklake;
LOAD ducklake;

ATTACH 'host=127.0.0.1 dbname=openivm_demo user=openivm' AS control (TYPE postgres);
ATTACH 'ducklake:demo.ducklake' AS lake (DATA_PATH 'demo_data/');
SET openivm_metadata_catalog = 'control';
SET openivm_metadata_schema = 'openivm_demo';

CREATE TABLE lake.main.sales (id INTEGER, product VARCHAR, amount INTEGER);
INSERT INTO lake.main.sales VALUES (1, 'tea', 10), (2, 'coffee', 20), (3, 'tea', 30);

CREATE MATERIALIZED VIEW lake.main.product_totals AS
    SELECT product, SUM(amount) AS total, COUNT(*) AS orders
    FROM lake.main.sales
    GROUP BY product;

INSERT INTO lake.main.sales VALUES (4, 'coffee', 5), (5, 'tea', 8);
UPDATE lake.main.sales SET amount = 7 WHERE id = 4;
DELETE FROM lake.main.sales WHERE id IN (1, 5);
```

Submit refresh as its own statement in autocommit mode.

```sql
PRAGMA refresh('lake.main.product_totals');
SELECT * FROM lake.main.product_totals ORDER BY product;
```

The result is `('coffee', 27, 2)` and `('tea', 30, 1)`. OpenIVM creates its control
tables in `control.openivm_demo`. Connections to the same DuckDB instance share its
attached catalogs. When you reopen DuckDB, attach both catalogs under the same
names and use the same metadata configuration.

## Explicit catalog arguments

`PRAGMA refresh_cross_system(view_catalog, view_schema, attached_catalog, attached_schema, view_name)`
takes five strings. The first two locate the view. The third and fourth provide a
fallback source catalog and schema when the stored source location lacks them. The
last argument is the unqualified view name. Configure control metadata separately
with `openivm_metadata_catalog` and `openivm_metadata_schema`.

The explicit form for the example above is
`PRAGMA refresh_cross_system('lake', 'main', 'lake', 'main', 'product_totals')`.

## Supported view shapes and limitations

- OpenIVM chooses a maintenance strategy from the view's operators and source catalogs.
  DuckLake supports incremental projections, filters, inner joins, and grouped
  `SUM` and `COUNT`. See [DuckLake maintenance](../ducklake.md) and the
  [refresh strategy table](refresh-strategies.md) for other shapes and their fallbacks.
- PostgreSQL is a control metadata destination in this setup. Materialized views
  cannot be created inside the remote metadata catalog, and that catalog does not
  store view-matching signature columns. PostgreSQL integration tests use PostgreSQL
  16. See [limitations](../limitations.md).
- With control metadata in a separate database, CREATE, refresh, and DROP run in
  autocommit mode. Refresh records intent, commits the view changes, then advances
  source watermarks. A refresh interrupted before the watermark commit can require a
  full recompute on retry. PostgreSQL metadata also provides a per-view lease for
  concurrent clients. See the [refresh protocol](../internals/metadata-placement.md#refresh-protocol-autocommit).
