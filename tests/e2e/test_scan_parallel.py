"""oraduck_scan parallel reads: split strategies, extent sources, snapshot, several scans, empty tables, re-runs."""

import sys
import textwrap

import pytest

from conftest import MILLION
from oraduck_helpers import (drop_view, env, explain_scan, fill_ids, json_documents, run_duckdb, scan_call, scan_json,
                             scan_output, secret_sql, split_warnings)

THREADS = "SET threads = 8;\n"
SUM = MILLION * (MILLION + 1) // 2


def oracle_totals(table) -> tuple[int, int, int]:
    return table.scalar("COUNT(*)"), table.scalar("SUM(ID)"), table.scalar("COUNT(DISTINCT ID)")


@pytest.mark.parametrize("split", ["extents", "hash", "none"])
def test_split_strategies_agree(million, split):
    call = scan_call(million.name, f"split := '{split}'")
    rows = scan_json(f"SELECT count(*) AS n, sum(id)::BIGINT AS s, count(DISTINCT id) AS d FROM {call}", setup=THREADS)
    assert (rows[0]["n"], rows[0]["s"], rows[0]["d"]) == oracle_totals(million) == (MILLION, SUM, MILLION)

    (info,) = explain_scan(f"SELECT sum(id) FROM {call}", setup=THREADS, analyze=True)
    assert info["Split"] == split
    assert info["Strategy"] == split
    units = int(info["Units"])
    if split == "extents":
        assert units >= 8
        assert "DBA_EXTENTS" in info["Notes"]
        assert int(info["Sessions"]) == 8
    elif split == "hash":
        assert units == 8 and int(info["Sessions"]) == 8
    else:
        assert units == 1 and int(info["Sessions"]) == 1
    # BENCH may run DBMS_FLASHBACK: one snapshot SCN for every session
    assert info["Snapshot"].isdigit(), info
    assert f'AS OF SCN TO_NUMBER(\'{info["Snapshot"]}\')' in info["Oracle SQL"]
    assert float(info["Planning (ms)"]) >= 0


def test_sessions_option_caps_the_sessions(million):
    call = scan_call(million.name, "sessions := 3, split := 'hash'")
    (info,) = explain_scan(f"SELECT sum(id) FROM {call}", setup=THREADS, analyze=True)
    assert (info["Units"], info["Sessions"]) == ("3", "3")
    assert scan_json(f"SELECT sum(id)::BIGINT AS s FROM {call}", setup=THREADS) == [{"s": SUM}]
    # snapshot 'none' never uses AS OF
    no_snapshot = scan_call(million.name, "snapshot := 'none'")
    (info,) = explain_scan(f"SELECT sum(id) FROM {no_snapshot}", setup=THREADS, analyze=True)
    assert info["Snapshot"] == "per session" and "AS OF" not in info["Oracle SQL"]


def test_reader_uses_parallel_execute_chunks(million, oracle):
    view = million.name + "_V"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT ID, PAD FROM {million.name}")
        cur.execute(f"GRANT SELECT ON {view} TO {env('ORADUCK_ORACLE_READER_USER')}")
    try:
        call = scan_call(f"BENCH.{million.name}")
        (info,) = explain_scan(f"SELECT sum(id) FROM {call}", setup=THREADS, analyze=True, reader=True)
        assert info["Strategy"] == "extents"
        assert "DBMS_PARALLEL_EXECUTE" in info["Notes"]
        assert int(info["Units"]) >= 8
        assert info["Snapshot"] == "per session"
        assert "AS OF" not in info["Oracle SQL"]
        rows = scan_json(f"SELECT count(*) AS n, sum(id)::BIGINT AS s FROM {call}", setup=THREADS, reader=True)
        assert rows == [{"n": MILLION, "s": SUM}]

        (info,) = explain_scan(f"SELECT sum(id) FROM {scan_call('BENCH.' + view)}", setup=THREADS, analyze=True,
                               reader=True)
        assert info["Strategy"] == "hash"
        rows = scan_json(f"SELECT count(*) AS n, sum(id)::BIGINT AS s FROM {scan_call('BENCH.' + view)}",
                         setup=THREADS, reader=True)
        assert rows == [{"n": MILLION, "s": SUM}]

        # The snapshot fallback is a WARNING log entry: printed by the CLI, and in duckdb_logs with a memory storage
        _, warnings = scan_output(f"SELECT count(*) AS n FROM {call}", setup=THREADS, reader=True)
        assert len(warnings) == 1, warnings
        assert "per session" in warnings[0] and "DBMS_FLASHBACK" in warnings[0] and million.name in warnings[0]
        logs = scan_json(
            f"SELECT count(*) AS n FROM {call};\n"
            "SELECT log_level, message FROM duckdb_logs WHERE message LIKE '%OraDuck%';\n",
            setup=THREADS + "CALL enable_logging(level = 'warning', storage = 'memory');\n", reader=True)
        assert [log["log_level"] for log in logs] == ["WARNING"]
        assert logs[0]["message"] == warnings[0]
    finally:
        drop_view(oracle, view)


def test_two_scans_in_one_query(million):
    rows = scan_json(
        f"SELECT count(*) AS n, sum(a.id)::BIGINT AS s FROM {scan_call(million.name)} a "
        f"JOIN {scan_call(million.name, 'sessions := 4')} b ON a.id = b.id AND a.pad = b.pad",
        setup=THREADS,
    )
    assert rows == [{"n": MILLION, "s": SUM}]


def test_nothing_to_read(table, million):
    table.create("ID NUMBER(10), NAME VARCHAR2(10)")
    call = scan_call(table.name)
    assert scan_json(f"SELECT count(*) AS n FROM {call}", setup=THREADS) == [{"n": 0}]
    rows = scan_json(
        "SELECT list(column_name ORDER BY column_index) AS names, (SELECT count(*) FROM t) AS n "
        "FROM duckdb_columns() WHERE table_name = 't'",
        setup=THREADS + f"CREATE TABLE t AS SELECT * FROM {call};\n",
    )
    assert rows == [{"names": ["ID", "NAME"], "n": 0}]
    (info,) = explain_scan(f"SELECT * FROM {call}", setup=THREADS, analyze=True)
    assert info["Units"] == "0" and info["Sessions"] == "0"
    assert scan_json(f"SELECT count(*) AS n FROM {scan_call(million.name)}", setup=THREADS) == [{"n": MILLION}]


def test_prepared_statement_after_truncate(table, tmp_path):
    table.create("ID NUMBER(10)")
    fill_ids(table, 100_000)
    script = tmp_path / "truncate.py"
    script.write_text(textwrap.dedent(f"""\
        import os
        import oracledb
        conn = oracledb.connect(user=os.environ["ORADUCK_ORACLE_USER"], password=os.environ["ORADUCK_ORACLE_PASSWORD"],
                                dsn=os.environ["ORADUCK_ORACLE_DSN"])
        cur = conn.cursor()
        cur.execute("TRUNCATE TABLE {table.name}")
        cur.execute("INSERT INTO {table.name} SELECT LEVEL FROM dual CONNECT BY LEVEL <= 2500")
        conn.commit()
        """))
    query = f"SELECT count(*) AS n, sum(id)::BIGINT AS s FROM {scan_call(table.name)}"
    proc = run_duckdb(
        secret_sql() + THREADS + f"PREPARE q AS {query};\nEXECUTE q;\n.shell {sys.executable} {script}\nEXECUTE q;\n",
        json_mode=True,
    )
    assert proc.returncode == 0, proc.stderr
    results = [d for d in json_documents(split_warnings(proc.stdout)[0]) if isinstance(d, list)]
    # The work plan is computed at each execution: the ROWID ranges of the truncated table are not reused
    assert results[-2:] == [[{"n": 100_000, "s": 5_000_050_000}], [{"n": 2500, "s": 3_126_250}]]
