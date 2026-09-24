import pytest

from oraduck_helpers import copy_ok

N = 3_000_000
QUERY = (
    "SELECT i::INTEGER AS id, 'v' || i AS s, ((i % 100000) / 100)::DECIMAL(15,2) AS m, "
    f"DATE '2000-01-01' + (i % 9000)::INTEGER AS d FROM range({N}) t(i)"
)


@pytest.mark.parametrize("threads", [1, 8])
def test_parallel(table, threads):
    table.create("ID NUMBER(10), S VARCHAR2(30), M NUMBER(15,2), D DATE")
    copy_ok(QUERY, table.name, setup=f"SET threads = {threads};\n")
    assert table.scalar("COUNT(*)") == N
    assert table.scalar("SUM(ID)") == N * (N - 1) // 2
    assert table.scalar("SUM(LENGTH(S))") == sum(1 + len(str(i)) for i in range(N))
    assert table.scalar("COUNT(DISTINCT D)") == 9000


@pytest.mark.parametrize("stream_size", [65536, 16 * 1024 * 1024])
def test_stream_size(table, stream_size):
    table.create("ID NUMBER(10), S VARCHAR2(30)")
    copy_ok("SELECT i::INTEGER AS id, 'v' || i AS s FROM range(200000) t(i)", table.name,
            extra=f", STREAM_SIZE {stream_size}")
    assert table.scalar("COUNT(*)") == 200000
    assert table.scalar("SUM(ID)") == 200000 * 199999 // 2
