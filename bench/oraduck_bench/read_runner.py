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


def duck_checksums_cast() -> read_schema.ReadChecksums:
    row = duck.query(read_schema.READ_CHECKSUM_DUCK_CAST.format(table="t"), READ_OUT_DB)[0]
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
    duck_checksums_cast: Callable = duck_checksums_cast,
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
    try:
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
                except duck.DuckDBError as exc:  # a competitor that cannot do the case; OraDuck failing is a bug
                    if method.startswith("oraduck"):
                        raise
                    active.remove(method)
                    records.append(_append(out, {"type": "error", **case, "method": method, "threads": threads,
                                                 "warmup": is_warmup, "message": str(exc)[:2000], "at": at}))
                    print(f"{READ_SUITE} {table} {method} N={threads}: failed: {str(exc)[:300]}", flush=True)
                    continue
                if m.extra.get("skipped"):
                    active.remove(method)
                    records.append(_append(out, {"type": "skipped", **case, "method": method, "threads": threads,
                                                 "reason": m.extra["skipped"], "at": at}))
                    print(f"{READ_SUITE} {table} {method} N={threads}: skipped ({m.extra['skipped']})", flush=True)
                    continue
                cast = False
                try:
                    got = duck_checksums()  # not timed
                except (duck.DuckDBError, ValueError):  # no sum on text, or a float sum
                    if method.startswith("oraduck"):
                        raise
                    got, cast = duck_checksums_cast(), True  # the method loaded a column as another type
                if got != expected:
                    message = (f"{method} N={threads} {table} run {round_index}: expected {expected}, got {got}")
                    if method.startswith("oraduck"):
                        raise ChecksumMismatch(message)
                    # a competitor that loads other data is reported as such, not timed
                    active.remove(method)
                    records.append(_append(out, {"type": "mismatch", **case, "method": method, "threads": threads,
                                                 "warmup": is_warmup, "message": message, "total_s": m.total_s,
                                                 "extra": m.extra, "at": at}))
                    print(f"{READ_SUITE} {table} {method} N={threads}: wrong data, not timed ({message})", flush=True)
                    continue
                m = replace(m, nrows=nrows, ncols=ncols)
                records.append(_append(out, {"type": "measure", "suite": READ_SUITE, "index": round_index,
                                             "warmup": is_warmup, "at": at, "checksum_cast": cast, **asdict(m)}))
                print(f"{READ_SUITE} {table} {method} N={threads}{' (warm-up)' if is_warmup else ''}: "
                      f"{m.total_s:.3f} s{' (checksums on casts)' if cast else ''}", flush=True)
    finally:
        read_methods.reset_out_db()
    return records
