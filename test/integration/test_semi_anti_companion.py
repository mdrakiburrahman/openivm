#!/usr/bin/env python3

import csv
import io
import json
import re
import sys
import tempfile
from pathlib import Path

from test_regular_nterm_compiled import bag_equality_sql, run_duckdb


def compile_program(binary, database, dialect, force):
    facts = json.dumps(
        {"target_dialect": dialect, "compile_only": True, "force_view_delta_cascade": force},
        separators=(",", ":"),
    )
    result = run_duckdb(
        binary,
        database,
        f"SELECT refresh_type_name, sql FROM openivm_compile_with_facts('companion_mv','{facts}') "
        "ORDER BY stmt_order;",
    )
    rows = list(csv.reader(io.StringIO(result)))
    types = {row[0] for row in rows}
    if types != {"SEMI_ANTI_RECOMPUTE"}:
        raise AssertionError(f"Expected incremental semi/anti classification, got {types}")
    return [row[1].rstrip(";") + ";" for row in rows]


def companion_statements(program, target):
    pattern = re.compile(r"\binsert\s+into\s+(?:\w+\.)*" + re.escape(target) + r"\b", re.I)
    return [index for index, sql in enumerate(program) if pattern.search(sql.replace('"', "").replace("`", ""))]


def assert_zero(binary, database, sql, label):
    actual = run_duckdb(binary, database, sql)
    if actual != "0":
        raise AssertionError(f"{label}: expected no bag mismatches, got {actual}")


def run_scenario(binary, root, name, predicate):
    database = root / f"{name}.db"
    body = f"SELECT t.id,t.txt FROM companion_source t WHERE {predicate}"
    run_duckdb(
        binary,
        database,
        """
CREATE TABLE companion_source(id INTEGER, txt VARCHAR);
CREATE TABLE companion_blocked(txt VARCHAR);
INSERT INTO companion_source VALUES (1,'Foo'),(2,'foo'),(3,NULL),(4,'Bar'),(4,'Bar'),(99,'stable');
INSERT INTO companion_blocked VALUES ('foo');
"""
        + f"CREATE MATERIALIZED VIEW companion_mv AS {body};"
        + "CREATE MATERIALIZED VIEW companion_child AS SELECT id,txt FROM companion_mv;",
    )
    view_name = run_duckdb(
        binary, database, "SELECT view_name FROM openivm_views WHERE view_sql_name='companion_mv';"
    )
    delta = "openivm_delta_" + view_name
    if companion_statements(compile_program(binary, database, "duckdb", False), delta):
        raise AssertionError("Default native compilation unexpectedly emits the opt-in companion")
    spark_program = compile_program(binary, database, "spark", True)
    if len(companion_statements(spark_program, delta)) != 1:
        raise AssertionError("Spark compilation must expose one real signed companion")

    for batch, dml in [
        ("right_insert", "INSERT INTO companion_blocked VALUES ('bar');"),
        (
            "mixed",
            """
DELETE FROM companion_blocked WHERE txt='foo';
INSERT INTO companion_source VALUES (5,'New'),(6,NULL),(4,'Bar');
DELETE FROM companion_source WHERE id=1;
UPDATE companion_source SET txt='Changed' WHERE id=2;
INSERT INTO companion_blocked VALUES (NULL);
""",
        ),
        (
            "remove_null",
            """
DELETE FROM companion_blocked WHERE txt IS NULL OR txt='bar';
DELETE FROM companion_source WHERE id=4 AND rowid IN
    (SELECT rowid FROM companion_source WHERE id=4 LIMIT 1);
INSERT INTO companion_source VALUES (7,'extra'),(7,'extra');
""",
        ),
    ]:
        run_duckdb(binary, database, "CREATE OR REPLACE TABLE companion_before AS SELECT * FROM companion_mv;" + dml)
        program = compile_program(binary, database, "duckdb", True)
        indices = companion_statements(program, delta)
        if len(indices) != 1:
            raise AssertionError(f"{name}/{batch}: expected one signed companion, got {indices}")
        program.insert(
            indices[0] + 1,
            f"CREATE OR REPLACE TABLE companion_captured AS SELECT * FROM {delta};",
        )
        run_duckdb(binary, database, "\n".join(program))
        assert_zero(binary, database, bag_equality_sql("companion_mv", body), f"{name}/{batch}/parent")
        assert_zero(
            binary,
            database,
            """
WITH expected AS (
    SELECT id,txt,SUM(weight) AS weight FROM (
        SELECT id,txt,CAST(-1 AS BIGINT) AS weight FROM companion_before
        UNION ALL SELECT id,txt,CAST(1 AS BIGINT) FROM companion_mv
    ) rows GROUP BY id,txt HAVING SUM(weight)<>0
), actual AS (
    SELECT id,txt,SUM(openivm_multiplicity) AS weight FROM companion_captured
    GROUP BY id,txt HAVING SUM(openivm_multiplicity)<>0
)
SELECT
    (SELECT COUNT(*) FROM (SELECT * FROM expected EXCEPT ALL SELECT * FROM actual)) +
    (SELECT COUNT(*) FROM (SELECT * FROM actual EXCEPT ALL SELECT * FROM expected));
""",
            f"{name}/{batch}/signed-delta",
        )
        run_duckdb(binary, database, "PRAGMA refresh('companion_child');")
        assert_zero(binary, database, bag_equality_sql("companion_child", body), f"{name}/{batch}/child")

    run_duckdb(
        binary,
        database,
        "CREATE OR REPLACE TABLE companion_before AS SELECT * FROM companion_mv;",
    )
    program = compile_program(binary, database, "duckdb", True)
    index = companion_statements(program, delta)[0]
    program.insert(index + 1, f"CREATE OR REPLACE TABLE companion_captured AS SELECT * FROM {delta};")
    run_duckdb(binary, database, "\n".join(program))
    assert_zero(binary, database, "SELECT count(*) FROM companion_captured;", f"{name}/noop-delta")
    assert_zero(binary, database, bag_equality_sql("companion_mv", body), f"{name}/noop-parent")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_semi_anti_companion.py /path/to/duckdb")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="openivm_semi_anti_companion_") as temp_dir:
        root = Path(temp_dir)
        for name, predicate in [
            ("anti_lower", "NOT EXISTS (SELECT 1 FROM companion_blocked u WHERE u.txt=lower(t.txt))"),
            ("semi_lower", "EXISTS (SELECT 1 FROM companion_blocked u WHERE u.txt=lower(t.txt))"),
            ("null_aware_anti", "t.txt NOT IN (SELECT u.txt FROM companion_blocked u)"),
        ]:
            run_scenario(binary, root, name, predicate)


if __name__ == "__main__":
    main()
