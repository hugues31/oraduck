import uuid

import oracledb
import pytest

from oraduck_helpers import OracleTable, env

oracledb.defaults.fetch_decimals = True


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
