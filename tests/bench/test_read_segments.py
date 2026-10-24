import json

import pytest

from oraduck_bench import config, read_report, read_segments

CFG = config.OracleConfig("bench", "pw", "host:1521/svc")

EXPLAIN_OUTPUT = """
│      ORADUCK_SCAN       │
│    Table: BENCH.RS_SEGMENTS    │
│ Snapshot: auto │
│ Strategy: extents      │
│ Units: 304             │
│ Sessions: 8            │
│ Snapshot: per session  │
│ Planning (ms): 45.6    │
"""


def test_ddl_has_75_small_partitions_and_the_target_one_with_4_subpartitions():
    ddl = read_segments.segments_ddl()
    assert ddl.startswith("CREATE TABLE RS_SEGMENTS (") and "TABLESPACE BENCH" in ddl
    assert ddl.count("PARTITION P_") == 76
    assert "PARTITION P_20240101 VALUES (DATE '2024-01-01')" in ddl
    assert "PARTITION P_20240315 VALUES (DATE '2024-03-15')" in ddl
    assert "PARTITION P_20240507 VALUES (DATE '2024-05-07')" in ddl
    assert "SUBPARTITION SP_A VALUES ('A'), SUBPARTITION SP_B VALUES ('B'), SUBPARTITION SP_C VALUES ('C'), " \
           "SUBPARTITION SP_DEF VALUES (DEFAULT)" in ddl
    assert read_segments.SEGMENTS == 304  # at most ~300 segments: each takes an 8 MB extent


def test_fill_puts_one_row_in_each_small_subpartition_and_the_rows_in_the_target():
    small, target = read_segments.segments_fill_sql()
    assert "CONNECT BY LEVEL <= 75" in small and "CONNECT BY LEVEL <= 4" in small and "CHR(65 + s)" in small
    assert "DATE '2024-01-01' + p" in small
    assert target.startswith("INSERT /*+ APPEND */ INTO RS_SEGMENTS SELECT DATE '2024-05-07'")
    assert "CONNECT BY LEVEL <= 100000" in target and "MOD(n, 4)" in target


def test_statements_filter_on_the_partition_key():
    assert read_segments.statement("count(*)", "RS_SEGMENTS", "default") == (
        "SELECT count(*) FROM oraduck_scan('BENCH.RS_SEGMENTS', connection := 'bench') "
        "WHERE PART_DATE = DATE '2024-05-07';")
    assert read_segments.statement("*", "RS_SEGMENTS", "none") == (
        "CREATE TABLE t AS SELECT * FROM oraduck_scan('BENCH.RS_SEGMENTS', connection := 'bench', split := 'none') "
        "WHERE PART_DATE = DATE '2024-05-07';")


def test_parse_explain():
    assert read_segments.parse_explain(EXPLAIN_OUTPUT) == {"units": 304, "planning_ms": 45.6,
                                                            "snapshot": "per session"}
    assert read_segments.parse_explain("nothing") == {"units": None, "planning_ms": None, "snapshot": None}


def fake_run(tmp_path, fail=False, dropped=None):
    calls, dropped = [], dropped if dropped is not None else []
    times = iter([9.0, 9.0] + [0.5, 0.2, 0.7, 0.3, 0.6, 0.25] + [9.0, 9.0] + [1.5, 1.0, 1.7, 1.2, 1.6, 1.1])

    def timed(cfg, table, split, query, threads):
        calls.append((query, split))
        if fail:
            raise RuntimeError("boom")
        return next(times)

    def explain_fn(cfg, table, split, query, threads):
        return {"units": 304 if split == "default" else 1, "planning_ms": 40.0 if split == "default" else 2.0,
                "snapshot": "per session"}

    records = read_segments.run_segments(CFG, tmp_path / "s.jsonl", runs=3, create=lambda cfg: "RS_SEGMENTS",
                                         drop=lambda cfg, table: dropped.append(table), timed=timed,
                                         explain_fn=explain_fn)
    return records, calls, dropped


def test_run_alternates_the_splits_after_a_warm_up_and_records_the_difference(tmp_path):
    records, calls, dropped = fake_run(tmp_path)
    assert calls == [("count(*)", "default"), ("count(*)", "none")] * 4 + [("*", "default"), ("*", "none")] * 4
    assert dropped == ["RS_SEGMENTS"]
    count, star = records
    assert count["type"] == "segments" and count["query"] == "count(*)" and count["segments"] == 304
    assert count["default"]["runs_s"] == [0.5, 0.7, 0.6] and count["default"]["median_s"] == 0.6
    assert count["none"]["runs_s"] == [0.2, 0.3, 0.25] and count["none"]["median_s"] == 0.25
    assert count["default"]["units"] == 304 and count["none"]["planning_ms"] == 2.0
    assert count["difference_s"] == pytest.approx(0.35)
    assert star["difference_s"] == pytest.approx(0.5)
    lines = [json.loads(line) for line in (tmp_path / "s.jsonl").read_text().splitlines()]
    assert [line["type"] for line in lines] == ["segments", "segments"]


def test_the_table_is_dropped_when_a_run_fails(tmp_path):
    dropped = []
    with pytest.raises(RuntimeError, match="boom"):
        fake_run(tmp_path, fail=True, dropped=dropped)
    assert dropped == ["RS_SEGMENTS"]


def test_report_shows_the_pruned_unit_overhead(tmp_path):
    records, _, _ = fake_run(tmp_path)
    text = read_report.write_read_report([tmp_path / "s.jsonl"], out=tmp_path / "REPORT_READ.md",
                                         analysis=tmp_path / "none.md").read_text()
    section = text.split("## Pruned-unit overhead")[1].split("\n## ")[0]
    assert "304 segments" in section and "100,000 rows" in section
    assert "| `count(*)` | default | 304 | 40.0 | per session | 0.600 | 0.500–0.700 | +0.350 s (1.16 ms per extra unit) |" \
        in section
    assert "| `count(*)` | none | 1 | 2.0 | per session | 0.250 | 0.200–0.300 |  |" in section
    assert "| `*` | default | 304 |" in section


def test_report_without_segments(tmp_path):
    path = tmp_path / "x-read.jsonl"
    path.write_text(json.dumps({"type": "env"}) + "\n")
    text = read_report.write_read_report([path], out=tmp_path / "REPORT_READ.md", analysis=None).read_text()
    assert "## Pruned-unit overhead" in text and "oraduck-bench read-segments" in text


def test_cli_validates_runs():
    from oraduck_bench import cli
    with pytest.raises(SystemExit, match="--runs"):
        cli.main(["read-segments", "--runs", "0"])
