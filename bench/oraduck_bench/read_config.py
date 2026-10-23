"""Constants of the read benchmark (Oracle -> DuckDB)."""

from __future__ import annotations

from .config import WORK

# (rows in the target partition, columns)
READ_CASES = ((1_000_000, 50), (1_000_000, 550))
TARGET_DATE = "2024-05-07"
OTHER_DATES = ("2024-05-06", "2024-05-08")
READ_RUNS = 5
READ_COMPETITOR_RUNS = 3
READ_TIMEOUT_S = 15 * 60
READ_OUT_DB = WORK / "read_out.duckdb"
READ_THREADS = 8
READ_SUITE = "read"
CLOB_FILL_PERCENT = 2

# (method, DuckDB threads): OraDuck at N = 8 and N = 1, then the six other ways to fill a DuckDB table
READ_COMPETITOR_SERIES = (
    ("oraduck", 8), ("oraduck", 1), ("quack_oracle", 8), ("rinie_oracle", 8), ("oracledb_thin", 8),
    ("oracledb_thick", 8), ("connectorx", 8), ("sqlplus_csv", 8),
)
PYTHON_BATCH_SIZE = 100_000
SQLPLUS_ARRAYSIZE = 5000
