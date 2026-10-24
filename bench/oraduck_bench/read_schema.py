"""Schema of the read benchmark tables: LIST-partitioned wide tables shaped like the target workload."""

from __future__ import annotations

from dataclasses import dataclass, fields
from decimal import Decimal
from pathlib import Path

from .config import ORACLE_SCHEMA
from .read_config import OTHER_DATES, TARGET_DATE
from .schema import _DATE, _DATETIME, PRESENCE, ColumnSpec, _epoch_seconds

CLOB_EVERY = 250
_P = {"presence": PRESENCE}
_TS_FIELD = "TIMESTAMP(6)"
_COMPLETE_COMMENT = "oraduck-bench: complete"


def read_table_name(nrows: int, ncols: int) -> str:
    return f"RD_{nrows}_{ncols}"


def _kind(prefix: str, index: int) -> ColumnSpec:
    """One column of the cycle; `index` is the per-type counter."""
    n = f"{prefix}{index}"
    kinds = {
        "v": ColumnSpec(n, {"provider": "Random.String.alphanumeric", "length": "5..30", **_P}, "VARCHAR",
                        "VARCHAR2(4000 BYTE)", ""),
        "i": ColumnSpec(n, {"provider": "Random.Number.i32", "min": -1000000, "max": 1000000, **_P}, "BIGINT",
                        "NUMBER(38,0)", ""),
        "d": ColumnSpec(n, {"provider": "Random.Number.f64", "min": 0, "max": 100000, **_P}, "DECIMAL(25,9)",
                        "NUMBER(25,9)", ""),
        "dt": ColumnSpec(n, {"provider": "Random.Date.date", **_DATE, **_P}, "DATE", "DATE", ""),
        "c": ColumnSpec(n, {"provider": "Random.String.alphanumeric", "length": 1, **_P}, "VARCHAR",
                        "CHAR(1 BYTE)", ""),
        "ts": ColumnSpec(n, {"provider": "Random.Date.datetime", **_DATETIME, **_P}, "TIMESTAMP", _TS_FIELD, "",
                         load_expr=_epoch_seconds(n)),
        "n": ColumnSpec(n, {"provider": "Random.Number.i32", "min": -1000000, "max": 1000000, **_P}, "DECIMAL(18,3)",
                        "NUMBER", ""),
    }
    return kinds[prefix]


# VARCHAR2(4000 BYTE) x3, NUMBER(38,0) x2, NUMBER(25,9) x2, DATE, CHAR(1 BYTE), TIMESTAMP(6), NUMBER
_CYCLE = ("v", "v", "v", "i", "i", "d", "d", "dt", "c", "ts", "n")


def read_clob_count(ncols: int) -> int:
    return max(1, ncols // CLOB_EVERY)


def read_columns(ncols: int) -> list[ColumnSpec]:
    nclob = read_clob_count(ncols)
    if ncols < 3 + nclob + len(_CYCLE):
        raise ValueError("too few columns")
    cols = [
        ColumnSpec("part_date", {}, "DATE", "DATE", ""),
        ColumnSpec("sub_key", {"provider": "Constant.string", "data": [
            {"value": "A", "weight": 6}, {"value": "B", "weight": 3}, {"value": "C", "weight": 1}]},
            "VARCHAR", "VARCHAR2(8)", ""),
        # Fakelake's Increment.integer wraps at 65,536: the id is the row number of the Parquet file
        ColumnSpec("id", {}, "BIGINT", "NUMBER(38,0)", "", load_expr="CAST(file_row_number + 1 AS BIGINT)"),
    ]
    counters: dict[str, int] = {}
    for i in range(ncols - 3 - nclob):
        prefix = _CYCLE[i % len(_CYCLE)]
        counters[prefix] = counters.get(prefix, 0) + 1
        cols.append(_kind(prefix, counters[prefix]))
    step = ncols // nclob  # CLOBs spread evenly, the first one in the middle of its share
    for j in range(nclob):
        cols.insert(step * j + step // 2, ColumnSpec(f"clob{j + 1}", {}, "VARCHAR", "CLOB", ""))
    return cols


def read_oracle_ddl(nrows: int, ncols: int) -> str:
    table = read_table_name(nrows, ncols)
    cols = ", ".join(f"{c.name.upper()} {c.oracle_type}" for c in read_columns(ncols))
    days = sorted((*OTHER_DATES, TARGET_DATE))
    partitions = ",\n  ".join(f"PARTITION P_{d.replace('-', '')} VALUES (DATE '{d}')" for d in days)
    return (
        f"CREATE TABLE {table} ({cols})\n"
        'PARTITION BY LIST ("PART_DATE")\n'
        'SUBPARTITION BY LIST ("SUB_KEY")\n'
        "SUBPARTITION TEMPLATE (SUBPARTITION SP_A VALUES ('A'), SUBPARTITION SP_B VALUES ('B'), "
        "SUBPARTITION SP_DEF VALUES (DEFAULT))\n"
        f"(\n  {partitions},\n  PARTITION P_DEF VALUES (DEFAULT)\n)"
    )


def read_load_sql(parquet: Path | str, ncols: int, nrows: int) -> str:
    """Parquet -> typed rows (without the CLOB columns): the first `nrows` ids in the target partition, then `nrows // 20` in each other one."""
    other = nrows // 20
    first, second = OTHER_DATES
    part = (f"CASE WHEN file_row_number < {nrows} THEN DATE '{TARGET_DATE}' WHEN file_row_number < {nrows + other} THEN DATE '{first}' "
            f"ELSE DATE '{second}' END")
    exprs = []
    for c in read_columns(ncols):
        if c.oracle_type == "CLOB":  # the COPY writer does not write CLOBs: left NULL, filled in Oracle afterwards
            continue
        expr = part if c.name == "part_date" else c.load_expr or f"CAST({c.name} AS {c.duck_type})"
        exprs.append(f"{expr} AS {c.name}")
    return f"SELECT {', '.join(exprs)} FROM read_parquet('{parquet}', file_row_number = true)"


@dataclass(frozen=True)
class ReadChecksums:
    rows: int
    sum_id: int
    len_v1: int
    nulls_d1: int
    len_clob1: int
    sum_d1: str  # exact decimal text of the sum of D1 (NUMBER(25,9))
    epoch_dt1: int  # sum of the epoch seconds of DT1 (DATE)
    epoch_us_ts1: int  # sum of the epoch microseconds of TS1 (TIMESTAMP(6))
    c1_frequent: int  # rows whose C1 (CHAR(1)) is C1_VALUE

    @classmethod
    def from_row(cls, row) -> "ReadChecksums":
        """From a result row (numbers or their text): sum_d1 stays text, the others are integers."""
        values = list(row)
        return cls(*(int(v) for v in values[:5]), str(values[5]), *(int(v) for v in values[6:]))


C1_VALUE = "A"  # one of the 62 equally frequent alphanumeric values of C1
D1_RELATIVE_TOLERANCE = 1e-12

READ_CHECKSUM_DUCK = (
    "SELECT count(*), coalesce(sum(id), 0), coalesce(sum(length(v1)), 0), count(*) - count(d1), "
    "coalesce(sum(length(clob1)), 0), CAST(coalesce(sum(d1), 0) AS VARCHAR), "
    "coalesce(sum(epoch_us(dt1) // 1000000), 0), coalesce(sum(epoch_us(ts1)), 0), "
    f"count(*) FILTER (WHERE c1 = '{C1_VALUE}') FROM {{table}};"
)
# Same values on casts, for methods that load numbers or text as other types (e.g. NUMBER as VARCHAR); D1 is summed
# as DECIMAL(38,9), so a D1 loaded as DOUBLE only differs by the rounding of each value
READ_CHECKSUM_DUCK_CAST = (
    "SELECT count(*), coalesce(sum(CAST(id AS HUGEINT)), 0), coalesce(sum(length(CAST(v1 AS VARCHAR))), 0), "
    "count(*) - count(d1), coalesce(sum(length(CAST(clob1 AS VARCHAR))), 0), "
    "CAST(coalesce(sum(CAST(d1 AS DECIMAL(38,9))), 0) AS VARCHAR), "
    "coalesce(sum(epoch_us(CAST(dt1 AS TIMESTAMP)) // 1000000), 0), coalesce(sum(epoch_us(CAST(ts1 AS TIMESTAMP))), 0), "
    f"count(*) FILTER (WHERE CAST(c1 AS VARCHAR) = '{C1_VALUE}') FROM {{table}};"
)
# The DuckDB type of D1 in the output table
READ_D1_TYPE_DUCK = "SELECT data_type FROM duckdb_columns() WHERE table_name = '{table}' AND lower(column_name) = 'd1';"
_TS1_SINCE_EPOCH = "(TS1 - TIMESTAMP '1970-01-01 00:00:00')"
READ_CHECKSUM_ORACLE = (
    "SELECT COUNT(*), NVL(SUM(ID), 0), NVL(SUM(LENGTH(V1)), 0), COUNT(*) - COUNT(D1), "
    "NVL(SUM(LENGTH(CLOB1)), 0), TO_CHAR(NVL(SUM(D1), 0), 'TM9'), "
    "TO_CHAR(NVL(SUM(ROUND((DT1 - DATE '1970-01-01') * 86400)), 0), 'TM9'), "
    f"TO_CHAR(NVL(SUM(EXTRACT(DAY FROM {_TS1_SINCE_EPOCH}) * 86400000000 "
    f"+ EXTRACT(HOUR FROM {_TS1_SINCE_EPOCH}) * 3600000000 + EXTRACT(MINUTE FROM {_TS1_SINCE_EPOCH}) * 60000000 "
    f"+ EXTRACT(SECOND FROM {_TS1_SINCE_EPOCH}) * 1000000), 0), 'TM9'), "
    f"COUNT(CASE WHEN C1 = '{C1_VALUE}' THEN 1 END) FROM {{table}} WHERE {{where}}"
)


def compare_checksums(expected: ReadChecksums, got: ReadChecksums, d1_decimal: bool) -> tuple[bool, str]:
    """(equal, how D1 was compared): D1's sum exactly when the method loaded D1 as DECIMAL, else within a relative
    D1_RELATIVE_TOLERANCE (a DOUBLE cannot hold every NUMBER(25,9)); every other value exactly."""
    others = all(getattr(expected, f.name) == getattr(got, f.name) for f in fields(ReadChecksums) if f.name != "sum_d1")
    want, have = Decimal(expected.sum_d1), Decimal(got.sum_d1)
    if d1_decimal:
        return others and want == have, "exact"
    close = abs(have - want) <= Decimal(repr(D1_RELATIVE_TOLERANCE)) * abs(want)
    return others and close, f"relative {D1_RELATIVE_TOLERANCE:g}"


def clob_fill_sql(table: str, ncols: int, percent: int = 2) -> str:
    """Server-side UPDATE giving every CLOB column a value for `percent` % of the rows: 100-3,000 characters,
    one row in five with 20,000 characters."""
    length = "CASE WHEN MOD(TRUNC(ID / 50), 5) = 0 THEN 20000 ELSE 100 + MOD(ID * 7919, 2901) END"
    assigns = []
    for j in range(read_clob_count(ncols)):
        text = "'" + chr(ord("a") + j) + "'"
        chunks = " || ".join(
            [f"TO_CLOB(RPAD(TO_CHAR(ID), LEAST({length}, 4000), {text}))"] +
            [f"CASE WHEN {length} > {4000 * k} THEN TO_CLOB(RPAD({text}, LEAST({length} - {4000 * k}, 4000), {text})) END"
             for k in range(1, 5)])
        assigns.append(f"CLOB{j + 1} = {chunks}")
    return f"UPDATE {table} SET {', '.join(assigns)} WHERE MOD(ID, {100 // percent}) = 0"


def complete_comment() -> str:
    return _COMPLETE_COMMENT
