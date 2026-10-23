"""Read-only ATTACH of an Oracle database (spec 4.7): names, listing, pushdown, options, refresh, read-only."""

import re
import socket
import subprocess
import threading
import time
import uuid

import oracledb
import pytest

from conftest import wait_for_snapshot
from oraduck_helpers import (DUCKDB, WORKDIR, OracleTable, assert_pruned, attach_sql, drop_view, env, explain_scan,
                             fill_ids, json_documents, owner, run_duckdb, scan_call, scan_error, scan_json, secret_sql,
                             split_warnings)
from test_scan_errors import run_and_watch_sessions

THREADS = "SET threads = 8;\n"
READ_ONLY = "OraDuck: attached Oracle databases are read-only; load data with COPY … TO '<SCHEMA.TABLE>' " \
            "(FORMAT ORADUCK, CONNECTION '<secret>')"

# LIST partitions on a DATE key, a NULL key in the DEFAULT partition; PCTFREE 90 spreads each partition over hundreds
# of blocks (V$SEGMENT_STATISTICS counts logical reads by steps of 16)
PARTITIONED_ROWS = 20_000
PARTITIONED_DDL = """
    ID NUMBER(10) NOT NULL, PART_DATE DATE, SUB_KEY VARCHAR2(8), N9 NUMBER(9), TS TIMESTAMP(6), PAD VARCHAR2(100))
    PCTFREE 90 PARTITION BY LIST (PART_DATE)
    (PARTITION P0506 VALUES (DATE '2024-05-06'), PARTITION P0507 VALUES (DATE '2024-05-07'),
     PARTITION P0508 VALUES (DATE '2024-05-08'), PARTITION POTHER VALUES (DEFAULT)
"""
PARTITIONED_VALUES = """n,
    CASE WHEN MOD(n, 97) = 0 THEN NULL ELSE DATE '2024-05-06' + MOD(n, 4) END,
    CASE MOD(n, 3) WHEN 0 THEN 'A' WHEN 1 THEN 'B' ELSE NULL END,
    CASE WHEN MOD(n, 11) = 0 THEN NULL ELSE MOD(n, 1000) - 500 END,
    CASE WHEN MOD(n, 37) = 0 THEN NULL ELSE TIMESTAMP '2024-01-01 00:00:00' + NUMTODSINTERVAL(n, 'HOUR') END,
    RPAD('x', 100, 'x')
"""

# Plans that DuckDB's optimizers copy, merge or reuse after the filters were pushed ({t}: the attached table or the
# DuckDB copy): each scan must keep its own Oracle WHERE, as with oraduck_scan
PLAN_SHAPES = [
    # common subplans: two scans that differ only by their pushed filters
    ("common_subplan", "SELECT (SELECT max(ts) FROM {t} WHERE sub_key = 'A') IS NULL AS a, "
                       "(SELECT max(ts) FROM {t} WHERE sub_key = 'NO_SUCH') IS NULL AS b"),
    ("common_subplan_values", "SELECT (SELECT max(id) FROM {t} WHERE n9 = 7) AS a, "
                              "(SELECT max(id) FROM {t} WHERE n9 = 8) AS b"),
    # window self-join: copies the scan below the window
    ("window", "SELECT id, sub_key, count(*) OVER (PARTITION BY sub_key) AS n FROM {t} "
               "WHERE part_date = DATE '2024-05-07' ORDER BY id"),
    # CTE inlining: copies the definition for each reference
    ("cte_not_materialized", "WITH c AS NOT MATERIALIZED (FROM {t}) SELECT (SELECT count(*) FROM c WHERE n9 = 7) AS x, "
                             "(SELECT max(id) FROM c WHERE sub_key = 'B') AS y"),
    # two references, each with its own filter
    ("self_join", "SELECT count(*) AS n, sum(a.id + b.n9)::BIGINT AS s FROM {t} a JOIN {t} b ON a.id = b.id "
                  "WHERE a.sub_key = 'A' AND b.part_date = DATE '2024-05-07'"),
    # aggregate reuse: an unfiltered and a filtered scan of the same table
    ("aggregates", "SELECT (SELECT count(*) FROM {t}) AS a, (SELECT count(*) FROM {t} WHERE n9 > 0) AS b, "
                   "(SELECT sum(n9) FROM {t}) AS c"),
    # aggregate reuse: the grouped sums of a SEMI join's subquery (filtered scan) must not replace the outer scan's
    ("semi_aggregate_reuse", "SELECT o.sub_key, sum(f.n9)::BIGINT AS s FROM own o JOIN {t} f USING (sub_key) "
                             "WHERE o.sub_key IN (SELECT sub_key FROM {t} WHERE id > 15000 GROUP BY sub_key "
                             "HAVING sum(n9) > -1000000000) GROUP BY o.sub_key ORDER BY 1"),
]


def results(sql: str, setup: str | None = None, db: str = ":memory:") -> list[list[dict]]:
    """Every result of sql, run after the ora secret and setup (default: ATTACH of ora), in -json mode."""
    proc = run_duckdb(secret_sql() + (attach_sql() if setup is None else setup) + sql, db=db, json_mode=True)
    assert proc.returncode == 0, f"query failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
    documents = [d for d in json_documents(split_warnings(proc.stdout)[0]) if isinstance(d, list)]
    # CREATE SECRET prints a Success row
    return [d for d in documents if d != [{"Success": True}]]


def operators(node: dict | list) -> list[tuple[str, dict]]:
    """(name, extra_info) of every operator of an EXPLAIN (FORMAT json) tree."""
    if isinstance(node, list):
        return [op for child in node for op in operators(child)]
    found = [(node.get("name", "").strip(), node.get("extra_info", {}))]
    return found + operators(node.get("children", []))


def attached(table: str) -> str:
    """Name of an Oracle table of the test user in the attached database ora."""
    return f"ora.{owner()}.{table}"


def run_in_steps(steps: list[tuple[str, object]], timeout: float = 300,
                 bail: bool = True) -> tuple[list[list[dict]], str, list[str]]:
    """Runs a script in one CLI process, in -json mode, step by step: after the CLI has run a step's SQL, the step's
    action (a function, or None) runs before the next step is sent. Returns every result of the script, its error
    output and the warnings it printed. With bail, the script must succeed; without, a failed statement does not stop
    it."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen([str(DUCKDB), "-batch"] + (["-bail"] if bail else []) + ["-no-agent", "-json", ":memory:"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            cwd=WORKDIR)
    try:
        for sql, action in steps:
            marker = WORKDIR / f"step_{uuid.uuid4().hex}"
            proc.stdin.write(sql + f".shell touch {marker}\n")
            proc.stdin.flush()
            deadline = time.monotonic() + timeout
            while not marker.exists():
                assert proc.poll() is None, f"the CLI ended early\n{proc.communicate()}"
                assert time.monotonic() < deadline, "timeout"
                time.sleep(0.05)
            marker.unlink()
            if action:
                action()
        out, err = proc.communicate(timeout=timeout)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert not bail or proc.returncode == 0, f"script failed\nSTDOUT:\n{out}\nSTDERR:\n{err}"
    stdout, warnings = split_warnings(out)
    documents = [d for d in json_documents(stdout) if isinstance(d, list)]
    return [d for d in documents if d != [{"Success": True}]], err, warnings


class CountingProxy:
    """TCP forwarder to the Oracle listener that counts the connections it accepts: each Oracle session opens one."""

    def __init__(self, host: str, port: int) -> None:
        self.target = (host, port)
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.connections = 0
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self) -> None:
        while True:
            try:
                client, _ = self.server.accept()
            except OSError:
                return
            self.connections += 1
            upstream = socket.create_connection(self.target)
            for source, target in ((client, upstream), (upstream, client)):
                threading.Thread(target=self._pipe, args=(source, target), daemon=True).start()

    @staticmethod
    def _pipe(source: socket.socket, target: socket.socket) -> None:
        try:
            while data := source.recv(65536):
                target.sendall(data)
        except OSError:
            pass
        finally:
            for s in (source, target):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def close(self) -> None:
        """Refuses every new connection (Oracle unreachable). shutdown wakes the thread blocked in accept, which a
        close alone does not."""
        try:
            self.server.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.server.close()


@pytest.fixture
def proxy():
    """A counting proxy to the test database, and secret_sql() for a secret px that connects through it."""
    match = re.fullmatch(r"([^:/]+):(\d+)/(.+)", env("ORADUCK_ORACLE_DSN"))
    if not match:
        pytest.skip("ORADUCK_ORACLE_DSN is not an EZConnect host:port/service")
    p = CountingProxy(match.group(1), int(match.group(2)))
    p.secret_sql = (f"CREATE SECRET px (TYPE oraduck, USER '{env('ORADUCK_ORACLE_USER')}', "
                    f"PASSWORD '{env('ORADUCK_ORACLE_PASSWORD')}', DSN '127.0.0.1:{p.port}/{match.group(3)}');\n")
    yield p
    p.close()


@pytest.fixture(scope="module")
def partitioned(oracle):
    t = OracleTable(oracle, f"E2E_A_{uuid.uuid4().hex[:10].upper()}")
    t.create(PARTITIONED_DDL)
    t.execute(f"INSERT /*+ APPEND */ INTO {t.name} (ID, PART_DATE, SUB_KEY, N9, TS, PAD) "
              f"SELECT {PARTITIONED_VALUES} FROM (SELECT LEVEL AS n FROM dual CONNECT BY LEVEL <= {PARTITIONED_ROWS})")
    oracle.commit()
    t.execute(f"BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{t.name}'); END;")
    wait_for_snapshot(t, PARTITIONED_ROWS)
    yield t
    t.drop()


@pytest.fixture(scope="module")
def duck_copy(partitioned):
    """DuckDB database holding dcopy, a copy of the partitioned table read by oraduck_scan without any filter, and own,
    the sub_key values A and B."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    path = WORKDIR / f"attach_{uuid.uuid4().hex[:8]}.duckdb"
    count = results(f"CREATE TABLE dcopy AS SELECT * FROM {scan_call(partitioned.name)};\n"
                    "CREATE TABLE own AS SELECT * FROM (VALUES ('A'), ('B')) v(sub_key);\n"
                    "SELECT count(*) AS n FROM dcopy;\n", setup="", db=str(path))[-1]
    assert count == [{"n": PARTITIONED_ROWS}]
    yield str(path)
    path.unlink(missing_ok=True)
    path.with_name(path.name + ".wal").unlink(missing_ok=True)


@pytest.fixture
def stats_conn():
    """A session that reads no test table, only V$SEGMENT_STATISTICS."""
    conn = oracledb.connect(user=env("ORADUCK_ORACLE_USER"), password=env("ORADUCK_ORACLE_PASSWORD"),
                            dsn=env("ORADUCK_ORACLE_DSN"))
    yield conn
    conn.close()


def test_default_schema_and_names(oracle, table):
    table.create("ID NUMBER(10)")
    fill_ids(table, 123)
    counts = results(
        f"SELECT count(*) AS n FROM ora.{owner()}.{table.name};\n"
        f"SELECT count(*) AS n FROM ora.{table.name};\n"
        f"SELECT count(*) AS n FROM ora.{owner().lower()}.{table.name.lower()};\n"
        f"USE ora.{owner()};\n"
        f"SELECT count(*) AS n FROM {table.name};\n"
        "SELECT current_schema() AS s;\n")
    assert counts == [[{"n": 123}]] * 4 + [[{"s": owner()}]]

    # A quoted lower-case name next to the upper-case one: the exact name wins
    lower = OracleTable(oracle, f'"{table.name.lower()}"')
    try:
        lower.create("ID NUMBER(10)")
        fill_ids(lower, 7)
        counts = results(
            f"SELECT count(*) AS n FROM ora.{owner().lower()}.{table.name.lower()};\n"
            f"SELECT count(*) AS n FROM ora.{owner()}.\"{table.name.lower()}\";\n"
            f"SELECT count(*) AS n FROM ora.{owner()}.{table.name};\n")
    finally:
        lower.drop()
    assert counts == [[{"n": 7}], [{"n": 7}], [{"n": 123}]]


# (column, Oracle type, DuckDB type of spec 5.3 in information_schema)
EVERY_TYPE = [
    ("N", "NUMBER", "DOUBLE"),
    ("N4", "NUMBER(4)", "SMALLINT"),
    ("N9", "NUMBER(9)", "INTEGER"),
    ("N18", "NUMBER(18)", "BIGINT"),
    ("N38", "NUMBER(38,0)", "DECIMAL(38,0)"),
    ("N259", "NUMBER(25,9)", "DECIMAL(25,9)"),
    ("F", "FLOAT", "DOUBLE"),
    ("BF", "BINARY_FLOAT", "FLOAT"),
    ("BD", "BINARY_DOUBLE", "DOUBLE"),
    ("D", "DATE", "TIMESTAMP"),
    ("TS", "TIMESTAMP(6)", "TIMESTAMP"),
    ("TZ", "TIMESTAMP WITH TIME ZONE", "TIMESTAMP WITH TIME ZONE"),
    ("LTZ", "TIMESTAMP WITH LOCAL TIME ZONE", "TIMESTAMP WITH TIME ZONE"),
    ("V", "VARCHAR2(4000 BYTE)", "VARCHAR"),
    ("NV", "NVARCHAR2(10)", "VARCHAR"),
    ("C", "CHAR(1 BYTE)", "VARCHAR"),
    ("NC", "NCHAR(2)", "VARCHAR"),
    ("CL", "CLOB", "VARCHAR"),
    ("NCL", "NCLOB", "VARCHAR"),
    ("R", "RAW(16)", "BLOB"),
    ("B", "BLOB", "BLOB"),
    ("RID", "ROWID", "VARCHAR"),
]


def test_show_tables_and_columns(oracle, table):
    table.create(", ".join(f"{name} {oracle_type}" for name, oracle_type, _ in EVERY_TYPE))
    view = table.name + "_V"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT N4, V FROM {table.name}")
    try:
        tables, columns, view_columns = results(
            "SHOW TABLES FROM ora;\n"
            "SELECT column_name, data_type FROM information_schema.columns "
            f"WHERE table_catalog = 'ora' AND table_schema = '{owner()}' AND table_name = '{table.name}' "
            "ORDER BY ordinal_position;\n"
            "SELECT column_name, data_type FROM information_schema.columns "
            f"WHERE table_catalog = 'ora' AND table_schema = '{owner()}' AND table_name = '{view}' "
            "ORDER BY ordinal_position;\n")
    finally:
        drop_view(oracle, view)
    names = {row["name"] for row in tables}
    assert {table.name, view} <= names
    assert [(r["column_name"], r["data_type"]) for r in columns] == [(n, duck) for n, _, duck in EVERY_TYPE]
    assert [(r["column_name"], r["data_type"]) for r in view_columns] == [("N4", "SMALLINT"), ("V", "VARCHAR")]


def test_view_and_synonym(oracle, table):
    table.create("ID NUMBER(10), NAME VARCHAR2(20)")
    table.execute(f"INSERT INTO {table.name} SELECT LEVEL, 'name ' || LEVEL FROM dual CONNECT BY LEVEL <= 50")
    oracle.commit()
    view, synonym = table.name + "_V", table.name + "_S"
    with oracle.cursor() as cur:
        cur.execute(f"CREATE VIEW {view} AS SELECT ID, UPPER(NAME) AS NAME FROM {table.name} WHERE MOD(ID, 5) = 0")
        cur.execute(f"CREATE SYNONYM {synonym} FOR {table.name}")
    try:
        through_view, through_synonym = results(
            f"SELECT id, name FROM {attached(view)} ORDER BY id;\n"
            f"SELECT id, name FROM {attached(synonym)} ORDER BY id;\n")
    finally:
        drop_view(oracle, view)
        with oracle.cursor() as cur:
            cur.execute(f"DROP SYNONYM {synonym}")
    assert [(r["ID"], r["NAME"]) for r in through_view] == [(i, f"NAME {i}") for i in range(5, 51, 5)]
    assert [(r["ID"], r["NAME"]) for r in through_synonym] == table.rows("ID, NAME")


def test_pushdown_through_attach(partitioned, duck_copy, stats_conn, table):
    source = attached(partitioned.name)
    proc = run_duckdb(secret_sql() + attach_sql() +
                      f"EXPLAIN (FORMAT json) SELECT * FROM {source} WHERE part_date = DATE '2024-05-07';\n",
                      json_mode=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    ops = operators(json_documents(split_warnings(proc.stdout)[0])[-1])
    scans = [info for _, info in ops if isinstance(info, dict) and "Table" in info]
    assert len(scans) == 1, ops
    assert scans[0]["Table"] == f"{owner()}.{partitioned.name}"
    assert scans[0]["Oracle WHERE"] == "(\"PART_DATE\" = DATE '2024-05-07')"
    assert not any(name == "FILTER" for name, _ in ops), ops

    # Only the target partition is read (expected rows from the DuckDB copy: an Oracle query would add reads)
    (expected,), = results("SELECT count(*) AS n, sum(id)::BIGINT AS s FROM dcopy "
                           "WHERE part_date = DATE '2024-05-07';\n", setup="", db=duck_copy)
    assert expected["n"] > 0
    assert_pruned(stats_conn, partitioned.name, "part_date = DATE '2024-05-07'", [expected], {"P0507"}, 4,
                  sources=[(attach_sql(options="SPLIT 'none'"), source, 0), (attach_sql(), source, 16)])

    table.create("A NUMBER(5), B VARCHAR2(10), C DATE")
    table.execute(f"INSERT INTO {table.name} VALUES (1, 'b', SYSDATE)")
    table.conn.commit()
    (info,) = explain_scan(f"SELECT a FROM {attached(table.name)}", setup=attach_sql(), analyze=True)
    assert info["Oracle SQL"].startswith(f'SELECT /* oraduck_scan */ "A" FROM "{owner()}"."{table.name}"')
    assert '"B"' not in info["Oracle SQL"] and '"C"' not in info["Oracle SQL"]


@pytest.mark.parametrize("query", [q for _, q in PLAN_SHAPES], ids=[name for name, _ in PLAN_SHAPES])
def test_plan_rewrites_keep_pushed_filters(partitioned, duck_copy, query):
    through_attach, in_duckdb = results(
        query.format(t=attached(partitioned.name)) + ";\n" + query.format(t="dcopy") + ";\n", db=duck_copy)
    assert through_attach == in_duckdb


def test_attach_options(million, table):
    source = attached(million.name)
    (info,) = explain_scan(f"SELECT sum(id) FROM {source}", setup=attach_sql(options="SPLIT 'none'") + THREADS,
                           analyze=True)
    assert (info["Split"], info["Strategy"], info["Sessions"]) == ("none", "none", "1")
    (info,) = explain_scan(f"SELECT sum(id) FROM {source}", setup=attach_sql() + THREADS, analyze=True)
    assert int(info["Sessions"]) > 2
    (info,) = explain_scan(f"SELECT sum(id) FROM {source}", setup=attach_sql(options="SESSIONS 2") + THREADS,
                           analyze=True)
    assert 1 <= int(info["Sessions"]) <= 2
    (info,) = explain_scan(f"SELECT sum(id) FROM {source}",
                           setup=attach_sql(options="SPLIT 'hash', SNAPSHOT 'none'") + THREADS, analyze=True)
    assert (info["Strategy"], info["Snapshot"]) == ("hash", "per session")

    table.create("ID NUMBER(5), AMOUNT NUMBER")
    table.execute(f"INSERT INTO {table.name} VALUES (1, 12345678901234567890.5)")
    table.conn.commit()
    query = f"SELECT typeof(amount) AS t, amount::VARCHAR AS a FROM {attached(table.name)}"
    assert scan_json(query, setup=attach_sql())[0]["t"] == "DOUBLE"
    assert scan_json(query, setup=attach_sql(options="UNCONSTRAINED_NUMBER 'VARCHAR'")) == \
        [{"t": "VARCHAR", "a": "12345678901234567890.5"}]


def test_two_attached_databases(oracle, table):
    table.create("ID NUMBER(10), V VARCHAR2(10)")
    fill_ids(table, 500, "ID, V", "n, 'v' || n")
    table.execute(f"GRANT SELECT ON {table.name} TO {env('ORADUCK_ORACLE_READER_USER')}")
    setup = attach_sql() + secret_sql("rd", reader=True) + attach_sql("rd", "rd")
    (rows,) = results(f"SELECT count(*) AS n, sum(a.id)::BIGINT AS s, bool_and(a.v = b.v) AS same "
                      f"FROM ora.{owner()}.{table.name} a JOIN rd.{owner()}.{table.name} b ON a.id = b.id;\n",
                      setup=setup)
    assert rows == [{"n": 500, "s": 500 * 501 // 2, "same": True}]
    # The reader's default schema is its own
    (rows,) = results("USE rd;\nSELECT current_schema() AS s;\n", setup=setup)
    assert rows == [{"s": env("ORADUCK_ORACLE_READER_USER").upper()}]


def test_refresh_by_detach(table):
    table.create("ID NUMBER(5)")
    table.execute(f"INSERT INTO {table.name} VALUES (1)")
    table.conn.commit()
    query = f"SELECT * FROM {attached(table.name)};\n"

    def add_column():
        table.execute(f"ALTER TABLE {table.name} ADD (EXTRA NUMBER(5) DEFAULT 7)")

    (before, cached, refreshed), _, _ = run_in_steps([
        (secret_sql() + attach_sql() + query, add_column),
        (query + "DETACH ora;\n" + attach_sql() + query, None),
    ])
    assert before == [{"ID": 1}]
    assert cached == [{"ID": 1}]
    assert refreshed == [{"ID": 1, "EXTRA": 7}]


READ_ONLY_STATEMENTS = [
    ("insert", "INSERT INTO {t} VALUES (99)"),
    ("create_table", "CREATE TABLE ora.{o}.x (i INTEGER)"),
    ("create_table_as", "CREATE TABLE ora.{o}.x AS SELECT 1 AS i"),
    ("drop_table", "DROP TABLE {t}"),
    ("delete", "DELETE FROM {t}"),
    ("update", "UPDATE {t} SET id = id + 1"),
    ("alter_table", "ALTER TABLE {t} ADD COLUMN j INTEGER"),
    ("create_view", "CREATE VIEW ora.{o}.v AS SELECT 1 AS i"),
    ("create_schema", "CREATE SCHEMA ora.x"),
    ("drop_schema", "DROP SCHEMA ora.{o}"),
    ("create_index", "CREATE INDEX i ON {t} (id)"),
]


@pytest.mark.parametrize("statement", [s for _, s in READ_ONLY_STATEMENTS], ids=[n for n, _ in READ_ONLY_STATEMENTS])
def test_read_only(table, statement):
    table.create("ID NUMBER(5)")
    table.execute(f"INSERT INTO {table.name} VALUES (1)")
    table.conn.commit()
    err = scan_error(statement.format(t=attached(table.name), o=owner()) + ";\n", setup=attach_sql())
    assert READ_ONLY in err, err
    assert table.rows() == [(1,)]


def test_missing_table_and_unsupported_column(table):
    err = scan_error(f"SELECT * FROM ora.{owner()}.nope;\n", setup=attach_sql())
    assert "Table with name nope does not exist" in err, err
    table.create("ID NUMBER(5), X XMLTYPE, NAME VARCHAR2(10)")
    table.execute(f"INSERT INTO {table.name} VALUES (1, XMLTYPE('<a/>'), 'one')")
    table.conn.commit()
    assert scan_json(f"SELECT id FROM {attached(table.name)}", setup=attach_sql()) == [{"ID": 1}]
    err = scan_error(f"SELECT * FROM {attached(table.name)};\n", setup=attach_sql())
    assert '"X"' in err and "XMLTYPE" in err, err


def test_no_session_left_open(oracle, table):
    """The catalog opens a session to attach and to describe, and closes it: none is left while the database stays
    attached."""
    table.create("ID NUMBER(5)")
    table.execute(f"INSERT INTO {table.name} VALUES (1)")
    table.conn.commit()
    output, most, left, _ = run_and_watch_sessions(
        oracle, secret_sql() + attach_sql() + f"SELECT count(*) FROM {attached(table.name)};\n"
        f"SHOW TABLES FROM ora.{owner()};\n")
    assert "Error" not in output, output
    assert most >= 1
    assert left == 0


def test_listings_describe_the_default_schema_once(proxy, table):
    """DuckDB lists the tables of every database for SHOW TABLES and information_schema: the first listing describes
    the default schema (one session), the next ones and the suggestions of a missing-entry error use the cache, even
    with Oracle unreachable."""
    table.create("ID NUMBER(5)")
    counts = []

    def unreachable():
        counts.append(proxy.connections)
        proxy.close()

    documents, err, warnings = run_in_steps([
        (proxy.secret_sql + attach_sql(secret="px"), lambda: counts.append(proxy.connections)),
        ("SHOW TABLES;\n", lambda: counts.append(proxy.connections)),
        ("SHOW TABLES;\nSHOW TABLES FROM ora;\n"
         "SELECT list(table_name) AS t FROM information_schema.tables WHERE table_catalog = 'ora';\n"
         "SELECT list(schema_name) AS s FROM duckdb_schemas() WHERE database_name = 'ora';\n"
         "CREATE TABLE m (i INTEGER);\nSELECT * FROM mm;\n", unreachable),
        ("SHOW TABLES;\nSHOW TABLES FROM ora;\nSELECT * FROM mm;\n", None),
    ], bail=False)
    assert counts[0] == 1, counts  # the ATTACH session
    assert counts[1] - counts[0] <= 1, counts  # the default schema, described once
    assert counts[2] == counts[1], counts
    from_ora = [d for d in documents if d and "name" in d[0] and table.name in {row["name"] for row in d}]
    assert len(from_ora) == 2, documents  # before and after Oracle became unreachable
    assert table.name in next(d for d in documents if d and "t" in d[0])[0]["t"]
    assert [{"s": [owner()]}] in documents, documents
    assert [{"name": "m"}] in documents, documents
    assert err.count("Table with name mm does not exist") == 2, err
    assert "OraDuck" not in err + "".join(warnings), (err, warnings)


def test_listing_with_oracle_unreachable(proxy, table):
    """Oracle unreachable at the first listing: the listing logs a warning and the schema lists nothing, local commands
    work as usual, and the lookup of an Oracle table raises its error."""
    table.create("ID NUMBER(5)")
    documents, err, warnings = run_in_steps([
        (proxy.secret_sql + attach_sql(secret="px"), proxy.close),
        ("CREATE TABLE m (i INTEGER);\nSHOW TABLES;\nSHOW TABLES FROM ora;\nSELECT * FROM mm;\n"
         f"SELECT * FROM {attached(table.name)};\n", None),
    ], bail=False)
    assert documents == [[{"name": "m"}], []], documents
    assert any(f"OraDuck: cannot list the tables of ora.{owner()}: " in w for w in warnings), warnings
    errors = [line for line in err.splitlines() if "Error" in line]
    assert len(errors) == 2, err
    assert "Table with name mm does not exist" in errors[0] and "OraDuck" not in errors[0], err
    assert "OraDuck: " in errors[1] and "ORA-" in errors[1], err


def test_show_all_tables_lists_named_schemas(table):
    reader = env("ORADUCK_ORACLE_READER_USER").upper()
    table.create("ID NUMBER(5)")
    schemas, tables = results(
        f"SELECT count(*) AS n FROM {attached(table.name)};\n"
        f"SHOW TABLES FROM ora.{reader};\n"
        "SELECT list(schema_name ORDER BY schema_name) AS s FROM duckdb_schemas() WHERE database_name = 'ora';\n"
        "SELECT list(DISTINCT schema) AS s, list(name) AS t FROM (SHOW ALL TABLES) WHERE database = 'ora';\n")[-2:]
    assert schemas == [{"s": sorted([owner(), reader])}]
    # READER owns no table; SYS and the other users are never listed
    assert tables[0]["s"] == [owner()]
    assert table.name in tables[0]["t"]


def test_missing_name_looked_up_once(proxy):
    """A name found missing stays missing until DETACH: DuckDB looks a name up twice before reporting it missing."""
    counts = []
    _, err, _ = run_in_steps([
        (proxy.secret_sql + attach_sql(secret="px"), lambda: counts.append(proxy.connections)),
        (f"SELECT * FROM ora.{owner()}.nope;\nSELECT * FROM ora.{owner()}.nope;\nSELECT * FROM ora.NOPE;\n",
         lambda: counts.append(proxy.connections)),
    ], bail=False)
    assert err.count("does not exist") == 3, err
    assert counts[1] - counts[0] == 1, counts


def test_public_synonym_in_default_schema():
    """In the default schema a name falls back to the public synonyms, as Oracle resolves an unqualified name; another
    schema does not."""
    assert scan_json("SELECT count(*) AS n FROM ora.DUAL", setup=attach_sql()) == [{"n": 1}]
    err = scan_error(f"SELECT * FROM ora.{env('ORADUCK_ORACLE_READER_USER').upper()}.DUAL;\n", setup=attach_sql())
    assert "Table with name DUAL does not exist" in err, err
