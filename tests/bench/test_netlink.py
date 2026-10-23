import os
import stat
import subprocess
from pathlib import Path

from oraduck_bench import netlink

ROOT = Path(__file__).resolve().parents[2]

CONTAINER = """qdisc netem 1: root refcnt 2 limit 1000 delay 250us
qdisc tbf 10: parent 1:1 rate 1Gbit burst 1Mb lat 50ms
"""
BRIDGE = "qdisc netem 8001: root refcnt 2 limit 1000 delay 250us\n"


def test_shaping_status_parses_tc_output():
    status = netlink.shaping_status(CONTAINER, BRIDGE)
    assert status["rate_bps"] == 1_000_000_000
    assert status["container_delay_us"] == 250 and status["bridge_delay_us"] == 250
    assert netlink.is_line_rate_shaped(status)


def test_unshaped_and_other_rates():
    plain = "qdisc noqueue 0: root refcnt 2\n"
    status = netlink.shaping_status(plain, plain)
    assert status["rate_bps"] is None and not netlink.is_line_rate_shaped(status)
    slow = netlink.shaping_status("qdisc tbf 10: parent 1:1 rate 100Mbit burst 1Mb lat 50ms\n", BRIDGE)
    assert slow["rate_bps"] == 100_000_000 and not netlink.is_line_rate_shaped(slow)
    ms = netlink.shaping_status("qdisc netem 1: root delay 1.5ms\n", "")
    assert ms["container_delay_us"] == 1500
    assert netlink.shaping_status(None, None)["rate_bps"] is None


def test_bridge_rx_bytes_reads_sysfs(tmp_path, monkeypatch):
    stats = tmp_path / "br-abc" / "statistics"
    stats.mkdir(parents=True)
    (stats / "rx_bytes").write_text("12345\n")
    monkeypatch.setattr(netlink, "SYS_NET", tmp_path)
    assert netlink.bridge_rx_bytes("br-abc") == 12345


def test_bridge_name_from_network_id(monkeypatch):
    monkeypatch.setattr(netlink, "_docker", lambda *a: "4659088bcdd2deadbeef\n")
    assert netlink.bridge_name() == "br-4659088bcdd2"


def _fake_docker(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    docker = bin_dir / "docker"
    docker.write_text('#!/bin/sh\ncase "$1" in network) echo 4659088bcdd2;; inspect) echo 4242;; esac\n')
    docker.chmod(docker.stat().st_mode | stat.S_IEXEC)
    return bin_dir


def test_shape_script_dry_run_builds_the_commands(tmp_path):
    env = {**os.environ, "PATH": f"{_fake_docker(tmp_path)}:{os.environ['PATH']}"}
    out = subprocess.run([str(ROOT / "infra/net/shape.sh"), "on", "1gbit", "0.5", "--dry-run"], env=env,
                         capture_output=True, text=True, check=True).stdout
    assert "nsenter -t 4242 -n tc qdisc replace dev eth0 root handle 1: netem delay 250us" in out
    assert "nsenter -t 4242 -n tc qdisc replace dev eth0 parent 1:1 handle 10: tbf rate 1gbit" in out
    assert "tc qdisc replace dev br-4659088bcdd2 root netem delay 250us" in out
    off = subprocess.run([str(ROOT / "infra/net/shape.sh"), "--dry-run", "off"], env=env, capture_output=True,
                         text=True, check=True).stdout
    assert "tc qdisc del dev br-4659088bcdd2 root" in off
    bad = subprocess.run([str(ROOT / "infra/net/shape.sh"), "on", "1gbit; rm", "--dry-run"], env=env,
                         capture_output=True, text=True)
    assert bad.returncode != 0


def test_compression_dsn_is_a_connect_descriptor():
    from oraduck_bench import cli
    assert cli.compression_dsn("10.210.19.10:1521/ORCLPDB1") == (
        "(DESCRIPTION=(COMPRESSION=on)(ADDRESS=(PROTOCOL=TCP)(HOST=10.210.19.10)(PORT=1521))"
        "(CONNECT_DATA=(SERVICE_NAME=ORCLPDB1)))")


def test_read_shaped_refuses_an_unshaped_link(monkeypatch):
    import pytest
    from oraduck_bench import cli
    monkeypatch.setattr(netlink, "shaping_status", lambda: {"rate_bps": None})
    with pytest.raises(SystemExit, match="not shaped"):
        cli.main(["read-shaped"])


def test_stale_state_is_rejected():
    ident = "4242 2026-10-24T01:00:00Z"
    ok, problem = netlink.read_state(f"# pid=4242 started=2026-10-24T01:00:00Z\n{CONTAINER}", ident)
    assert ok == CONTAINER and problem is None
    for text in (f"# pid=4243 started=2026-10-24T01:00:00Z\n{CONTAINER}",
                 f"# pid=4242 started=2026-10-24T09:00:00Z\n{CONTAINER}", CONTAINER):
        shaped, problem = netlink.read_state(text, ident)
        assert shaped is None and "run `sudo ./infra/net/shape.sh on` again" in problem


def test_shaping_status_ignores_a_state_file_of_another_container(tmp_path, monkeypatch):
    state = tmp_path / "shaping.state"
    state.write_text(f"# pid=1 started=old\n{CONTAINER}")
    monkeypatch.setattr(netlink, "SHAPING_STATE", state)
    monkeypatch.setattr(netlink, "_docker", lambda *a: "2 new\n" if a[0] == "inspect" else "4659088bcdd2\n")
    monkeypatch.setattr(netlink.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 1, "", "denied"))
    status = netlink.shaping_status(bridge_text=BRIDGE)
    assert not netlink.is_line_rate_shaped(status) and "restarted" in status["note"]
    state.write_text(f"# pid=2 started=new\n{CONTAINER}")
    assert netlink.is_line_rate_shaped(netlink.shaping_status(bridge_text=BRIDGE))


def test_shape_script_off_works_without_the_container(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    docker = bin_dir / "docker"
    docker.write_text('#!/bin/sh\ncase "$1" in network) echo 4659088bcdd2;; inspect) echo 0;; esac\n')
    docker.chmod(docker.stat().st_mode | stat.S_IEXEC)
    env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}"}
    proc = subprocess.run([str(ROOT / "infra/net/shape.sh"), "off", "--dry-run"], env=env, capture_output=True,
                          text=True)
    assert proc.returncode == 0
    assert "tc qdisc del dev br-4659088bcdd2 root" in proc.stdout and "not running" in proc.stderr
    assert "nsenter" not in proc.stdout


def test_local_runs_refuse_a_shaped_bridge(monkeypatch):
    import pytest
    from oraduck_bench import cli
    monkeypatch.setattr(netlink, "bridge_delay_us", lambda: 250)
    for command in ("read-smoke", "read-matrix", "read-competitors", "read-compression"):
        with pytest.raises(SystemExit, match="netem delay"):
            cli.main([command])


def test_read_shaped_takes_no_only():
    import pytest
    from oraduck_bench import cli
    with pytest.raises(SystemExit):
        cli.main(["read-shaped", "--only", "1000000x50"])


def test_default_read_report_inputs_leave_out_smoke_runs(tmp_path):
    from oraduck_bench import cli
    for name in ("a-read-smoke.jsonl", "b-read-matrix.jsonl", "c-read-shaped.jsonl", "d-matrix.jsonl"):
        (tmp_path / name).write_text("")
    assert [p.name for p in cli.default_read_results(tmp_path)] == ["b-read-matrix.jsonl", "c-read-shaped.jsonl"]
