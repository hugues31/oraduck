#!/usr/bin/env bash
# Shapes the link between the host and the benchmark Oracle container to a data-centre-like network.
#   sudo ./infra/net/shape.sh on [RATE] [RTT_MS]   (defaults 1gbit, 0.5)
#   sudo ./infra/net/shape.sh off
#        ./infra/net/shape.sh status
# Oracle -> client: inside the container's network namespace, on eth0, netem delay RTT/2 with a tbf rate limit
# as its child. Client -> Oracle: netem delay RTT/2 on the Docker bridge. `--dry-run` prints the tc/nsenter
# commands instead of running them (no root needed).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NAME=oraduck-ora19
NETWORK=oraduck-net
STATE="$ROOT/bench/work/shaping.state"

DRY=0
ARGS=()
for arg in "$@"; do
	if [ "$arg" = "--dry-run" ]; then DRY=1; else ARGS+=("$arg"); fi
done
CMD="${ARGS[0]:-}"
RATE="${ARGS[1]:-1gbit}"
RTT_MS="${ARGS[2]:-0.5}"

usage() {
	echo "usage: [sudo] $0 on [RATE] [RTT_MS] | off | status  [--dry-run]" >&2
	exit 2
}

[[ "$CMD" =~ ^(on|off|status)$ ]] || usage
[[ "$RATE" =~ ^[0-9]+(\.[0-9]+)?([KkMmGg]?bit|[KkMmGg]?bps)$ ]] || { echo "invalid RATE: $RATE" >&2; exit 2; }
[[ "$RTT_MS" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "invalid RTT_MS: $RTT_MS" >&2; exit 2; }

run() {
	if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi
}

BRIDGE="br-$(docker network inspect -f '{{printf "%.12s" .Id}}' "$NETWORK")"
PID="$(docker inspect -f '{{.State.Pid}}' "$NAME" 2>/dev/null || true)"
STARTED="$(docker inspect -f '{{.State.StartedAt}}' "$NAME" 2>/dev/null || true)"
RUNNING=1
if [ -z "$PID" ] || [ "$PID" = 0 ]; then
	RUNNING=0
	if [ "$CMD" != off ]; then
		echo "container $NAME is not running" >&2
		exit 1
	fi
fi
# half of the round-trip time, in microseconds
HALF_US="$(awk -v ms="$RTT_MS" 'BEGIN { printf "%d", ms * 1000 / 2 }')"
NS=(nsenter -t "$PID" -n)

show() {
	echo "== container eth0 (Oracle -> client) =="
	if [ "$DRY" = 1 ]; then
		echo "+ ${NS[*]} tc qdisc show dev eth0"
	elif [ "$(id -u)" = 0 ]; then
		"${NS[@]}" tc qdisc show dev eth0
	elif [ -f "$STATE" ]; then
		if [ "$(head -n1 "$STATE")" = "# pid=$PID started=$STARTED" ]; then
			echo "(namespace needs root; recorded by the last 'on')"
			grep -v '^#' "$STATE"
		else
			echo "(namespace needs root; the recorded shaping is from another container instance: stale)"
		fi
	else
		echo "(namespace needs root; no shaping recorded)"
	fi
	echo "== bridge $BRIDGE (client -> Oracle) =="
	run tc qdisc show dev "$BRIDGE"
}

case "$CMD" in
on)
	run "${NS[@]}" tc qdisc replace dev eth0 root handle 1: netem delay "${HALF_US}us"
	run "${NS[@]}" tc qdisc replace dev eth0 parent 1:1 handle 10: tbf rate "$RATE" burst 1mb latency 50ms
	run tc qdisc replace dev "$BRIDGE" root netem delay "${HALF_US}us"
	if [ "$DRY" != 1 ]; then
		# the container namespace is readable by root only: keep what was set for unprivileged `status` and the benchmark
		mkdir -p "$(dirname "$STATE")"
		# the header ties the record to this container instance (a restart drops the eth0 qdiscs)
		{ echo "# pid=$PID started=$STARTED"; "${NS[@]}" tc qdisc show dev eth0; } >"$STATE"
		[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$STATE" || true
	fi
	show
	;;
off)
	if [ "$RUNNING" = 1 ]; then
		run "${NS[@]}" tc qdisc del dev eth0 root || true
	else
		echo "warning: container $NAME is not running: its eth0 qdiscs went away with it" >&2
	fi
	run tc qdisc del dev "$BRIDGE" root || true
	[ "$DRY" = 1 ] || rm -f "$STATE"
	;;
status)
	show
	;;
esac
