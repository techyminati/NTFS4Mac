#!/bin/bash
# NTFS4Mac installer
#
#   curl -fsSL https://raw.githubusercontent.com/techyminati/NTFS4Mac/main/install.sh | bash
#
# Gets ntfs4mac (the latest prebuilt release, or builds it from source when
# there is none), then runs "sudo ntfs4mac install" to turn on plug and play.
# Run it from inside a source checkout to install what you built yourself.
#
# Everything is downloaded and built in a fresh temporary folder, and that
# folder is the only thing this script ever deletes.
set -euo pipefail

REPO="${NTFS4MAC_REPO:-techyminati/NTFS4Mac}"
ASSET="ntfs4mac-macos.tar.gz"
MIN_MACOS="15.4"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
info() { printf '  %s\n' "$*"; }
die() { printf '\033[31mError:\033[0m %s\n' "$*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ntfs4mac-install.XXXXXX")"
cleanup() {
	case "$WORK" in
	*/ntfs4mac-install.??????) rm -rf -- "$WORK" ;;
	esac
}
trap cleanup EXIT

[ "$(uname -s)" = Darwin ] || die "NTFS4Mac only runs on macOS."
ver="$(sw_vers -productVersion)"
oldest="$(printf '%s\n%s\n' "$MIN_MACOS" "$ver" |
	sort -t. -k1,1n -k2,2n -k3,3n | head -1)"
[ "$oldest" = "$MIN_MACOS" ] ||
	die "NTFS4Mac needs macOS $MIN_MACOS or newer, this Mac has $ver."

bold "Installing NTFS4Mac"

build_from() {	# build_from <source dir>
	local missing="" t
	xcode-select -p >/dev/null 2>&1 && command -v clang >/dev/null ||
		die "building needs Apple's command line tools. Run: xcode-select --install   then run this installer again."
	for t in autoconf automake glibtoolize; do
		command -v "$t" >/dev/null || missing="$missing $t"
	done
	if [ -n "$missing" ]; then
		command -v brew >/dev/null ||
			die "building needs autoconf, automake and libtool. Install Homebrew (https://brew.sh), then run this again."
		info "installing build tools with Homebrew"
		brew install autoconf automake libtool
	fi
	info "building, this takes a minute or two"
	if ! make -C "$1" build/ntfs4mac >"$WORK/build.log" 2>&1; then
		tail -20 "$WORK/build.log" >&2
		die "the build failed"
	fi
	BIN="$1/build/ntfs4mac"
}

BIN=""
here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd -P || true)"
if [ -n "$here" ] && [ -f "$here/src/core/n4m.h" ] && [ -f "$here/Makefile" ]; then
	info "installing from the source in $here"
	build_from "$here"
else
	url="${NTFS4MAC_RELEASE_URL:-https://github.com/$REPO/releases/latest/download}/$ASSET"
	info "downloading the latest release"
	if curl -fsSL -o "$WORK/$ASSET" "$url" &&
			curl -fsSL -o "$WORK/$ASSET.sha256" "$url.sha256"; then
		(cd "$WORK" && shasum -a 256 -c "$ASSET.sha256" >/dev/null) ||
			die "the download is damaged (checksum mismatch), please try again"
		tar -xzf "$WORK/$ASSET" -C "$WORK"
		BIN="$WORK/ntfs4mac/ntfs4mac"
	else
		info "no release found, building from source instead"
		command -v git >/dev/null || die "git is needed to get the source"
		git clone --quiet --depth 1 --recursive \
			"https://github.com/$REPO.git" "$WORK/src" ||
			die "could not download the source"
		build_from "$WORK/src"
	fi
fi

[ -x "$BIN" ] || die "ntfs4mac is missing after the build"
"$BIN" version >/dev/null 2>&1 || die "ntfs4mac does not run on this Mac"

if [ "${NTFS4MAC_SKIP_SETUP:-0}" = 1 ]; then
	info "NTFS4MAC_SKIP_SETUP=1, not running: sudo $BIN install"
	exit 0
fi
bold "Turning on plug and play (enter your Mac password if asked)"
sudo "$BIN" install || die "setup failed"

echo
bold "Done! Plug in an NTFS drive and it shows up in Finder, ready to write to."
info "Drives that are plugged in right now get remounted read-write in a moment."
info "To remove NTFS4Mac later: sudo ntfs4mac uninstall"
