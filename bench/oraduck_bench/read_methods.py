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


def oraduck_sql(cfg: OracleConfig, table: str, threads: int, where: str, via: str = "attach") -> tuple[str, str]:
    """(setup, timed statement). `table` is a BENCH table name or OWNER.TABLE."""
    qualified = table if "." in table else f"{ORACLE_SCHEMA}.{table}"
    setup = f"SET threads = {threads};\n{_secret(cfg)}"
    if via == "attach":
        setup += "\nATTACH '' AS ora (TYPE oraduck, SECRET bench);"
        source = f"ora.{qualified}"
    elif via == "scan":
        source = f"oraduck_scan('{qualified}', connection := 'bench')"
    else:
        raise ValueError(f"unknown path {via!r}")
    return setup, f"CREATE TABLE t AS SELECT * FROM {source} WHERE {where};"


def reset_out_db() -> None:
    for suffix in ("", ".wal"):
        READ_OUT_DB.with_name(READ_OUT_DB.name + suffix).unlink(missing_ok=True)


def run_oraduck(cfg: OracleConfig, table: str, threads: int, where: str, via: str = "attach",
                timeout_s: float | None = READ_TIMEOUT_S, nrows: int = 0, ncols: int = 0) -> Measurement:
    """Time of the CTAS alone (`timed` in extra): the ATTACH and secret are set up untimed, the bind/describe of
    the first scan is inside the timer. The count and the file size are read afterwards, untimed."""
    reset_out_db()
    setup, statement = oraduck_sql(cfg, table, threads, where, via)
    total = duck.timed(setup, statement, READ_OUT_DB, timeout_s=timeout_s)
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB)[0][0])
    extra = {"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size,
             "timed": "CTAS (ATTACH untimed; bind/describe of the first scan included)"}
    return Measurement("oraduck" if via == "attach" else "oraduck_scan", table, nrows, ncols, threads, total,
                       None, extra)


def _qualified(table: str) -> str:
    return table if "." in table else f"{ORACLE_SCHEMA}.{table}"


def _duckdb_version(cli: Path) -> str:
    return duck.version(None if cli == DUCKDB_CLI else cli)


def _extension_run(method: str, cli: Path, setup: str, statement: str, cfg_table: str, threads: int, where: str,
                   timeout_s: float | None, unsigned: bool, timed: str) -> Measurement:
    reset_out_db()
    total = duck.timed(setup, statement, READ_OUT_DB, timeout_s=timeout_s, unsigned=unsigned,
                       cli=None if cli == DUCKDB_CLI else cli)
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB)[0][0])
    extra = {"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size, "duckdb": _duckdb_version(cli), "timed": timed}
    return Measurement(method, cfg_table, 0, 0, threads, total, None, extra)


def quack_oracle_sql(cfg: OracleConfig, table: str, threads: int, where: str) -> tuple[str, str]:
    setup = (f"SET threads = {threads};\nINSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;\n"
             f"CREATE SECRET ora (TYPE oracle, HOST '{cfg.host}', PORT {cfg.port}, SERVICE_NAME '{cfg.service}', "
             f"USER '{cfg.user}', PASSWORD '{cfg.password}');\nATTACH 'ora' AS o (TYPE oracle_scanner);")
    return setup, f"CREATE TABLE t AS SELECT * FROM o.{table} WHERE {where};"


def rinie_oracle_sql(cfg: OracleConfig, table: str, threads: int, where: str) -> tuple[str, str]:
    setup = (f"SET threads = {threads};\nLOAD '{RINIE_EXTENSION}';\n"
             f"ATTACH '{cfg.user}/{cfg.password}@{cfg.dsn}' AS r (TYPE oracle);")
    return setup, f"CREATE TABLE t AS SELECT * FROM r.{_qualified(table)} WHERE {where};"


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
    setup, statement = quack_oracle_sql(cfg, table, threads, where)
    return _extension_run("quack_oracle", quack_cli(), setup, statement, table, threads, where, timeout_s, False,
                          "CTAS (ATTACH untimed)")


def run_rinie_oracle(cfg: OracleConfig, table: str, threads: int, where: str,
                     timeout_s: float | None = READ_TIMEOUT_S) -> Measurement:
    setup, statement = rinie_oracle_sql(cfg, table, threads, where)
    return _extension_run("rinie_oracle", DUCKDB15_CLI, setup, statement, table, threads, where, timeout_s,
                          RINIE_UNSIGNED, "CTAS (LOAD and ATTACH untimed)")


# ---- Python clients, run as subprocesses ----

def connectorx_url(cfg: OracleConfig) -> str:
    return f"oracle://{quote(cfg.user, safe='')}:{quote(cfg.password, safe='')}@{cfg.dsn}"


def python_sql(table: str, where: str) -> str:
    return f"SELECT * FROM {_qualified(table)} WHERE {where}"


def parse_python_result(stdout: str) -> dict:
    for line in reversed(stdout.splitlines()):
        if line.startswith(RESULT_PREFIX):
            return json.loads(line[len(RESULT_PREFIX):])
    raise duck.DuckDBError(f"no result line in the output:\n{stdout[-2000:]}")


def _estimated_row_bytes(cfg: OracleConfig, table: str) -> int:
    """Average row length from the optimizer statistics (the read tables are analyzed), 0 when unknown."""
    from . import oracle

    conn = oracle.connect(cfg)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT avg_row_len FROM user_tables WHERE table_name = :t", t=table.split(".")[-1])
            row = cur.fetchone()
            return int(row[0] or 0) if row else 0
    finally:
        conn.close()


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
        raise duck.DuckDBError(duck.redact(f"{method} failed ({proc.returncode}):\n{proc.stderr.strip()[-3000:]}"))
    result = parse_python_result(proc.stdout)
    total = result.pop("total_s")
    if result.get("skipped"):
        return Measurement(method, table, 0, 0, threads, 0.0, None, result)
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB)[0][0])
    result.update({"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size})
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
                                               f"\n{proc.stderr[-1000:]}"))
        csv_bytes = csv_path.stat().st_size
        remaining = None if timeout_s is None else max(1.0, timeout_s - spool_s)
        load_s = duck.timed(f"SET threads = {threads};", sqlplus_load_sql(str(csv_path), ncols), READ_OUT_DB,
                            timeout_s=remaining)
    finally:
        csv_path.unlink(missing_ok=True)
        script.unlink(missing_ok=True)
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB)[0][0])
    extra = {"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size, "spool_s": spool_s, "load_s": load_s,
             "csv_bytes": csv_bytes, "duckdb": _duckdb_version(DUCKDB_CLI),
             "timed": "sqlplus connect + spool to CSV, then read_csv CTAS"}
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
