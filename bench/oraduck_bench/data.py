"""Benchmark data generation (Fakelake) and loading (DuckDB) — not timed."""

from __future__ import annotations

import subprocess
from pathlib import Path

from . import duck, schema
from .config import DUCKDB_FILE, FAKELAKE, WORK


def table_exists(table: str) -> bool:
    if not DUCKDB_FILE.exists():
        return False
    rows = duck.query(f"SELECT count(*) FROM information_schema.tables WHERE table_name = '{table}';", DUCKDB_FILE)
    return rows[0][0] == "1"


def generate_parquet(nrows: int, ncols: int) -> Path:
    if not FAKELAKE.exists():
        raise RuntimeError("fakelake not found: run infra/fakelake/install.sh")
    table = schema.table_name(nrows, ncols)
    out_dir = WORK / "parquet"
    out_dir.mkdir(parents=True, exist_ok=True)
    config_file = out_dir / f"{table}.yaml"
    config_file.write_text(schema.fakelake_yaml(ncols, nrows, str(out_dir / table)))
    proc = subprocess.run([str(FAKELAKE), "generate", str(config_file)], cwd=WORK, capture_output=True, text=True)
    parquet = out_dir / f"{table}.parquet"
    if proc.returncode != 0 or not parquet.exists():
        raise RuntimeError(f"fakelake did not produce {parquet}:\n{proc.stdout}\n{proc.stderr}")
    return parquet


def ensure_case(nrows: int, ncols: int) -> str:
    table = schema.table_name(nrows, ncols)
    if table_exists(table):
        return table
    parquet = generate_parquet(nrows, ncols)
    duck.run(schema.duckdb_load_sql(table, parquet, ncols), DUCKDB_FILE)
    parquet.unlink()
    return table
