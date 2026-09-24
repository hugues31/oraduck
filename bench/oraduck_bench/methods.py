"""The two timed export methods: OraDuck and CSV + SQL*Loader."""

from __future__ import annotations

import shutil
import subprocess
import time
from dataclasses import dataclass

from . import duck, schema
from .config import CSV_ROOT, DUCKDB_FILE, ORACLE_SCHEMA, SQLLDR_PARAMS, OracleConfig

CSV_COPY_OPTIONS = (
    "FORMAT csv, HEADER false, PER_THREAD_OUTPUT true, "
    "DATEFORMAT '%Y-%m-%d', TIMESTAMPFORMAT '%Y-%m-%d %H:%M:%S.%f'"
)


@dataclass
class Measurement:
    method: str  # "oraduck" | "sqlldr"
    table: str
    nrows: int
    ncols: int
    threads: int
    total_s: float
    csv_s: float | None = None
    load_s: float | None = None
    csv_bytes: int | None = None
    csv_files: int | None = None
    stream_size: int | None = None


def _secret(cfg: OracleConfig) -> str:
    return f"CREATE SECRET bench (TYPE oraduck, USER '{cfg.user}', PASSWORD '{cfg.password}', DSN '{cfg.dsn}');"


def run_oraduck(cfg: OracleConfig, nrows: int, ncols: int, threads: int, stream_size: int,
                 oracle_table: str | None = None) -> Measurement:
    table = schema.table_name(nrows, ncols)
    target = f"{ORACLE_SCHEMA}.{oracle_table or table}"
    setup = f"SET threads = {threads};\n{_secret(cfg)}"
    statement = (f"COPY (SELECT * FROM {table}) TO '{target}' "
                 f"(FORMAT oraduck, CONNECTION 'bench', STREAM_SIZE {stream_size});")
    total = duck.timed(setup, statement, DUCKDB_FILE)
    return Measurement("oraduck", table, nrows, ncols, threads, total, stream_size=stream_size)


def run_sqlldr(cfg: OracleConfig, nrows: int, ncols: int, threads: int,
               oracle_table: str | None = None) -> Measurement:
    if shutil.which("sqlldr") is None:
        raise RuntimeError("sqlldr not found: run 'source env.sh'")
    table = schema.table_name(nrows, ncols)
    out_dir = CSV_ROOT / table
    work = CSV_ROOT / f"{table}_sqlldr"
    # Preparation, not timed
    shutil.rmtree(out_dir, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    control = work / "load.ctl"
    control.write_text(schema.sqlldr_control(oracle_table or table, ncols))

    # 1) CSV export: N files (timed)
    csv_s = duck.timed(
        f"SET threads = {threads};",
        f"COPY ({schema.csv_select_sql(table, ncols)}) TO '{out_dir}' ({CSV_COPY_OPTIONS});",
        DUCKDB_FILE,
    )
    files = sorted(p for p in out_dir.glob("*.csv") if p.stat().st_size > 0)
    csv_bytes = sum(p.stat().st_size for p in files)

    # 2) N concurrent sqlldr processes, from launch until the last one exits (timed)
    start = time.perf_counter()
    procs = []
    for i, data_file in enumerate(files):
        args = [
            "sqlldr",
            f"userid={cfg.sqlldr_userid}",
            f"control={control}",
            f"data={data_file}",
            f"log={work / f'load_{i}.log'}",
            f"bad={work / f'load_{i}.bad'}",
            *SQLLDR_PARAMS,
        ]
        procs.append(subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=work))
    codes = [p.wait() for p in procs]
    load_s = time.perf_counter() - start

    failed = [i for i, code in enumerate(codes) if code != 0]
    if failed:
        log = (work / f"load_{failed[0]}.log").read_text(errors="replace")[-3000:]
        raise RuntimeError(f"sqlldr failed ({len(failed)}/{len(procs)} processes, exit code {codes[failed[0]]}):\n{log}")
    # Cleanup, not timed
    shutil.rmtree(out_dir, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    return Measurement("sqlldr", table, nrows, ncols, threads, csv_s + load_s, csv_s=csv_s, load_s=load_s,
                       csv_bytes=csv_bytes, csv_files=len(files))
