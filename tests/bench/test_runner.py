import json

from oraduck_bench import config, runner


def test_run_case_interleaves_and_records(tmp_path):
    cfg = config.OracleConfig.from_env()
    out = tmp_path / "r.jsonl"
    records = runner.run_case(cfg, 10_000, 5, 2, "test", out, runs=2, warmup=True)
    assert [(r["method"], r["warmup"]) for r in records] == [
        ("oraduck", True), ("sqlldr", True),
        ("oraduck", False), ("sqlldr", False),
        ("oraduck", False), ("sqlldr", False),
    ]
    lines = [json.loads(line) for line in out.read_text().splitlines()]
    assert len(lines) == 6
    assert all(line["type"] == "measure" and line["suite"] == "test" for line in lines)


def test_results_file_starts_with_environment(tmp_path, monkeypatch):
    monkeypatch.setattr(runner, "RESULTS", tmp_path)
    path = runner.new_results_file(config.OracleConfig.from_env(), "test")
    env = json.loads(path.read_text().splitlines()[0])
    assert env["type"] == "env"
    assert env["duckdb"].startswith("v1.5.5")
    assert env["oci_client"].startswith("19.32")
    assert env["oracle"].startswith("19.")


def test_competitor_case_records_a_timeout_then_skips_the_method(tmp_path):
    cfg = config.OracleConfig.from_env()
    out = tmp_path / "c.jsonl"
    timeouts = set()
    series = (("oraduck", 8), ("rinie_oracle", 8))
    records = runner.run_competitor_case(cfg, 10_000, 5, out, timeouts, series=series, runs=1, timeout_s=5)
    assert [(r["type"], r["method"], r["warmup"]) for r in records] == [
        ("measure", "oraduck", True), ("timeout", "rinie_oracle", True), ("measure", "oraduck", False)]
    assert records[1]["limit_s"] == 5 and records[1]["threads"] == 8
    assert timeouts == {("rinie_oracle", 10_000, 5)}
    records = runner.run_competitor_case(cfg, 10_000, 5, out, timeouts, series=series, runs=1, timeout_s=5)
    assert [(r["type"], r["method"]) for r in records] == [
        ("skipped", "rinie_oracle"), ("measure", "oraduck"), ("measure", "oraduck")]
    lines = [json.loads(line) for line in out.read_text().splitlines()]
    assert len(lines) == 6 and all(line["suite"] == "competitors" for line in lines)


def test_competitor_results_file_records_the_extension_versions(tmp_path, monkeypatch):
    monkeypatch.setattr(runner, "RESULTS", tmp_path)
    path = runner.new_results_file(config.OracleConfig.from_env(), "competitors")
    env = json.loads(path.read_text().splitlines()[0])
    assert env["competitors"] == {"quack_oracle": "0.2.2", "rinie_oracle": "20d4389"}
