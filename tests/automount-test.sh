#!/bin/bash
# Tests the auto-mount daemon end to end against a disk image.
#
# No root needed and real drives are never touched:
#  - the daemon runs in test mode and only handles the image's disk
#  - every step that writes to a /dev node first checks that the node is
#    the disk image we attached (BusProtocol "Disk Image"), or aborts
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/ntfs4mac"
MKNTFS="$ROOT/build/ntfs-3g/sbin/mkntfs"
# real path (/private/var/...), that is how mount(8) shows it
WORK="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/ntfs4mac-auto.XXXXXX")" && pwd -P)"
IMG="${WORK:?}/autodisk.img"
MNT="${WORK:?}/mounts"
DEV=""
DPID=""
fail=0

say() { echo "== $*"; }
bad() { echo "  FAIL $*"; fail=1; }

# true only for a whole disk node that is a disk image
is_image() {
	[[ "$1" =~ ^/dev/disk[0-9]+$ ]] || return 1
	[ "$(diskutil info -plist "$1" 2>/dev/null |
		plutil -extract BusProtocol raw - 2>/dev/null)" = "Disk Image" ]
}

attach() {
	local d
	d="$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$@" \
		"$IMG" | awk 'NR==1 {print $1}')"
	is_image "$d" || { echo "attach gave '$d', not a disk image"; exit 1; }
	echo "$d"
}

our_mount() {
	mount | grep -F "127.0.0.1:/ntfs4mac/$1 on " | sed 's/^.* on \(.*\) (nfs.*$/\1/'
}

has_mount() { test -n "$(our_mount "$1")"; }

wait_for() {	# wait_for <seconds> <command...>
	local n=$(( $1 * 4 )); shift
	while [ $n -gt 0 ]; do
		"$@" && return 0
		sleep 0.25
		n=$(( n - 1 ))
	done
	return 1
}

cleanup() {
	set +e
	if [ -n "$DPID" ]; then
		kill "$DPID" 2>/dev/null
		wait "$DPID" 2>/dev/null
	fi
	mount | grep -F " on $MNT/" | sed 's/^.* on \(.*\) (nfs.*$/\1/' |
		while read -r m; do umount "$m" 2>/dev/null; done
	if [ -n "$DEV" ] && is_image "$DEV"; then
		hdiutil detach "$DEV" -force >/dev/null 2>&1
	fi
	rm -f -- "$IMG" "$WORK/daemon.log"
	[ -d "$MNT" ] && find "$MNT" -mindepth 1 -maxdepth 1 -type d -empty \
		-exec rmdir {} \; 2>/dev/null
	rmdir -- "$MNT" "$WORK" 2>/dev/null
}
trap cleanup EXIT

mkdir -p "$MNT"

say "making a disk image that looks like a Windows USB drive"
mkfile -n 300m "$IMG"
DEV="$(attach -nomount)"
is_image "$DEV"
diskutil partitionDisk "$DEV" MBR "MS-DOS FAT32" AUTOTMP 100% >/dev/null
diskutil unmount "${DEV}s1" >/dev/null 2>&1 || true
is_image "$DEV"
"$MKNTFS" -f -q -L "Auto Test" "${DEV}s1" >/dev/null 2>&1
printf 'setpid 1\n07\nwrite\nquit\n' | fdisk -e "/dev/r${DEV#/dev/}" >/dev/null 2>&1
hdiutil detach "$DEV" >/dev/null
DEV=""

say "plugging it in with the daemon running"
DEV="$(attach -nomount)"
NAME="${DEV#/dev/}"
"$BIN" daemon --test-only "$NAME" --mount-dir "$MNT" >"$WORK/daemon.log" 2>&1 &
DPID=$!
sleep 1
diskutil mount "${DEV}s1" >/dev/null 2>&1 || true	# the daemon declines this
if wait_for 20 has_mount "${NAME}s1"; then
	MP="$(our_mount "${NAME}s1")"
	echo "  mounted read-write at $MP"
	echo "written by the daemon test" > "$MP/hello.txt"
	[ "$(cat "$MP/hello.txt")" = "written by the daemon test" ] ||
		bad "read back"
	mount | grep -F "${DEV}s1 on" && bad "macOS also mounted it"
else
	bad "daemon did not mount it"
	sed 's/^/    /' "$WORK/daemon.log"
fi

say "ejecting it like Finder does"
if [ -n "${MP:-}" ]; then
	umount "$MP"
	if wait_for 20 bash -c "! diskutil info '$DEV' >/dev/null 2>&1"; then
		echo "  volume closed and drive ejected"
		DEV=""
	else
		bad "drive was not ejected"
	fi
fi
kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null || true
DPID=""

say "drive already mounted read only when the daemon starts"
if [ -n "$DEV" ] && is_image "$DEV"; then
	hdiutil detach "$DEV" -force >/dev/null || true
fi
DEV="$(attach)"			# macOS mounts it read only by itself
NAME="${DEV#/dev/}"
wait_for 10 bash -c "mount | grep -qF '${DEV}s1 on'" ||
	bad "macOS did not mount it"
"$BIN" daemon --test-only "$NAME" --mount-dir "$MNT" >>"$WORK/daemon.log" 2>&1 &
DPID=$!
if wait_for 20 has_mount "${NAME}s1"; then
	MP="$(our_mount "${NAME}s1")"
	echo "  taken over, read-write at $MP"
	[ "$(cat "$MP/hello.txt" 2>/dev/null)" = "written by the daemon test" ] ||
		bad "file from the first round is missing"
	umount "$MP"
	wait_for 20 bash -c "! diskutil info '$DEV' >/dev/null 2>&1" &&
		DEV=""
else
	bad "daemon did not take it over"
	sed 's/^/    /' "$WORK/daemon.log"
fi

if [ $fail = 0 ]; then say "ALL PASSED"; else say "FAILED"; fi
exit $fail
