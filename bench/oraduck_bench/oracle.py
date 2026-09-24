"""Oracle access for the benchmark (python-oracledb, thin mode)."""

from __future__ import annotations

import oracledb

from . import schema
from .checks import ORACLE_CHECKSUM_SQL, Checksums
from .config import OracleConfig


# A load killed on timeout leaves a session that holds its table lock until Oracle rolls it back:
# DDL (TRUNCATE, DROP) waits for it instead of failing with ORA-00054.
DDL_LOCK_TIMEOUT_S = 600


def connect(cfg: OracleConfig) -> oracledb.Connection:
    conn = oracledb.connect(user=cfg.user, password=cfg.password, dsn=cfg.dsn)
    execute(conn, f"ALTER SESSION SET DDL_LOCK_TIMEOUT = {DDL_LOCK_TIMEOUT_S}")
    return conn


def execute(conn: oracledb.Connection, sql: str) -> None:
    with conn.cursor() as cur:
        cur.execute(sql)


def drop_table(conn: oracledb.Connection, table: str) -> None:
    execute(conn, f"BEGIN EXECUTE IMMEDIATE 'DROP TABLE {table} PURGE'; "
                  "EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;")


def recreate_table(conn: oracledb.Connection, table: str, ncols: int) -> None:
    drop_table(conn, table)
    execute(conn, schema.oracle_ddl(table, ncols))


def truncate(conn: oracledb.Connection, table: str) -> None:
    execute(conn, f"TRUNCATE TABLE {table} DROP STORAGE")


def checksums(conn: oracledb.Connection, table: str) -> Checksums:
    with conn.cursor() as cur:
        cur.execute(ORACLE_CHECKSUM_SQL.format(table=table))
        return Checksums(*(int(v) for v in cur.fetchone()))


def symmetric_difference(conn: oracledb.Connection, a: str, b: str) -> int:
    with conn.cursor() as cur:
        cur.execute(f"SELECT COUNT(*) FROM ((SELECT * FROM {a} MINUS SELECT * FROM {b}) "
                    f"UNION ALL (SELECT * FROM {b} MINUS SELECT * FROM {a}))")
        return int(cur.fetchone()[0])
