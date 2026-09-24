from oraduck_bench import config, data, duck


def test_generation_and_loading():
    table = data.ensure_case(1000, 20)
    assert table == "B_1000_20"
    assert data.table_exists(table)
    rows = duck.query(
        f"SELECT count(*), count(*) - count(amount), min(id), max(id) FROM {table};", config.DUCKDB_FILE
    )
    total, nulls, first, last = (int(v) for v in rows[0])
    assert total == 1000
    assert 10 <= nulls <= 100  # about 5 % NULLs
    assert (first, last) == (1, 1000)
    types = dict(duck.query(f"SELECT column_name, data_type FROM information_schema.columns "
                            f"WHERE table_name = '{table}';", config.DUCKDB_FILE))
    assert types["amount"] == "DECIMAL(15,2)"
    assert types["event_time"] == "TIMESTAMP"
    assert types["c14_bool"] == "BOOLEAN"


def test_timer_measurement():
    seconds = duck.timed("SELECT 1;", "SELECT sum(i) FROM range(10000000) t(i);", ":memory:")
    assert 0 < seconds < 60


def test_checksums():
    table = data.ensure_case(1000, 20)
    c = duck.checksums(table, config.DUCKDB_FILE)
    assert c.rows == 1000
    assert c.sum_id == 500500
