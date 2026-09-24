"""Runs the DuckDB CLI built with the OraDuck extension."""

from __future__ import annotations

import csv
import io
import re
import subprocess
from pathlib import Path

from .checks import DUCK_CHECKSUM_SQL, Checksums
from .config import DUCKDB_CLI, WORK

_TIMER = re.compile(r"Run Time \(s\): real (\d+(?:\.\d+)?)")


class DuckDBError(RuntimeError):
    pass


class MethodTimeout(DuckDBError):
    pass


def run(sql: str, db: Path | str = ":memory:", csv_output: bool = False, timeout_s: float | None = None,
        unsigned: bool = False) -> str:
    args = [str(DUCKDB_CLI), "-batch", "-bail"]
    if csv_output:
        args += ["-csv", "-noheader"]
    if unsigned:
        args.append("-unsigned")
    args.append(str(db))
    WORK.mkdir(parents=True, exist_ok=True)
    try:
        proc = subprocess.run(args, input=sql, text=True, capture_output=True, cwd=WORK, timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:  # the CLI was killed
        raise MethodTimeout(f"duckdb killed after {timeout_s} s") from exc
    if proc.returncode != 0:
        raise DuckDBError(f"duckdb failed ({proc.returncode}):\n{proc.stderr.strip()}\n--- script ---\n{sql[:2000]}")
    return proc.stdout


def query(sql: str, db: Path | str, unsigned: bool = False) -> list[list[str]]:
    return [row for row in csv.reader(io.StringIO(run(sql, db, csv_output=True, unsigned=unsigned))) if row]


def timed(setup: str, statement: str, db: Path | str, timeout_s: float | None = None,
          unsigned: bool = False) -> float:
    """Time in seconds of `statement` alone, measured by `.timer on`."""
    out = run(f"{setup}\n.timer on\n{statement}\n.timer off\n", db, timeout_s=timeout_s, unsigned=unsigned)
    times = _TIMER.findall(out)
    if len(times) != 1:
        raise DuckDBError(f"missing or ambiguous timing in the output:\n{out[-2000:]}")
    return float(times[0])


def checksums(table: str, db: Path | str) -> Checksums:
    return Checksums(*(int(v) for v in query(DUCK_CHECKSUM_SQL.format(table=table), db)[0]))
