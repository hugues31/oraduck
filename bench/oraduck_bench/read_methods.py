"""Timed read methods: Oracle partition -> DuckDB table."""

from __future__ import annotations

from dataclasses import dataclass, field

from . import duck
from .config import ORACLE_SCHEMA, OracleConfig
from .methods import _secret
from .read_config import READ_OUT_DB, READ_TIMEOUT_S

METHODS = ("oraduck", "oraduck_scan")


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


def _reset_out_db() -> None:
    for suffix in ("", ".wal"):
        READ_OUT_DB.with_name(READ_OUT_DB.name + suffix).unlink(missing_ok=True)


def run_oraduck(cfg: OracleConfig, table: str, threads: int, where: str, via: str = "attach",
                timeout_s: float | None = READ_TIMEOUT_S, nrows: int = 0, ncols: int = 0) -> Measurement:
    _reset_out_db()
    setup, statement = oraduck_sql(cfg, table, threads, where, via)
    total = duck.timed(setup, statement, READ_OUT_DB, timeout_s=timeout_s)
    rows = int(duck.query("SELECT count(*) FROM t;", READ_OUT_DB)[0][0])
    extra = {"rows": rows, "db_bytes": READ_OUT_DB.stat().st_size}
    return Measurement("oraduck" if via == "attach" else "oraduck_scan", table, nrows, ncols, threads, total,
                       None, extra)


def run(cfg: OracleConfig, method: str, table: str, threads: int, where: str,
        timeout_s: float | None = READ_TIMEOUT_S, nrows: int = 0, ncols: int = 0) -> Measurement:
    if method == "oraduck":
        return run_oraduck(cfg, table, threads, where, "attach", timeout_s, nrows, ncols)
    if method == "oraduck_scan":
        return run_oraduck(cfg, table, threads, where, "scan", timeout_s, nrows, ncols)
    raise ValueError(f"unknown method {method!r}")
