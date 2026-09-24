"""OraDuck against the other DuckDB -> Oracle extensions, from an in-memory DuckDB table.

- quack_oracle: community extension `oracle_scanner` (github.com/krokozyab/quack-oracle)
- rinie_oracle: github.com/rinie/duckdb-oracle, built from source (config.RINIE_EXTENSION)
"""

from __future__ import annotations

from . import duck, schema
from .config import (COMPETITOR_TIMEOUT_S, DUCKDB_FILE, FILE_SOURCE_CASES, ORACLE_SCHEMA, ORADUCK_STREAM_SIZE,
                     RINIE_EXTENSION, OracleConfig)
from .duck import MethodTimeout
from .methods import Measurement, _secret

__all__ = ["MethodTimeout", "method_sql", "run", "should_skip", "source_sql", "versions"]

MEMORY_TABLE = "src_mem"


def source_sql(table: str, nrows: int, ncols: int) -> tuple[str, str]:
    """SQL that prepares the source (not timed) and the name the timed statement reads from."""
    setup = f"ATTACH '{DUCKDB_FILE}' AS src (READ_ONLY);"
    if (nrows, ncols) in FILE_SOURCE_CASES:
        return setup, f"src.{table}"
    return f"{setup}\nCREATE TABLE {MEMORY_TABLE} AS SELECT * FROM src.{table};", MEMORY_TABLE


def method_sql(method: str, cfg: OracleConfig, oracle_table: str, source: str,
               threads: int) -> tuple[str, str, bool]:
    """(setup, timed statement, needs -unsigned) of one method."""
    threads_sql = f"SET threads = {threads};"
    if method == "oraduck":
        return (f"{threads_sql}\n{_secret(cfg)}",
                f"COPY (SELECT * FROM {source}) TO '{ORACLE_SCHEMA}.{oracle_table}' "
                f"(FORMAT oraduck, CONNECTION 'bench', STREAM_SIZE {ORADUCK_STREAM_SIZE});",
                False)
    if method == "quack_oracle":
        return (f"{threads_sql}\nINSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;\n"
                f"CREATE SECRET ora (TYPE oracle, HOST '{cfg.host}', PORT {cfg.port}, "
                f"SERVICE_NAME '{cfg.service}', USER '{cfg.user}', PASSWORD '{cfg.password}');\n"
                "ATTACH 'ora' AS o (TYPE oracle_scanner);",
                f"INSERT INTO o.{oracle_table} SELECT * FROM {source};",
                False)
    if method == "rinie_oracle":
        return (f"{threads_sql}\nLOAD '{RINIE_EXTENSION}';\n"
                f"ATTACH '{cfg.user}/{cfg.password}@{cfg.dsn}' AS r (TYPE oracle);",
                f"INSERT INTO r.{ORACLE_SCHEMA}.{oracle_table} SELECT * FROM {source};",
                True)
    raise ValueError(f"unknown method {method!r}")


def should_skip(timeouts: set[tuple[str, int, int]], method: str, nrows: int, ncols: int) -> bool:
    """A method that timed out is not run on cases with at least as many rows and columns."""
    return any(m == method and nrows >= r and ncols >= c for m, r, c in timeouts)


def run(cfg: OracleConfig, method: str, nrows: int, ncols: int, threads: int,
        timeout_s: float = COMPETITOR_TIMEOUT_S, oracle_table: str | None = None) -> Measurement:
    """One timed load; raises MethodTimeout when the DuckDB CLI runs longer than timeout_s."""
    table = schema.table_name(nrows, ncols)
    source_setup, source = source_sql(table, nrows, ncols)
    setup, statement, unsigned = method_sql(method, cfg, oracle_table or table, source, threads)
    total = duck.timed(f"{setup}\n{source_setup}", statement, ":memory:", timeout_s=timeout_s, unsigned=unsigned)
    stream_size = ORADUCK_STREAM_SIZE if method == "oraduck" else None
    return Measurement(method, table, nrows, ncols, threads, total, stream_size=stream_size)


def versions() -> dict[str, str]:
    """Versions reported by the two other extensions (duckdb_extensions())."""
    sql = "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = '{}';"
    quack = duck.query(f"LOAD oracle_scanner;\n{sql.format('oracle_scanner')}", ":memory:")
    rinie = duck.query(f"LOAD '{RINIE_EXTENSION}';\n{sql.format('oracle')}", ":memory:", unsigned=True)
    return {"quack_oracle": quack[0][0], "rinie_oracle": rinie[0][0]}
