#!/usr/bin/env python3

import json
import subprocess
import sys
import tempfile
from pathlib import Path

LOS_ANGELES = "America/Los_Angeles"
TOKYO = "Asia/Tokyo"
T0 = "2026-01-02T07:30:00Z"  # 2026-01-01 23:30:00 in Los Angeles
T1 = "2026-01-02T08:00:00Z"  # local midnight
T2 = "2026-01-02T08:00:00.000001Z"  # one microsecond after local midnight

SEED = """INSERT INTO {t} VALUES
 (1, TIMESTAMP '2026-01-01 23:30:00', DATE '2026-01-01', 1),
 (1, TIMESTAMP '2026-01-02 00:00:00', DATE '2026-01-02', 2),
 (2, TIMESTAMP '2026-01-02 00:00:00', DATE '2026-01-01', 10),
 (3, TIMESTAMP '2026-01-01 23:45:00', DATE '2026-01-01', 100),
 (4, TIMESTAMP '2026-01-02 00:00:00.000001', DATE '2026-01-03', 1000),
 (5, TIMESTAMP '2026-01-01 23:29:59.999999', DATE '2025-12-31', 10000),
 (6, NULL, NULL, 7),
 (7, TIMESTAMP '2026-01-02 00:00:00', DATE '2026-01-02', 3),
 (7, TIMESTAMP '2026-01-02 00:00:00', DATE '2026-01-02', 3),
 (NULL, TIMESTAMP '2026-01-01 23:50:00', DATE '2026-01-01', 5);"""

# `named` holds the same rows in columns named after the clock keywords; its views read the clock through the
# unshadowable functions. DML is applied to both tables.
TABLES = {
    "src": "CREATE TABLE src(k INT, ts TIMESTAMP, d DATE, v INT)",
    "named": 'CREATE TABLE named(k INT, "current_timestamp" TIMESTAMP, "current_date" DATE, v INT)',
}

# kind -> (table, column, comparison, clock)
VIEWS = {
    "ts": ("src", "ts", "<=", "current_timestamp"),
    "dt": ("src", "d", ">=", "current_date"),
    "named_ts": ("named", '"current_timestamp"', "<=", "openivm_clock_timestamp()"),
    "named_dt": ("named", '"current_date"', ">=", "openivm_clock_date()"),
}

INSERT_K5 = "INSERT INTO {t} VALUES (5, TIMESTAMP '2026-01-01 12:00:00', DATE '2025-12-31', 20000)"

# (name, initial instant, zone, steps); each step is
# (instant, zone, DML, expected clock_mode, expected clock_reason, clock alone must change the answer)
SCENARIOS = [
    ("empty_delta_clock_advance", T0, LOS_ANGELES, [(T1, LOS_ANGELES, [], "incremental", "ok", True)]),
    (
        "one_group_mutation_while_others_cross",
        T0,
        LOS_ANGELES,
        [(T1, LOS_ANGELES, [INSERT_K5], "incremental", "ok", False)],
    ),
    (
        "delete_and_key_move",
        T0,
        LOS_ANGELES,
        [
            (
                T1,
                LOS_ANGELES,
                ["DELETE FROM {t} WHERE k = 3", "UPDATE {t} SET k = 4 WHERE k = 1 AND v = 1"],
                "incremental",
                "ok",
                False,
            )
        ],
    ),
    (
        "midnight_endpoints",
        T0,
        LOS_ANGELES,
        [
            (T1, LOS_ANGELES, [], "incremental", "ok", True),
            (T2, LOS_ANGELES, ["INSERT INTO {t} VALUES (6, NULL, NULL, 1)"], "incremental", "ok", True),
            (T2, LOS_ANGELES, [], "incremental", "ok", False),
        ],
    ),
    (
        "clock_moves_backwards",
        T1,
        LOS_ANGELES,
        [(T0, LOS_ANGELES, [INSERT_K5], "full_at_as_of", "clock_regressed", False)],
    ),
    (
        "zone_change",
        T0,
        LOS_ANGELES,
        [(T1, TOKYO, [INSERT_K5], "full_at_as_of", "zone_changed", False)],
    ),
]


def decode_outputs(text: str) -> list:
    decoder = json.JSONDecoder()
    outputs, position = [], 0
    while True:
        while position < len(text) and text[position].isspace():
            position += 1
        if position >= len(text):
            return outputs
        value, position = decoder.raw_decode(text, position)
        outputs.append(value)


def run_duckdb(binary: Path, database: Path, files: Path, zone: str, sql: str) -> list:
    prefix = (
        f"SET openivm_files_path='{files}';\n"
        "SET default_null_order='NULLS_FIRST_ON_ASC_LAST_ON_DESC';\n"
        f"SET TimeZone='{zone}';\n"
    )
    result = subprocess.run(
        [str(binary), str(database), "-json", "-bail"],
        input=prefix + sql,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"DuckDB failed:\n{result.stdout}\n{result.stderr}")
    return decode_outputs(result.stdout)


def instant_literal(instant: str) -> str:
    return "TIMESTAMPTZ '" + instant.replace("T", " ").replace("Z", "+00") + "'"


def view_query(kind: str, clock: str = None) -> str:
    table, column, comparison, live_clock = VIEWS[kind]
    return (
        f"SELECT k, SUM(CASE WHEN {column} {comparison} {clock or live_clock} THEN v ELSE 0 END) AS s "
        f"FROM {table} GROUP BY k"
    )


def oracle(kind: str, instant: str, zone: str) -> str:
    local = f"timezone('{zone}', {instant_literal(instant)})"
    return view_query(kind, local if kind.endswith("ts") else f"CAST({local} AS DATE)")


def mismatch_sql(kind: str, query: str) -> str:
    view = f"mv_clock_{kind}"
    return (
        f"SELECT (SELECT COUNT(*) FROM (SELECT * FROM {view} EXCEPT ALL ({query}))) + "
        f"(SELECT COUNT(*) FROM (({query}) EXCEPT ALL SELECT * FROM {view})) AS mismatches;"
    )


def compile_program(binary, database, files, zone, kind, clock: dict) -> list:
    facts = json.dumps({"target_dialect": "duckdb", "compile_only": True, "clock": clock}, separators=(",", ":"))
    sql = (
        "SELECT refresh_type_name, clock_mode, clock_reason, stmt_order, sql "
        f"FROM openivm_compile_with_facts('mv_clock_{kind}', '{facts}') ORDER BY stmt_order;"
    )
    return run_duckdb(binary, database, files, zone, sql)[0]


def run_scenario(binary: Path, root: Path, name: str, initial: str, initial_zone: str, steps: list):
    scenario_dir = root / name
    files = scenario_dir / "files"
    files.mkdir(parents=True)
    database = scenario_dir / "test.db"

    setup = "".join(f"{create};\n{SEED.format(t=table)}\n" for table, create in TABLES.items())
    for kind in VIEWS:
        setup += f"CREATE MATERIALIZED VIEW mv_clock_{kind} AS {view_query(kind)};\n"
    run_duckdb(binary, database, files, initial_zone, setup)

    # Kinds whose same-instant answer differed from a column self-comparison at some step.
    distinguished = set()

    previous = None
    sequence = [(initial, initial_zone, [], "full_at_as_of", "missing_previous", False)] + steps
    for index, (instant, zone, dml, expected_mode, expected_reason, clock_changes_answer) in enumerate(sequence):
        label = f"{name} step {index}"
        if dml:
            run_duckdb(binary, database, files, zone, "".join(s.format(t=t) + ";\n" for t in TABLES for s in dml))
        clock = {"as_of": instant, "time_zone": zone}
        if previous:
            clock["previous_as_of"], clock["previous_time_zone"] = previous

        if clock_changes_answer:
            changed = run_duckdb(
                binary,
                database,
                files,
                zone,
                "SELECT COUNT(*) AS changed FROM (("
                + oracle("ts", instant, zone)
                + ") EXCEPT ALL ("
                + oracle("ts", previous[0], previous[1])
                + "));",
            )[0][0]["changed"]
            if changed == 0:
                raise AssertionError(f"{label}: the clock advance must change the answer or the case proves nothing")

        replay = ""
        for kind in VIEWS:
            rows = compile_program(binary, database, files, zone, kind, clock)
            observed = (rows[0]["clock_mode"], rows[0]["clock_reason"])
            if observed != (expected_mode, expected_reason):
                raise AssertionError(f"{label} {kind}: expected {(expected_mode, expected_reason)}, got {observed}")
            expected_type = "GROUP_RECOMPUTE" if expected_mode == "incremental" else "FULL_REFRESH"
            if {row["refresh_type_name"] for row in rows} != {expected_type}:
                raise AssertionError(f"{label} {kind}: expected {expected_type} program")
            live_clock = "current_timestamp" if VIEWS[kind][0] == "src" else "openivm_clock"
            if any(live_clock in row["sql"].lower() for row in rows):
                raise AssertionError(f"{label} {kind}: the program still reads the live clock")
            for row in rows:
                statement = row["sql"].rstrip()
                replay += statement + ("" if statement.endswith(";") else ";") + "\n"
        run_duckdb(binary, database, files, zone, replay)

        for kind in VIEWS:
            mismatches = run_duckdb(binary, database, files, zone, mismatch_sql(kind, oracle(kind, instant, zone)))[0][
                0
            ]["mismatches"]
            if mismatches != 0:
                raise AssertionError(f"{label} {kind}: {mismatches} bag mismatches against the same-instant answer")
            if (
                kind.startswith("named_")
                and run_duckdb(binary, database, files, zone, mismatch_sql(kind, view_query(kind, VIEWS[kind][1])))[0][
                    0
                ]["mismatches"]
            ):
                distinguished.add(kind)
        previous = (instant, zone)
    return distinguished


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_clock_window_group_recompute.py /path/to/duckdb")
    binary = Path(sys.argv[1])
    if not binary.exists() and binary.suffix.lower() != ".exe":
        windows_binary = binary.with_name(f"{binary.name}.exe")
        if windows_binary.exists():
            binary = windows_binary
    binary = binary.resolve()
    if not binary.exists():
        raise SystemExit(f"DuckDB binary not found: {binary}")

    distinguished = set()
    with tempfile.TemporaryDirectory(prefix="openivm_clock_window_") as temp_dir:
        for name, initial, zone, steps in SCENARIOS:
            distinguished |= run_scenario(binary, Path(temp_dir), name, initial, zone, steps)
            print(f"PASS {name}")
    named = {kind for kind in VIEWS if kind.startswith("named_")}
    if distinguished != named:
        raise AssertionError(f"rows never distinguish the clock from a self-comparison for {named - distinguished}")


if __name__ == "__main__":
    main()
