"""Timed read methods: Oracle partition -> DuckDB table."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import quote

from . import duck, read_schema
from .config import CSV_ROOT, DUCKDB15_CLI, DUCKDB_CLI, ORACLE_SCHEMA, RINIE_EXTENSION, WORK, OracleConfig
from .methods import _secret
from .read_config import READ_OUT_DB, READ_TIMEOUT_S, SQLPLUS_ARRAYSIZE

METHODS = ("oraduck", "oraduck_scan")
COMPETITORS = ("quack_oracle", "rinie_oracle", "oracledb_thin", "oracledb_thick", "connectorx", "sqlplus_csv")
RINIE_UNSIGNED = True
RESULT_PREFIX = "ORADUCK_BENCH_RESULT "


@dataclass
class Measurement:
    method: str
    table: str
    nrows: int
    ncols: int
    threads: int
    total_s: float
    link_bytes: int | None = None  # bytes received on the Oracle link (network-shaped runs)
    extra: dict = field(default_factory=dict)


TIMED_SCOPE = "first contact with Oracle to the rows stored in DuckDB"


def oraduck_sql(cfg: OracleConfig, table: str, threads: int, where: str,
                via: str = "attach") -> tuple[str, list[str]]:
    """(untimed setup, timed statements). `table` is a BENCH table name or OWNER.TABLE."""
    qualified = _qualified(table)
    setup = f"SET threads = {threads};\n{_secret(cfg)}"
    timed = []
    if via == "attach":
        timed.append("ATTACH '' AS ora (TYPE oraduck, SECRET bench);")
        source = f"ora.{qualified}"
    elif via == "scan":
        source = f"oraduck_scan('{qualified}', connection := 'bench')"
    else:
        raise ValueError(f"unknown path {via!r}")
    timed.append(f"CREATE TABLE t AS SELECT * FROM {source} WHERE {where};")
    return setup, timed


def reset_out_db() -> None:
    for suffix in ("", ".wal"):
        READ_OUT_DB.with_name(READ_OUT_DB.name + suffix).unlink(missing_ok=True)


def sessions_for(method: str, threads: int) -> int | str:
    """Oracle sessions the method really opens: `threads` is the DuckDB setting, not the parallelism."""
    if method in ("oraduck", "oraduck_scan", "connectorx"):
        return threads  # one session per scan thread / ConnectorX partition
    return 1  # quack-oracle and Rinie use one session by design; python-oracledb and SQL*Plus have one connection


def output_extra(cli: Path | None = None) -> dict:
    """Facts read from the output table, untimed: row count, file size, digest of the column types."""
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB, cli=cli)[0][0])
    types = duck.query("SELECT column_type, count(*) FROM (DESCRIBE t) GROUP BY 1 ORDER BY 1;", READ_OUT_DB,
                       cli=cli)
    return {"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size, "types": {t: int(n) for t, n in types}}


def run_oraduck(cfg: OracleConfig, table: str, threads: int, where: str, via: str = "attach",
                timeout_s: float | None = READ_TIMEOUT_S, nrows: int = 0, ncols: int = 0) -> Measurement:
    """Time of ATTACH + CTAS (secret creation untimed; the bind/describe of the first scan is inside the timer).
    The count, the file size and the types are read afterwards, untimed."""
    reset_out_db()
    setup, statements = oraduck_sql(cfg, table, threads, where, via)
    total = duck.timed_sum(setup, statements, READ_OUT_DB, timeout_s=timeout_s)
    method = "oraduck" if via == "attach" else "oraduck_scan"
    extra = {**output_extra(), "duckdb": _duckdb_version(DUCKDB_CLI), "sessions": sessions_for(method, threads),
             "timed": f"{TIMED_SCOPE}: " + ("ATTACH + CTAS" if via == "attach" else "CTAS")}
    return Measurement(method, table, nrows, ncols, threads, total, None, extra)


def _qualified(table: str) -> str:
    return table if "." in table else f"{ORACLE_SCHEMA}.{table}"


def _duckdb_version(cli: Path) -> str:
    return duck.version(None if cli == DUCKDB_CLI else cli)


def _extension_run(method: str, cli: Path, setup: str, statements: list[str], table: str, threads: int,
                   timeout_s: float | None, unsigned: bool, detail: str) -> Measurement:
    reset_out_db()
    cli_arg = None if cli == DUCKDB_CLI else cli
    total = duck.timed_sum(setup, statements, READ_OUT_DB, timeout_s=timeout_s, unsigned=unsigned, cli=cli_arg)
    extra = {**output_extra(cli_arg), "duckdb": _duckdb_version(cli), "sessions": sessions_for(method, threads),
             "timed": f"{TIMED_SCOPE}: {detail}"}
    return Measurement(method, table, 0, 0, threads, total, None, extra)


def quack_oracle_sql(cfg: OracleConfig, table: str, threads: int, where: str) -> tuple[str, list[str]]:
    """(untimed setup, timed statements): INSTALL, LOAD (the secret type needs it) and the secret are untimed,
    ATTACH + CTAS timed."""
    setup = (f"SET threads = {threads};\nINSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;\n"
             f"CREATE SECRET ora (TYPE oracle, HOST '{cfg.host}', PORT {cfg.port}, SERVICE_NAME '{cfg.service}', "
             f"USER '{cfg.user}', PASSWORD '{cfg.password}');")
    return setup, ["ATTACH 'ora' AS o (TYPE oracle_scanner);",
                   f"CREATE TABLE t AS SELECT * FROM o.{table} WHERE {where};"]


def rinie_oracle_sql(cfg: OracleConfig, table: str, threads: int, where: str) -> tuple[str, list[str]]:
    setup = f"SET threads = {threads};\nLOAD '{RINIE_EXTENSION}';"  # LOAD untimed, as for quack
    return setup, [f"ATTACH '{cfg.user}/{cfg.password}@{cfg.dsn}' AS r (TYPE oracle);",
                   f"CREATE TABLE t AS SELECT * FROM r.{_qualified(table)} WHERE {where};"]


_quack_cli: Path | None = None


def quack_cli() -> Path:
    """The DuckDB 2.0 CLI when the community `oracle_scanner` installs and loads there, else the 1.5.5 one."""
    global _quack_cli
    if _quack_cli is None:
        try:
            duck.run("INSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;")
            _quack_cli = DUCKDB_CLI
        except duck.DuckDBError:
            _quack_cli = DUCKDB15_CLI
    return _quack_cli


def run_quack_oracle(cfg: OracleConfig, table: str, threads: int, where: str,
                     timeout_s: float | None = READ_TIMEOUT_S) -> Measurement:
    setup, statements = quack_oracle_sql(cfg, table, threads, where)
    return _extension_run("quack_oracle", quack_cli(), setup, statements, table, threads, timeout_s, False,
                          "ATTACH + CTAS (extension LOAD untimed)")


def run_rinie_oracle(cfg: OracleConfig, table: str, threads: int, where: str,
                     timeout_s: float | None = READ_TIMEOUT_S) -> Measurement:
    setup, statements = rinie_oracle_sql(cfg, table, threads, where)
    return _extension_run("rinie_oracle", DUCKDB15_CLI, setup, statements, table, threads, timeout_s,
                          RINIE_UNSIGNED, "ATTACH + CTAS (extension LOAD untimed)")


# ---- Python clients, run as subprocesses ----

def connectorx_url(cfg: OracleConfig) -> str:
    """Used by read_python_methods (the one implementation)."""
    return f"oracle://{quote(cfg.user, safe='')}:{quote(cfg.password, safe='')}@{cfg.dsn}"


PYTHON_TIMED = {
    "oracledb_thin": "connect + fetch_df_batches + insert of every batch",
    "oracledb_thick": "connect + fetch_df_batches + insert of every batch",
    "connectorx": "read_sql (partitioned) + insert of every batch",
}


def python_sql(table: str, where: str) -> str:
    return f"SELECT * FROM {_qualified(table)} WHERE {where}"


def parse_python_result(stdout: str) -> dict:
    for line in reversed(stdout.splitlines()):
        if line.startswith(RESULT_PREFIX):
            return json.loads(line[len(RESULT_PREFIX):])
    raise duck.DuckDBError(f"no result line in the output:\n{stdout[-2000:]}")


FALLBACK_ROW_BYTES_PER_COLUMN = 100  # conservative: used when the optimizer statistics are missing


def _estimated_row_bytes(cfg: OracleConfig, table: str) -> int:
    """Average row length from the optimizer statistics; a conservative per-column estimate when they are
    missing. An Oracle error is reported as a DuckDBError (an `error` record, not an aborted case)."""
    import oracledb

    from . import oracle

    try:
        conn = oracle.connect(cfg)
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT avg_row_len FROM user_tables WHERE table_name = :t",
                            t=table.split(".")[-1].upper())
                row = cur.fetchone()
        finally:
            conn.close()
    except oracledb.Error as exc:
        raise duck.DuckDBError(duck.redact(f"cannot estimate the row size of {table}: {exc}",
                                           (cfg.password,))) from exc
    if row and row[0]:
        return int(row[0])
    return int(table.split("_")[2]) * FALLBACK_ROW_BYTES_PER_COLUMN


def _target_rows(table: str) -> int:
    return int(table.split("_")[1])


def available_memory() -> int:
    with open("/proc/meminfo") as fh:
        for line in fh:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable not found")


def connectorx_skip(estimated_bytes: int, available_bytes: int) -> str | None:
    return "result does not fit in memory" if estimated_bytes > available_bytes / 2 else None


def sqlplus_skip(estimated_csv_bytes: int, free_bytes: int) -> str | None:
    return "not enough free disk for the CSV" if free_bytes < 1.5 * estimated_csv_bytes else None


def _python_run(method: str, cfg: OracleConfig, table: str, threads: int, where: str,
                timeout_s: float | None, extra_env: dict | None = None) -> Measurement:
    reset_out_db()
    env = {**os.environ, "ORADUCK_ORACLE_USER": cfg.user, "ORADUCK_ORACLE_PASSWORD": cfg.password,
           "ORADUCK_ORACLE_DSN": cfg.dsn, **(extra_env or {})}
    args = [sys.executable, "-I", "-m", "oraduck_bench.read_python_methods", method, table, str(threads), where,
            str(READ_OUT_DB)]
    WORK.mkdir(parents=True, exist_ok=True)
    try:
        proc = subprocess.run(args, text=True, capture_output=True, cwd=WORK, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:
        raise duck.MethodTimeout(f"{method} killed after {timeout_s} s") from exc
    if proc.returncode != 0:
        raise duck.DuckDBError(duck.redact(f"{method} failed ({proc.returncode}):\n{proc.stderr.strip()[-3000:]}",
                                           (cfg.password,)))
    result = parse_python_result(proc.stdout)
    total = result.pop("total_s")
    if result.get("skipped"):
        return Measurement(method, table, 0, 0, threads, 0.0, None, result)
    result.update({**output_extra(), "duckdb": _duckdb_version(DUCKDB_CLI), "sessions": sessions_for(method, threads),
                   "timed": f"{TIMED_SCOPE}: {PYTHON_TIMED[method]}"})
    return Measurement(method, table, 0, 0, threads, total, None, result)


def run_oracledb_thin(cfg, table, threads, where, timeout_s=READ_TIMEOUT_S):
    return _python_run("oracledb_thin", cfg, table, threads, where, timeout_s)


def run_oracledb_thick(cfg, table, threads, where, timeout_s=READ_TIMEOUT_S):
    return _python_run("oracledb_thick", cfg, table, threads, where, timeout_s)


def run_connectorx(cfg, table, threads, where, timeout_s=READ_TIMEOUT_S):
    # the estimate only matters when the installed ConnectorX cannot stream (the script then decides to skip)
    env = {"ORADUCK_BENCH_CX_ESTIMATE": str(_target_rows(table) * _estimated_row_bytes(cfg, table)),
           "ORADUCK_BENCH_CX_AVAILABLE": str(available_memory())}
    return _python_run("connectorx", cfg, table, threads, where, timeout_s, env)


# ---- SQL*Plus -> CSV -> read_csv ----

def sqlplus_script(table: str, where: str, csv_path: str) -> str:
    # LONGCHUNKSIZE 1000000 (as planned) crashes SQL*Plus 19.32 (SIGSEGV) on tables with CLOBs, from 500000 up
    return "\n".join([
        "WHENEVER SQLERROR EXIT SQL.SQLCODE", "WHENEVER OSERROR EXIT FAILURE",
        "SET TERMOUT OFF", "SET ECHO OFF", "SET VERIFY OFF", "SET TRIMSPOOL ON", "SET TRIMOUT ON",
        "SET PAGESIZE 0", "SET LINESIZE 32767", "SET NUMWIDTH 40", "SET TIMING OFF",
        f"SET ARRAYSIZE {SQLPLUS_ARRAYSIZE}", "SET LONG 1000000 LONGCHUNKSIZE 100000", "SET FEEDBACK OFF",
        "SET MARKUP CSV ON QUOTE ON",
        "ALTER SESSION SET NLS_DATE_FORMAT = 'YYYY-MM-DD' NLS_TIMESTAMP_FORMAT = 'YYYY-MM-DD HH24:MI:SS.FF6' "
        "NLS_NUMERIC_CHARACTERS = '.,';",
        f"SPOOL {csv_path}",
        f"SELECT * FROM {_qualified(table)} WHERE {where};",
        "SPOOL OFF", "EXIT", ""])


def sqlplus_load_sql(csv_path: str, ncols: int) -> str:
    cols = ", ".join(f"'{c.name.upper()}': '{c.duck_type}'" for c in read_schema.read_columns(ncols))
    return (f"CREATE TABLE t AS SELECT * FROM read_csv('{csv_path}', header = true, columns = {{{cols}}}, "
            "dateformat = '%Y-%m-%d', timestampformat = '%Y-%m-%d %H:%M:%S.%f');")


def run_sqlplus_csv(cfg, table, threads, where, timeout_s=READ_TIMEOUT_S) -> Measurement:
    ncols = int(table.split("_")[2])
    CSV_ROOT.mkdir(parents=True, exist_ok=True)
    csv_path = CSV_ROOT / f"{table}_sqlplus.csv"
    script = CSV_ROOT / f"{table}_sqlplus.sql"
    estimate = int(_target_rows(table) * _estimated_row_bytes(cfg, table) * 1.3)
    reason = sqlplus_skip(estimate, shutil.disk_usage(CSV_ROOT).free)
    if reason:
        return Measurement("sqlplus_csv", table, 0, 0, threads, 0.0, None, {"skipped": reason})
    script.write_text(sqlplus_script(table, where, str(csv_path)))
    reset_out_db()
    csv_path.unlink(missing_ok=True)
    try:
        started = time.perf_counter()
        try:
            proc = subprocess.run(["taskset", "-c", "0-7", "sqlplus", "-F", "-S", "/nolog"],
                                  input=f"CONNECT {cfg.user}/{cfg.password}@//{cfg.dsn}\n@{script}\n",
                                  text=True, capture_output=True, cwd=WORK, timeout=timeout_s)
        except subprocess.TimeoutExpired as exc:
            raise duck.MethodTimeout(f"sqlplus killed after {timeout_s} s") from exc
        spool_s = time.perf_counter() - started
        if proc.returncode != 0 or "ORA-" in proc.stdout or "SP2-" in proc.stdout or not csv_path.exists():
            raise duck.DuckDBError(duck.redact(f"sqlplus failed ({proc.returncode}):\n{proc.stdout[-2000:]}"
                                               f"\n{proc.stderr[-1000:]}", (cfg.password,)))
        csv_bytes = csv_path.stat().st_size
        remaining = None if timeout_s is None else max(1.0, timeout_s - spool_s)
        load_s = duck.timed(f"SET threads = {threads};", sqlplus_load_sql(str(csv_path), ncols), READ_OUT_DB,
                            timeout_s=remaining)
    finally:
        csv_path.unlink(missing_ok=True)
        script.unlink(missing_ok=True)
    extra = {**output_extra(), "spool_s": spool_s, "load_s": load_s, "csv_bytes": csv_bytes,
             "duckdb": _duckdb_version(DUCKDB_CLI), "sessions": sessions_for("sqlplus_csv", threads),
             "timed": f"{TIMED_SCOPE}: sqlplus process start + connect + spool to CSV, then read_csv CTAS "
                      "(start-up cannot be separated)"}
    return Measurement("sqlplus_csv", table, 0, 0, threads, spool_s + load_s, None, extra)


def run(cfg: OracleConfig, method: str, table: str, threads: int, where: str,
        timeout_s: float | None = READ_TIMEOUT_S, nrows: int = 0, ncols: int = 0) -> Measurement:
    if method == "oraduck":
        return run_oraduck(cfg, table, threads, where, "attach", timeout_s, nrows, ncols)
    if method == "oraduck_scan":
        return run_oraduck(cfg, table, threads, where, "scan", timeout_s, nrows, ncols)
    if method in COMPETITORS:
        return globals()[f"run_{method}"](cfg, table, threads, where, timeout_s)
    raise ValueError(f"unknown method {method!r}")


def versions() -> dict[str, str]:
    """Versions of the competitors, with the DuckDB that runs each extension."""
    import importlib.metadata as md

    sql = "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = '{}';"
    quack_cli_path = quack_cli()
    quack = duck.query(f"INSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;\n{sql.format('oracle_scanner')}",
                       ":memory:", cli=None if quack_cli_path == DUCKDB_CLI else quack_cli_path)[0][0]
    rinie = duck.query(f"LOAD '{RINIE_EXTENSION}';\n{sql.format('oracle')}", ":memory:", unsigned=True,
                       cli=DUCKDB15_CLI)[0][0]
    out = subprocess.run(["sqlplus", "-V"], text=True, capture_output=True).stdout
    return {"quack_oracle": f"{quack} on DuckDB {_duckdb_version(quack_cli_path)}",
            "rinie_oracle": f"{rinie} on DuckDB {_duckdb_version(DUCKDB15_CLI)}",
            "oracledb": md.version("oracledb"), "connectorx": md.version("connectorx"),
            "sqlplus": " ".join(out.split())}
