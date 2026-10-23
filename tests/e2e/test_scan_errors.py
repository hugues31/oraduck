"""oraduck_scan errors: Oracle errors mid-scan, interrupts (sessions released), bind and option errors."""

import subprocess
import time
import uuid

import pytest

from oraduck_helpers import (DUCKDB, WORKDIR, OracleTable, drop_view, env, fill_ids, scan_call, scan_error,
                             secret_sql)

ROWS_7M = 7_000_000


def client_sessions(oracle, pid: int) -> int:
    """Oracle sessions opened by the client process pid (V$SESSION.PROCESS is the client's process id)."""
    with oracle.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM v$session WHERE process = :p", p=str(pid))
        return cur.fetchone()[0]


def run_and_watch_sessions(oracle, script: str, timeout: float = 300) -> tuple[str, int, int]:
    """Runs script in the CLI without -bail, followed by a marker file and a 3 s sleep. Returns the CLI output, the
    most sessions seen while the script ran and the sessions left during the sleep."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    marker = WORKDIR / f"done_{uuid.uuid4().hex}"
    script += f".shell touch {marker}\n.shell sleep 3\n"
    proc = subprocess.Popen([str(DUCKDB), "-batch", "-no-agent", ":memory:"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=WORKDIR)
    try:
        proc.stdin.write(script)
        proc.stdin.close()
        most = 0
        deadline = time.monotonic() + timeout
        while not marker.exists():
            assert proc.poll() is None, "the CLI ended before the marker"
            assert time.monotonic() < deadline, "timeout"
            most = max(most, client_sessions(oracle, proc.pid))
            time.sleep(0.05)
        left = client_sessions(oracle, proc.pid)
        out, err = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
        marker.unlink(missing_ok=True)
    return out + err, most, left


@pytest.fixture(scope="module")
def seven_million(oracle):
    """7,000,000 rows in a narrow table (the machine's RAM is shared with Oracle)."""
    t = OracleTable(oracle, f"E2E_7M_{uuid.uuid4().hex[:10].upper()}")
    t.create("ID NUMBER(10) NOT NULL")
    fill_ids(t, ROWS_7M)
    yield t
    t.drop()


def test_error_mid_scan_releases_sessions(oracle, million):
    view = million.name + "_DIV"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT ID, 1 / CASE WHEN ID = 50000 THEN 0 ELSE 1 END AS X "
                    f"FROM {million.name}")
    try:
        output, _, left = run_and_watch_sessions(
            oracle, secret_sql() + "SET threads = 8;\n" + f"SELECT sum(x) FROM {scan_call(view)};\n")
        assert "ORA-01476" in output, output
        assert "OraDuck" in output
        assert left == 0
    finally:
        drop_view(oracle, view)


def test_interrupt_releases_sessions(oracle, seven_million):
    # The narrow table alone is read in about 0.25 s here: a view adds a computed 1,000-character column (no storage),
    # so that reading its 7,000,000 rows takes about 20 s without the interrupt
    view = seven_million.name + "_V"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT ID, RPAD(TO_CHAR(ID), 1000, 'x') AS PAD FROM {seven_million.name}")
    try:
        start = time.monotonic()
        output, most, left = run_and_watch_sessions(
            oracle,
            secret_sql() + "SET threads = 8;\nSET max_execution_time = 1000;\n"
            f"SELECT sum(length(pad)) FROM {scan_call(view)};\n",
        )
        assert "maximum execution time" in output, output
        assert time.monotonic() - start < 10, "the scan was not stopped promptly"
        assert most >= 2, "the sessions of the scan were never seen in V$SESSION"
        assert left == 0
    finally:
        drop_view(oracle, view)


def test_bind_errors(oracle, table):
    table.create("ID NUMBER(5)")
    table.execute(f"GRANT SELECT ON {table.name} TO {env('ORADUCK_ORACLE_READER_USER')}")
    assert "not found or not accessible" in scan_error(f"SELECT * FROM {scan_call('E2E_DOES_NOT_EXIST')};")
    wrong = (
        f"CREATE SECRET wrong (TYPE oraduck, USER '{env('ORADUCK_ORACLE_USER')}', PASSWORD 'wrong', "
        f"DSN '{env('ORADUCK_ORACLE_DSN')}');\n"
    )
    assert "ORA-01017" in scan_error(f"SELECT * FROM oraduck_scan('{table.name}', connection := 'wrong');",
                                     setup=wrong)
    err = scan_error(f"SELECT * FROM {scan_call(table.name, 'sessions := 0')};")
    assert "sessions" in err and "1024" in err
    bigint = scan_call(table.name, "unconstrained_number := 'BIGINT'")
    err = scan_error(f"SELECT * FROM {bigint};")
    assert "BIGINT" in err and "DECIMAL(p,s)" in err
    # READER may read neither DBMS_FLASHBACK nor V$DATABASE: no common snapshot, an error under 'scn'
    scn = scan_call("BENCH." + table.name, "snapshot := 'scn'")
    err = scan_error(f"SELECT * FROM {scn};", reader=True)
    assert "snapshot 'scn'" in err and "DBMS_FLASHBACK" in err, err


def test_forced_strategy_that_cannot_run(oracle, table):
    table.create("ID NUMBER(5)")
    view = table.name + "_V"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT ID FROM {table.name}")
    try:
        extents = scan_call(view, "split := 'extents'")
        err = scan_error(f"SELECT * FROM {extents};")
        assert "split 'extents' needs a table" in err, err
    finally:
        drop_view(oracle, view)

