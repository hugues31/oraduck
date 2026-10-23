import time
import uuid

import oracledb
import pytest

from oraduck_helpers import OracleTable, env, fill_ids

oracledb.defaults.fetch_decimals = True

MILLION = 1_000_000


@pytest.fixture(scope="session")
def oracle():
    conn = oracledb.connect(
        user=env("ORADUCK_ORACLE_USER"), password=env("ORADUCK_ORACLE_PASSWORD"), dsn=env("ORADUCK_ORACLE_DSN")
    )
    yield conn
    conn.close()


@pytest.fixture
def table(oracle):
    t = OracleTable(oracle, f"E2E_{uuid.uuid4().hex[:12].upper()}")
    yield t
    t.drop()


@pytest.fixture
def make_table(oracle):
    """Factory of extra disposable tables, dropped after the test."""
    created = []

    def make() -> OracleTable:
        t = OracleTable(oracle, f"E2E_{uuid.uuid4().hex[:12].upper()}")
        created.append(t)
        return t

    yield make
    for t in created:
        t.drop()


def wait_for_snapshot(t: OracleTable, rows: int, timeout: float = 60) -> None:
    """Waits until a query AS OF the current SCN sees the table's rows: right after DDL, AS OF SCN raises ORA-01466
    for a few seconds."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            with t.conn.cursor() as cur:
                cur.execute(
                    f"SELECT COUNT(*) FROM {t.name} AS OF SCN DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER"
                )
                if cur.fetchone()[0] == rows:
                    return
        except oracledb.DatabaseError as e:
            if "ORA-01466" not in str(e):
                raise
        if time.monotonic() > deadline:
            raise TimeoutError(f"{t.name} not readable AS OF SCN after {timeout} s")
        time.sleep(0.5)


@pytest.fixture(scope="session")
def million(oracle):
    """1,000,000 rows (ID 1..1,000,000 and a short text), statistics gathered, readable by READER and AS OF SCN."""
    t = OracleTable(oracle, f"E2E_M_{uuid.uuid4().hex[:10].upper()}")
    t.create("ID NUMBER(10) NOT NULL, PAD VARCHAR2(20)")
    fill_ids(t, MILLION, "ID, PAD", "n, 'row ' || n")
    t.execute(f"GRANT SELECT ON {t.name} TO {env('ORADUCK_ORACLE_READER_USER')}")
    wait_for_snapshot(t, MILLION)
    yield t
    t.drop()
