"""Read protocol: warm-up, interleaved runs, checksums after every run, JSONL results."""

from __future__ import annotations

import time
from dataclasses import asdict, replace
from pathlib import Path
from typing import Callable

from . import competitors, duck, oracle, read_data, read_methods, read_schema
from .config import OracleConfig
from .read_config import READ_OUT_DB, READ_RUNS, READ_SUITE, READ_THREADS, READ_TIMEOUT_S
from .runner import ChecksumMismatch, _append

__all__ = ["ChecksumMismatch", "oracle_checksums", "duck_checksums", "run_read_case", "default_where"]


def default_where() -> str:
    return read_data.TARGET_FILTER


def oracle_checksums(cfg: OracleConfig, table: str, where: str) -> read_schema.ReadChecksums:
    conn = oracle.connect(cfg)
    try:
        with conn.cursor() as cur:
            cur.execute(read_schema.READ_CHECKSUM_ORACLE.format(table=table, where=where))
            return read_schema.ReadChecksums(*(int(v) for v in cur.fetchone()))
    finally:
        conn.close()


def duck_checksums() -> read_schema.ReadChecksums:
    row = duck.query(read_schema.READ_CHECKSUM_DUCK.format(table="t"), READ_OUT_DB)[0]
    return read_schema.ReadChecksums(*(int(v) for v in row))


def run_read_case(
    cfg: OracleConfig,
    nrows: int,
    ncols: int,
    methods: list[str],
    runs: int,
    out: Path,
    where: str | None = None,
    threads: int = READ_THREADS,
    timeouts: set[tuple[str, int, int]] | None = None,
    timeout_s: float = READ_TIMEOUT_S,
    warmup: bool = True,
    ensure: Callable = read_data.ensure_read_table,
    run_method: Callable = read_methods.run,
    duck_checksums: Callable = duck_checksums,
    oracle_checksums: Callable = oracle_checksums,
) -> list[dict]:
    """One warm-up round (unless warmup is false) then `runs` interleaved rounds of `methods`. A method that
    exceeds timeout_s is added to `timeouts` and is no longer run on cases at least as large. Oracle's checksums
    are computed once (the data does not change); DuckDB's are compared after every run."""
    where = where or default_where()
    timeouts = timeouts if timeouts is not None else set()
    table = ensure(cfg, nrows, ncols)
    expected = oracle_checksums(cfg, table, where)
    case = {"suite": READ_SUITE, "nrows": nrows, "ncols": ncols}
    records = []
    active = []
    for method in methods:
        if competitors.should_skip(timeouts, method, nrows, ncols):
            records.append(_append(out, {"type": "skipped", **case, "method": method, "threads": threads}))
            print(f"{READ_SUITE} {table} {method} N={threads}: not run", flush=True)
        else:
            active.append(method)
    for round_index in range(runs + (1 if warmup else 0)):
        is_warmup = warmup and round_index == 0
        for method in list(active):
            if method not in active:
                continue
            at = time.strftime("%Y-%m-%dT%H:%M:%S")
            try:
                m = run_method(cfg, method, table, threads, where, timeout_s)
            except duck.MethodTimeout:
                timeouts.add((method, nrows, ncols))
                active.remove(method)
                records.append(_append(out, {"type": "timeout", **case, "method": method, "threads": threads,
                                             "warmup": is_warmup, "limit_s": timeout_s, "at": at}))
                print(f"{READ_SUITE} {table} {method} N={threads}: stopped after {timeout_s} s", flush=True)
                continue
            got = duck_checksums()  # not timed
            if got != expected:
                raise ChecksumMismatch(f"{method} N={threads} {table} run {round_index}: expected {expected}, "
                                       f"got {got}")
            m = replace(m, nrows=nrows, ncols=ncols)
            records.append(_append(out, {"type": "measure", "suite": READ_SUITE, "index": round_index,
                                         "warmup": is_warmup, "at": at, **asdict(m)}))
            print(f"{READ_SUITE} {table} {method} N={threads}{' (warm-up)' if is_warmup else ''}: "
                  f"{m.total_s:.3f} s", flush=True)
    return records
