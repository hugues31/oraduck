import json

import pytest

from oraduck_bench import config, duck, read_runner, read_schema

CFG = config.OracleConfig("bench", "pw", "host:1521/svc")
SUMS = read_schema.ReadChecksums(10, 55, 100, 1, 7)


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


def run(tmp_path, methods, runs=2, duck_sums=SUMS, timeouts=None, fake=None, **kw):
    fake = fake or FakeRun()
    records = read_runner.run_read_case(
        CFG, 10, 5, methods, runs, tmp_path / "r.jsonl", timeouts=timeouts if timeouts is not None else set(),
        ensure=lambda cfg, n, c: "RD_10_5", run_method=fake, duck_checksums=lambda: duck_sums,
        oracle_checksums=lambda cfg, table, where: SUMS, **kw)
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
        run(tmp_path, ["oraduck"], duck_sums=read_schema.ReadChecksums(10, 55, 100, 1, 8))


def test_timeout_is_recorded_then_the_method_is_skipped(tmp_path):
    timeouts = set()
    records, fake = run(tmp_path, ["oraduck", "x"], timeouts=timeouts, fake=FakeRun(fail="x"))
    assert [(r["type"], r["method"]) for r in records] == [
        ("measure", "oraduck"), ("timeout", "x"), ("measure", "oraduck"), ("measure", "oraduck")]
    assert timeouts == {("x", 10, 5)}
    records, _ = run(tmp_path, ["oraduck", "x"], runs=1, timeouts=timeouts)
    assert [(r["type"], r["method"]) for r in records][0] == ("skipped", "x")
