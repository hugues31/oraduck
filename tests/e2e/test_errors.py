import pytest

from oraduck_helpers import WORKDIR, copy_error, env


def test_missing_table():
    assert "not found" in copy_error("SELECT 1 AS id", "E2E_DOES_NOT_EXIST")


def test_missing_column(table):
    table.create("ID NUMBER")
    assert "does not exist" in copy_error("SELECT 1 AS id, 2 AS other", table.name)


def test_incompatible_type(table):
    table.create("D DATE")
    assert "not compatible" in copy_error("SELECT 'x' AS d", table.name)


def test_unsupported_oracle_column(table):
    table.create("C CLOB")
    assert "not compatible" in copy_error("SELECT 'x' AS c", table.name)


@pytest.mark.parametrize(
    "expr", ["INTERVAL 1 DAY", "[1, 2]", "TIMESTAMPTZ '2024-01-01 00:00:00+00'", "12::HUGEINT"]
)
def test_unsupported_type(table, expr):
    table.create("C VARCHAR2(10)")
    assert "unsupported" in copy_error(f"SELECT {expr} AS c", table.name)


def test_wrong_password(table):
    table.create("ID NUMBER")
    setup = (
        f"CREATE SECRET wrong (TYPE oraduck, USER '{env('ORADUCK_ORACLE_USER')}', PASSWORD 'wrong', "
        f"DSN '{env('ORADUCK_ORACLE_DSN')}');\n"
    )
    assert "ORA-01017" in copy_error("SELECT 1 AS id", table.name, options=", CONNECTION 'wrong'", setup=setup,
                                     secret=False)


def test_missing_not_null_column(table):
    table.create("ID NUMBER NOT NULL, X NUMBER")
    assert "NOT NULL" in copy_error("SELECT 1 AS x", table.name)


def test_null_in_not_null_column_loads_nothing(table):
    table.create("ID NUMBER NOT NULL")
    copy_error("SELECT CASE WHEN i = 3000 THEN NULL ELSE i END AS id FROM range(5000) t(i)", table.name)
    assert table.scalar("COUNT(*)") == 0


def test_value_too_long_loads_nothing(table):
    table.create("ID NUMBER, S VARCHAR2(5)")
    err = copy_error(
        "SELECT i AS id, CASE WHEN i = 4000 THEN 'much too long' ELSE 'ok' END AS s FROM range(10000) t(i)",
        table.name,
    )
    assert "OraDuck" in err
    assert table.scalar("COUNT(*)") == 0


@pytest.mark.parametrize(
    "expr, oracle_type",
    [
        ("DATE '9999-12-31' + 1", "DATE"),
        ("DATE '0001-01-01' - 1", "DATE"),
        ("'infinity'::DATE", "DATE"),
        ("'-infinity'::TIMESTAMP", "TIMESTAMP(6)"),
        ("'infinity'::TIMESTAMP", "DATE"),
    ],
)
def test_date_out_of_range(table, expr, oracle_type):
    table.create(f"ID NUMBER, C {oracle_type}")
    err = copy_error(
        f"SELECT i AS id, CASE WHEN i = 3000 THEN {expr} ELSE DATE '2024-01-01' END AS c FROM range(5000) t(i)",
        table.name,
    )
    assert "outside the Oracle range" in err
    assert table.scalar("COUNT(*)") == 0


@pytest.mark.parametrize("value", ["'nan'::DOUBLE", "'inf'::DOUBLE", "'-inf'::FLOAT"])
def test_non_finite_to_number(table, value):
    table.create("ID NUMBER, X NUMBER")
    err = copy_error(f"SELECT i AS id, CASE WHEN i = 10 THEN {value} ELSE i END AS x FROM range(100) t(i)",
                     table.name)
    assert "NUMBER" in err
    assert table.scalar("COUNT(*)") == 0


def test_local_file_named_like_target(table):
    table.create("ID NUMBER")
    WORKDIR.mkdir(parents=True, exist_ok=True)
    clash = WORKDIR / table.name
    clash.write_text("do not touch")
    try:
        assert "local file" in copy_error("SELECT 1 AS id", table.name)
        assert clash.read_text() == "do not touch"
        assert table.scalar("COUNT(*)") == 0
    finally:
        clash.unlink()


def test_file_options_rejected(table):
    table.create("ID NUMBER")
    try:
        err = copy_error("SELECT 1 AS id", table.name, options=", CONNECTION 'ora', PER_THREAD_OUTPUT true")
        assert "file options" in err
    finally:
        # DuckDB creates the output directory before calling the copy function
        target = WORKDIR / table.name
        if target.is_dir():
            target.rmdir()


def test_invalid_stream_size(table):
    table.create("ID NUMBER")
    assert "STREAM_SIZE" in copy_error("SELECT 1 AS id", table.name, options=", CONNECTION 'ora', STREAM_SIZE 1000")
