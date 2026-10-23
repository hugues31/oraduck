"""oraduck_scan filter pushdown (spec 4.3): equivalence with DuckDB, partition pruning, views, filters kept in DuckDB."""

import time
import uuid

import oracledb
import pytest

from conftest import wait_for_snapshot
from oraduck_helpers import (WORKDIR, OracleTable, env, json_documents, run_duckdb, scan_call, scan_json, secret_sql,
                             split_warnings)

ROWS = 30_000
NOTHING = "—"

# LIST partitions on a DATE key, LIST subpartitions on a VARCHAR2 key; every subpartition gets rows. PCTFREE 90
# spreads the rows over hundreds of blocks per subpartition: V$SEGMENT_STATISTICS counts logical reads by steps of 16.
PARTITIONED_DDL = """
    ID NUMBER(10) NOT NULL, PART_DATE DATE, SUB_KEY VARCHAR2(8), N9 NUMBER(9), N18 NUMBER(18), N38 NUMBER(38),
    DEC NUMBER(10,2), AMOUNT NUMBER, BF BINARY_FLOAT, BD BINARY_DOUBLE, TS TIMESTAMP(6), NAME VARCHAR2(40),
    NNAME NVARCHAR2(20), FLAG CHAR(3), NOTE CLOB) PCTFREE 90
    PARTITION BY LIST (PART_DATE) SUBPARTITION BY LIST (SUB_KEY)
    SUBPARTITION TEMPLATE (SUBPARTITION SA VALUES ('A'), SUBPARTITION SB VALUES ('B'),
                           SUBPARTITION SO VALUES (DEFAULT))
    (PARTITION P0506 VALUES (DATE '2024-05-06'), PARTITION P0507 VALUES (DATE '2024-05-07'),
     PARTITION P0508 VALUES (DATE '2024-05-08'), PARTITION POTHER VALUES (DEFAULT)
"""

# Values of row n (n = 1..ROWS): NULLs in every column, '' (NULL in Oracle) in SUB_KEY, a DATE with a time of day in
# the DEFAULT partition, CHAR(3) values 'A  ' and 'AB ', NaN and infinities, multi-byte strings.
PARTITIONED_VALUES = """n,
    CASE WHEN MOD(n, 97) = 0 THEN NULL
         WHEN MOD(n, 5) = 0 THEN DATE '2024-05-06'
         WHEN MOD(n, 5) = 1 THEN DATE '2024-05-07'
         WHEN MOD(n, 5) = 2 THEN DATE '2024-05-08'
         WHEN MOD(n, 5) = 3 THEN DATE '2024-05-09' + MOD(n, 3)
         ELSE TO_DATE('2024-05-07 10:20:30', 'YYYY-MM-DD HH24:MI:SS') END,
    CASE MOD(n, 4) WHEN 0 THEN 'A' WHEN 1 THEN 'B' WHEN 2 THEN 'C' || MOD(n, 7) ELSE '' END,
    CASE WHEN MOD(n, 11) = 0 THEN NULL ELSE MOD(n, 1000) - 500 END,
    n * 1000000,
    CASE WHEN MOD(n, 17) = 0 THEN NULL ELSE (MOD(n, 100) - 50) * 100000000000000000000 END,
    CASE WHEN MOD(n, 13) = 0 THEN NULL ELSE (MOD(n, 2000) - 1000) / 4 END,
    CASE WHEN MOD(n, 19) = 0 THEN NULL WHEN MOD(n, 3) = 0 THEN MOD(n, 7) / 2 WHEN MOD(n, 3) = 1 THEN 0.1
         ELSE n / 3 END,
    CASE WHEN MOD(n, 1001) = 0 THEN BINARY_FLOAT_NAN WHEN MOD(n, 1003) = 0 THEN BINARY_FLOAT_INFINITY
         WHEN MOD(n, 23) = 0 THEN NULL WHEN MOD(n, 29) = 0 THEN TO_BINARY_FLOAT(0.1)
         ELSE TO_BINARY_FLOAT(MOD(n, 100) / 4) END,
    CASE WHEN MOD(n, 1009) = 0 THEN -BINARY_DOUBLE_INFINITY WHEN MOD(n, 31) = 0 THEN NULL
         ELSE TO_BINARY_DOUBLE(MOD(n, 50) - 25) / 8 END,
    CASE WHEN MOD(n, 37) = 0 THEN NULL
         ELSE TIMESTAMP '2024-01-01 00:00:00' + NUMTODSINTERVAL(MOD(n, 1000) * 3600.5, 'SECOND') END,
    CASE MOD(n, 6) WHEN 0 THEN 'é' || MOD(n, 50) WHEN 1 THEN 'Z' || MOD(n, 50)
         WHEN 2 THEN CASE MOD(n, 4) WHEN 0 THEN 'abc' WHEN 1 THEN 'Abc' WHEN 2 THEN 'abc ' ELSE 'ab' END
         WHEN 3 THEN '😀' || MOD(n, 10) WHEN 4 THEN 'a b' || MOD(n, 3) ELSE NULL END,
    CASE WHEN MOD(n, 7) = 0 THEN NULL ELSE N'ü' || MOD(n, 20) END,
    CASE MOD(n, 3) WHEN 0 THEN 'A' WHEN 1 THEN 'AB' ELSE NULL END,
    CASE WHEN MOD(n, 2) = 0 THEN TO_CLOB('x' || n) END
"""

PARTITIONED_COLUMNS = "ID, PART_DATE, SUB_KEY, N9, N18, N38, DEC, AMOUNT, BF, BD, TS, NAME, NNAME, FLAG, NOTE"

PUSHED, KEPT, PARTIAL = "pushed", "kept", "partial"

# (DuckDB predicate, where it runs: PUSHED to Oracle entirely, KEPT in DuckDB entirely, PARTIAL: some AND terms each)
PREDICATES = [
    # NUMBER(9) -> INTEGER
    ("n9 = 7", PUSHED),
    ("n9 <> 7", PUSHED),
    ("n9 < -100", PUSHED),
    ("n9 <= 0", PUSHED),
    ("n9 > 400", PUSHED),
    ("n9 >= 499", PUSHED),
    ("n9 BETWEEN -10 AND 10", PUSHED),
    ("n9 IN (1, 2, 3, -500)", PUSHED),
    ("n9 IS NULL", PUSHED),
    ("n9 IS NOT NULL", PUSHED),
    ("7 = n9", PUSHED),
    ("100 > n9", PUSHED),
    ("n9 = 1.5", KEPT),  # DuckDB casts the column to DECIMAL
    ("n9 NOT IN (1, 2)", KEPT),
    ("n9 IS DISTINCT FROM 5", KEPT),
    # NUMBER(18) -> BIGINT, NUMBER(38) -> DECIMAL(38,0) (128-bit constants)
    ("n18 >= 9000000000", PUSHED),
    ("n38 > 1000000000000000000000", PUSHED),
    ("n38 = 0", PUSHED),
    # NUMBER(10,2) -> DECIMAL(10,2)
    ("dec = 12.25", PUSHED),
    ("dec < -100.5", PUSHED),
    ("dec BETWEEN -1 AND 1.5", PUSHED),
    ("dec IN (0, 0.25, -250)", KEPT),  # DuckDB casts the column to DECIMAL(12,2) for -250
    ("dec IN (0.00, 0.25, -250.00)", PUSHED),
    ("dec > 3", PUSHED),
    # NUMBER without precision -> DOUBLE: only IS [NOT] NULL is exact
    ("amount = 1.5", KEPT),
    ("amount > 0.1", KEPT),
    ("amount IS NULL", PUSHED),
    # BINARY_FLOAT, BINARY_DOUBLE (NaN and infinities in the data)
    ("bf = 2.5", PUSHED),
    ("bf = 0.1", PUSHED),
    ("bf > 20", PUSHED),
    ("bf <> 0.25", PUSHED),
    ("bd < -1.5", PUSHED),
    ("bd >= 0", PUSHED),
    ("bd = 'infinity'::DOUBLE", KEPT),
    # DATE -> TIMESTAMP (the partition key)
    ("part_date = DATE '2024-05-07'", PUSHED),
    ("part_date = TIMESTAMP '2024-05-07 10:20:30'", PUSHED),
    ("part_date = TIMESTAMP '2024-05-07 10:20:30.5'", KEPT),  # fractional seconds: no Oracle DATE has them
    ("part_date > DATE '2024-05-07'", PUSHED),
    ("part_date BETWEEN DATE '2024-05-06' AND DATE '2024-05-07'", PUSHED),
    ("part_date IN (DATE '2024-05-06', DATE '2024-05-08')", PUSHED),
    ("CAST(part_date AS DATE) = DATE '2024-05-07'", PUSHED),
    ("DATE '2024-05-07' = CAST(part_date AS DATE)", PUSHED),
    ("CAST(part_date AS DATE) >= DATE '2024-05-07'", KEPT),
    # TIMESTAMP -> DATE cannot fail: DuckDB rewrites [TRY_]CAST(part_date AS DATE) = d into a range on part_date
    ("TRY_CAST(part_date AS DATE) = DATE '2024-05-07'", PUSHED),
    ("TRY_CAST(name AS INTEGER) = 3", KEPT),
    ("part_date IS NULL", PUSHED),
    # TIMESTAMP(6)
    ("ts >= TIMESTAMP '2024-01-10 12:00:00.5'", PUSHED),
    ("ts < TIMESTAMP '2024-01-02'", PUSHED),
    ("ts = TIMESTAMP '2024-01-01 01:00:00.5'", PUSHED),
    # VARCHAR2 (the subpartition key), string ranges on AL32UTF8 with binary comparison
    ("sub_key = 'A'", PUSHED),
    ("sub_key IN ('A', 'B')", PUSHED),
    ("sub_key <> 'A'", PUSHED),
    ("sub_key <> ''", KEPT),  # '' is NULL in Oracle
    ("sub_key = ''", KEPT),
    ("name > 'Z'", PUSHED),
    ("name < 'é'", PUSHED),
    ("name BETWEEN 'a' AND 'abc'", PUSHED),
    ("name >= '😀'", PUSHED),
    ("name = 'abc'", PUSHED),
    ("name = 'abc '", PUSHED),
    ("length(name) > 3", KEPT),
    ("name LIKE 'Z%'", KEPT),
    # NVARCHAR2: equality only
    ("nname = 'ü3'", PUSHED),
    ("nname > 'ü1'", KEPT),
    # CHAR(3): DuckDB receives the padded value
    ("flag = 'A  '", PUSHED),
    ("flag = 'A'", PUSHED),
    ("flag IN ('AB ', 'A  ')", PUSHED),
    ("flag > 'A'", PUSHED),
    # CLOB: IS [NOT] NULL only
    ("note IS NULL", PUSHED),
    ("note IS NOT NULL", PUSHED),
    ("note = 'x2'", KEPT),
    # AND / OR
    ("n9 = 1 OR sub_key = 'B'", PUSHED),
    ("(part_date = DATE '2024-05-07' AND sub_key = 'A') OR n9 < 0", PUSHED),
    ("n9 = 1 OR length(name) > 3", KEPT),
    ("part_date = DATE '2024-05-07' AND length(name) > 3", PARTIAL),
    ("part_date = DATE '2024-05-07' AND sub_key = 'A'", PUSHED),
]


def explain_tree(sql: str, setup: str = "") -> dict | list:
    """EXPLAIN (FORMAT json) tree of sql."""
    proc = run_duckdb(secret_sql() + setup + f"EXPLAIN (FORMAT json) {sql};\n", json_mode=True)
    assert proc.returncode == 0, f"EXPLAIN failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
    return json_documents(split_warnings(proc.stdout)[0])[-1]


def operators(node: dict | list) -> list[tuple[str, dict]]:
    """(name, extra_info) of every operator of an EXPLAIN (FORMAT json) tree."""
    if isinstance(node, list):
        return [op for child in node for op in operators(child)]
    found = [(node.get("name", "").strip(), node.get("extra_info", {}))]
    return found + operators(node.get("children", []))


def plan_of(sql: str, setup: str = "") -> tuple[str, bool]:
    """Oracle WHERE of the single oraduck_scan of sql, and whether a DuckDB FILTER remains in the plan."""
    ops = operators(explain_tree(sql, setup))
    scans = [info for _, info in ops if isinstance(info, dict) and "Table" in info]
    assert len(scans) == 1, ops
    return scans[0]["Oracle WHERE"], any(name == "FILTER" for name, _ in ops)


def results(sql: str, db: str) -> list[list[dict]]:
    """Every result of a script run after the ora secret (whose result is left out), in -json mode, against the
    DuckDB database db."""
    proc = run_duckdb(secret_sql() + sql, db=db, json_mode=True)
    assert proc.returncode == 0, f"query failed\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
    documents = [d for d in json_documents(split_warnings(proc.stdout)[0]) if isinstance(d, list)]
    assert documents[0] == [{"Success": True}], documents[0]
    return documents[1:]


def logical_reads(conn, table: str) -> dict[str, int]:
    """V$SEGMENT_STATISTICS logical reads of each (sub)partition segment of table."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT subobject_name, value FROM v$segment_statistics WHERE owner = USER AND object_name = :t "
            "AND statistic_name = 'logical reads'", t=table)
        return {name: int(value) for name, value in cur.fetchall()}


def settled_reads(conn, table: str, timeout: float = 60) -> dict[str, int]:
    """Logical reads once two readings a second apart agree (sessions of the query may still be ending)."""
    deadline = time.monotonic() + timeout
    previous = logical_reads(conn, table)
    while True:
        time.sleep(1)
        current = logical_reads(conn, table)
        if current == previous:
            return current
        assert time.monotonic() < deadline, f"segment statistics of {table} keep changing"
        previous = current


def reads_during(conn, table: str, sql: str) -> tuple[list[dict], dict[str, int]]:
    """Result of sql and the logical reads it caused on each segment of table (by steps of 16: V$SEGMENT_STATISTICS
    counts them so). conn is a session that does not read table."""
    before = settled_reads(conn, table)
    rows = scan_json(sql)
    after = settled_reads(conn, table)
    assert after, f"no segment statistics for {table}"
    return rows, {name: after[name] - before.get(name, 0) for name in after}


def assert_pruned(conn, table: str, where: str, rows: list[dict], read: set[str], segments: int) -> None:
    """SELECT count(*) AS n, sum(id) AS s FROM the scan of table WHERE where returns rows and reads only the segments
    in read. With split 'none', the other segments have no logical read at all. With the default split (extents), the
    planner's DBA_EXTENTS query also reads a few blocks of every segment header, which V$SEGMENT_STATISTICS (steps of
    16) may show as one step on another segment; reading a segment's rows costs hundreds."""
    for options, stray in (("split := 'none'", 0), ("", 16)):
        sql = f"SELECT count(*) AS n, sum(id)::BIGINT AS s FROM {scan_call(table, options)} WHERE {where}"
        result, reads = reads_during(conn, table, sql)
        assert result == rows, sql
        assert len(reads) == segments, reads
        assert all(reads[name] > 0 for name in read), (sql, reads)
        assert {name: n for name, n in reads.items() if name not in read and n > stray} == {}, (sql, reads)


@pytest.fixture(scope="module")
def partitioned(oracle):
    t = OracleTable(oracle, f"E2E_F_{uuid.uuid4().hex[:10].upper()}")
    t.create(PARTITIONED_DDL)
    t.execute(
        f"INSERT /*+ APPEND */ INTO {t.name} ({PARTITIONED_COLUMNS}) SELECT {PARTITIONED_VALUES} FROM "
        f"(SELECT (a.i - 1) * 1000 + b.i AS n FROM (SELECT LEVEL AS i FROM dual CONNECT BY LEVEL <= {ROWS // 1000}) a, "
        f"(SELECT LEVEL AS i FROM dual CONNECT BY LEVEL <= 1000) b)")
    oracle.commit()
    t.execute(f"BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{t.name}'); END;")
    wait_for_snapshot(t, ROWS)
    yield t
    t.drop()


@pytest.fixture(scope="module")
def duck_copy(partitioned):
    """DuckDB database holding dcopy, a copy of the partitioned table read without any filter."""
    WORKDIR.mkdir(parents=True, exist_ok=True)
    path = WORKDIR / f"filters_{uuid.uuid4().hex[:8]}.duckdb"
    count = results(f"CREATE TABLE dcopy AS SELECT * FROM {scan_call(partitioned.name)};\n"
                    "SELECT count(*) AS n FROM dcopy;\n", str(path))[-1]
    assert count == [{"n": ROWS}]
    yield str(path)
    path.unlink(missing_ok=True)
    path.with_name(path.name + ".wal").unlink(missing_ok=True)


@pytest.mark.parametrize("predicate,where", PREDICATES, ids=[p for p, _ in PREDICATES])
def test_filter_equivalence(partitioned, duck_copy, predicate, where):
    call = scan_call(partitioned.name)
    through_scan, in_duckdb = results(
        f"SELECT list(id ORDER BY id) AS ids, count(*) AS n FROM {call} WHERE {predicate};\n"
        f"SELECT list(id ORDER BY id) AS ids, count(*) AS n FROM dcopy WHERE {predicate};\n",
        duck_copy,
    )
    assert through_scan == in_duckdb

    oracle_where, duck_filter = plan_of(f"SELECT id FROM {call} WHERE {predicate}")
    if where == PUSHED:
        assert oracle_where != NOTHING and not duck_filter, (oracle_where, duck_filter)
    elif where == KEPT:
        assert oracle_where == NOTHING and duck_filter, (oracle_where, duck_filter)
    else:
        assert oracle_where != NOTHING and duck_filter, (oracle_where, duck_filter)


def test_battery_is_not_trivial(partitioned, duck_copy):
    """At least 40 predicates, most of them selective and not empty, so that the equivalence means something."""
    assert len(PREDICATES) >= 40
    counts = results(
        "".join(f"SELECT count(*) AS n FROM dcopy WHERE {p};\n" for p, _ in PREDICATES), duck_copy)
    selective = [p for (p, _), rows in zip(PREDICATES, counts) if 0 < rows[0]["n"] < ROWS]
    assert len(selective) >= 40, selective


@pytest.fixture
def stats_conn():
    """A session that reads no test table, only V$SEGMENT_STATISTICS."""
    conn = oracledb.connect(user=env("ORADUCK_ORACLE_USER"), password=env("ORADUCK_ORACLE_PASSWORD"),
                            dsn=env("ORADUCK_ORACLE_DSN"))
    yield conn
    conn.close()


def test_partition_pruning(oracle, partitioned, duck_copy, make_table, stats_conn):
    # Expected results from the DuckDB copy: an Oracle query here would add logical reads of its own
    (expected,), (expected_partition,) = results(
        "SELECT count(*) AS n, sum(id)::BIGINT AS s FROM dcopy WHERE part_date = DATE '2024-05-07' AND sub_key = 'A';\n"
        "SELECT count(*) AS n, sum(id)::BIGINT AS s FROM dcopy WHERE part_date = DATE '2024-05-07';\n", duck_copy)
    assert expected["n"] > 0

    assert_pruned(stats_conn, partitioned.name, "part_date = DATE '2024-05-07' AND sub_key = 'A'", [expected],
                  {"P0507_SA"}, 12)
    assert_pruned(stats_conn, partitioned.name, "part_date = DATE '2024-05-07'", [expected_partition],
                  {"P0507_SA", "P0507_SB", "P0507_SO"}, 12)

    # LIST partitions on a VARCHAR2 key, no subpartitions
    region = make_table()
    region.create("ID NUMBER(10) NOT NULL, REGION VARCHAR2(8), PAD VARCHAR2(100)) PCTFREE 90 "
                  "PARTITION BY LIST (REGION) (PARTITION P_EU VALUES ('EU'), PARTITION P_US VALUES ('US'), "
                  "PARTITION P_APAC VALUES ('APAC'), PARTITION P_OTHER VALUES (DEFAULT)")
    region.execute(
        f"INSERT /*+ APPEND */ INTO {region.name} (ID, REGION, PAD) SELECT LEVEL, "
        "CASE MOD(LEVEL, 5) WHEN 0 THEN 'EU' WHEN 1 THEN 'US' WHEN 2 THEN 'APAC' WHEN 3 THEN 'LATAM' END, "
        "RPAD('x', 100, 'x') FROM dual CONNECT BY LEVEL <= 20000")
    oracle.commit()
    wait_for_snapshot(region, 20000)
    assert_pruned(stats_conn, region.name, "region = 'EU'", [{"n": 4000, "s": sum(range(5, 20001, 5))}], {"P_EU"}, 4)


def test_partition_pruning_number_key(oracle, make_table, stats_conn):
    """LIST partitions on a NUMBER(8) key (days written as integers): equality, IN and ranges prune."""
    t = make_table()
    t.create("ID NUMBER(10) NOT NULL, DAY_KEY NUMBER(8), PAD VARCHAR2(100)) PCTFREE 90 PARTITION BY LIST (DAY_KEY) "
             "(PARTITION P0506 VALUES (20240506), PARTITION P0507 VALUES (20240507), "
             "PARTITION P0508 VALUES (20240508), PARTITION POTHER VALUES (DEFAULT)")
    t.execute(f"INSERT /*+ APPEND */ INTO {t.name} (ID, DAY_KEY, PAD) SELECT LEVEL, 20240506 + MOD(LEVEL, 4), "
              "RPAD('x', 100, 'x') FROM dual CONNECT BY LEVEL <= 20000")
    oracle.commit()
    wait_for_snapshot(t, 20000)

    def expected(*offsets: int) -> list[dict]:
        ids = [i for i in range(1, 20001) if i % 4 in offsets]
        return [{"n": len(ids), "s": sum(ids)}]

    assert_pruned(stats_conn, t.name, "day_key = 20240507", expected(1), {"P0507"}, 4)
    assert_pruned(stats_conn, t.name, "day_key IN (20240506, 20240508)", expected(0, 2), {"P0506", "P0508"}, 4)
    # A range also reads the DEFAULT partition, which may hold values inside it
    assert_pruned(stats_conn, t.name, "day_key BETWEEN 20240507 AND 20240508", expected(1, 2),
                  {"P0507", "P0508", "POTHER"}, 4)


def test_view_approach(partitioned):
    view = f"CREATE VIEW toto AS FROM {scan_call(partitioned.name)};\n"
    oracle_where, duck_filter = plan_of("SELECT * FROM toto WHERE part_date = DATE '2024-05-07'", setup=view)
    assert oracle_where == "(\"PART_DATE\" = DATE '2024-05-07')"
    assert not duck_filter

    through_view = scan_json("SELECT list(id ORDER BY id) AS ids FROM toto WHERE part_date = DATE '2024-05-07'",
                             setup=view)
    direct = scan_json(f"SELECT list(id ORDER BY id) AS ids FROM {scan_call(partitioned.name)} "
                       "WHERE part_date = DATE '2024-05-07'")
    assert through_view == direct
    with partitioned.conn.cursor() as cur:
        cur.execute(f"SELECT ID FROM {partitioned.name} WHERE PART_DATE = DATE '2024-05-07' ORDER BY ID")
        assert direct[0]["ids"] == [row[0] for row in cur.fetchall()]


def test_unpushable_filter_stays_in_duckdb(partitioned):
    call = scan_call(partitioned.name)
    oracle_where, duck_filter = plan_of(f"SELECT * FROM {call} WHERE amount = 1.5")
    assert oracle_where == NOTHING
    assert duck_filter
    rows = scan_json(f"SELECT list(id ORDER BY id) AS ids FROM {call} WHERE amount = 1.5")
    with partitioned.conn.cursor() as cur:
        cur.execute(f"SELECT ID FROM {partitioned.name} WHERE AMOUNT = 1.5 ORDER BY ID")
        expected = [row[0] for row in cur.fetchall()]
    assert expected
    assert rows == [{"ids": expected}]


def test_each_reference_keeps_its_own_filters(partitioned, duck_copy):
    """Two scans of one view, or one CTE referenced twice, with different filters: each scan gets its own."""
    view = f"CREATE TEMP VIEW toto AS FROM {scan_call(partitioned.name)};\n"
    queries = [
        "SELECT count(*) AS n, sum(a.id + b.n9)::BIGINT AS s FROM {t} a JOIN {t} b ON a.id = b.id "
        "WHERE a.sub_key = 'A' AND b.part_date = DATE '2024-05-07'",
        "WITH c AS (FROM {t}) SELECT (SELECT count(*) FROM c WHERE n9 = 7) AS x, "
        "(SELECT count(*) FROM c WHERE sub_key = 'B') AS y, (SELECT count(*) FROM c) AS z",
    ]
    for query in queries:
        through_scan, in_duckdb = results(view + query.format(t="toto") + ";\n" + query.format(t="dcopy") + ";\n",
                                          duck_copy)
        assert through_scan == in_duckdb, query


def test_prepared_statement_parameters(partitioned, duck_copy):
    """A parameter is not a constant at planning time: each execution gets the filter of its own value."""
    query = "SELECT count(*) AS n, list(id ORDER BY id) AS ids FROM {t} WHERE n9 = $1 AND sub_key = $2"
    through_scan = results(
        f"PREPARE q AS {query.format(t=scan_call(partitioned.name))};\n"
        "EXECUTE q(1, 'B');\nEXECUTE q(8, 'A');\nEXECUTE q(-500, 'A');\n", duck_copy)
    in_duckdb = results("".join(f"SELECT count(*) AS n, list(id ORDER BY id) AS ids FROM dcopy "
                                f"WHERE n9 = {n} AND sub_key = '{k}';\n" for n, k in [(1, "B"), (8, "A"), (-500, "A")]),
                        duck_copy)
    assert through_scan[-3:] == in_duckdb
    assert all(rows[0]["n"] > 0 for rows in in_duckdb)


def test_cast_to_date_equality(partitioned, duck_copy):
    """CAST(part_date AS DATE) = d: DuckDB itself rewrites it into a range on part_date; without that rewrite, OraDuck
    pushes the same sargable range."""
    predicate = "CAST(part_date AS DATE) = DATE '2024-05-07'"
    call = scan_call(partitioned.name)
    oracle_where, duck_filter = plan_of(f"SELECT id FROM {call} WHERE {predicate}")
    assert oracle_where == "((\"PART_DATE\" >= DATE '2024-05-07') AND (\"PART_DATE\" < DATE '2024-05-08'))"
    assert not duck_filter

    no_rewrite = "SET disabled_optimizers = 'expression_rewriter';\n"
    oracle_where, duck_filter = plan_of(f"SELECT id FROM {call} WHERE {predicate}", setup=no_rewrite)
    assert oracle_where == "(\"PART_DATE\" >= DATE '2024-05-07' AND \"PART_DATE\" < DATE '2024-05-07' + 1)"
    assert not duck_filter
    through_scan, in_duckdb = results(
        no_rewrite + f"SELECT list(id ORDER BY id) AS ids FROM {call} WHERE {predicate};\n"
        f"SELECT list(id ORDER BY id) AS ids FROM dcopy WHERE {predicate};\n", duck_copy)
    assert through_scan == in_duckdb
    assert len(in_duckdb[0]["ids"]) > 0


# Plans that DuckDB's optimizers copy or merge after the filters were pushed: each scan must keep its own Oracle WHERE
# ({t}: the scan or the DuckDB copy)
PLAN_SHAPES = [
    # common subplans: two scans that differ only by their pushed filters
    ("common_subplan", "SELECT (SELECT max(ts) FROM {t} WHERE sub_key = 'A') IS NULL AS a, "
                       "(SELECT max(ts) FROM {t} WHERE sub_key = 'NO_SUCH') IS NULL AS b"),
    ("common_subplan_values", "SELECT (SELECT max(id) FROM {t} WHERE n9 = 7) AS a, "
                              "(SELECT max(id) FROM {t} WHERE n9 = 8) AS b"),
    # window self-join: copies the scan below the window
    ("window", "SELECT id, sub_key, count(*) OVER (PARTITION BY sub_key) AS n FROM {t} "
               "WHERE part_date = DATE '2024-05-07' ORDER BY id"),
    ("window_in", "SELECT id, flag, count(*) OVER (PARTITION BY flag) AS n FROM {t} WHERE n9 IN (1, 2, 3) ORDER BY id"),
    # CTE inlining: copies the definition for each reference
    ("cte_not_materialized", "WITH c AS NOT MATERIALIZED (FROM {t}) SELECT (SELECT count(*) FROM c WHERE n9 = 7) AS x, "
                             "(SELECT max(id) FROM c WHERE sub_key = 'B') AS y"),
    ("cte_limit", "WITH c AS (FROM {t}) SELECT list(id ORDER BY id) AS ids FROM ("
                  "(SELECT id FROM c WHERE n9 = 7 ORDER BY id LIMIT 3) UNION ALL "
                  "(SELECT id FROM c WHERE n9 = 8 ORDER BY id LIMIT 3))"),
    ("limit", "SELECT id FROM {t} WHERE sub_key = 'A' AND n9 > 0 ORDER BY id LIMIT 7"),
    ("limit_no_order", "SELECT count(*) AS n, bool_and(sub_key = 'A' AND n9 > 0) AS ok FROM "
                       "(SELECT * FROM {t} WHERE sub_key = 'A' AND n9 > 0 LIMIT 50)"),
]


@pytest.mark.parametrize("query", [q for _, q in PLAN_SHAPES], ids=[name for name, _ in PLAN_SHAPES])
def test_plan_rewrites_keep_pushed_filters(partitioned, duck_copy, query):
    through_scan, in_duckdb = results(
        query.format(t=scan_call(partitioned.name)) + ";\n" + query.format(t="dcopy") + ";\n", duck_copy)
    assert through_scan == in_duckdb


def test_copy_to_file(partitioned, duck_copy):
    target = WORKDIR / f"filters_{uuid.uuid4().hex[:8]}.csv"
    try:
        through_scan, in_duckdb = results(
            f"COPY (SELECT id FROM {scan_call(partitioned.name)} WHERE sub_key = 'A' AND n9 > 0) TO '{target}';\n"
            f"SELECT list(id ORDER BY id) AS ids FROM read_csv('{target}');\n"
            "SELECT list(id ORDER BY id) AS ids FROM dcopy WHERE sub_key = 'A' AND n9 > 0;\n", duck_copy)[-2:]
        assert through_scan == in_duckdb
    finally:
        target.unlink(missing_ok=True)
