#!/usr/bin/env bash
# Stops the benchmark Oracle container (the database files stay in .deps/oradata)
set -euo pipefail
docker stop oraduck-ora19
