from oraduck_bench import config, data, duck, methods, oracle


def test_both_methods_load_identical_data():
    cfg = config.OracleConfig.from_env()
    table = data.ensure_case(10_000, 20)
    expected = duck.checksums(table, config.DUCKDB_FILE)
    other = f"{table}_SQLLDR"
    conn = oracle.connect(cfg)
    try:
        oracle.recreate_table(conn, table, 20)
        oracle.recreate_table(conn, other, 20)

        ora = methods.run_oraduck(cfg, 10_000, 20, 4, config.ORADUCK_STREAM_SIZE)
        assert ora.total_s > 0
        assert oracle.checksums(conn, table) == expected

        ldr = methods.run_sqlldr(cfg, 10_000, 20, 4, oracle_table=other)
        assert ldr.csv_s > 0 and ldr.load_s > 0 and ldr.csv_files >= 1 and ldr.csv_bytes > 0
        assert abs(ldr.total_s - (ldr.csv_s + ldr.load_s)) < 1e-9
        assert oracle.checksums(conn, other) == expected

        # Identical content, column by column, between the two methods
        assert oracle.symmetric_difference(conn, table, other) == 0
    finally:
        oracle.drop_table(conn, table)
        oracle.drop_table(conn, other)
        conn.close()
