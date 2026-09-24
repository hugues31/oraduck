#!/usr/bin/env bash
# Downloads Oracle Instant Client 19.32 into .deps/instantclient_19_32
#   usage: install.sh [PLATFORM] [PACKAGE...]
#   PLATFORM: linux-x64 (default), linux-arm64, windows-x64
#   PACKAGE:  basic, sdk, tools (default: basic tools; tools = sqlldr, used by the benchmark;
#             the SDK is not needed: OraDuck loads OCI at run time)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLATFORM="${1:-linux-x64}"
shift || true
PACKAGES="${*:-basic tools}"
VERSION=19.32.0.0.0dbru
DIR=1932000
case "$PLATFORM" in
	linux-x64) URL_OS=linux; ZIP_OS=linux.x64; CHECK=libclntsh.so ;;
	linux-arm64) URL_OS=linux; ZIP_OS=linux.arm64; CHECK=libclntsh.so ;;
	windows-x64) URL_OS=nt; ZIP_OS=windows.x64; CHECK=oci.dll ;;
	*) echo "unknown platform: $PLATFORM" >&2; exit 2 ;;
esac
DEST="$ROOT/.deps"
mkdir -p "$DEST/dl"
for pkg in $PACKAGES; do
	zip="instantclient-$pkg-$ZIP_OS-$VERSION.zip"
	if [ ! -f "$DEST/dl/$zip" ]; then
		curl -fL -o "$DEST/dl/$zip" "https://download.oracle.com/otn_software/$URL_OS/instantclient/$DIR/$zip"
	fi
	if command -v unzip >/dev/null; then
		unzip -qo "$DEST/dl/$zip" -d "$DEST"
	else
		7z x -y -o"$DEST" "$DEST/dl/$zip" >/dev/null
	fi
done
ls -l "$DEST/instantclient_19_32/$CHECK"
