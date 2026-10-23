#!/usr/bin/env bash
# Downloads the official DuckDB v1.5.5 CLI into .deps/duckdb-1.5.5 (linux-amd64).
# The benchmark runs the competitor extensions that are not built for DuckDB 2.0 in it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION=v1.5.5
DEST="$ROOT/.deps/duckdb-1.5.5"
case "$(uname -s)-$(uname -m)" in
	Linux-x86_64) ASSET=duckdb_cli-linux-amd64.zip ;;
	*) echo "unsupported platform: $(uname -s)-$(uname -m) (linux x86_64 only)" >&2; exit 2 ;;
esac
mkdir -p "$DEST" "$ROOT/.deps/dl"
ZIP="$ROOT/.deps/dl/duckdb-$VERSION-$ASSET"
if [ ! -f "$ZIP" ]; then
	curl -fL -o "$ZIP" "https://github.com/duckdb/duckdb/releases/download/$VERSION/$ASSET"
fi
if command -v unzip >/dev/null; then
	unzip -qo "$ZIP" -d "$DEST"
else
	7z x -y -o"$DEST" "$ZIP" >/dev/null
fi
chmod +x "$DEST/duckdb"
"$DEST/duckdb" -batch -c "SELECT version();"
