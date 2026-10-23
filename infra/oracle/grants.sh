#!/usr/bin/env bash
# Applies the read-path grants, users and tablespaces to the existing Oracle container (idempotent).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/env.sh"
docker exec -i -e READER_PASSWORD="$ORADUCK_ORACLE_READER_PASSWORD" oraduck-ora19 bash -s <"$ROOT/infra/oracle/setup/02_read.sh"
