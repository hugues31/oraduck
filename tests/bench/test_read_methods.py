import pytest

from oraduck_bench import config, read_config, read_methods


CFG = config.OracleConfig("bench", "pw", "host:1521/svc")


def test_attach_sql():
    setup, statements = read_methods.oraduck_sql(CFG, "RD_1000000_50", 8, "PART_DATE = DATE '2024-05-07'")
    assert "SET threads = 8;" in setup and "ATTACH" not in setup
    assert "TYPE oraduck, USER 'bench', PASSWORD 'pw', DSN 'host:1521/svc'" in setup
    assert statements == ["ATTACH '' AS ora (TYPE oraduck, SECRET bench);",
                          "CREATE TABLE t AS SELECT * FROM ora.BENCH.RD_1000000_50 "
                          "WHERE PART_DATE = DATE '2024-05-07';"]


def test_scan_sql():
    setup, statements = read_methods.oraduck_sql(CFG, "RD_1000000_50", 4, "1 = 1", via="scan")
    assert "SET threads = 4;" in setup
    assert len(statements) == 1 and "ATTACH" not in statements[0]
    assert "FROM oraduck_scan('BENCH.RD_1000000_50', connection := 'bench') WHERE 1 = 1" in statements[0]


def test_qualified_table_keeps_its_owner():
    _, statements = read_methods.oraduck_sql(CFG, "APP.ORDERS", 8, "1 = 1")
    assert "FROM ora.APP.ORDERS" in statements[-1]
    _, statements = read_methods.oraduck_sql(CFG, "APP.ORDERS", 8, "1 = 1", via="scan")
    assert "oraduck_scan('APP.ORDERS'" in statements[-1]


def test_method_names():
    assert read_methods.METHODS == ("oraduck", "oraduck_scan")


def test_read_cases_are_the_two_one_million_row_cases():
    from oraduck_bench import read_config
    assert read_config.READ_CASES == ((1_000_000, 50), (1_000_000, 550))


def test_duckdb_errors_hide_passwords():
    from oraduck_bench import duck
    text = ("CREATE SECRET bench (TYPE oraduck, USER 'bench', PASSWORD 'S3cret!', DSN 'h:1521/s');\n"
            "ATTACH 'bench/S3cret!@h:1521/s' AS r (TYPE oracle);")
    out = duck.redact(text)
    assert "S3cret" not in out and "PASSWORD '***'" in out and "bench/***@h" in out


# ---- competitors ----

WHERE = "PART_DATE = DATE '2024-05-07'"


def test_quack_oracle_sql():
    setup, statements = read_methods.quack_oracle_sql(CFG, "RD_1000000_50", 8, WHERE)
    assert "SET threads = 8;" in setup
    assert "INSTALL oracle_scanner FROM community;" in setup and "LOAD oracle_scanner;" in setup
    assert "CREATE SECRET ora (TYPE oracle, HOST 'host', PORT 1521, SERVICE_NAME 'svc', USER 'bench', PASSWORD 'pw');" in setup
    # the timed scope starts at the first contact with Oracle: ATTACH, CTAS (LOAD is untimed, the secret needs it)
    assert statements == ["ATTACH 'ora' AS o (TYPE oracle_scanner);",
                          f"CREATE TABLE t AS SELECT * FROM o.RD_1000000_50 WHERE {WHERE};"]


def test_rinie_oracle_sql(tmp_path):
    setup, statements = read_methods.rinie_oracle_sql(CFG, "RD_1000000_50", 8, WHERE)
    assert setup == f"SET threads = 8;\nLOAD '{config.RINIE_EXTENSION}';"
    assert statements == ["ATTACH 'bench/pw@host:1521/svc' AS r (TYPE oracle);",
                          f"CREATE TABLE t AS SELECT * FROM r.BENCH.RD_1000000_50 WHERE {WHERE};"]
    assert read_methods.RINIE_UNSIGNED


def test_extension_clis():
    from oraduck_bench import competitors
    assert competitors.cli_for("rinie_oracle") == config.DUCKDB15_CLI
    assert competitors.cli_for("quack_oracle") == config.DUCKDB15_CLI  # write side: the 0.2.2 community build


def test_sqlplus_script():
    script = read_methods.sqlplus_script("RD_1000000_50", WHERE, "/w/csv/RD.csv")
    assert "SET MARKUP CSV ON QUOTE ON" in script
    assert script.startswith("WHENEVER SQLERROR EXIT SQL.SQLCODE\nWHENEVER OSERROR EXIT FAILURE")
    assert "SPOOL /w/csv/RD.csv" in script and "SPOOL OFF" in script
    for setting in ("SET ARRAYSIZE 5000", "SET LONG 1000000 LONGCHUNKSIZE 100000", "SET FEEDBACK OFF",
                    "NLS_NUMERIC_CHARACTERS = '.,'", "NLS_DATE_FORMAT", "NLS_TIMESTAMP_FORMAT"):
        assert setting in script
    assert f"SELECT * FROM BENCH.RD_1000000_50 WHERE {WHERE};" in script
    assert "pw" not in script  # the password goes through stdin


def test_sqlplus_read_csv_has_explicit_types():
    sql = read_methods.sqlplus_load_sql("/w/csv/RD.csv", 50)
    assert sql.startswith("CREATE TABLE t AS SELECT * FROM read_csv('/w/csv/RD.csv'")
    assert "header = true" in sql and "'ID': 'BIGINT'" in sql and "'PART_DATE': 'DATE'" in sql
    assert "'D1': 'DECIMAL(25,9)'" in sql and "'CLOB1': 'VARCHAR'" in sql


def test_connectorx_url_and_sql():
    cfg = config.OracleConfig("bench", "Pw@1/x", "10.210.19.10:1521/ORCLPDB1")
    assert read_methods.connectorx_url(CFG) == "oracle://bench:pw@host:1521/svc"
    assert read_methods.connectorx_url(cfg) == "oracle://bench:Pw%401%2Fx@10.210.19.10:1521/ORCLPDB1"
    assert read_methods.python_sql("RD_1_50", WHERE) == f"SELECT * FROM BENCH.RD_1_50 WHERE {WHERE}"


def test_skip_decisions():
    gb = 1 << 30
    assert read_methods.connectorx_skip(5 * gb, 16 * gb) is None
    assert read_methods.connectorx_skip(9 * gb, 16 * gb) == "result does not fit in memory"
    assert read_methods.sqlplus_skip(10 * gb, 100 * gb) is None
    assert read_methods.sqlplus_skip(10 * gb, 14 * gb) == "not enough free disk for the CSV"


def test_series_and_methods():
    assert read_config.READ_COMPETITOR_SERIES == (
        ("oraduck", 8), ("oraduck", 1), ("quack_oracle", 8), ("rinie_oracle", 8), ("oracledb_thin", 8),
        ("oracledb_thick", 8), ("connectorx", 8), ("sqlplus_csv", 8))
    for m in ("quack_oracle", "rinie_oracle", "oracledb_thin", "oracledb_thick", "connectorx", "sqlplus_csv"):
        assert m in read_methods.COMPETITORS and hasattr(read_methods, f"run_{m}")


def test_python_result_line_round_trip():
    from oraduck_bench import read_python_methods
    line = read_python_methods.result_line({"total_s": 1.5, "rows": 3})
    assert read_methods.parse_python_result("noise\n" + line + "\n") == {"total_s": 1.5, "rows": 3}


def test_only_option():
    from oraduck_bench import cli
    assert cli.parse_only(["1000000x50", "1000000x550"]) == [(1_000_000, 50), (1_000_000, 550)]
    with pytest.raises(SystemExit):
        cli.parse_only(["1000000x51"])  # not one of the read cases
    with pytest.raises(SystemExit):
        cli.parse_only(["abc"])


def test_runner_records_skips_and_errors(tmp_path):
    from oraduck_bench import duck, read_runner
    ok = read_schema_checksums = read_runner.read_schema.ReadChecksums(1, 1, 1, 0, 1)

    def run_method(cfg, method, table, threads, where, timeout_s):
        if method == "connectorx":
            return read_methods.Measurement(method, table, 0, 0, threads, 0.0, None, {"skipped": "result does not fit in memory"})
        if method == "rinie_oracle":
            raise duck.DuckDBError("boom")
        return read_methods.Measurement(method, table, 0, 0, threads, 1.0, None, {})

    out = tmp_path / "r.jsonl"
    records = read_runner.run_read_case(
        CFG, 10, 50, ["oraduck", "connectorx", "rinie_oracle"], 1, out, warmup=False, ensure=lambda *a: "RD_10_50",
        run_method=run_method, duck_checksums=lambda: ok, oracle_checksums=lambda *a: ok)
    assert [(r["type"], r["method"]) for r in records] == [
        ("measure", "oraduck"), ("skipped", "connectorx"), ("error", "rinie_oracle")]
    assert records[1]["reason"] == "result does not fit in memory"
    assert "boom" in records[2]["message"]


def test_runner_reports_a_competitor_with_wrong_data_and_raises_for_oraduck(tmp_path):
    from oraduck_bench import read_runner
    ok = read_runner.read_schema.ReadChecksums(1, 1, 1, 0, 1)
    bad = read_runner.read_schema.ReadChecksums(1, 1, 1, 0, 0)

    def run_method(cfg, method, table, threads, where, timeout_s):
        return read_methods.Measurement(method, table, 0, 0, threads, 1.0, None, {})

    kwargs = dict(warmup=False, ensure=lambda *a: "RD_10_50", run_method=run_method,
                  oracle_checksums=lambda *a: ok)
    records = read_runner.run_read_case(CFG, 10, 50, ["rinie_oracle"], 1, tmp_path / "a.jsonl",
                                        duck_checksums=lambda: bad, **kwargs)
    assert records[0]["type"] == "mismatch" and "got" in records[0]["message"]
    with pytest.raises(read_runner.ChecksumMismatch):
        read_runner.run_read_case(CFG, 10, 50, ["oraduck"], 1, tmp_path / "b.jsonl", duck_checksums=lambda: bad,
                                  **kwargs)


def test_redaction_covers_urls_and_literal_secrets():
    from oraduck_bench import duck
    text = "cx failed for oracle://bench:Pw%401%2Fx@10.0.0.1:1521/SVC and Pw@1/x again, user/Pw@1/x@h"
    out = duck.redact(text, ("Pw@1/x",))
    assert "Pw" not in out and "oracle://bench:***@10.0.0.1" in out
    assert "S3cret" not in duck.redact("oracle://u:S3cret@h:1/s")


def test_sessions_are_not_the_thread_setting():
    assert read_methods.sessions_for("oraduck", 8) == 8 and read_methods.sessions_for("oraduck", 1) == 1
    assert read_methods.sessions_for("connectorx", 8) == 8
    for m in ("quack_oracle", "rinie_oracle", "oracledb_thin", "oracledb_thick", "sqlplus_csv"):
        assert read_methods.sessions_for(m, 8) == 1


def test_timed_sum_adds_the_timers(monkeypatch):
    from oraduck_bench import duck
    seen = {}

    def fake_run(sql, *a, **k):
        seen["sql"] = sql
        return "Run Time (s): real 1.5 user 0 sys 0\nRun Time (s): real 2.0 user 0 sys 0\n"

    monkeypatch.setattr(duck, "run", fake_run)
    assert duck.timed_sum("SET x;", ["A;", "B;"], ":memory:") == 3.5
    assert seen["sql"].count(".timer on") == 2 and seen["sql"].startswith("SET x;")
    with pytest.raises(duck.DuckDBError):
        duck.timed_sum("", ["A;", "B;", "C;"], ":memory:")


def test_python_methods_share_the_sql_and_url_code():
    from oraduck_bench import read_python_methods as rpm
    assert rpm.python_sql is read_methods.python_sql and rpm.connectorx_url is read_methods.connectorx_url
    assert rpm.RESULT_PREFIX == read_methods.RESULT_PREFIX


def test_fallback_row_estimate_without_statistics(monkeypatch):
    import oracledb

    from oraduck_bench import oracle

    class Cur:
        def __enter__(self): return self
        def __exit__(self, *a): return False
        def execute(self, *a, **k): pass
        def fetchone(self): return (None,)

    class Conn:
        def cursor(self): return Cur()
        def close(self): pass

    monkeypatch.setattr(oracle, "connect", lambda cfg: Conn())
    assert read_methods._estimated_row_bytes(CFG, "RD_10_50") == 50 * read_methods.FALLBACK_ROW_BYTES_PER_COLUMN

    def boom(cfg):
        raise oracledb.Error("ORA-12541 for bench/pw@h")

    monkeypatch.setattr(oracle, "connect", boom)
    from oraduck_bench import duck
    with pytest.raises(duck.DuckDBError) as err:
        read_methods._estimated_row_bytes(CFG, "RD_10_50")
    assert "pw" not in str(err.value).replace("PASSWORD", "")
