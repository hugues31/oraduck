from oraduck_bench import read_schema, schema


def test_table_name():
    assert read_schema.read_table_name(1_000_000, 50) == "RD_1000000_50"


def test_columns_start_with_the_keys_and_have_the_requested_width():
    for ncols, clobs in ((50, 1), (550, 2)):
        cols = read_schema.read_columns(ncols)
        assert len(cols) == ncols
        assert [c.name for c in cols[:3]] == ["part_date", "sub_key", "id"]
        assert [c.oracle_type for c in cols[:3]] == ["DATE", "VARCHAR2(8)", "NUMBER(38,0)"]
        assert sum(c.oracle_type == "CLOB" for c in cols) == clobs
        assert len({c.name for c in cols}) == ncols


def test_column_mix_follows_the_target_workload():
    types = {c.oracle_type for c in read_schema.read_columns(50)}
    assert {"VARCHAR2(4000 BYTE)", "NUMBER(38,0)", "NUMBER(25,9)", "DATE", "CHAR(1 BYTE)", "TIMESTAMP(6)",
            "NUMBER", "CLOB"} <= types
    names = {c.name: c.oracle_type for c in read_schema.read_columns(50)}
    assert names["v1"] == "VARCHAR2(4000 BYTE)" and names["d1"] == "NUMBER(25,9)" and names["clob1"] == "CLOB"


def test_oracle_ddl_is_list_partitioned_with_a_subpartition_template():
    ddl = read_schema.read_oracle_ddl(1_000_000, 50)
    assert ddl.startswith("CREATE TABLE RD_1000000_50 (")
    assert 'PARTITION BY LIST ("PART_DATE")' in ddl
    assert 'SUBPARTITION BY LIST ("SUB_KEY")' in ddl
    assert "SUBPARTITION TEMPLATE" in ddl
    for day in ("2024-05-06", "2024-05-07", "2024-05-08"):
        assert f"DATE '{day}'" in ddl
    assert ddl.count("VALUES (DEFAULT)") == 2  # DEFAULT partition and DEFAULT subpartition


def test_checksums_cover_the_five_aggregates():
    assert "length(clob1)" in read_schema.READ_CHECKSUM_DUCK
    assert "LENGTH(CLOB1)" in read_schema.READ_CHECKSUM_ORACLE
    assert "{where}" in read_schema.READ_CHECKSUM_ORACLE


def test_fakelake_yaml_skips_computed_columns():
    cols = read_schema.read_columns(50)
    yaml = schema.fakelake_yaml(50, 10, "x", cols=cols)
    assert "name: v1" in yaml and "name: sub_key" in yaml
    assert "name: id" not in yaml and "name: part_date" not in yaml and "name: clob1" not in yaml


def test_load_sql_places_rows_in_partitions():
    sql = read_schema.read_load_sql("/tmp/x.parquet", 50, 1000)
    assert "file_row_number < 1000 THEN DATE '2024-05-07'" in sql
    assert "file_row_number < 1050 THEN DATE '2024-05-06'" in sql
    assert "file_row_number = true" in sql and "file_row_number + 1 AS BIGINT) AS id" in sql
    assert "ELSE DATE '2024-05-08'" in sql


def test_load_sql_leaves_the_clob_columns_out():
    assert "clob" not in read_schema.read_load_sql("/tmp/x.parquet", 550, 1000)


def test_clob_fill_updates_two_percent_of_the_rows():
    sql = read_schema.clob_fill_sql("RD_1_550", 550)
    assert sql.startswith("UPDATE RD_1_550 SET CLOB1 = ") and ", CLOB2 = " in sql and "MOD(ID, 50) = 0" in sql
    assert "20000" in sql
