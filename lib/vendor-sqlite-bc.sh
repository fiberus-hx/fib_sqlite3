#!/usr/bin/env bash
# Vendor the SQLite "begin-concurrent" branch amalgamation for fib_sqlite3.
#
# Builds in a throwaway dir OUTSIDE the repo (the full fossil checkout must never
# be committed) and copies ONLY the three generated amalgamation files into
# lib/sqlite-begin-concurrent-<YYYYMMDD>/, alongside a FOSSIL_HASH pin. Re-running
# this against a newer branch hash is the update story.
#
# Requirements: fossil, tclsh, a C compiler, make, awk.
# Override the build dir (and keep it) with FIB_SQLITE_BC_BUILD=/some/dir.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRANCH="${SQLITE_BC_BRANCH:-begin-concurrent}"
OUT="$SCRIPT_DIR/sqlite-begin-concurrent-$(date +%Y%m%d)"

KEEP_BUILD=1
if [ -z "${FIB_SQLITE_BC_BUILD:-}" ]; then
  BUILD="$(mktemp -d)"
  KEEP_BUILD=0
else
  BUILD="$FIB_SQLITE_BC_BUILD"
  mkdir -p "$BUILD"
fi
cleanup() { [ "$KEEP_BUILD" -eq 0 ] && rm -rf "$BUILD" || true; }
trap cleanup EXIT

echo ">> build dir: $BUILD"
echo ">> branch:    $BRANCH"
echo ">> output:    $OUT"

# 1. Clone + open the branch
fossil clone https://sqlite.org/src "$BUILD/sqlite.fossil"
SRC="$BUILD/src"
mkdir -p "$SRC"
( cd "$SRC" && fossil open "$BUILD/sqlite.fossil" "$BRANCH" )

# 2. Pin the exact checkout hash
HASH="$( cd "$SRC" && fossil info | awk '/^checkout:/{print $2; exit}' )"
echo ">> checkout hash: $HASH"

# 3. Build the amalgamation (tclsh-driven)
( cd "$SRC" && ./configure >/dev/null && make sqlite3.c )

# 4. Locate the three files and vendor them
C_FILE="$(find "$SRC" -maxdepth 2 -name sqlite3.c   | head -1)"
H_FILE="$(find "$SRC" -maxdepth 2 -name sqlite3.h   | head -1)"
E_FILE="$(find "$SRC" -maxdepth 3 -name sqlite3ext.h | head -1)"
[ -n "$C_FILE" ] && [ -n "$H_FILE" ] && [ -n "$E_FILE" ] || { echo "ERROR: missing generated files"; exit 1; }

mkdir -p "$OUT"
cp -f "$C_FILE" "$H_FILE" "$E_FILE" "$OUT/"
printf '%s  %s\n' "$HASH" "$BRANCH" > "$OUT/FOSSIL_HASH"

VER="$(awk -F'"' '/#define SQLITE_VERSION /{print $2; exit}' "$OUT/sqlite3.h")"
echo ">> vendored SQLITE_VERSION $VER -> $OUT"
grep -q "BEGIN CONCURRENT\|sqlite3_begin_concurrent\|beginConcurrent" "$OUT/sqlite3.c" \
  && echo ">> begin-concurrent markers present" \
  || echo ">> WARNING: no begin-concurrent markers found in sqlite3.c — wrong branch?"
