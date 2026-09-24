"""Entry point: oraduck-bench {smoke,tune,scaling,matrix,competitors,report}."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from . import runner
from .config import (COLS, RESULTS, ROWS, SCALING_CASE, SCALING_THREADS, THREADS, TUNE_STREAM_SIZES,
                     OracleConfig)


def _check_affinity() -> None:
    current = os.sched_getaffinity(0)
    if current != set(range(8)):
        raise SystemExit(f"CPU affinity {sorted(current)}: run under `taskset -c 0-7 uv run oraduck-bench ...`")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="oraduck-bench")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("smoke", "tune", "scaling", "matrix", "competitors"):
        sub.add_parser(name)
    report_parser = sub.add_parser("report")
    report_parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)

    if args.command == "report":
        from . import report

        print(report.write_report(args.files or sorted(RESULTS.glob("*.jsonl"))))
        return 0

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
    print(f"results: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
