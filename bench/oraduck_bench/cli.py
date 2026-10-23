"""Entry point: oraduck-bench {smoke,tune,scaling,matrix,competitors,report,read-smoke,read-matrix,read-external}."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from . import read_runner, runner
from .config import (COLS, RESULTS, ROWS, SCALING_CASE, SCALING_THREADS, THREADS, TUNE_STREAM_SIZES,
                     OracleConfig)
from .read_config import READ_CASES, READ_RUNS


def _check_affinity() -> None:
    current = os.sched_getaffinity(0)
    if current != set(range(8)):
        raise SystemExit(f"CPU affinity {sorted(current)}: run under `taskset -c 0-7 uv run oraduck-bench ...`")


def _read_external(args: argparse.Namespace) -> int:
    """OraDuck alone on an existing table: no data generation, no checksums."""
    import statistics

    from . import read_methods

    password = os.environ.get("ORADUCK_EXTERNAL_PASSWORD")
    if not password:
        raise SystemExit("ORADUCK_EXTERNAL_PASSWORD is not set")
    cfg = OracleConfig(args.user, password, args.dsn)
    runs = [read_methods.run_oraduck(cfg, args.table, 8, args.where) for _ in range(args.runs)]
    times = [m.total_s for m in runs]
    rows, size = runs[-1].extra["rows"], runs[-1].extra["db_bytes"]
    median = statistics.median(times)
    print(f"runs: {', '.join(f'{t:.3f}' for t in times)} s")
    print(f"median {median:.3f} s: {rows / median:,.0f} rows/s, {size / median / 1e6:,.1f} MB/s of DuckDB storage "
          f"({rows:,} rows, {size / 1e6:,.0f} MB)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="oraduck-bench")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("smoke", "tune", "scaling", "matrix", "competitors", "read-smoke", "read-matrix"):
        sub.add_parser(name)
    external = sub.add_parser("read-external")
    external.add_argument("--dsn", required=True)
    external.add_argument("--user", required=True)
    external.add_argument("--table", required=True, help="OWNER.TABLE")
    external.add_argument("--where", required=True, help="SQL predicate of the partition to read")
    external.add_argument("--runs", type=int, default=3)
    report_parser = sub.add_parser("report")
    report_parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)

    if args.command == "report":
        from . import report

        print(report.write_report(args.files or sorted(RESULTS.glob("*.jsonl"))))
        return 0

    if args.command == "read-external":
        return _read_external(args)

    _check_affinity()
    cfg = OracleConfig.from_env()
    out = runner.new_results_file(cfg, args.command)
    if args.command == "smoke":
        runner.run_case(cfg, 100_000, 5, THREADS, "smoke", out, runs=1, warmup=False)
    elif args.command == "tune":
        for size in TUNE_STREAM_SIZES:
            runner.run_case(cfg, *SCALING_CASE, THREADS, "tune", out, methods_order=("oraduck",), stream_size=size)
    elif args.command == "scaling":
        for threads in SCALING_THREADS:
            runner.run_case(cfg, *SCALING_CASE, threads, "scaling", out)
    elif args.command == "matrix":
        for nrows in ROWS:
            for ncols in COLS:
                runner.run_case(cfg, nrows, ncols, THREADS, "matrix", out)
    elif args.command == "competitors":
        timeouts: set[tuple[str, int, int]] = set()
        for nrows in ROWS:
            for ncols in COLS:
                runner.run_competitor_case(cfg, nrows, ncols, out, timeouts)
    elif args.command == "read-smoke":
        records = read_runner.run_read_case(cfg, 1_000_000, 50, ["oraduck"], 1, out, warmup=False)
        print(f"{records[0]['method']}: {records[0]['total_s']:.3f} s\nchecksums OK")
    elif args.command == "read-matrix":
        timeouts: set[tuple[str, int, int]] = set()
        for nrows, ncols in READ_CASES:
            methods = ["oraduck", "oraduck_scan"] if (nrows, ncols) == (1_000_000, 550) else ["oraduck"]
            read_runner.run_read_case(cfg, nrows, ncols, methods, READ_RUNS, out, timeouts=timeouts)
    print(f"results: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
