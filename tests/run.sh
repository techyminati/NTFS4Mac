#!/bin/bash
# Runs the NTFS4Mac test suite against throwaway NTFS images.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/ntfs-3g"
# our own fresh temp folder, the only thing we ever delete
TMP="$(mktemp -d "${TMPDIR:-/tmp}/ntfs4mac-test.XXXXXX")"
IMG="${TMP:?}/test.img"
fail=0

cleanup() {
	rm -f -- "${TMP:?}/test.img" "${TMP:?}/fix.log" "${TMP:?}/ls.log"
	rmdir -- "${TMP:?}"
}
trap cleanup EXIT

echo "== making a 512 MiB NTFS image"
"$ROOT/tests/mkimage.sh" "$IMG" 512m N4MTest >/dev/null || {
	echo "could not create the test image"; exit 1; }

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
