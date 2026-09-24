"""Schema of the benchmark tables, for Fakelake, DuckDB, Oracle and SQL*Loader."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .config import ORACLE_SCHEMA, SEED

PRESENCE = 0.95
_DATE = {"format": "%Y-%m-%d", "after": "2000-01-01", "before": "2025-12-31"}
_DATETIME = {"format": "%Y-%m-%d %H:%M:%S", "after": "2000-01-01 00:00:00", "before": "2025-12-31 23:59:59"}
_STATUSES = [
    {"value": "ACTIVE", "weight": 6},
    {"value": "SUSPENDED", "weight": 2},
    {"value": "CLOSED", "weight": 1},
    {"value": "PENDING", "weight": 1},
]
_DATE_CTL = 'DATE "YYYY-MM-DD"'
_TIMESTAMP_CTL = 'TIMESTAMP "YYYY-MM-DD HH24:MI:SS.FF6"'


def _epoch_seconds(name: str) -> str:
    # Fakelake writes Random.Date.datetime as epoch seconds (BIGINT) in the Parquet file
    return f"make_timestamp({name} * 1000000)"


@dataclass(frozen=True)
class ColumnSpec:
    name: str
    fakelake: dict  # Fakelake parameters of the column (except "name")
    duck_type: str  # DuckDB type after loading
    oracle_type: str  # type of the target Oracle column
    ctl_field: str  # field description in the SQL*Loader control file
    csv_expr: str | None = None  # CSV export expression when different from the name
    load_expr: str | None = None  # load expression from Parquet when a CAST is not enough


def _base() -> list[ColumnSpec]:
    p = {"presence": PRESENCE}
    return [
        ColumnSpec("id", {"provider": "Increment.integer", "start": 1}, "INTEGER", "NUMBER(10)", "INTEGER EXTERNAL"),
        ColumnSpec("amount", {"provider": "Random.Number.f64", "min": 0, "max": 100000, **p},
                   "DECIMAL(15,2)", "NUMBER(15,2)", "DECIMAL EXTERNAL"),
        ColumnSpec("label", {"provider": "Random.String.alphanumeric", "length": "5..30", **p},
                   "VARCHAR", "VARCHAR2(30)", "CHAR(30)"),
        ColumnSpec("operation_date", {"provider": "Random.Date.date", **_DATE, **p}, "DATE", "DATE", _DATE_CTL),
        ColumnSpec("event_time", {"provider": "Random.Date.datetime", **_DATETIME, **p},
                   "TIMESTAMP", "TIMESTAMP(6)", _TIMESTAMP_CTL, load_expr=_epoch_seconds("event_time")),
    ]


def _cycle(index: int, kind: int) -> ColumnSpec:
    n = f"c{index:02d}"
    p = {"presence": PRESENCE}
    return [
        ColumnSpec(f"{n}_i32", {"provider": "Random.Number.i32", "min": -1000000, "max": 1000000, **p},
                   "INTEGER", "NUMBER(10)", "INTEGER EXTERNAL"),
        ColumnSpec(f"{n}_f64", {"provider": "Random.Number.f64", "min": -1000000, "max": 1000000, **p},
                   "DOUBLE", "BINARY_DOUBLE", "FLOAT EXTERNAL"),
        ColumnSpec(f"{n}_dec", {"provider": "Random.Number.f64", "min": 0, "max": 100000, **p},
                   "DECIMAL(15,2)", "NUMBER(15,2)", "DECIMAL EXTERNAL"),
        ColumnSpec(f"{n}_alnum", {"provider": "Random.String.alphanumeric", "length": "5..30", **p},
                   "VARCHAR", "VARCHAR2(30)", "CHAR(30)"),
        ColumnSpec(f"{n}_email", {"provider": "Person.email", **p}, "VARCHAR", "VARCHAR2(64)", "CHAR(64)"),
        ColumnSpec(f"{n}_fname", {"provider": "Person.fname", **p}, "VARCHAR", "VARCHAR2(64)", "CHAR(64)"),
        ColumnSpec(f"{n}_date", {"provider": "Random.Date.date", **_DATE, **p}, "DATE", "DATE", _DATE_CTL),
        ColumnSpec(f"{n}_ts", {"provider": "Random.Date.datetime", **_DATETIME, **p},
                   "TIMESTAMP", "TIMESTAMP(6)", _TIMESTAMP_CTL, load_expr=_epoch_seconds(f"{n}_ts")),
        ColumnSpec(f"{n}_bool", {"provider": "Random.bool", **p}, "BOOLEAN", "NUMBER(1)", "INTEGER EXTERNAL",
                   csv_expr=f"CAST({n}_bool AS INTEGER)"),
        ColumnSpec(f"{n}_status", {"provider": "Constant.string", "data": _STATUSES, **p},
                   "VARCHAR", "VARCHAR2(16)", "CHAR(16)"),
    ][kind]


def columns(ncols: int) -> list[ColumnSpec]:
    if ncols < 5:
        raise ValueError("at least 5 columns")
    cols = _base()
    for i in range(5, ncols):
        cols.append(_cycle(i + 1, (i - 5) % 10))
    return cols


def table_name(nrows: int, ncols: int) -> str:
    return f"B_{nrows}_{ncols}"


def _yaml_scalar(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def fakelake_yaml(ncols: int, nrows: int, output_name: str, seed: int = SEED) -> str:
    lines = ["columns:"]
    for col in columns(ncols):
        lines.append(f"  - name: {col.name}")
        for key, value in col.fakelake.items():
            if isinstance(value, list):
                lines.append(f"    {key}:")
                for item in value:
                    for position, (k, v) in enumerate(item.items()):
                        prefix = "      - " if position == 0 else "        "
                        lines.append(f"{prefix}{k}: {_yaml_scalar(v)}")
            else:
                lines.append(f"    {key}: {_yaml_scalar(value)}")
    lines += [
        "info:",
        f"  output_name: {_yaml_scalar(output_name)}",
        "  output_format: parquet",
        f"  rows: {nrows}",
        f"  seed: {seed}",
    ]
    return "\n".join(lines) + "\n"


def duckdb_load_sql(table: str, parquet: Path | str, ncols: int) -> str:
    select = ", ".join(f"{c.load_expr or f'CAST({c.name} AS {c.duck_type})'} AS {c.name}" for c in columns(ncols))
    return f"CREATE OR REPLACE TABLE {table} AS SELECT {select} FROM read_parquet('{parquet}');\nCHECKPOINT;\n"


def oracle_ddl(table: str, ncols: int) -> str:
    cols = ", ".join(f"{c.name.upper()} {c.oracle_type}" for c in columns(ncols))
    return f"CREATE TABLE {table} ({cols})"


def csv_select_sql(table: str, ncols: int) -> str:
    exprs = ", ".join(f"{c.csv_expr} AS {c.name}" if c.csv_expr else c.name for c in columns(ncols))
    return f"SELECT {exprs} FROM {table}"


def sqlldr_control(oracle_table: str, ncols: int, schema: str = ORACLE_SCHEMA) -> str:
    fields = ",\n".join(f"  {c.name.upper()} {c.ctl_field}" for c in columns(ncols))
    return (
        "LOAD DATA\nCHARACTERSET AL32UTF8\nAPPEND\n"
        f"INTO TABLE {schema}.{oracle_table}\n"
        "FIELDS TERMINATED BY ',' OPTIONALLY ENCLOSED BY '\"'\n"
        "TRAILING NULLCOLS\n"
        f"(\n{fields}\n)\n"
    )
