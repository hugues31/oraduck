"""Pruned-unit overhead: a LIST/LIST table of 300 one-row subpartitions plus one target partition, read on the target
partition with the default split (one unit or more per segment, most of them pruned by the filter) and with split
'none' (one query)."""

from __future__ import annotations

import re
import statistics
import time
from pathlib import Path
from typing import Callable

from . import duck, oracle
from .config import ORACLE_SCHEMA, OracleConfig
from .methods import _secret
from .read_config import READ_THREADS, READ_TIMEOUT_S, TARGET_DATE
from .read_methods import parse_planning_ms
from .runner import _append

SEGMENTS_TABLE = "RS_SEGMENTS"
SMALL_PARTITIONS = 75
SUBPARTITIONS = ("A", "B", "C")  # and DEFAULT: 4 subpartitions per partition
TARGET_ROWS = 100_000
SEGMENTS_RUNS = 5
# 4 subpartitions of each small partition hold one row; the target partition's 4 hold TARGET_ROWS rows. Each segment
# takes one 8 MB initial extent: 304 segments, about 2.4 GB of tablespace (the table is dropped after the run).
SEGMENTS = (SMALL_PARTITIONS + 1) * (len(SUBPARTITIONS) + 1)
WHERE = f"PART_DATE = DATE '{TARGET_DATE}'"
# Timed statements, and the query EXPLAIN ANALYZE shows for each
QUERIES = {
    "count(*)": "SELECT count(*) FROM {source} WHERE {where};",
    "*": "CREATE TABLE t AS SELECT * FROM {source} WHERE {where};",
}
EXPLAINED = {
    "count(*)": "SELECT count(*) FROM {source} WHERE {where}",
    "*": "SELECT * FROM {source} WHERE {where}",
}
SPLITS = ("default", "none")


def small_dates() -> list[str]:
    """The dates of the one-row partitions: 2024-01-01 and the next 74 days."""
    import datetime

    start = datetime.date(2024, 1, 1)
    return [(start + datetime.timedelta(days=k)).isoformat() for k in range(SMALL_PARTITIONS)]


def segments_ddl(table: str = SEGMENTS_TABLE) -> str:
    days = [*small_dates(), TARGET_DATE]
    partitions = ",\n  ".join(f"PARTITION P_{d.replace('-', '')} VALUES (DATE '{d}')" for d in days)
    template = ", ".join(f"SUBPARTITION SP_{k} VALUES ('{k}')" for k in SUBPARTITIONS)
    return (
        f"CREATE TABLE {table} (PART_DATE DATE NOT NULL, SUB_KEY VARCHAR2(8) NOT NULL, ID NUMBER(38,0), "
        "V1 VARCHAR2(100), N1 NUMBER(25,9), TS1 TIMESTAMP(6))\n"
        "TABLESPACE BENCH\n"
        'PARTITION BY LIST ("PART_DATE")\n'
        'SUBPARTITION BY LIST ("SUB_KEY")\n'
        f"SUBPARTITION TEMPLATE ({template}, SUBPARTITION SP_DEF VALUES (DEFAULT))\n"
        f"(\n  {partitions}\n)"
    )


def segments_fill_sql(table: str = SEGMENTS_TABLE) -> list[str]:
    """One row in each subpartition of the small partitions (SUB_KEY 'D' goes to DEFAULT), then TARGET_ROWS rows in
    the target partition, spread over its 4 subpartitions."""
    keys = len(SUBPARTITIONS) + 1
    small = (
        f"INSERT INTO {table} SELECT DATE '2024-01-01' + p, CHR(65 + s), {TARGET_ROWS} + p * {keys} + s + 1, "
        "'small', p + s / 8, TIMESTAMP '2024-01-01 00:00:00' + NUMTODSINTERVAL(p, 'DAY') "
        f"FROM (SELECT LEVEL - 1 AS p FROM dual CONNECT BY LEVEL <= {SMALL_PARTITIONS}), "
        f"(SELECT LEVEL - 1 AS s FROM dual CONNECT BY LEVEL <= {keys})"
    )
    target = (
        f"INSERT /*+ APPEND */ INTO {table} SELECT DATE '{TARGET_DATE}', CHR(65 + MOD(n, {keys})), n, 'row ' || n, "
        f"n / 8, TIMESTAMP '{TARGET_DATE} 00:00:00' + NUMTODSINTERVAL(n, 'SECOND') "
        f"FROM (SELECT LEVEL AS n FROM dual CONNECT BY LEVEL <= {TARGET_ROWS})"
    )
    return [small, target]


def create_segments_table(cfg: OracleConfig, table: str = SEGMENTS_TABLE) -> str:
    conn = oracle.connect(cfg)
    try:
        oracle.drop_table(conn, table)
        oracle.execute(conn, segments_ddl(table))
        for sql in segments_fill_sql(table):
            oracle.execute(conn, sql)
            conn.commit()
        oracle.execute(conn, f"BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{table}'); END;")
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM user_segments WHERE segment_name = :t", t=table)
            segments = int(cur.fetchone()[0])
        if segments != SEGMENTS:
            raise RuntimeError(f"{table} has {segments} segments, expected {SEGMENTS}")
        return table
    finally:
        conn.close()


def drop_segments_table(cfg: OracleConfig, table: str = SEGMENTS_TABLE) -> None:
    conn = oracle.connect(cfg)
    try:
        oracle.drop_table(conn, table)
    finally:
        conn.close()


def scan_source(table: str, split: str) -> str:
    option = "" if split == "default" else f", split := '{split}'"
    return f"oraduck_scan('{ORACLE_SCHEMA}.{table}', connection := 'bench'{option})"


def statement(query: str, table: str, split: str) -> str:
    return QUERIES[query].format(source=scan_source(table, split), where=WHERE)


_UNITS = re.compile(r"Units:\s*(\d+)")
_SNAPSHOT = re.compile(r"Snapshot:\s*([^│\n]*?)\s*(?:│|\n|$)")


def parse_explain(output: str) -> dict:
    """Units, Planning (ms) and Snapshot (the snapshot used) of the OraDuck scan in an EXPLAIN ANALYZE output; None
    when absent."""
    units = _UNITS.search(output)
    snapshots = _SNAPSHOT.findall(output)
    return {"units": int(units[1]) if units else None, "planning_ms": parse_planning_ms(output),
            "snapshot": snapshots[-1] if snapshots else None}


def explain(cfg: OracleConfig, table: str, split: str, query: str, threads: int = READ_THREADS) -> dict:
    """One untimed EXPLAIN ANALYZE of the query's scan."""
    sql = EXPLAINED[query].format(source=scan_source(table, split), where=WHERE)
    out = duck.run(f"SET threads = {threads};\n{_secret(cfg)}\nEXPLAIN ANALYZE {sql};", timeout_s=READ_TIMEOUT_S)
    return parse_explain(out)


def timed_query(cfg: OracleConfig, table: str, split: str, query: str, threads: int = READ_THREADS) -> float:
    """Time of the statement alone (a new CLI per run: the scan's bind, planning and logons are timed; the secret
    is not)."""
    return duck.timed(f"SET threads = {threads};\n{_secret(cfg)}", statement(query, table, split), ":memory:",
                      timeout_s=READ_TIMEOUT_S)


def run_segments(cfg: OracleConfig, out: Path, runs: int = SEGMENTS_RUNS, threads: int = READ_THREADS,
                 create: Callable = create_segments_table, drop: Callable = drop_segments_table,
                 timed: Callable = timed_query, explain_fn: Callable = explain) -> list[dict]:
    """For each query: one warm-up round, then `runs` rounds alternating the default split and split 'none'; one
    `segments` record per query with the median of each split and their difference. The table is created first and
    dropped at the end."""
    duck.add_secret(cfg.password)
    table = create(cfg)
    records = []
    try:
        for query in QUERIES:
            times: dict[str, list[float]] = {split: [] for split in SPLITS}
            for round_index in range(runs + 1):
                for split in SPLITS:
                    t = timed(cfg, table, split, query, threads)
                    if round_index > 0:  # round 0 is the warm-up
                        times[split].append(t)
            record = {"type": "segments", "suite": "read", "table": table, "segments": SEGMENTS,
                      "target_rows": TARGET_ROWS, "threads": threads, "query": query, "where": WHERE,
                      "at": time.strftime("%Y-%m-%dT%H:%M:%S")}
            for split in SPLITS:
                record[split] = {**explain_fn(cfg, table, split, query, threads), "runs_s": times[split],
                                 "median_s": statistics.median(times[split])}
            record["difference_s"] = record["default"]["median_s"] - record["none"]["median_s"]
            records.append(_append(out, record))
            print(f"read-segments {query}: default {record['default']['median_s']:.3f} s "
                  f"({record['default']['units']} units, planning {record['default']['planning_ms']} ms), "
                  f"none {record['none']['median_s']:.3f} s, difference {record['difference_s']:+.3f} s", flush=True)
    finally:
        drop(cfg, table)
    return records
