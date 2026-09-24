import json

import pytest

from oraduck_bench import competitors, config, data, duck, oracle, report


def test_dsn_parts():
    cfg = config.OracleConfig("u", "p", "dbhost:1522/SVC1")
    assert (cfg.host, cfg.port, cfg.service) == ("dbhost", 1522, "SVC1")


def test_source_is_an_in_memory_copy_except_for_the_largest_case():
    setup, ref = competitors.source_sql("B_1000000_20", 1_000_000, 20)
    assert f"ATTACH '{config.DUCKDB_FILE}' AS src (READ_ONLY);" in setup
    assert "CREATE TABLE src_mem AS SELECT * FROM src.B_1000000_20;" in setup
    assert ref == "src_mem"
    setup, ref = competitors.source_sql("B_10000000_50", 10_000_000, 50)
    assert "CREATE TABLE" not in setup
    assert ref == "src.B_10000000_50"


def test_a_timeout_skips_larger_cases_for_that_method_only():
    skips = {("rinie_oracle", 100_000, 50)}
    assert competitors.should_skip(skips, "rinie_oracle", 1_000_000, 50)
    assert competitors.should_skip(skips, "rinie_oracle", 100_000, 50)
    assert not competitors.should_skip(skips, "rinie_oracle", 1_000_000, 20)
    assert not competitors.should_skip(skips, "quack_oracle", 1_000_000, 50)


def test_statements():
    cfg = config.OracleConfig("bench", "pw", "h:1521/S")
    _, stmt, _ = competitors.method_sql("quack_oracle", cfg, "B_1_5", "src_mem", 8)
    assert stmt == "INSERT INTO o.B_1_5 SELECT * FROM src_mem;"
    setup, stmt, unsigned = competitors.method_sql("rinie_oracle", cfg, "B_1_5", "src_mem", 8)
    assert "ATTACH 'bench/pw@h:1521/S' AS r (TYPE oracle);" in setup
    assert stmt == "INSERT INTO r.BENCH.B_1_5 SELECT * FROM src_mem;"
    assert unsigned
    setup, stmt, _ = competitors.method_sql("oraduck", cfg, "B_1_5", "src_mem", 1)
    assert "SET threads = 1;" in setup
    assert stmt.startswith("COPY (SELECT * FROM src_mem) TO 'BENCH.B_1_5' (FORMAT oraduck, CONNECTION 'bench'")


def test_competitor_report_section(tmp_path):
    base = {"type": "measure", "suite": "competitors", "table": "B_100000_5", "nrows": 100000, "ncols": 5,
            "warmup": False, "stream_size": None, "csv_s": None, "load_s": None, "csv_bytes": None}
    rows = [{"type": "env", "competitors": {"quack_oracle": "0.2.2", "rinie_oracle": "20d4389"}},
            *[{**base, "method": "oraduck", "threads": 8, "total_s": t} for t in (0.3, 0.4, 0.2)],
            *[{**base, "method": "oraduck", "threads": 1, "total_s": t} for t in (0.5, 0.6, 0.4)],
            *[{**base, "method": "quack_oracle", "threads": 8, "total_s": t} for t in (60.0, 61.0, 59.0)],
            {"type": "timeout", "suite": "competitors", "method": "rinie_oracle", "threads": 8, "nrows": 100000,
             "ncols": 5, "limit_s": 900},
            {"type": "skipped", "suite": "competitors", "method": "rinie_oracle", "threads": 8, "nrows": 1000000,
             "ncols": 5}]
    results = tmp_path / "x-competitors.jsonl"
    results.write_text("".join(json.dumps(r) + "\n" for r in rows))
    text = report.write_report([results], out=tmp_path / "REPORT.md", analysis=None).read_text()
    assert "## Comparison with other DuckDB → Oracle extensions" in text
    assert "| 100k | 5 | 0.300 | 0.500 | 60.000 | > 15 min | ×200 | > ×3000 |" in text
    assert "| 1M | 5 | — | — | — | not run |" in text
    assert "`oracle_scanner` 0.2.2" in text and "commit 20d4389" in text
    # computed from the medians, not written by hand
    assert "With a single session (N = 1), OraDuck is 120 times faster than quack-oracle." in text
    assert (tmp_path / "report_competitors.png").exists()


def test_the_three_methods_load_identical_data():
    cfg = config.OracleConfig.from_env()
    table = data.ensure_case(10_000, 5)
    expected = duck.checksums(table, config.DUCKDB_FILE)
    conn = oracle.connect(cfg)
    try:
        loaded = {}
        for method, threads in (("oraduck", 8), ("quack_oracle", 8), ("rinie_oracle", 8)):
            target = f"{table}_{method.upper()[:5]}"
            oracle.recreate_table(conn, target, 5)
            m = competitors.run(cfg, method, 10_000, 5, threads, timeout_s=600, oracle_table=target)
            assert m.total_s > 0
            assert oracle.checksums(conn, target) == expected
            loaded[method] = target
        assert oracle.symmetric_difference(conn, loaded["oraduck"], loaded["quack_oracle"]) == 0
        assert oracle.symmetric_difference(conn, loaded["oraduck"], loaded["rinie_oracle"]) == 0
    finally:
        for target in (f"{table}_{m.upper()[:5]}" for m in ("oraduck", "quack_oracle", "rinie_oracle")):
            oracle.drop_table(conn, target)
        conn.close()


def test_a_run_over_the_limit_times_out_and_the_table_stays_usable():
    cfg = config.OracleConfig.from_env()
    table = data.ensure_case(10_000, 5)
    conn = oracle.connect(cfg)
    try:
        oracle.recreate_table(conn, table, 5)
        with pytest.raises(competitors.MethodTimeout):
            competitors.run(cfg, "rinie_oracle", 10_000, 5, 8, timeout_s=3)
        oracle.truncate(conn, table)  # waits for the killed session to release its lock
        assert oracle.checksums(conn, table).rows == 0
    finally:
        oracle.drop_table(conn, table)
        conn.close()
