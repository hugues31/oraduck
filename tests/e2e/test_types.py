import datetime as dt
from decimal import Decimal

import pytest

from oraduck_helpers import copy_ok

D = dt.datetime

# (id, DuckDB type, Oracle type, DuckDB literals, values read back from Oracle)
CASES = [
    ("tinyint", "TINYINT", "NUMBER(3)", ["-128", "127", "NULL"], [-128, 127, None]),
    ("smallint", "SMALLINT", "NUMBER(5)", ["-32768", "32767"], [-32768, 32767]),
    ("integer", "INTEGER", "NUMBER(10)", ["-2147483648", "2147483647", "0"], [-2147483648, 2147483647, 0]),
    ("bigint", "BIGINT", "NUMBER(19)", ["-9223372036854775808", "9223372036854775807", "1", "-1"],
     [-9223372036854775808, 9223372036854775807, 1, -1]),
    ("utinyint", "UTINYINT", "NUMBER(3)", ["0", "255"], [0, 255]),
    ("usmallint", "USMALLINT", "NUMBER(5)", ["65535"], [65535]),
    ("uinteger", "UINTEGER", "NUMBER(10)", ["4294967295"], [4294967295]),
    ("ubigint", "UBIGINT", "NUMBER(20)", ["18446744073709551615", "NULL"], [18446744073709551615, None]),
    ("boolean", "BOOLEAN", "NUMBER(1)", ["true", "false", "NULL"], [1, 0, None]),
    ("decimal_15_2", "DECIMAL(15,2)", "NUMBER(15,2)", ["0.00", "123.45", "-0.01", "9999999999999.99", "NULL"],
     [Decimal("0"), Decimal("123.45"), Decimal("-0.01"), Decimal("9999999999999.99"), None]),
    ("decimal_4_1", "DECIMAL(4,1)", "NUMBER(4,1)", ["-999.9", "0.5"], [Decimal("-999.9"), Decimal("0.5")]),
    ("decimal_38_10", "DECIMAL(38,10)", "NUMBER(38,10)",
     # -1844674407.3709551616 is stored as -2^64: the negation carries into the high 64 bits
     ["1234567890123456789012345678.0123456789", "-0.0000000001", "-1234567890123456789012345678.0123456789",
      "-1844674407.3709551616"],
     [Decimal("1234567890123456789012345678.0123456789"), Decimal("-0.0000000001"),
      Decimal("-1234567890123456789012345678.0123456789"), Decimal("-1844674407.3709551616")]),
    ("double_binary_double", "DOUBLE", "BINARY_DOUBLE",
     ["0.0", "-1.5", "1e300", "2.2250738585072014e-308", "'inf'", "NULL"],
     [0.0, -1.5, 1e300, 2.2250738585072014e-308, float("inf"), None]),
    ("float_binary_float", "FLOAT", "BINARY_FLOAT", ["1.5", "-0.25"], [1.5, -0.25]),
    # SQLT_FLT conversions done by OCI (4- and 8-byte values)
    ("float_number", "FLOAT", "NUMBER", ["1.5", "-0.25", "1024"], [Decimal("1.5"), Decimal("-0.25"), Decimal("1024")]),
    ("double_binary_float", "DOUBLE", "BINARY_FLOAT", ["1.5", "-0.25"], [1.5, -0.25]),
    ("float_binary_double", "FLOAT", "BINARY_DOUBLE", ["1.5", "-0.25"], [1.5, -0.25]),
    ("double_number", "DOUBLE", "NUMBER", ["0.5", "-123.25", "1e20"],
     [Decimal("0.5"), Decimal("-123.25"), Decimal("1E+20")]),
    ("varchar", "VARCHAR", "VARCHAR2(40 CHAR)",
     ["'a'", "''", "'exactly12chr'", "'more_than_twelve_chars'", "'é漢字😀'", "NULL"],
     ["a", None, "exactly12chr", "more_than_twelve_chars", "é漢字😀", None]),
    ("char", "VARCHAR", "CHAR(5)", ["'ab'"], ["ab   "]),
    ("date", "DATE", "DATE", ["'0001-01-01'", "'1969-12-31'", "'2024-02-29'", "'9999-12-31'", "NULL"],
     [D(1, 1, 1), D(1969, 12, 31), D(2024, 2, 29), D(9999, 12, 31), None]),
    ("timestamp", "TIMESTAMP", "TIMESTAMP(6)",
     ["'1969-12-31 23:59:59.999999'", "'2024-02-29 13:45:07.123456'", "'0001-01-01 00:00:00'",
      "'9999-12-31 23:59:59.999999'", "NULL"],
     [D(1969, 12, 31, 23, 59, 59, 999999), D(2024, 2, 29, 13, 45, 7, 123456), D(1, 1, 1),
      D(9999, 12, 31, 23, 59, 59, 999999), None]),
    # Precision below 6: text path, Oracle does the rounding
    ("timestamp_3", "TIMESTAMP", "TIMESTAMP(3)", ["'2024-02-29 13:45:07.123456'"], [D(2024, 2, 29, 13, 45, 7, 123000)]),
    ("timestamp_vers_date", "TIMESTAMP", "DATE", ["'2024-02-29 13:45:07.999'"], [D(2024, 2, 29, 13, 45, 7)]),
    ("date_vers_timestamp", "DATE", "TIMESTAMP(6)", ["'2024-02-29'"], [D(2024, 2, 29)]),
]


@pytest.mark.parametrize("case", CASES, ids=[c[0] for c in CASES])
def test_round_trip(table, case):
    _, duck_type, oracle_type, literals, expected = case
    table.create(f"ID NUMBER(5), C {oracle_type}")
    values = ", ".join(f"({i}, CAST({lit} AS {duck_type}))" for i, lit in enumerate(literals))
    copy_ok(f"SELECT * FROM (VALUES {values}) t(id, c)", table.name)
    assert [row[1] for row in table.rows("ID, C", order_by="ID")] == expected
