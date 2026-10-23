from oraduck_bench import config, read_methods


CFG = config.OracleConfig("bench", "pw", "host:1521/svc")


def test_attach_sql():
    setup, statement = read_methods.oraduck_sql(CFG, "RD_1000000_50", 8, "PART_DATE = DATE '2024-05-07'")
    assert "SET threads = 8;" in setup
    assert "ATTACH '' AS ora (TYPE oraduck, SECRET bench)" in setup
    assert "TYPE oraduck, USER 'bench', PASSWORD 'pw', DSN 'host:1521/svc'" in setup
    assert statement == ("CREATE TABLE t AS SELECT * FROM ora.BENCH.RD_1000000_50 "
                         "WHERE PART_DATE = DATE '2024-05-07';")


def test_scan_sql():
    setup, statement = read_methods.oraduck_sql(CFG, "RD_1000000_50", 4, "1 = 1", via="scan")
    assert "SET threads = 4;" in setup and "ATTACH" not in setup
    assert "FROM oraduck_scan('BENCH.RD_1000000_50', connection := 'bench') WHERE 1 = 1" in statement


def test_qualified_table_keeps_its_owner():
    _, statement = read_methods.oraduck_sql(CFG, "APP.ORDERS", 8, "1 = 1")
    assert "FROM ora.APP.ORDERS" in statement
    _, statement = read_methods.oraduck_sql(CFG, "APP.ORDERS", 8, "1 = 1", via="scan")
    assert "oraduck_scan('APP.ORDERS'" in statement


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
