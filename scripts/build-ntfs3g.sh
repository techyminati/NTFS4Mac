#!/bin/bash
# Builds libntfs-3g (static) and the ntfsprogs tools from vendor/ntfs-3g
# into build/ntfs-3g. We copy the source first so the submodule stays clean.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/vendor/ntfs-3g"
# Deliberately three levels below the repo: without an explicit aux dir,
# autoconf looks for "install.sh" in ../.. and would take our installer.
WORK="$ROOT/build/src/ntfs-3g"
PREFIX="$ROOT/build/ntfs-3g"
ARCHS="${ARCHS:-arm64 x86_64}"
MIN_MACOS="${MIN_MACOS:-15.4}"

if [ ! -f "$SRC/configure.ac" ]; then
	echo "vendor/ntfs-3g is empty, run: git submodule update --init" >&2
	exit 1
fi

# rebuild when our patches change
PATCH_SUM="$(cat "$ROOT"/patches/ntfs-3g/*.patch 2>/dev/null | shasum | cut -d' ' -f1)"
if [ -f "$PREFIX/lib/libntfs-3g.a" ] && [ "${FORCE:-0}" != "1" ] &&
		[ "$(cat "$PREFIX/.patches" 2>/dev/null)" = "$PATCH_SUM" ]; then
	echo "libntfs-3g already built (FORCE=1 to rebuild)"
	exit 0
fi

ARCH_FLAGS=""
for a in $ARCHS; do ARCH_FLAGS="$ARCH_FLAGS -arch $a"; done

rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK"
rsync -a --exclude .git "$SRC/" "$WORK/"

# fixes we carry on top of upstream, see patches/ntfs-3g
for p in "$ROOT"/patches/ntfs-3g/*.patch; do
	[ -f "$p" ] || continue
	echo "applying $(basename "$p")"
	patch -s -p1 -d "$WORK" < "$p"
done

cd "$WORK"
if ! autoreconf --install --force >"$WORK/autoreconf.log" 2>&1; then
	tail -20 "$WORK/autoreconf.log" >&2
	echo "autoreconf failed" >&2
	exit 1
fi

export CFLAGS="$ARCH_FLAGS -mmacosx-version-min=$MIN_MACOS -O2 -g"
export LDFLAGS="$ARCH_FLAGS -mmacosx-version-min=$MIN_MACOS"

./configure \
	--prefix="$PREFIX" \
	--exec-prefix="$PREFIX" \
	--disable-shared \
	--enable-static \
	--disable-ntfs-3g \
	--enable-ntfsprogs \
	--enable-extras \
	--disable-plugins \
	--disable-mtab \
	--disable-ldconfig \
	--with-uuid=no \
	>/dev/null

make -j"$(sysctl -n hw.ncpu)" >/dev/null
make install >/dev/null

# config.h is needed by anything including the internal headers
cp "$WORK/config.h" "$PREFIX/include/ntfs-3g/config.h"

echo "$PATCH_SUM" > "$PREFIX/.patches"
echo "libntfs-3g built into $PREFIX"
