import uuid

from oraduck_helpers import OracleTable, copy_ok, env, run_duckdb, secret_sql


def test_empty_source(table):
    table.create("ID NUMBER")
    copy_ok("SELECT 1 AS id WHERE false", table.name)
    assert table.scalar("COUNT(*)") == 0


def test_inline_and_dictionary_strings(table, tmp_path):
    table.create("ID NUMBER, S VARCHAR2(64), K VARCHAR2(64)")
    db = str(tmp_path / "src.duckdb")
    setup = (
        "CREATE TABLE src AS SELECT i AS id, repeat('x', (i % 40) + 1) AS s, "
        "CASE i % 3 WHEN 0 THEN 'short' WHEN 1 THEN 'a_value_longer_than_twelve_bytes' ELSE 'other' END AS k "
        "FROM range(20000) t(i);\nCHECKPOINT;\n"
    )
    assert run_duckdb(setup, db).returncode == 0
    # New process: the scan reads compressed segments (dictionary vectors)
    copy_ok("SELECT id, s, k FROM src", table.name, db=db)
    labels = ["short", "a_value_longer_than_twelve_bytes", "other"]
    assert table.rows("ID, S, K", order_by="ID") == [
        (i, "x" * ((i % 40) + 1), labels[i % 3]) for i in range(20000)
    ]


def test_constant_vector(table):
    table.create("ID NUMBER, S VARCHAR2(64), C VARCHAR2(8)")
    copy_ok("SELECT i AS id, 'constant_longer_than_twelve' AS s, 'short' AS c FROM range(5000) t(i)", table.name)
    assert table.scalar("COUNT(*)") == 5000
    assert table.scalar("COUNT(DISTINCT S) || '/' || MIN(S) || '/' || MIN(C)") == \
        "1/constant_longer_than_twelve/short"


def test_mixed_case_identifiers(oracle):
    name = f'"Mixed_{uuid.uuid4().hex[:8]}"'
    t = OracleTable(oracle, name)
    t.create('"MyCol" NUMBER, "Other" VARCHAR2(10)')
    try:
        copy_ok("SELECT 1 AS \"MyCol\", 'a' AS \"Other\"", name)
        assert t.rows('"MyCol", "Other"') == [(1, "a")]
    finally:
        t.drop()


def test_lower_case_target_with_schema(table):
    table.create("ID NUMBER")
    copy_ok("SELECT 42 AS id", f"{env('ORADUCK_ORACLE_USER').lower()}.{table.name.lower()}")
    assert table.scalar("SUM(ID)") == 42


def test_case_insensitive_columns(table):
    table.create("LABEL VARCHAR2(10)")
    copy_ok("SELECT 'ok' AS \"Label\"", table.name)
    assert table.scalar("MIN(LABEL)") == "ok"


def test_row_wider_than_stream(table):
    table.create("ID NUMBER, " + ", ".join(f"C{i} VARCHAR2(4000)" for i in range(20)))
    select = ", ".join(f"repeat('{chr(97 + i)}', 4000) AS c{i}" for i in range(20))
    proc = run_duckdb(
        secret_sql()
        + f"COPY (SELECT i AS id, {select} FROM range(50) t(i)) TO '{table.name}' "
        "(FORMAT oraduck, CONNECTION 'ora', STREAM_SIZE 65536);\n",
        timeout=300,
    )
    if proc.returncode == 0:
        assert table.scalar("COUNT(*)") == 50
        assert table.scalar("SUM(LENGTH(C0) + LENGTH(C19))") == 50 * 8000
    else:
        assert "STREAM_SIZE" in proc.stderr
        assert table.scalar("COUNT(*)") == 0


def test_timestamp_without_fraction_is_comparable(table):
    # A loaded TIMESTAMP must equal the SQL literal (Oracle canonical form)
    table.create("ID NUMBER, C TIMESTAMP(6)")
    copy_ok("SELECT 1 AS id, TIMESTAMP '2024-02-29 13:45:07' AS c UNION ALL "
            "SELECT 2, TIMESTAMP '2024-02-29 13:45:07.5'", table.name)
    assert table.scalar("COUNT(*)") == 2
    assert table.scalar("SUM(CASE WHEN C = TIMESTAMP '2024-02-29 13:45:07' THEN 1 ELSE 0 END)") == 1
    assert table.scalar("SUM(CASE WHEN C = TIMESTAMP '2024-02-29 13:45:07.5' THEN 1 ELSE 0 END)") == 1
