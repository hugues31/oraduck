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
