#!/usr/bin/env bash
# Creates or starts the benchmark Oracle 19c container, then waits until it is ready.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/env.sh"
NAME=oraduck-ora19
IMAGE=container-registry.oracle.com/database/enterprise:19.3.0.0
# Dedicated network with a fixed IP, reached directly from the host (no port publishing, no DNAT)
NETWORK=oraduck-net
SUBNET=10.210.19.0/24
ORACLE_IP=10.210.19.10
# Database files on btrfs without copy-on-write (chattr +C): steady fsync for the redo logs
ORADATA="$ROOT/.deps/oradata"

now() { date -u -d '-5 seconds' +%Y-%m-%dT%H:%M:%SZ; }

wait_ready() {
	local since="$1"
	for _ in $(seq 1 240); do
		if docker logs --since "$since" "$NAME" 2>&1 | grep -q "DATABASE IS READY TO USE"; then
			echo "Oracle is ready"
			return 0
		fi
		if [ "$(docker inspect -f '{{.State.Running}}' "$NAME")" != "true" ]; then
			echo "Container $NAME stopped" >&2
			docker logs --tail 80 "$NAME" >&2
			return 1
		fi
		sleep 15
	done
	echo "Oracle is not ready after 60 minutes" >&2
	docker logs --tail 80 "$NAME" >&2
	return 1
}

if ! docker network inspect "$NETWORK" >/dev/null 2>&1; then
	docker network create --subnet "$SUBNET" "$NETWORK" >/dev/null
fi

if [ ! -d "$ORADATA" ]; then
	mkdir -p "$ORADATA"
	chattr +C "$ORADATA"
	chmod 777 "$ORADATA" # the container writes as the oracle uid (54321)
fi

if ! docker container inspect "$NAME" >/dev/null 2>&1; then
	since="$(now)"
	docker run -d --name "$NAME" \
		--cpuset-cpus=8-15 --memory=6g --shm-size=1g \
		--ulimit memlock=-1:-1 --cap-add=IPC_LOCK \
		--network "$NETWORK" --ip "$ORACLE_IP" \
		-e ORACLE_SID=ORCLCDB -e ORACLE_PDB=ORCLPDB1 \
		-e ORACLE_PWD="$ORADUCK_ORACLE_SYS_PASSWORD" \
		-e ORACLE_EDITION=enterprise -e ORACLE_CHARACTERSET=AL32UTF8 \
		-e INIT_SGA_SIZE=3072 -e INIT_PGA_SIZE=1024 \
		-e ENABLE_ARCHIVELOG=false \
		-e BENCH_PASSWORD="$ORADUCK_ORACLE_PASSWORD" \
		-v "$ORADATA:/opt/oracle/oradata" \
		-v "$ROOT/infra/oracle/setup:/opt/oracle/scripts/setup:ro" \
		"$IMAGE" >/dev/null
	wait_ready "$since"
	# Restart to apply the SPFILE parameters set by setup/01_bench.sh
	since="$(now)"
	docker restart "$NAME" >/dev/null
	wait_ready "$since"
elif [ "$(docker inspect -f '{{.State.Running}}' "$NAME")" != "true" ]; then
	since="$(now)"
	docker start "$NAME" >/dev/null
	wait_ready "$since"
else
	echo "Oracle is already running"
fi
