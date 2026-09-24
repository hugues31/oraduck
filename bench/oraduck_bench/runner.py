"""Protocol: warm-up, interleaved runs, checks, JSONL results."""

from __future__ import annotations

import json
import time
from dataclasses import asdict
from pathlib import Path

from . import competitors, data, duck, env_info, methods, oracle
from .config import (COMPETITOR_RUNS, COMPETITOR_SERIES, COMPETITOR_TIMEOUT_S, ORADUCK_STREAM_SIZE, DUCKDB_FILE,
                     RESULTS, RUNS, OracleConfig)


class ChecksumMismatch(RuntimeError):
    pass


def new_results_file(cfg: OracleConfig, suite: str) -> Path:
    RESULTS.mkdir(parents=True, exist_ok=True)
    path = RESULTS / f"{time.strftime('%Y%m%d-%H%M%S')}-{suite}.jsonl"
    env = env_info.collect(cfg)
    if suite == "competitors":
        env["competitors"] = competitors.versions()
    path.write_text(json.dumps(env) + "\n")
    return path


def _append(out: Path, record: dict) -> dict:
    with out.open("a") as fh:
        fh.write(json.dumps(record) + "\n")
    return record


def run_case(
    cfg: OracleConfig,
    nrows: int,
    ncols: int,
    threads: int,
    suite: str,
    out: Path,
    methods_order: tuple[str, ...] = ("oraduck", "sqlldr"),
    runs: int = RUNS,
    warmup: bool = True,
    stream_size: int = ORADUCK_STREAM_SIZE,
) -> list[dict]:
    table = data.ensure_case(nrows, ncols)
    expected = duck.checksums(table, DUCKDB_FILE)
    plan = [(m, True) for m in methods_order] if warmup else []
    for _ in range(runs):
        plan += [(m, False) for m in methods_order]
    conn = oracle.connect(cfg)
    records = []
    try:
        oracle.recreate_table(conn, table, ncols)
        for index, (method, is_warmup) in enumerate(plan):
            oracle.truncate(conn, table)  # not timed
            if method == "oraduck":
                m = methods.run_oraduck(cfg, nrows, ncols, threads, stream_size)
            else:
                m = methods.run_sqlldr(cfg, nrows, ncols, threads)
            got = oracle.checksums(conn, table)  # not timed
            if got != expected:
                raise ChecksumMismatch(f"{method} {table} run {index}: expected {expected}, got {got}")
            record = {"type": "measure", "suite": suite, "index": index, "warmup": is_warmup,
                      "at": time.strftime("%Y-%m-%dT%H:%M:%S"), **asdict(m)}
            with out.open("a") as fh:
                fh.write(json.dumps(record) + "\n")
            print(f"{suite} {table} N={threads} {method}{' (warm-up)' if is_warmup else ''}: {m.total_s:.3f} s",
                  flush=True)
            records.append(record)
    finally:
        oracle.drop_table(conn, table)  # frees the tablespace
        conn.close()
    return records


def run_competitor_case(
    cfg: OracleConfig,
    nrows: int,
    ncols: int,
    out: Path,
    timeouts: set[tuple[str, int, int]],
    series: tuple[tuple[str, int], ...] = COMPETITOR_SERIES,
    runs: int = COMPETITOR_RUNS,
    timeout_s: float = COMPETITOR_TIMEOUT_S,
) -> list[dict]:
    """Warm-up round then `runs` interleaved rounds. A method that exceeds timeout_s is added to
    `timeouts` and is no longer run, on this case and on cases at least as large."""
    table = data.ensure_case(nrows, ncols)
    expected = duck.checksums(table, DUCKDB_FILE)
    case = {"suite": "competitors", "nrows": nrows, "ncols": ncols}
    records = []
    active = []
    for method, threads in series:
        if competitors.should_skip(timeouts, method, nrows, ncols):
            records.append(_append(out, {"type": "skipped", **case, "method": method, "threads": threads}))
            print(f"competitors {table} {method} N={threads}: not run", flush=True)
        else:
            active.append((method, threads))
    conn = oracle.connect(cfg)
    try:
        oracle.recreate_table(conn, table, ncols)
        for round_index in range(runs + 1):
            is_warmup = round_index == 0
            for method, threads in list(active):
                if (method, threads) not in active:
                    continue
                oracle.truncate(conn, table)  # not timed; waits for a killed session to release its lock
                at = time.strftime("%Y-%m-%dT%H:%M:%S")
                try:
                    m = competitors.run(cfg, method, nrows, ncols, threads, timeout_s=timeout_s)
                except competitors.MethodTimeout:
                    timeouts.add((method, nrows, ncols))
                    active = [s for s in active if s[0] != method]
                    records.append(_append(out, {"type": "timeout", **case, "method": method, "threads": threads,
                                                 "warmup": is_warmup, "limit_s": timeout_s, "at": at}))
                    print(f"competitors {table} {method} N={threads}: stopped after {timeout_s} s", flush=True)
                    continue
                got = oracle.checksums(conn, table)  # not timed
                if got != expected:
                    raise ChecksumMismatch(f"{method} N={threads} {table}: expected {expected}, got {got}")
                records.append(_append(out, {"type": "measure", "suite": "competitors", "index": round_index,
                                             "warmup": is_warmup, "at": at, **asdict(m)}))
                print(f"competitors {table} {method} N={threads}{' (warm-up)' if is_warmup else ''}: "
                      f"{m.total_s:.3f} s", flush=True)
    finally:
        oracle.drop_table(conn, table)
        conn.close()
    return records
