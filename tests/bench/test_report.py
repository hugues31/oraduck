import json

from oraduck_bench import report


def _write(path, rows):
    path.write_text("".join(json.dumps(r) + "\n" for r in rows))


def test_matrix_report(tmp_path):
    env = {"type": "env", "duckdb": "v1.5.5", "oci_client": "19.32.0.0.0", "oracle": "19.3.0.0.0",
           "sqlldr": "Release 19", "fakelake": "fakelake 1.7.2", "cpu": "cpu", "client_affinity": [0, 1],
           "oracle_cpuset": "8-15", "git_commit": "abc", "mem_available_gb": 8.0, "threads": 8,
           "sqlldr_params": ["direct=true"], "at": "2026-09-23T10:00:00"}
    base = {"type": "measure", "suite": "matrix", "table": "B_100000_5", "nrows": 100000, "ncols": 5, "threads": 8,
            "stream_size": None, "csv_bytes": None, "csv_files": None, "csv_s": None, "load_s": None}
    rows = [env,
            {**base, "method": "oraduck", "warmup": True, "total_s": 5.0, "stream_size": 1048576},
            *[{**base, "method": "oraduck", "warmup": False, "total_s": t, "stream_size": 1048576}
              for t in (1.0, 1.2, 1.1)],
            *[{**base, "method": "sqlldr", "warmup": False, "total_s": t, "csv_s": t / 3, "load_s": 2 * t / 3,
               "csv_bytes": 10_000_000, "csv_files": 8} for t in (3.0, 3.3, 3.1)]]
    results = tmp_path / "x-matrix.jsonl"
    _write(results, rows)
    out = report.write_report([results], out=tmp_path / "REPORT.md")
    text = out.read_text()
    assert "| 100k | 5 | 10.0 | 1.100 | 3.100 | 1.033 | 2.067 | ×2.8 |" in text
    # Throughput in MB/s of CSV equivalent (10 MB), then run spread (min–max)
    assert "| 90,909 | 9.1 | 3.2 | 1.000–1.200 | 3.000–3.300 |" in text
    assert "v1.5.5" in text and "8-15" in text
    assert (tmp_path / "report_matrix.png").exists()


def test_report_includes_analysis_and_run_count(tmp_path):
    results = tmp_path / "x-matrix.jsonl"
    _write(results, [{"type": "env", "sqlldr": "SQL*Loader: Release 19.0.0.0.0 - Production on Thu Sep 24 / Version 19.32.0.0.0"}])
    analysis = tmp_path / "analysis.md"
    analysis.write_text("OraDuck wins everywhere.\n")
    text = report.write_report([results], out=tmp_path / "REPORT.md", analysis=analysis).read_text()
    assert "## Analysis\n\nOraDuck wins everywhere." in text
    assert "Median of 5 runs" in text
    assert "SQL*Loader: Release 19.0.0.0.0 / Version 19.32.0.0.0" in text
    assert "19.30" in text


def test_tuning_notes_missing_interleaving(tmp_path):
    base = {"type": "measure", "suite": "tune", "nrows": 1000000, "ncols": 20, "threads": 8, "method": "oraduck",
            "warmup": False, "csv_s": None, "load_s": None, "csv_bytes": None}
    rows = [{"type": "env"}] + [{**base, "stream_size": size, "total_s": t}
                                for size, t in ((65536, 0.7), (4194304, 0.6))]
    results = tmp_path / "x-tune.jsonl"
    _write(results, rows)
    text = report.write_report([results], out=tmp_path / "REPORT.md", analysis=None).read_text()
    assert "| 4096 KiB | 0.600 |" in text
    assert "not interleaved" in text
