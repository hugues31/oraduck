import json

from oraduck_bench import read_report, read_runner, config, read_schema

TIMED = "first contact with Oracle to the rows stored in DuckDB: ATTACH + CTAS"
SHAPING = {"rate_bps": 1_000_000_000, "container_delay_us": 250, "bridge_delay_us": 250}


def _write(path, rows):
    path.write_text("".join(json.dumps(r) + "\n" for r in rows))


def measure(method, nrows, ncols, total_s, threads=8, warmup=False, link_bytes=None, shaping=None, **extra):
    record = {"type": "measure", "suite": "read", "index": 1, "warmup": warmup, "method": method,
              "table": f"RD_{nrows}_{ncols}", "nrows": nrows, "ncols": ncols, "threads": threads,
              "total_s": total_s, "link_bytes": link_bytes,
              "extra": {"rows": nrows, "db_bytes": 100_000_000, "duckdb": "v2.0.0", "sessions": threads,
                        "timed": TIMED, **extra}}
    if shaping:
        record["shaping"] = shaping
    return record


def rows(oraduck_s=1.0, other_s=2.0):
    out = [{"type": "env", "duckdb": "v2.0.0", "oci_client": "19.32", "oracle": "19.3", "cpu": "cpu",
            "client_affinity": [0, 1], "oracle_cpuset": "8-15", "mem_available_gb": 6.0,
            "competitors": {"quack_oracle": "0.5.1 on DuckDB v2.0.0", "oracledb": "26.0.1"}}]
    for nrows, ncols in ((1_000_000, 50), (1_000_000, 550)):
        out.append(measure("oraduck", nrows, ncols, 9.0, warmup=True))
        for t in (oraduck_s, oraduck_s * 1.1, oraduck_s * 1.2):
            out.append(measure("oraduck", nrows, ncols, t))
        for t in (other_s, other_s * 1.1, other_s * 1.2):
            out.append(measure("quack_oracle", nrows, ncols, t))
        out.append({"type": "planning", "suite": "read", "nrows": nrows, "ncols": ncols, "planning_ms": 100.0})
    out.append(measure("oraduck", 1_000_000, 550, 1.0, link_bytes=125_000_000, shaping=SHAPING))
    return out


def report(tmp_path, data):
    results = tmp_path / "x-read.jsonl"
    _write(results, data)
    return read_report.write_read_report([results], out=tmp_path / "REPORT_READ.md",
                                         analysis=tmp_path / "none.md").read_text()


def test_four_tables_utilisation_and_criteria(tmp_path):
    text = report(tmp_path, rows())
    assert "## Matrix" in text and "## Competitors" in text and "## Shaped to 1 Gbit/s" in text
    assert "## Planning" in text
    assert "100.0 %" in text and "upper bound" in text and "merged into one median" in text
    assert "| OraDuck | 8 | 1,000,000 | 550 | 1.100 | 1.000–1.200 |" in text
    assert "×2.0" in text
    assert "fastest on every case" in text and "met" in text.split("fastest on every case")[1].splitlines()[0]
    assert "## Analysis" in text
    assert (tmp_path / "report_read_competitors.png").exists()
    assert (tmp_path / "report_read_shaped.png").exists()


def test_planning_share(tmp_path):
    text = report(tmp_path, rows())
    # 100 ms of a 1.100 s median: 9.1 %
    assert "9.1 %" in text


def test_criteria_not_met(tmp_path):
    text = report(tmp_path, rows(oraduck_s=3.0, other_s=2.0))
    line = text.split("fastest on every case")[1].splitlines()[0]
    assert "not met" in line


def test_line_rate_not_met_below_90_percent(tmp_path):
    data = rows()
    data[-1]["link_bytes"] = 100_000_000
    text = report(tmp_path, data)
    assert "80.0 %" in text
    assert "not met" in text.split("line rate")[1].splitlines()[0]


def test_other_record_types_are_listed_not_ranked(tmp_path):
    data = rows()
    case = {"suite": "read", "nrows": 1_000_000, "ncols": 550}
    data += [{"type": "mismatch", **case, "method": "connectorx", "threads": 8, "message": "expected 1, got 2",
              "total_s": 0.1, "extra": {}},
             {"type": "skipped", **case, "method": "sqlplus_csv", "threads": 8, "reason": "not enough free disk"},
             {"type": "timeout", **case, "method": "oracledb_thin", "threads": 8, "limit_s": 900},
             {"type": "error", **case, "method": "rinie_oracle", "threads": 8, "message": "boom"}]
    text = report(tmp_path, data)
    assert "incorrect result" in text and "not enough free disk" in text and "boom" in text
    assert "900 s" in text
    assert "| connectorx | 8 | 0.100" not in text
    assert "Correctness" in text and "not met" in text.split("Correctness")[1].splitlines()[0]


def test_scope_sessions_and_versions_stated(tmp_path):
    text = report(tmp_path, rows())
    assert text.count("first contact with Oracle to the rows stored in DuckDB") >= 1
    assert "0.5.1 on DuckDB v2.0.0" in text and "26.0.1" in text


def test_compression_variant_is_separate(tmp_path):
    data = rows()
    rec = measure("oraduck", 1_000_000, 550, 0.5)
    rec["variant"] = "compression"
    data.append(rec)
    text = report(tmp_path, data)
    assert "## SQL*Net compression" in text and "0.500" in text
    assert "1.100" in text  # the local median is not polluted by the compression run


def test_empty_results_do_not_crash(tmp_path):
    assert "OraDuck" in report(tmp_path, [{"type": "env"}])


def test_runner_fills_link_bytes_and_shaping(tmp_path):
    from oraduck_bench.read_methods import Measurement
    sums = read_schema.ReadChecksums(1, 2, 3, 4, 5)
    counter = iter([1000, 126_000_000]).__next__

    def run_method(cfg, method, table, threads, where, timeout_s):
        return Measurement(method, table, 0, 0, threads, 1.0, None, {})

    records = read_runner.run_read_case(
        config.OracleConfig("u", "pw", "h:1/s"), 10, 5, ["oraduck"], 1, tmp_path / "r.jsonl", warmup=False,
        ensure=lambda *a: "RD_10_5", run_method=run_method, duck_checksums=lambda: sums,
        oracle_checksums=lambda *a: sums, link_counter=counter, shaping=SHAPING, variant="compression",
        planning=lambda *a: 12.5)
    assert records[0]["link_bytes"] == 125_999_000
    assert records[0]["shaping"] == SHAPING and records[0]["variant"] == "compression"
    assert records[-1]["type"] == "planning" and records[-1]["planning_ms"] == 12.5
