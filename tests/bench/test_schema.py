from oraduck_bench import schema


def test_base_columns():
    assert [c.name for c in schema.columns(5)] == ["id", "amount", "label", "operation_date", "event_time"]


def test_column_cycle():
    cols = schema.columns(50)
    assert len(cols) == 50
    assert cols[5].name == "c06_i32"
    assert cols[13].name == "c14_bool"
    assert cols[14].name == "c15_status"
    assert cols[15].name == "c16_i32"
    assert len(schema.columns(20)) == 20


def test_fewer_than_five_columns_rejected():
    import pytest

    with pytest.raises(ValueError):
        schema.columns(4)


def test_table_name():
    assert schema.table_name(1_000_000, 20) == "B_1000000_20"


def test_fakelake_yaml():
    text = schema.fakelake_yaml(20, 1000, "/w/B_1000_20")
    assert '  - name: id\n    provider: "Increment.integer"\n    start: 1\n' in text
    assert "    presence: 0.95\n" in text
    assert '      - value: "ACTIVE"\n        weight: 6\n' in text
    assert text.endswith('info:\n  output_name: "/w/B_1000_20"\n  output_format: parquet\n  rows: 1000\n  seed: 42\n')


def test_oracle_ddl():
    assert schema.oracle_ddl("B_100_5", 5) == (
        "CREATE TABLE B_100_5 (ID NUMBER(10), AMOUNT NUMBER(15,2), LABEL VARCHAR2(30), "
        "OPERATION_DATE DATE, EVENT_TIME TIMESTAMP(6))"
    )


def test_duckdb_load():
    sql = schema.duckdb_load_sql("B_100_5", "/w/x.parquet", 5)
    assert sql.startswith("CREATE OR REPLACE TABLE B_100_5 AS SELECT CAST(id AS INTEGER) AS id, ")
    # Fakelake writes datetimes as epoch seconds (BIGINT) in the Parquet file
    assert "make_timestamp(event_time * 1000000) AS event_time FROM read_parquet('/w/x.parquet');" in sql
    assert sql.endswith("CHECKPOINT;\n")


def test_csv_export_converts_booleans():
    assert "CAST(c14_bool AS INTEGER) AS c14_bool" in schema.csv_select_sql("B_100_20", 20)


def test_sqlldr_control_file():
    ctl = schema.sqlldr_control("B_100_20", 20)
    assert ctl.startswith("LOAD DATA\nCHARACTERSET AL32UTF8\nAPPEND\nINTO TABLE BENCH.B_100_20\n")
    assert "FIELDS TERMINATED BY ',' OPTIONALLY ENCLOSED BY '\"'\nTRAILING NULLCOLS\n" in ctl
    assert '  EVENT_TIME TIMESTAMP "YYYY-MM-DD HH24:MI:SS.FF6",\n' in ctl
    assert "  C14_BOOL INTEGER EXTERNAL,\n" in ctl
