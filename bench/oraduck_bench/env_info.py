"""Description of the measurement environment, written at the top of every results file."""

from __future__ import annotations

import os
import subprocess
import time

from . import duck, oracle
from .config import FAKELAKE, ORACLE_CONTAINER, ROOT, SQLLDR_PARAMS, THREADS, OracleConfig


def _run(args: list[str]) -> str:
    proc = subprocess.run(args, capture_output=True, text=True)
    return (proc.stdout + proc.stderr).strip()


def _cpu_model() -> str:
    with open("/proc/cpuinfo") as fh:
        for line in fh:
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return "unknown"


def _mem_available_gb() -> float:
    with open("/proc/meminfo") as fh:
        for line in fh:
            if line.startswith("MemAvailable:"):
                return round(int(line.split()[1]) / 1024 / 1024, 1)
    return 0.0


def collect(cfg: OracleConfig) -> dict:
    conn = oracle.connect(cfg)
    try:
        oracle_version = conn.version
    finally:
        conn.close()
    sqlldr_banner = [line for line in _run(["sqlldr"]).splitlines() if "Release" in line or "Version" in line]
    return {
        "type": "env",
        "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "duckdb": duck.query("SELECT version();", ":memory:")[0][0],
        "oci_client": duck.query("SELECT oraduck_oci_version();", ":memory:")[0][0],
        "sqlldr": " / ".join(sqlldr_banner),
        "oracle": oracle_version,
        "fakelake": _run([str(FAKELAKE), "--version"]),
        "cpu": _cpu_model(),
        "client_affinity": sorted(os.sched_getaffinity(0)),
        "oracle_cpuset": _run(["docker", "inspect", "-f", "{{.HostConfig.CpusetCpus}}", ORACLE_CONTAINER]),
        "git_commit": _run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"]),
        "mem_available_gb": _mem_available_gb(),
        "threads": THREADS,
        "sqlldr_params": list(SQLLDR_PARAMS),
    }
