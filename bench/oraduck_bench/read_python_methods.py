"""Python read methods, run as `python -I -m oraduck_bench.read_python_methods <method> <table> <threads> <where> <db>`.

The time is measured here, from the connection to the last insert into DuckDB; the credentials come from the
environment. The last stdout line is `ORADUCK_BENCH_RESULT {json}`.
"""

from __future__ import annotations

import json
import os
import sys
import time

RESULT_PREFIX = "ORADUCK_BENCH_RESULT "
BATCH_SIZE = 100_000


def result_line(result: dict) -> str:
    return RESULT_PREFIX + json.dumps(result)


def _credentials() -> tuple[str, str, str]:
    return (os.environ["ORADUCK_ORACLE_USER"], os.environ["ORADUCK_ORACLE_PASSWORD"],
            os.environ["ORADUCK_ORACLE_DSN"])


def _sink(db: str):
    import duckdb

    con = duckdb.connect(db)
    state = {"created": False, "batches": 0}

    def add(table) -> None:
        con.register("batch_tbl", table)
        if state["created"]:
            con.execute("INSERT INTO t SELECT * FROM batch_tbl")
        else:
            con.execute("CREATE TABLE t AS SELECT * FROM batch_tbl")
            state["created"] = True
        con.unregister("batch_tbl")
        state["batches"] += 1

    return con, state, add


def run_oracledb(thick: bool, sql: str, db: str) -> dict:
    import oracledb
    import pyarrow

    if thick:
        oracledb.init_oracle_client(lib_dir=os.environ["OCI_HOME"])
    user, password, dsn = _credentials()
    con, state, add = _sink(db)
    started = time.perf_counter()
    conn = oracledb.connect(user=user, password=password, dsn=dsn)
    try:
        for batch in conn.fetch_df_batches(sql, size=BATCH_SIZE):
            add(pyarrow.table(batch))
    finally:
        conn.close()
    total = time.perf_counter() - started
    con.close()
    return {"total_s": total, "batches": state["batches"], "oracledb": oracledb.__version__,
            "timed": "connect, fetch_df_batches and insert of every batch"}


def run_connectorx(sql: str, db: str, threads: int) -> dict:
    import connectorx as cx
    import pyarrow

    user, password, dsn = _credentials()
    from urllib.parse import quote

    url = f"oracle://{quote(user, safe='')}:{quote(password, safe='')}@{dsn}"
    con, state, add = _sink(db)
    extra = {"connectorx": cx.__version__}
    started = time.perf_counter()
    try:
        reader = cx.read_sql(url, sql, return_type="arrow_stream", partition_on="ID", partition_num=threads,
                             batch_size=BATCH_SIZE)
    except ValueError:  # this version cannot stream: one Arrow table, only when it fits in memory
        if int(os.environ["ORADUCK_BENCH_CX_ESTIMATE"]) > int(os.environ["ORADUCK_BENCH_CX_AVAILABLE"]) / 2:
            return {"total_s": 0.0, "skipped": "result does not fit in memory", **extra}
        add(cx.read_sql(url, sql, return_type="arrow", partition_on="ID", partition_num=threads))
        extra["return_type"] = "arrow"
    else:
        for batch in reader:
            add(pyarrow.Table.from_batches([batch]))
        extra["return_type"] = "arrow_stream"
    total = time.perf_counter() - started
    con.close()
    return {"total_s": total, "batches": state["batches"], **extra,
            "timed": "read_sql (partitioned) and insert of every batch"}


def main(argv: list[str]) -> int:
    method, table, threads, where, db = argv
    sql = f"SELECT * FROM BENCH.{table} WHERE {where}" if "." not in table else f"SELECT * FROM {table} WHERE {where}"
    if method == "oracledb_thin":
        result = run_oracledb(False, sql, db)
    elif method == "oracledb_thick":
        result = run_oracledb(True, sql, db)
    elif method == "connectorx":
        result = run_connectorx(sql, db, int(threads))
    else:
        raise SystemExit(f"unknown method {method!r}")
    print(result_line(result), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
