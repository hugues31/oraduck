#!/usr/bin/env bash
# Builds Fakelake (soma-smart/Fakelake) into .deps/fakelake
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cargo install --locked --git https://github.com/soma-smart/Fakelake --root "$ROOT/.deps/fakelake"
"$ROOT/.deps/fakelake/bin/fakelake" --version
