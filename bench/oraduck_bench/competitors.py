"""OraDuck against the other DuckDB -> Oracle extensions, from an in-memory DuckDB table.

- quack_oracle: community extension `oracle_scanner` (github.com/krokozyab/quack-oracle)
- rinie_oracle: github.com/rinie/duckdb-oracle, built from source (config.RINIE_EXTENSION)
"""

from __future__ import annotations

from pathlib import Path

from . import duck, schema
from .config import (COMPETITOR_TIMEOUT_S, DUCKDB15_CLI, DUCKDB_FILE, FILE_SOURCE_CASES, ORACLE_SCHEMA, ORADUCK_STREAM_SIZE,
                     RINIE_EXTENSION, WORK, OracleConfig)
from .duck import MethodTimeout
from .methods import Measurement, _secret

__all__ = ["MethodTimeout", "cli_for", "export_source", "method_sql", "parquet_source_sql", "run", "should_skip", "source_sql", "versions"]

MEMORY_TABLE = "src_mem"


def source_sql(table: str, nrows: int, ncols: int) -> tuple[str, str]:
    """SQL that prepares the source (not timed) and the name the timed statement reads from."""
    setup = f"ATTACH '{DUCKDB_FILE}' AS src (READ_ONLY);"
    if (nrows, ncols) in FILE_SOURCE_CASES:
        return setup, f"src.{table}"
    return f"{setup}\nCREATE TABLE {MEMORY_TABLE} AS SELECT * FROM src.{table};", MEMORY_TABLE


def parquet_source_sql(table: str, nrows: int, ncols: int, parquet: Path) -> tuple[str, str]:
    """Same as source_sql for the 1.5.5 CLI, which cannot open the 2.0 database file: a Parquet export of it."""
    if (nrows, ncols) in FILE_SOURCE_CASES:
        return "", f"read_parquet('{parquet}')"
    return f"CREATE TABLE {MEMORY_TABLE} AS SELECT * FROM read_parquet('{parquet}');", MEMORY_TABLE


def export_source(table: str) -> Path:
    """Parquet copy of a source table (made once, by the 2.0 CLI, not timed)."""
    path = WORK / "parquet" / f"{table}_src15.parquet"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        duck.run(f"ATTACH '{DUCKDB_FILE}' AS src (READ_ONLY);\nCOPY src.{table} TO '{tmp}' (FORMAT parquet);")
        tmp.rename(path)
    return path


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


def cli_for(method: str):
    """The 1.5.5 CLI for the two extensions (the community build 0.2.2 and Rinie's are not built for DuckDB 2.0);
    None is the 2.0 CLI. The 1.5.5 CLI cannot open the 2.0 database file: its source is a Parquet export."""
    return DUCKDB15_CLI if method in ("quack_oracle", "rinie_oracle") else None


def should_skip(timeouts: set[tuple[str, int, int]], method: str, nrows: int, ncols: int) -> bool:
    """A method that timed out is not run on cases with at least as many rows and columns."""
    return any(m == method and nrows >= r and ncols >= c for m, r, c in timeouts)


def run(cfg: OracleConfig, method: str, nrows: int, ncols: int, threads: int,
        timeout_s: float = COMPETITOR_TIMEOUT_S, oracle_table: str | None = None) -> Measurement:
    """One timed load; raises MethodTimeout when the DuckDB CLI runs longer than timeout_s."""
    table = schema.table_name(nrows, ncols)
    if cli_for(method) is None:
        source_setup, source = source_sql(table, nrows, ncols)
    else:
        source_setup, source = parquet_source_sql(table, nrows, ncols, export_source(table))
    setup, statement, unsigned = method_sql(method, cfg, oracle_table or table, source, threads)
    total = duck.timed(f"{setup}\n{source_setup}", statement, ":memory:", timeout_s=timeout_s, unsigned=unsigned,
                       cli=cli_for(method))
    stream_size = ORADUCK_STREAM_SIZE if method == "oraduck" else None
    return Measurement(method, table, nrows, ncols, threads, total, stream_size=stream_size)


def versions() -> dict[str, str]:
    """Versions reported by the two other extensions (duckdb_extensions())."""
    sql = "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = '{}';"
    quack = duck.query(f"INSTALL oracle_scanner FROM community;\nLOAD oracle_scanner;\n{sql.format('oracle_scanner')}",
                       ":memory:", cli=cli_for("quack_oracle"))
    rinie = duck.query(f"LOAD '{RINIE_EXTENSION}';\n{sql.format('oracle')}", ":memory:", unsigned=True,
                       cli=cli_for("rinie_oracle"))
    return {"quack_oracle": quack[0][0], "rinie_oracle": rinie[0][0]}
