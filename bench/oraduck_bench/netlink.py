"""Network shaping facts: bytes received on the Oracle bridge and the qdiscs set by infra/net/shape.sh."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from .config import ORACLE_CONTAINER, WORK

NETWORK = "oraduck-net"
SYS_NET = Path("/sys/class/net")
SHAPING_STATE = WORK / "shaping.state"  # written by `shape.sh on` (the container namespace needs root)
LINE_RATE_BPS = 1_000_000_000
LINE_RATE_BYTES_S = 125e6

_UNITS = {"": 1, "k": 1e3, "m": 1e6, "g": 1e9}
_TBF_RATE = re.compile(r"\btbf\b.*?\brate\s+(\d+(?:\.\d+)?)([KkMmGg]?)(bit|bps)", re.IGNORECASE)
_NETEM_DELAY = re.compile(r"\bnetem\b.*?\bdelay\s+(\d+(?:\.\d+)?)(us|ms|s)\b")
_DELAY_US = {"us": 1, "ms": 1000, "s": 1_000_000}


def _docker(*args: str) -> str:
    return subprocess.run(["docker", *args], text=True, capture_output=True, check=True).stdout


def bridge_name(network: str = NETWORK) -> str:
    return "br-" + _docker("network", "inspect", "-f", "{{.Id}}", network).strip()[:12]


def bridge_rx_bytes(bridge: str | None = None) -> int:
    """Bytes received by the host from the Oracle network: the Oracle -> client direction."""
    bridge = bridge or bridge_name()
    return int((SYS_NET / bridge / "statistics" / "rx_bytes").read_text())


def parse_rate_bps(text: str | None) -> int | None:
    match = _TBF_RATE.search(text or "")
    if not match:
        return None
    bits = float(match[1]) * _UNITS[match[2].lower()] * (8 if match[3].lower() == "bps" else 1)
    return round(bits)


def parse_delay_us(text: str | None) -> int | None:
    match = _NETEM_DELAY.search(text or "")
    return round(float(match[1]) * _DELAY_US[match[2]]) if match else None


STATE_HEADER = re.compile(r"^# pid=(\d+) started=(\S+)\n")
_state_note: str | None = None


def _container_identity(container: str = ORACLE_CONTAINER) -> str:
    return _docker("inspect", "-f", "{{.State.Pid}} {{.State.StartedAt}}", container).strip()


def read_state(text: str, identity: str) -> tuple[str | None, str | None]:
    """(qdiscs, problem): the recorded qdiscs when the state file was written for the running container (same
    pid and start time), else None and why."""
    match = STATE_HEADER.match(text)
    if not match:
        return None, "the recorded shaping has no container identity: run `sudo ./infra/net/shape.sh on` again"
    if f"{match[1]} {match[2]}" != identity:
        return None, ("the container was restarted since `shape.sh on` (its eth0 qdiscs are gone): "
                      "run `sudo ./infra/net/shape.sh on` again")
    return text[match.end():], None


def _container_qdiscs(container: str = ORACLE_CONTAINER) -> str | None:
    """The container's eth0 qdiscs: read live when we may enter the namespace, else as recorded by `shape.sh on`
    for this very container instance."""
    global _state_note
    _state_note = None
    try:
        identity = _container_identity(container)
        pid = identity.split()[0]
        proc = subprocess.run(["nsenter", "-t", pid, "-n", "tc", "qdisc", "show", "dev", "eth0"], text=True,
                              capture_output=True)
        if proc.returncode == 0:
            return proc.stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    if not SHAPING_STATE.exists():
        return None
    text, _state_note = read_state(SHAPING_STATE.read_text(), identity)
    return text


def _bridge_qdiscs() -> str | None:
    try:
        return subprocess.run(["tc", "qdisc", "show", "dev", bridge_name()], text=True, capture_output=True,
                              check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def shaping_status(container_text: str | None = ..., bridge_text: str | None = ...) -> dict:
    """Parsed `tc qdisc show` of both directions; the texts are read from the system when not given. `note` says
    why the container side is unknown when the recorded shaping belongs to another container instance."""
    note = None
    if container_text is ...:
        container_text = _container_qdiscs()
        note = _state_note
    if bridge_text is ...:
        bridge_text = _bridge_qdiscs()
    status = {"rate_bps": parse_rate_bps(container_text), "container_delay_us": parse_delay_us(container_text),
              "bridge_delay_us": parse_delay_us(bridge_text)}
    if note:
        status["note"] = note
    return status


def is_line_rate_shaped(status: dict) -> bool:
    """The 1 Gbit/s qdisc is set on the container side, and both directions carry a delay."""
    return (status.get("rate_bps") == LINE_RATE_BPS and bool(status.get("container_delay_us"))
            and bool(status.get("bridge_delay_us")))


def bridge_delay_us() -> int | None:
    """The netem delay left on the Oracle bridge (None when there is none or it cannot be read)."""
    return parse_delay_us(_bridge_qdiscs())
