"""Indexed tables: refused, unless SKIP_INDEX_MAINTENANCE (sqlldr skip_index_maintenance=true)."""

from oraduck_helpers import copy_error, copy_ok

N = 200_000
QUERY = f"SELECT i::INTEGER AS id, 'v' || i AS s FROM range({N}) t(i)"


def _index_status(table, index: str) -> str:
    with table.conn.cursor() as cur:
        cur.execute("SELECT status FROM user_indexes WHERE index_name = :1", [index])
        return cur.fetchone()[0]


def _create(table) -> None:
    table.create("ID NUMBER(10), S VARCHAR2(30)")
    table.execute(f"CREATE INDEX {table.name}_IX ON {table.name} (ID)")
    table.execute(f"CREATE UNIQUE INDEX {table.name}_UX ON {table.name} (S)")


def test_indexed_table_is_refused_and_the_error_names_the_option(table):
    _create(table)
    for options in (", CONNECTION 'ora'", ", CONNECTION 'ora', SKIP_INDEX_MAINTENANCE false"):
        error = copy_error(QUERY, table.name, options=options)
        assert "index" in error and "SKIP_INDEX_MAINTENANCE" in error
    assert table.scalar("COUNT(*)") == 0


def test_skip_index_maintenance_loads_in_parallel_and_leaves_the_indexes_unusable(table):
    _create(table)
    copy_ok(QUERY, table.name, extra=", SKIP_INDEX_MAINTENANCE true", setup="SET threads = 8;\n")
    assert table.scalar("COUNT(*)") == N
    assert table.scalar("SUM(ID)") == N * (N - 1) // 2
    for index in (f"{table.name}_IX", f"{table.name}_UX"):
        assert _index_status(table, index) == "UNUSABLE"
        table.execute(f"ALTER INDEX {index} REBUILD")
        assert _index_status(table, index) == "VALID"
