#!/usr/bin/env bash
# Lays out the CI artifacts as a DuckDB extension repository and as release files.
#   usage: DUCKDB_VERSION=v1.5.5 package.sh DIST SITE RELEASE
#   DIST/oraduck-<platform>/oraduck.duckdb_extension          (actions/download-artifact layout)
#   -> SITE/<version>/<platform>/oraduck.duckdb_extension.gz  (read by INSTALL oraduck FROM '<SITE url or path>')
#   -> RELEASE/oraduck-repository.tar.gz                       (the same tree, for offline installs; DuckDB takes
#                                                              the extension name from the file name)
set -euo pipefail
shopt -s nullglob
DIST="$1"
SITE="$2"
RELEASE="$3"
VERSION="${DUCKDB_VERSION:?DUCKDB_VERSION is not set}"
REPOSITORY_URL="https://hugues31.github.io/oraduck"

artifacts=("$DIST"/oraduck-*/oraduck.duckdb_extension)
if [ ${#artifacts[@]} -eq 0 ]; then
	echo "no artifact in $DIST" >&2
	exit 1
fi
mkdir -p "$SITE" "$RELEASE"
platforms=()
for file in "${artifacts[@]}"; do
	platform="$(basename "$(dirname "$file")")"
	platform="${platform#oraduck-}"
	platforms+=("$platform")
	mkdir -p "$SITE/$VERSION/$platform"
	gzip -9 -n -c "$file" >"$SITE/$VERSION/$platform/oraduck.duckdb_extension.gz"
done
tar -czf "$RELEASE/oraduck-repository.tar.gz" -C "$SITE" "$VERSION"

cat >"$SITE/index.html" <<EOF
<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><title>OraDuck extension repository</title></head>
<body>
<h1>OraDuck extension repository</h1>
<p>DuckDB $VERSION, platforms: ${platforms[*]}. Requires Oracle Instant Client 19.</p>
<pre>-- duckdb -unsigned
INSTALL oraduck FROM '$REPOSITORY_URL';
LOAD oraduck;</pre>
<p><a href="https://github.com/hugues31/oraduck">github.com/hugues31/oraduck</a></p>
</body>
</html>
EOF
