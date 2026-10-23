"""Read benchmark data: Fakelake -> Parquet -> OraDuck COPY into a partitioned Oracle table (not timed)."""

from __future__ import annotations

import subprocess
import time

import oracledb

from . import duck, oracle, read_schema, schema
from .config import FAKELAKE, ORACLE_SCHEMA, WORK, OracleConfig
from .methods import _secret
from .read_config import CLOB_FILL_PERCENT, OTHER_DATES, TARGET_DATE

TARGET_FILTER = f"PART_DATE = DATE '{TARGET_DATE}'"


def _is_complete(conn: oracledb.Connection, table: str, nrows: int) -> bool:
    """True when the table exists, was fully loaded (comment set at the end) and its target partition is right."""
    with conn.cursor() as cur:
        cur.execute("SELECT comments FROM user_tab_comments WHERE table_name = :t", t=table)
        row = cur.fetchone()
        if row is None or row[0] != read_schema.complete_comment():
            return False
        cur.execute(f"SELECT COUNT(*) FROM {table} WHERE {TARGET_FILTER}")
        return cur.fetchone()[0] == nrows


def generate_parquet(nrows: int, ncols: int):
    if not FAKELAKE.exists():
        raise RuntimeError("fakelake not found: run infra/fakelake/install.sh")
    table = read_schema.read_table_name(nrows, ncols)
    out_dir = WORK / "parquet"
    out_dir.mkdir(parents=True, exist_ok=True)
    total = nrows + len(OTHER_DATES) * (nrows // 20)
    config_file = out_dir / f"{table}.yaml"
    config_file.write_text(schema.fakelake_yaml(ncols, total, str(out_dir / table), cols=read_schema.read_columns(ncols)))
    proc = subprocess.run([str(FAKELAKE), "generate", str(config_file)], cwd=WORK, capture_output=True, text=True)
    parquet = out_dir / f"{table}.parquet"
    if proc.returncode != 0 or not parquet.exists():
        raise RuntimeError(f"fakelake did not produce {parquet}:\n{proc.stdout}\n{proc.stderr}")
    return parquet


def ensure_read_table(cfg: OracleConfig, nrows: int, ncols: int) -> str:
    """The partitioned table BENCH.RD_<rows>_<cols>; a complete existing table is kept."""
    table = read_schema.read_table_name(nrows, ncols)
    conn = oracle.connect(cfg)
    try:
        if _is_complete(conn, table, nrows):
            return table
        oracle.drop_table(conn, table)
        started = time.perf_counter()
        parquet = generate_parquet(nrows, ncols)
        print(f"{table}: parquet generated in {time.perf_counter() - started:.0f} s", flush=True)
        oracle.execute(conn, read_schema.read_oracle_ddl(nrows, ncols))
        load = read_schema.read_load_sql(parquet, ncols, nrows)
        duck.run(f"{_secret(cfg)}\nCOPY ({load}) TO '{ORACLE_SCHEMA}.{table}' (FORMAT oraduck, CONNECTION 'bench');",
                 ":memory:")
        parquet.unlink()
        print(f"{table}: loaded in {time.perf_counter() - started:.0f} s", flush=True)
        oracle.execute(conn, read_schema.clob_fill_sql(table, ncols, CLOB_FILL_PERCENT))
        conn.commit()
        oracle.execute(conn, f"BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{table}', degree => 4); END;")
        oracle.execute(conn, f"COMMENT ON TABLE {table} IS '{read_schema.complete_comment()}'")
        print(f"{table}: ready in {time.perf_counter() - started:.0f} s", flush=True)
        return table
    finally:
        conn.close()
