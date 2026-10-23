"""Entry point: oraduck-bench {smoke,tune,scaling,matrix,competitors,report,read-smoke,read-matrix,read-competitors,read-shaped,read-compression,read-report,read-external}."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
from pathlib import Path

from . import netlink, read_methods, read_runner, runner
from .config import (COLS, RESULTS, ROWS, SCALING_CASE, SCALING_THREADS, THREADS, TUNE_STREAM_SIZES,
                     OracleConfig)
from .read_config import READ_CASES, READ_COMPETITOR_RUNS, READ_COMPETITOR_SERIES, READ_RUNS

SHAPED_CASE = (1_000_000, 550)


TABLE_NAME = re.compile(r"^[A-Za-z0-9_$#]+(\.[A-Za-z0-9_$#]+)?$")


def parse_only(values: list[str]) -> list[tuple[int, int]]:
    """`--only 1000000x50` (repeatable): read cases to run; default all of them."""
    cases = []
    for value in values:
        match = re.fullmatch(r"(\d+)x(\d+)", value)
        if not match or (int(match[1]), int(match[2])) not in READ_CASES:
            raise SystemExit(f"--only {value!r}: expected ROWSxCOLS, one of "
                             f"{', '.join(f'{r}x{c}' for r, c in READ_CASES)}")
        cases.append((int(match[1]), int(match[2])))
    return cases


def _check_affinity() -> None:
    current = os.sched_getaffinity(0)
    if current != set(range(8)):
        raise SystemExit(f"CPU affinity {sorted(current)}: run under `taskset -c 0-7 uv run oraduck-bench ...`")


def _read_external(args: argparse.Namespace) -> int:
    """OraDuck alone on an existing table: no data generation, no checksums."""
    import statistics

    from . import read_methods

    if not TABLE_NAME.match(args.table):
        raise SystemExit(f"--table {args.table!r}: expected OWNER.TABLE with letters, digits, _, $ and #")
    if args.runs < 1:
        raise SystemExit("--runs must be at least 1")
    password = os.environ.get("ORADUCK_EXTERNAL_PASSWORD")
    if not password:
        raise SystemExit("ORADUCK_EXTERNAL_PASSWORD is not set")
    cfg = OracleConfig(args.user, password, args.dsn)
    try:
        runs = [read_methods.run_oraduck(cfg, args.table, 8, args.where) for _ in range(args.runs)]
    finally:
        read_methods.reset_out_db()
    times = [m.total_s for m in runs]
    rows, size = runs[-1].extra["rows"], runs[-1].extra["db_bytes"]
    median = statistics.median(times)
    print(f"runs: {', '.join(f'{t:.3f}' for t in times)} s")
    print(f"median {median:.3f} s: {rows / median:,.0f} rows/s, {size / median / 1e6:,.1f} MB/s of DuckDB storage "
          f"({rows:,} rows, {size / 1e6:,.0f} MB)")
    return 0


def compression_dsn(dsn: str) -> str:
    """The same Oracle service as a connect descriptor with SQL*Net compression."""
    host_port, service = dsn.split("/", 1)
    host, port = host_port.rsplit(":", 1)
    return (f"(DESCRIPTION=(COMPRESSION=on)(ADDRESS=(PROTOCOL=TCP)(HOST={host})(PORT={port}))"
            f"(CONNECT_DATA=(SERVICE_NAME={service})))")


def _link_counter():
    """Bytes received on the Oracle bridge, or None when the bridge cannot be read (no Docker)."""
    try:
        name = netlink.bridge_name()
        netlink.bridge_rx_bytes(name)
    except (OSError, ValueError, FileNotFoundError, subprocess.CalledProcessError):
        return None
    return lambda: netlink.bridge_rx_bytes(name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="oraduck-bench")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("smoke", "tune", "scaling", "matrix", "competitors", "read-smoke", "read-compression"):
        sub.add_parser(name)
    for name in ("read-matrix", "read-competitors", "read-shaped"):
        read_parser = sub.add_parser(name)
        read_parser.add_argument("--only", action="append", default=[], metavar="ROWSxCOLS",
                                 help="run only this case (repeatable)")
        read_parser.add_argument("--runs", type=int, default=None, help="measured runs per method")
    external = sub.add_parser("read-external")
    external.add_argument("--dsn", required=True)
    external.add_argument("--user", required=True)
    external.add_argument("--table", required=True, help="OWNER.TABLE (plain identifiers only)")
    external.add_argument("--where", required=True, help="SQL predicate of the partition to read, used as given (not validated, by design)")
    external.add_argument("--runs", type=int, default=3)
    report_parser = sub.add_parser("report")
    report_parser.add_argument("files", nargs="*", type=Path)
    read_report_parser = sub.add_parser("read-report")
    read_report_parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)

    if args.command == "report":
        from . import report

        print(report.write_report(args.files or sorted(RESULTS.glob("*.jsonl"))))
        return 0

    if args.command == "read-report":
        from . import read_report

        print(read_report.write_read_report(args.files or sorted(RESULTS.glob("*read*.jsonl"))))
        return 0

    if args.command == "read-external":
        return _read_external(args)

    if getattr(args, "runs", None) is not None and args.runs < 1:
        raise SystemExit("--runs must be at least 1")
    only = parse_only(args.only) if hasattr(args, "only") else []
    shaping = None
    if args.command == "read-shaped":
        shaping = netlink.shaping_status()
        if not netlink.is_line_rate_shaped(shaping):
            raise SystemExit(f"the link is not shaped to 1 Gbit/s ({shaping}): run `sudo ./infra/net/shape.sh on` "
                             "and check `./infra/net/shape.sh status`")
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
        for nrows, ncols in only or READ_CASES:
            methods = ["oraduck", "oraduck_scan"] if (nrows, ncols) == (1_000_000, 550) else ["oraduck"]
            read_runner.run_read_case(cfg, nrows, ncols, methods, args.runs or READ_RUNS, out, timeouts=timeouts,
                                      link_counter=_link_counter(), planning=read_methods.explain_planning)
    elif args.command == "read-competitors":
        timeouts: set[tuple[str, int, int]] = set()
        for nrows, ncols in only or READ_CASES:
            for threads in dict.fromkeys(t for _, t in READ_COMPETITOR_SERIES):  # one interleaved group per N
                methods = [m for m, t in READ_COMPETITOR_SERIES if t == threads]
                read_runner.run_read_case(cfg, nrows, ncols, methods, args.runs or READ_COMPETITOR_RUNS, out,
                                          threads=threads, timeouts=timeouts, link_counter=_link_counter())
    elif args.command == "read-shaped":
        timeouts = set()
        counter = _link_counter()
        if counter is None:
            raise SystemExit("cannot read the Oracle bridge statistics (is Docker running?)")
        for threads in dict.fromkeys(t for _, t in READ_COMPETITOR_SERIES):
            methods = [m for m, t in READ_COMPETITOR_SERIES if t == threads]
            read_runner.run_read_case(cfg, *SHAPED_CASE, methods, args.runs or READ_COMPETITOR_RUNS, out,
                                      threads=threads, timeouts=timeouts, link_counter=counter, shaping=shaping)
    elif args.command == "read-compression":
        compressed = OracleConfig(cfg.user, cfg.password, compression_dsn(cfg.dsn))
        read_runner.run_read_case(compressed, *SHAPED_CASE, ["oraduck"], READ_RUNS, out, link_counter=_link_counter(),
                                  variant="compression")
    print(f"results: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
