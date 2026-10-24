import json

import pytest

from oraduck_bench import config, duck, read_runner, read_schema

CFG = config.OracleConfig("bench", "pw", "host:1521/svc")
SUMS = read_schema.ReadChecksums(10, 55, 100, 1, 7, "12.5", 3, 4, 2)


class FakeRun:
    def __init__(self, fail=None):
        self.calls = []
        self.fail = fail

    def __call__(self, cfg, method, table, threads, where, timeout_s):
        from oraduck_bench.read_methods import Measurement
        self.calls.append(method)
        if self.fail == method:
            raise duck.MethodTimeout("slow")
        return Measurement(method, table, 10, 5, threads, 1.5, None, {})


def run(tmp_path, methods, runs=2, duck_sums=SUMS, timeouts=None, fake=None, d1_decimal=True, **kw):
    fake = fake or FakeRun()
    records = read_runner.run_read_case(
        CFG, 10, 5, methods, runs, tmp_path / "r.jsonl", timeouts=timeouts if timeouts is not None else set(),
        ensure=lambda cfg, n, c: "RD_10_5", run_method=fake, duck_checksums=lambda: duck_sums,
        duck_d1_decimal=lambda: d1_decimal, oracle_checksums=lambda cfg, table, where: SUMS, **kw)
    return records, fake


def test_methods_are_interleaved_after_the_warm_up(tmp_path):
    records, fake = run(tmp_path, ["oraduck", "x"])
    assert fake.calls == ["oraduck", "x"] * 3
    assert [r["warmup"] for r in records] == [True, True, False, False, False, False]
    lines = [json.loads(line) for line in (tmp_path / "r.jsonl").read_text().splitlines()]
    assert all(line["type"] == "measure" and line["suite"] == "read" for line in lines)


def test_no_warm_up(tmp_path):
    _, fake = run(tmp_path, ["oraduck"], runs=1, warmup=False)
    assert fake.calls == ["oraduck"]


def test_checksum_mismatch_raises(tmp_path):
    with pytest.raises(read_runner.ChecksumMismatch):
        run(tmp_path, ["oraduck"], duck_sums=read_schema.ReadChecksums(10, 55, 100, 1, 8, "12.5", 3, 4, 2))
    with pytest.raises(read_runner.ChecksumMismatch, match="exact"):  # D1 loaded as DECIMAL: no tolerance
        run(tmp_path, ["oraduck"], duck_sums=read_schema.ReadChecksums(10, 55, 100, 1, 7, "12.5000000000001", 3, 4, 2))


def test_d1_comparison_is_recorded(tmp_path):
    records, _ = run(tmp_path, ["oraduck"], runs=1, warmup=False)
    assert records[0]["d1_comparison"] == "exact"
    near = read_schema.ReadChecksums(10, 55, 100, 1, 7, "12.5000000000001", 3, 4, 2)
    records, _ = run(tmp_path, ["x"], runs=1, warmup=False, duck_sums=near, d1_decimal=False)
    assert records[0]["type"] == "measure" and records[0]["d1_comparison"] == "relative 1e-12"
    records, _ = run(tmp_path, ["x"], runs=1, warmup=False, duck_sums=near, d1_decimal=True)
    assert records[0]["type"] == "mismatch" and "D1 compared: exact" in records[0]["message"]


def test_timeout_is_recorded_then_the_method_is_skipped(tmp_path):
    timeouts = set()
    records, fake = run(tmp_path, ["oraduck", "x"], timeouts=timeouts, fake=FakeRun(fail="x"))
    assert [(r["type"], r["method"]) for r in records] == [
        ("measure", "oraduck"), ("timeout", "x"), ("measure", "oraduck"), ("measure", "oraduck")]
    assert timeouts == {("x", 10, 5)}
    records, _ = run(tmp_path, ["oraduck", "x"], runs=1, timeouts=timeouts)
    assert [(r["type"], r["method"]) for r in records][0] == ("skipped", "x")


def test_out_db_is_deleted_after_the_last_run(tmp_path, monkeypatch):
    from oraduck_bench import read_methods
    called = []
    monkeypatch.setattr(read_methods, "reset_out_db", lambda: called.append(1))
    run(tmp_path, ["oraduck"], runs=1)
    assert called == [1]


def test_read_external_validates_its_arguments(monkeypatch):
    from oraduck_bench import cli
    monkeypatch.setenv("ORADUCK_EXTERNAL_PASSWORD", "x")
    base = ["read-external", "--dsn", "d", "--user", "u", "--where", "1=1"]
    with pytest.raises(SystemExit, match="OWNER.TABLE"):
        cli.main(base + ["--table", "A.B'; DROP"])
    with pytest.raises(SystemExit, match="--runs"):
        cli.main(base + ["--table", "A.B", "--runs", "0"])
