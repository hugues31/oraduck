"""End-to-end test helpers: the DuckDB CLI built with OraDuck and disposable Oracle tables."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DUCKDB = ROOT / "build" / "release" / "duckdb"
# The CLI's working directory: no file in it is named after a target table
WORKDIR = ROOT / "build" / "e2e"


def env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} is not set: run 'source env.sh'")
    return value


def secret_sql(name: str = "ora") -> str:
    return (
        f"CREATE SECRET {name} (TYPE oraduck, USER '{env('ORADUCK_ORACLE_USER')}', "
        f"PASSWORD '{env('ORADUCK_ORACLE_PASSWORD')}', DSN '{env('ORADUCK_ORACLE_DSN')}');\n"
    )


def run_duckdb(sql: str, db: str = ":memory:", timeout: float = 600) -> subprocess.CompletedProcess[str]:
    """Runs a script in the DuckDB CLI; does not raise on SQL failure."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    return subprocess.run(
        [str(DUCKDB), "-batch", "-bail", db],
        input=sql,
        text=True,
        capture_output=True,
        cwd=WORKDIR,
        timeout=timeout,
    )


def _copy_script(query: str, target: str, options: str, setup: str, secret: bool) -> str:
    return setup + (secret_sql() if secret else "") + f"COPY ({query}) TO '{target}' (FORMAT oraduck{options});\n"


def copy_ok(query: str, target: str, extra: str = "", setup: str = "", db: str = ":memory:") -> None:
    proc = run_duckdb(_copy_script(query, target, ", CONNECTION 'ora'" + extra, setup, True), db)
    assert proc.returncode == 0, f"COPY failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"


def copy_error(
    query: str,
    target: str,
    options: str = ", CONNECTION 'ora'",
    setup: str = "",
    secret: bool = True,
    db: str = ":memory:",
) -> str:
    proc = run_duckdb(_copy_script(query, target, options, setup, secret), db)
    assert proc.returncode != 0, f"COPY unexpectedly succeeded\nSTDOUT:\n{proc.stdout}"
    return proc.stderr + proc.stdout


class OracleTable:
    """Oracle table used by a test; name may be a double-quoted identifier."""

    def __init__(self, conn, name: str) -> None:
        self.conn = conn
        self.name = name

    def execute(self, sql: str) -> None:
        with self.conn.cursor() as cur:
            cur.execute(sql)

    def create(self, columns_ddl: str) -> None:
        self.execute(f"CREATE TABLE {self.name} ({columns_ddl})")

    def rows(self, select: str = "*", order_by: str = "1") -> list[tuple]:
        with self.conn.cursor() as cur:
            cur.execute(f"SELECT {select} FROM {self.name} ORDER BY {order_by}")
            return cur.fetchall()

    def scalar(self, expr: str):
        with self.conn.cursor() as cur:
            cur.execute(f"SELECT {expr} FROM {self.name}")
            return cur.fetchone()[0]

    def drop(self) -> None:
        self.execute(
            f"BEGIN EXECUTE IMMEDIATE 'DROP TABLE {self.name} PURGE'; "
            "EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;"
        )
