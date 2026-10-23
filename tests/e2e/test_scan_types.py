"""oraduck_scan type mapping (spec 5.3), names and projection pushdown."""

from decimal import Decimal

import oracledb
import pytest

from oraduck_helpers import explain_scan, owner, scan_call, scan_error, scan_json

UTC = "SET TimeZone = 'UTC';\n"

# (column, Oracle type, DuckDB type, row 1 literal, row 1 DuckDB text, row 3 literal, row 3 DuckDB text)
# Row 2 is all NULL. Rows 1 and 3 are written as Oracle SQL literals; larger LOB values are bound separately.
CASES = [
    ("N4", "NUMBER(4)", "SMALLINT", "-9999", "-9999", "1234", "1234"),
    ("N9", "NUMBER(9)", "INTEGER", "999999999", "999999999", "-5", "-5"),
    ("N18", "NUMBER(18)", "BIGINT", "-999999999999999999", "-999999999999999999", "123456789012345678",
     "123456789012345678"),
    ("N15_2", "NUMBER(15,2)", "DECIMAL(15,2)", "1234567890123.45", "1234567890123.45", "-0.01", "-0.01"),
    ("N25_9", "NUMBER(25,9)", "DECIMAL(25,9)", "1234567890123456.123456789", "1234567890123456.123456789",
     "-0.000000001", "-0.000000001"),
    ("N38", "NUMBER(38,0)", "DECIMAL(38,0)", "12345678901234567890123456789012345678",
     "12345678901234567890123456789012345678", "-99999999999999999999999999999999999999",
     "-99999999999999999999999999999999999999"),
    ("N38_10", "NUMBER(38,10)", "DECIMAL(38,10)", "-1844674407.3709551616", "-1844674407.3709551616",
     "1234567890123456789012345678.0123456789", "1234567890123456789012345678.0123456789"),
    ("NNEG", "NUMBER(5,-2)", "DECIMAL(7,0)", "1234500", "1234500", "-9999900", "-9999900"),
    ("NSGP", "NUMBER(3,5)", "DECIMAL(5,5)", "0.00123", ".00123", "-0.00999", "-.00999"),
    ("NUM", "NUMBER", "DOUBLE", "0.1", "0.1", "-1234.5", "-1234.5"),
    ("FLT", "FLOAT(10)", "DOUBLE", "2.5", "2.5", "1e20", "1e+20"),
    ("BF", "BINARY_FLOAT", "FLOAT", "1.5", "1.5", "-0.25", "-0.25"),
    ("BD", "BINARY_DOUBLE", "DOUBLE", "1e300d", "1e+300", "BINARY_DOUBLE_INFINITY", "inf"),
    ("D", "DATE", "TIMESTAMP", "TO_DATE('2024-05-07 10:20:30', 'YYYY-MM-DD HH24:MI:SS')", "2024-05-07 10:20:30",
     "TO_DATE('1969-12-31 23:59:59', 'YYYY-MM-DD HH24:MI:SS')", "1969-12-31 23:59:59"),
    ("TS", "TIMESTAMP(6)", "TIMESTAMP", "TIMESTAMP '2024-05-07 10:20:30.123456'", "2024-05-07 10:20:30.123456",
     "TIMESTAMP '1969-12-31 23:59:59.999999'", "1969-12-31 23:59:59.999999"),
    ("TS9", "TIMESTAMP(9)", "TIMESTAMP", "TIMESTAMP '2024-05-07 10:20:30.123456789'", "2024-05-07 10:20:30.123456",
     "TIMESTAMP '0001-01-01 00:00:00.000000001'", "0001-01-01 00:00:00"),
    ("TS0", "TIMESTAMP(0)", "TIMESTAMP", "TIMESTAMP '2024-05-07 10:20:30'", "2024-05-07 10:20:30",
     "TIMESTAMP '9999-12-31 23:59:59'", "9999-12-31 23:59:59"),
    ("TSTZ", "TIMESTAMP(6) WITH TIME ZONE", "TIMESTAMP WITH TIME ZONE",
     "TIMESTAMP '2024-05-07 10:20:30.5 +02:00'", "2024-05-07 08:20:30.5+00",
     "TIMESTAMP '2024-01-01 00:30:00 America/New_York'", "2024-01-01 05:30:00+00"),
    ("TSLTZ", "TIMESTAMP WITH LOCAL TIME ZONE", "TIMESTAMP WITH TIME ZONE",
     "TIMESTAMP '2024-05-07 10:20:30 +02:00'", "2024-05-07 08:20:30+00",
     "TIMESTAMP '1999-12-31 23:00:00 -01:00'", "2000-01-01 00:00:00+00"),
    ("VC", "VARCHAR2(4000 BYTE)", "VARCHAR", "'é漢字😀 text'", "é漢字😀 text", "'x'", "x"),
    ("NVC", "NVARCHAR2(20)", "VARCHAR", "N'Ünïcødé 漢字😀'", "Ünïcødé 漢字😀", "N'n'", "n"),
    ("C1", "CHAR(1 BYTE)", "VARCHAR", "'Y'", "Y", "'N'", "N"),
    ("C3", "CHAR(3)", "VARCHAR", "'A'", "A  ", "'abc'", "abc"),
    ("NC", "NCHAR(4)", "VARCHAR", "N'é'", "é   ", "N'漢字'", "漢字  "),
    ("CL", "CLOB", "VARCHAR", "TO_CLOB('clob é漢字😀')", "clob é漢字😀", "EMPTY_CLOB()", ""),
    ("NCL", "NCLOB", "VARCHAR", "TO_NCLOB('nclob 漢字😀')", "nclob 漢字😀", "TO_NCLOB('z')", "z"),
    ("R", "RAW(16)", "BLOB", "HEXTORAW('DEAD')", "\\xDE\\xAD", "HEXTORAW('00412E')", "\\x00A."),
    ("BL", "BLOB", "BLOB", "HEXTORAW('0001414BFF')", "\\x00\\x01AK\\xFF", "EMPTY_BLOB()", ""),
]


def test_every_type_round_trip(table, make_table):
    columns = ", ".join(f"{c[0]} {c[1]}" for c in CASES)
    table.create(f"ID NUMBER(5), {columns}, RID ROWID, URID UROWID, BIGC CLOB, BIGB BLOB")
    names = ", ".join(c[0] for c in CASES)
    table.execute(f"INSERT INTO {table.name} (ID, {names}) VALUES (1, {', '.join(c[3] for c in CASES)})")
    table.execute(f"INSERT INTO {table.name} (ID) VALUES (2)")
    table.execute(f"INSERT INTO {table.name} (ID, {names}) VALUES (3, {', '.join(c[5] for c in CASES)})")
    # Values above the LOB prefetch size (32,768 characters) and above 4,000 bytes
    big_text = "é漢字😀" * 20000 + "end"
    big_bytes = bytes(range(256)) * 300
    with table.conn.cursor() as cur:
        cur.setinputsizes(c=oracledb.DB_TYPE_CLOB, b=oracledb.DB_TYPE_BLOB)
        cur.execute(f"UPDATE {table.name} SET BIGC = :c, BIGB = :b WHERE ID = 1", c=big_text, b=big_bytes)
        cur.execute(f"UPDATE {table.name} SET RID = ROWID, URID = ROWID WHERE ID IN (1, 3)")
    table.conn.commit()
    rowids = {row[0]: (row[1], row[2]) for row in table.rows("ID, ROWIDTOCHAR(RID), CAST(URID AS VARCHAR2(4000))")}

    selects = ", ".join(f"CAST({c[0]} AS VARCHAR) AS {c[0]}" for c in CASES)
    rows = scan_json(
        f"SELECT CAST(ID AS VARCHAR) AS ID, {selects}, RID, URID, length(BIGC) AS BIGC_LEN, BIGC = "
        f"repeat('é漢字😀', 20000) || 'end' AS BIGC_OK, hex(BIGB) AS BIGB FROM {scan_call(table.name)} ORDER BY ID",
        setup=UTC,
    )
    assert [r["ID"] for r in rows] == ["1", "2", "3"]
    for column, oracle_type, _, _, text1, _, text3 in CASES:
        assert rows[0][column] == text1, f"{column} {oracle_type} row 1"
        assert rows[1][column] is None, f"{column} {oracle_type} NULL row"
        assert rows[2][column] == text3, f"{column} {oracle_type} row 3"
    assert (rows[0]["RID"], rows[0]["URID"]) == rowids[1]
    assert (rows[2]["RID"], rows[2]["URID"]) == rowids[3]
    assert rows[1]["RID"] is None and rows[1]["URID"] is None
    assert rows[0]["BIGC_LEN"] == len(big_text)
    assert rows[0]["BIGC_OK"] is True
    assert rows[0]["BIGB"] == big_bytes.hex().upper()

    types = scan_json(
        "SELECT " + ", ".join(f"typeof({c[0]}) AS {c[0]}" for c in CASES)
        + f", typeof(ID) AS ID, typeof(RID) AS RID, typeof(URID) AS URID FROM {scan_call(table.name)} LIMIT 1"
    )[0]
    for column, oracle_type, duck_type, *_ in CASES:
        assert types[column] == duck_type, f"{column} {oracle_type}"
    assert (types["ID"], types["RID"], types["URID"]) == ("INTEGER", "VARCHAR", "VARCHAR")

    # Dates against Oracle's own text, including the Julian/Gregorian switch and the bounds
    dates = make_table()
    dates.create("ID NUMBER(5), D DATE, TS TIMESTAMP(6)")
    for i, text in enumerate(["0001-01-01 00:00:00", "1582-10-04 12:00:00", "1582-10-15 00:00:00",
                              "1969-12-31 23:59:59", "1970-01-01 00:00:00", "2000-02-29 06:07:08",
                              "9999-12-31 23:59:59"]):
        dates.execute(
            f"INSERT INTO {dates.name} VALUES ({i}, TO_DATE('{text}', 'YYYY-MM-DD HH24:MI:SS'), "
            f"TO_TIMESTAMP('{text}.654321', 'YYYY-MM-DD HH24:MI:SS.FF6'))"
        )
    dates.conn.commit()
    expected = dates.rows("ID, TO_CHAR(D, 'YYYY-MM-DD HH24:MI:SS'), TO_CHAR(TS, 'YYYY-MM-DD HH24:MI:SS.FF6')",
                          order_by="ID")
    got = scan_json(
        f"SELECT ID, CAST(D AS VARCHAR) AS D, strftime(TS, '%Y-%m-%d %H:%M:%S.%f') AS TS "
        f"FROM {scan_call(dates.name)} ORDER BY ID"
    )
    assert [(r["ID"], r["D"], r["TS"]) for r in got] == [(e[0], e[1], e[2]) for e in expected]


def test_unconstrained_number_options(table):
    table.create("ID NUMBER(5), NUM NUMBER, FLT FLOAT")
    table.execute(f"INSERT INTO {table.name} VALUES (1, 1234567890123456.78915, 0.5)")
    table.execute(f"INSERT INTO {table.name} VALUES (2, -0.00005, NULL)")
    table.execute(f"INSERT INTO {table.name} VALUES (3, 12.3, -2)")
    table.conn.commit()
    query = "SELECT CAST(NUM AS VARCHAR) AS NUM, typeof(NUM) AS T, CAST(FLT AS VARCHAR) AS FLT FROM {} ORDER BY ID"

    rows = scan_json(query.format(scan_call(table.name, "unconstrained_number := 'VARCHAR'")))
    assert [(r["NUM"], r["FLT"]) for r in rows] == [("1234567890123456.78915", "0.5"), ("-0.00005", None),
                                                    ("12.3", "-2")]
    assert {r["T"] for r in rows} == {"VARCHAR"}

    # Rounded half away from zero to the scale
    rows = scan_json(query.format(scan_call(table.name, "unconstrained_number := 'DECIMAL(20,4)'")))
    assert [(r["NUM"], r["FLT"]) for r in rows] == [("1234567890123456.7892", "0.5000"), ("-0.0001", None),
                                                    ("12.3000", "-2.0000")]
    assert {r["T"] for r in rows} == {"DECIMAL(20,4)"}

    rows = scan_json(query.format(scan_call(table.name)))
    assert [r["NUM"] for r in rows] == ["1234567890123456.8", "-5e-05", "12.3"]
    assert {r["T"] for r in rows} == {"DOUBLE"}


def test_unconstrained_decimal_overflow_names_the_column(table):
    table.create("ID NUMBER(5), NUM NUMBER")
    table.execute(f"INSERT INTO {table.name} VALUES (1, 1)")
    # 17 integer digits do not fit DECIMAL(20,4) (the 64-bit decoder accepts them), 39 digits do not fit DECIMAL(38,0)
    # (the 128-bit decoder accepts them)
    table.execute(f"INSERT INTO {table.name} VALUES (2, 12345678901234567)")
    table.conn.commit()

    def error(decimal: str) -> str:
        return scan_error(f"SELECT * FROM {scan_call(table.name, f'unconstrained_number := {decimal!r}')};")

    for decimal in ["DECIMAL(20,4)", "DECIMAL(10,2)", "DECIMAL(16,0)"]:
        err = error(decimal)
        assert '"NUM"' in err and decimal in err, err
    table.execute(f"UPDATE {table.name} SET NUM = 123456789012345678901234567890123456789 WHERE ID = 2")
    table.conn.commit()
    err = error("DECIMAL(38,0)")
    assert '"NUM"' in err and "DECIMAL(38,0)" in err, err


def test_empty_clob_is_not_null(table):
    table.create("ID NUMBER(5), C CLOB, NC NCLOB, B BLOB")
    table.execute(f"INSERT INTO {table.name} VALUES (1, EMPTY_CLOB(), EMPTY_CLOB(), EMPTY_BLOB())")
    table.execute(f"INSERT INTO {table.name} VALUES (2, NULL, NULL, NULL)")
    table.conn.commit()
    rows = scan_json(
        "SELECT ID, C IS NULL AS C_NULL, C, NC IS NULL AS NC_NULL, NC, B IS NULL AS B_NULL, octet_length(B) AS B_LEN "
        f"FROM {scan_call(table.name)} ORDER BY ID"
    )
    assert rows == [
        {"ID": 1, "C_NULL": False, "C": "", "NC_NULL": False, "NC": "", "B_NULL": False, "B_LEN": 0},
        {"ID": 2, "C_NULL": True, "C": None, "NC_NULL": True, "NC": None, "B_NULL": True, "B_LEN": None},
    ]


def test_colliding_and_keyword_names(table):
    table.create('"Id" NUMBER(5), ID NUMBER(5), "ORDER" NUMBER(5)')
    table.execute(f"INSERT INTO {table.name} VALUES (1, 2, 3)")
    table.conn.commit()
    rows = scan_json(f"SELECT * FROM {scan_call(table.name)}")
    # deduplicated case-insensitively in column order
    assert list(rows[0].items()) == [("Id", 1), ("ID_1", 2), ("ORDER", 3)]
    rows = scan_json(f'SELECT "ORDER" AS o, "order" + 1 AS o1, "ID_1" AS i, "id" AS j FROM {scan_call(table.name)}')
    assert rows == [{"o": 3, "o1": 4, "i": 2, "j": 1}]


def test_generated_name_skips_existing_names(table):
    table.create('"Id" NUMBER(5), ID NUMBER(5), "Id_1" NUMBER(5)')
    table.execute(f"INSERT INTO {table.name} VALUES (1, 2, 3)")
    table.conn.commit()
    rows = scan_json(f"SELECT * FROM {scan_call(table.name)}")
    # ID's first suffix _1 is taken by "Id_1" (case-insensitively): it gets _2
    assert list(rows[0].items()) == [("Id", 1), ("ID_2", 2), ("Id_1", 3)]


def test_unsupported_column_only_when_selected(table):
    table.create("ID NUMBER(5), X XMLTYPE, NAME VARCHAR2(10)")
    table.execute(f"INSERT INTO {table.name} VALUES (1, XMLTYPE('<a/>'), 'one')")
    table.conn.commit()
    assert scan_json(f"SELECT id, name FROM {scan_call(table.name)}") == [{"ID": 1, "NAME": "one"}]
    assert scan_json(f"SELECT count(*) AS n FROM {scan_call(table.name)}") == [{"n": 1}]
    err = scan_error(f"SELECT * FROM {scan_call(table.name)};")
    assert '"X"' in err and "XMLTYPE" in err


def test_projection_pushdown(table):
    table.create("A NUMBER(5), B VARCHAR2(10), C DATE")
    table.execute(f"INSERT INTO {table.name} VALUES (1, 'b', SYSDATE)")
    table.conn.commit()
    (info,) = explain_scan(f"SELECT a FROM {scan_call(table.name)}", analyze=True)
    assert info["Oracle SQL"].startswith(f'SELECT /* oraduck_scan */ "A" FROM "{owner()}"."{table.name}"')
    assert '"B"' not in info["Oracle SQL"] and '"C"' not in info["Oracle SQL"]
    assert info["Columns"] == "A"
    (info,) = explain_scan(f"SELECT count(*) FROM {scan_call(table.name)}", analyze=True)
    assert info["Oracle SQL"].startswith(f'SELECT /* oraduck_scan */ 1 FROM "{owner()}"."{table.name}"')
    assert scan_json(f"SELECT a, count(*) AS n FROM {scan_call(table.name)} GROUP BY a") == [{"A": 1, "n": 1}]


def test_explain_shows_requested_options(table):
    table.create("A NUMBER(5)")
    options = "split := 'hash', snapshot := 'none'"
    (info,) = explain_scan(f"SELECT a FROM {scan_call(table.name, options)}")
    assert info["Table"] == f"{owner()}.{table.name}"
    assert info["Oracle WHERE"] == "—"
    assert (info["Split"], info["Snapshot"]) == ("hash", "none")
    (info,) = explain_scan(f"SELECT a FROM {scan_call(table.name)}")
    assert (info["Split"], info["Snapshot"]) == ("auto", "auto")


@pytest.mark.parametrize("form", ["table", "owner.table lower case", "quoted"])
def test_table_name_forms(table, form):
    table.create("A NUMBER(5)")
    table.execute(f"INSERT INTO {table.name} VALUES (7)")
    table.conn.commit()
    target = {
        "table": table.name,
        "owner.table lower case": f"{owner()}.{table.name}".lower(),
        "quoted": f'"{owner()}"."{table.name}"',
    }[form]
    assert scan_json(f"SELECT a FROM oraduck_scan('{target}', connection := 'ora')") == [{"A": 7}]


def test_invalid_utf8_names_the_column(table):
    table.create("ID NUMBER(5), V VARCHAR2(20), CH CHAR(4), C CLOB, OK VARCHAR2(40)")
    table.execute(f"INSERT INTO {table.name} (ID, OK) VALUES (1, 'é漢字😀')")
    # The database (AL32UTF8) keeps the bytes of a relabelled RAW as they are: 'A', then C3 28 (invalid UTF-8)
    bad = "UTL_RAW.CAST_TO_VARCHAR2(HEXTORAW('41C328'))"
    table.execute(f"INSERT INTO {table.name} (ID, V, CH, C) VALUES (2, {bad}, {bad}, TO_CLOB({bad}))")
    table.conn.commit()
    assert table.rows("DUMP(V, 16)", order_by="ID")[1][0] == "Typ=1 Len=3: 41,c3,28"
    # Valid multi-byte values still read
    rows = scan_json(f"SELECT id, ok FROM {scan_call(table.name)} ORDER BY id")
    assert rows == [{"ID": 1, "OK": "é漢字😀"}, {"ID": 2, "OK": None}]
    for column in ["V", "CH"]:
        err = scan_error(f"SELECT {column} FROM {scan_call(table.name)};")
        assert f'column "{column}"' in err and "invalid UTF-8" in err, err
    # A CLOB cannot hold them: Oracle replaces the invalid byte by U+FFFD when it stores the value
    rows = scan_json(f"SELECT id, c FROM {scan_call(table.name)} ORDER BY id")
    assert rows[0]["C"] is None and rows[1]["C"].startswith("A\ufffd")


def test_nulls_across_chunks_and_batches(table):
    # NULL patterns that cross DuckDB vectors (2,048 rows) and fetch batches, with one session
    table.create("ID NUMBER(10), N NUMBER(10), S VARCHAR2(10), D DATE, X NUMBER(25,9), C CLOB")
    table.execute(
        f"INSERT INTO {table.name} SELECT LEVEL, DECODE(MOD(LEVEL, 3), 0, NULL, LEVEL), "
        "DECODE(MOD(LEVEL, 5), 0, NULL, 's' || LEVEL), DECODE(MOD(LEVEL, 7), 0, NULL, DATE '2024-01-01' + LEVEL), "
        "DECODE(MOD(LEVEL, 11), 0, NULL, LEVEL / 8), CASE WHEN MOD(LEVEL, 13) > 0 THEN TO_CLOB('c' || LEVEL) END "
        "FROM dual CONNECT BY LEVEL <= 30000"
    )
    table.conn.commit()
    expected = table.rows("COUNT(N), COUNT(S), COUNT(D), COUNT(X), COUNT(DBMS_LOB.GETLENGTH(C)), SUM(N), "
                          "SUM(LENGTH(S)), SUM(X)", order_by="1")[0]
    for options in ["split := 'none'", "split := 'hash', sessions := 3"]:
        rows = scan_json(
            "SELECT count(n) AS n, count(s) AS s, count(d) AS d, count(x) AS x, count(c) AS c, sum(n)::BIGINT AS sn, "
            "sum(length(s))::BIGINT AS ss, sum(x) AS sx, "
            "count(*) FILTER (WHERE (n IS NULL) <> (id % 3 = 0) OR (s IS NULL) <> (id % 5 = 0) "
            "OR (d IS NULL) <> (id % 7 = 0) OR (x IS NULL) <> (id % 11 = 0) OR (c IS NULL) <> (id % 13 = 0)) AS wrong "
            f"FROM {scan_call(table.name, options)}"
        )[0]
        # DuckDB's JSON writes DECIMAL values as text
        got = (rows["n"], rows["s"], rows["d"], rows["x"], rows["c"], rows["sn"], rows["ss"], Decimal(rows["sx"]))
        assert got == expected
        assert rows["wrong"] == 0
