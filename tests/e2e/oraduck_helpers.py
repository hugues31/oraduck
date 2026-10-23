"""End-to-end test helpers: the DuckDB CLI built with OraDuck and disposable Oracle tables."""

from __future__ import annotations

import json
import os
import re
import subprocess
from decimal import Decimal
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


def owner() -> str:
    """Schema of the configured Oracle user (ORADUCK_ORACLE_USER), which owns the test tables."""
    return env("ORADUCK_ORACLE_USER").upper()


def secret_sql(name: str = "ora", reader: bool = False) -> str:
    """CREATE SECRET for the test user, or for the READER user (CREATE SESSION only) when reader is set."""
    prefix = "ORADUCK_ORACLE_READER" if reader else "ORADUCK_ORACLE"
    return (
        f"CREATE SECRET {name} (TYPE oraduck, USER '{env(prefix + '_USER')}', "
        f"PASSWORD '{env(prefix + '_PASSWORD')}', DSN '{env('ORADUCK_ORACLE_DSN')}');\n"
    )


def run_duckdb(sql: str, db: str = ":memory:", timeout: float = 600,
               env: dict[str, str] | None = None, json_mode: bool = False) -> subprocess.CompletedProcess[str]:
    """Runs a script in the DuckDB CLI; does not raise on SQL failure. -no-agent: the same output whether or not an AI
    agent's environment variable is set."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    return subprocess.run(
        [str(DUCKDB), "-batch", "-bail", "-no-agent"] + (["-json"] if json_mode else []) + [db],
        input=sql,
        text=True,
        capture_output=True,
        cwd=WORKDIR,
        timeout=timeout,
        env=env,
    )


def json_documents(text: str) -> list:
    """Every JSON document printed by the CLI in -json mode, in order."""
    decoder = json.JSONDecoder(parse_float=Decimal)
    documents = []
    pos = 0
    while True:
        while pos < len(text) and text[pos] not in "[{":
            pos += 1
        if pos >= len(text):
            return documents
        document, pos = decoder.raw_decode(text, pos)
        documents.append(document)


# The CLI prints WARNING log entries on stdout (colored), even in -json mode, in the middle of a result
_WARNING = re.compile(r"(?:\x1b\[[0-9;]*m)*WARNING:\n(?:\x1b\[[0-9;]*m)*(.*?)\n\n(?:\x1b\[[0-9;]*m)*", re.DOTALL)


def split_warnings(stdout: str) -> tuple[str, list[str]]:
    """stdout without the CLI's WARNING blocks, and their messages."""
    return _WARNING.sub("", stdout), [m.strip() for m in _WARNING.findall(stdout)]


def scan_output(sql: str, setup: str = "", reader: bool = False, timeout: float = 600) -> tuple[list[dict], list[str]]:
    """Runs setup then sql after the ora secret, in -json mode; returns the rows of the last result and the warnings
    the CLI printed."""
    proc = run_duckdb(secret_sql(reader=reader) + setup + sql, json_mode=True, timeout=timeout)
    if proc.returncode != 0:
        raise AssertionError(f"query failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    stdout, warnings = split_warnings(proc.stdout)
    arrays = [d for d in json_documents(stdout) if isinstance(d, list)]
    assert arrays, f"no result\nSTDOUT:\n{proc.stdout}"
    return arrays[-1], warnings


def scan_json(sql: str, setup: str = "", reader: bool = False, timeout: float = 600) -> list[dict]:
    """Runs setup then sql after the ora secret, in -json mode; returns the rows of the last result."""
    return scan_output(sql, setup, reader, timeout)[0]


def scan_error(sql: str, setup: str = "", reader: bool = False) -> str:
    """Runs setup then sql after the ora secret; returns the error output (the script must fail)."""
    proc = run_duckdb(secret_sql(reader=reader) + setup + sql)
    assert proc.returncode != 0, f"query unexpectedly succeeded\nSTDOUT:\n{proc.stdout}"
    return proc.stderr + proc.stdout


def scan_nodes(node: dict | list) -> list[dict]:
    """extra_info of every oraduck_scan operator (the ones showing "Table") of an EXPLAIN (FORMAT json) tree."""
    found = []
    if isinstance(node, list):
        for child in node:
            found += scan_nodes(child)
        return found
    info = node.get("extra_info", {})
    if isinstance(info, dict) and "Table" in info:
        found.append(info)
    for key in ("children", "operator"):
        if key in node:
            found += scan_nodes(node[key])
    return found


def explain_scan(sql: str, setup: str = "", analyze: bool = False, reader: bool = False) -> list[dict]:
    """Extra info of the oraduck_scan operators of EXPLAIN [ANALYZE] (FORMAT json) sql."""
    options = "ANALYZE, FORMAT json" if analyze else "FORMAT json"
    proc = run_duckdb(secret_sql(reader=reader) + setup + f"EXPLAIN ({options}) {sql};\n", json_mode=True)
    if proc.returncode != 0:
        raise AssertionError(f"EXPLAIN failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")
    nodes = scan_nodes(json_documents(split_warnings(proc.stdout)[0])[-1])
    assert nodes, f"no oraduck_scan operator\nSTDOUT:\n{proc.stdout}"
    return nodes


def scan_call(table: str, options: str = "") -> str:
    """oraduck_scan('<table>', connection := 'ora'[, options])."""
    return f"oraduck_scan('{table}', connection := 'ora'{', ' + options if options else ''})"


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


def drop_view(conn, name: str) -> None:
    with conn.cursor() as cur:
        cur.execute(
            f"BEGIN EXECUTE IMMEDIATE 'DROP VIEW {name}'; "
            "EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;"
        )


def fill_ids(table: OracleTable, rows: int, columns: str = "ID", values: str = "n") -> None:
    """Inserts rows with n = 1..rows (direct path) and gathers statistics; values are expressions of n."""
    thousands = (rows + 999) // 1000
    table.execute(
        f"INSERT /*+ APPEND */ INTO {table.name} ({columns}) "
        f"SELECT {values} FROM (SELECT (a.i - 1) * 1000 + b.i AS n FROM "
        f"(SELECT LEVEL AS i FROM dual CONNECT BY LEVEL <= {thousands}) a, "
        f"(SELECT LEVEL AS i FROM dual CONNECT BY LEVEL <= 1000) b) WHERE n <= {rows}"
    )
    table.conn.commit()
    table.execute(f"BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{table.name}'); END;")
