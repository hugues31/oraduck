"""Constants and configuration of the OraDuck benchmark."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DUCKDB_CLI = ROOT / "build" / "release" / "duckdb"
FAKELAKE = ROOT / ".deps" / "fakelake" / "bin" / "fakelake"
WORK = ROOT / "bench" / "work"  # on the NVMe under /home, never /tmp (tmpfs)
DUCKDB_FILE = WORK / "bench.duckdb"
CSV_ROOT = WORK / "csv"
RESULTS = ROOT / "bench" / "results"

ROWS = (100_000, 1_000_000, 10_000_000)
COLS = (5, 20, 50)
SEED = 42
THREADS = 8
SCALING_THREADS = (1, 2, 4, 8)
SCALING_CASE = (1_000_000, 20)
RUNS = 5
ORADUCK_STREAM_SIZE = 4 << 20  # probe (E4), confirmed by `oraduck-bench tune`
TUNE_STREAM_SIZES = (64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024, 1 << 20, 4 << 20, 16 << 20)
SQLLDR_PARAMS = (
    "multithreading=true",
    "direct=true",
    "parallel=true",
    "bindsize=134217728",
    "readsize=134217728",
    "streamsize=1000000",
    "columnarrayrows=1000",
)
ORACLE_SCHEMA = "BENCH"
ORACLE_CONTAINER = "oraduck-ora19"

# Comparison with other DuckDB -> Oracle extensions
RINIE_EXTENSION = ROOT / ".deps" / "competitors" / "rinie" / "oracle.duckdb_extension"
RINIE_COMMIT = "20d4389"
COMPETITOR_RUNS = 3
# (method, DuckDB threads): OraDuck at N = 8 and N = 1 (the other extensions use a single session)
COMPETITOR_SERIES = (("oraduck", 8), ("oraduck", 1), ("quack_oracle", 8), ("rinie_oracle", 8))
COMPETITOR_TIMEOUT_S = 15 * 60
# An in-memory copy of 10M x 50 needs about 9 GB: that case reads the attached database file instead.
FILE_SOURCE_CASES = {(10_000_000, 50)}


@dataclass(frozen=True)
class OracleConfig:
    user: str
    password: str
    dsn: str

    @staticmethod
    def from_env() -> "OracleConfig":
        names = ("ORADUCK_ORACLE_USER", "ORADUCK_ORACLE_PASSWORD", "ORADUCK_ORACLE_DSN")
        missing = [n for n in names if not os.environ.get(n)]
        if missing:
            raise RuntimeError(f"missing variables: {', '.join(missing)} — run 'source env.sh'")
        return OracleConfig(*(os.environ[n] for n in names))

    @property
    def host(self) -> str:
        return self.dsn.split("/", 1)[0].rsplit(":", 1)[0]

    @property
    def port(self) -> int:
        return int(self.dsn.split("/", 1)[0].rsplit(":", 1)[1])

    @property
    def service(self) -> str:
        return self.dsn.split("/", 1)[1]

    @property
    def sqlldr_userid(self) -> str:
        return f"{self.user}/{self.password}@//{self.dsn}"
