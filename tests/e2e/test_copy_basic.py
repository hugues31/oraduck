from oraduck_helpers import copy_ok


def test_integer_and_varchar(table):
    table.create("ID NUMBER(10), LABEL VARCHAR2(30)")
    copy_ok("SELECT i::INTEGER AS id, 'lab_' || i AS label FROM range(10000) t(i)", table.name)
    assert table.scalar("COUNT(*)") == 10000
    assert table.scalar("SUM(ID)") == sum(range(10000))
    assert table.scalar("SUM(LENGTH(LABEL))") == sum(len(f"lab_{i}") for i in range(10000))


def test_nulls(table):
    table.create("ID NUMBER(19), LABEL VARCHAR2(30)")
    copy_ok(
        "SELECT i AS id, CASE WHEN i % 3 = 0 THEN NULL ELSE 'x' || i END AS label FROM range(3000) t(i)",
        table.name,
    )
    assert table.scalar("COUNT(*)") == 3000
    assert table.scalar("COUNT(LABEL)") == 2000
