#!/bin/bash
# Runs the NTFS4Mac test suite against throwaway NTFS images.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/ntfs-3g"
TMP="${TMPDIR:-/tmp}/ntfs4mac-test.$$"
IMG="$TMP/test.img"
fail=0

mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

echo "== making a 512 MiB NTFS image"
mkfile -n 512m "$IMG"
"$BIN/sbin/mkntfs" -F -f -q -L N4MTest "$IMG" >/dev/null 2>&1 || {
	echo "mkntfs failed"; exit 1; }

echo "== engine test"
"$ROOT/build/enginetest" "$IMG" || fail=1

echo "== independent check with ntfsprogs"
if ! "$BIN/bin/ntfsfix" -n "$IMG" >"$TMP/fix.log" 2>&1; then
	echo "  FAIL ntfsfix -n reported problems:"; sed 's/^/    /' "$TMP/fix.log"
	fail=1
else
	echo "  ntfsfix -n: clean"
fi
if "$BIN/bin/ntfsls" -f "$IMG" -p /Folder >"$TMP/ls.log" 2>&1 &&
		grep -q '^link.txt$' "$TMP/ls.log"; then
	echo "  ntfsls sees Folder/link.txt"
else
	echo "  FAIL ntfsls could not see Folder/link.txt"; cat "$TMP/ls.log"
	fail=1
fi
if [ "$("$BIN/bin/ntfscat" -f "$IMG" /Folder/link.txt 2>/dev/null)" = "hello world" ]; then
	echo "  ntfscat reads the right data"
else
	echo "  FAIL ntfscat returned different data"
	fail=1
fi

if [ $fail = 0 ]; then echo "== ALL PASSED"; else echo "== FAILED"; fi
exit $fail
