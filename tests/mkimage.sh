#!/bin/bash
# Creates a fresh NTFS disk image file for testing.
#
#   tests/mkimage.sh <path.img> <size, eg 256m> [label]
#
# Safety: only ever writes a NEW regular file ending in .img. It refuses
# anything under /dev, any existing path that is not a regular file, and
# never formats a real disk.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MKNTFS="$ROOT/build/ntfs-3g/sbin/mkntfs"
IMG="${1:?usage: mkimage.sh <path.img> <size> [label]}"
SIZE="${2:?usage: mkimage.sh <path.img> <size> [label]}"
LABEL="${3:-N4MTest}"

case "$IMG" in
	/dev/*) echo "mkimage: refusing to touch a device: $IMG" >&2; exit 1 ;;
	*.img) ;;
	*) echo "mkimage: image name must end in .img: $IMG" >&2; exit 1 ;;
esac
if [ -e "$IMG" ] && [ ! -f "$IMG" ]; then
	echo "mkimage: $IMG exists and is not a regular file" >&2
	exit 1
fi
if [ -L "$IMG" ]; then
	echo "mkimage: $IMG is a symlink, refusing" >&2
	exit 1
fi

rm -f -- "$IMG"
mkfile -n "$SIZE" "$IMG"
# check again right before formatting
[ -f "$IMG" ] && [ ! -L "$IMG" ] || { echo "mkimage: not a file" >&2; exit 1; }
"$MKNTFS" -F -f -q -L "$LABEL" "$IMG" >/dev/null 2>&1
echo "$IMG"
