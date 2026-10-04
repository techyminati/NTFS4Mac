#!/bin/bash
# NTFS4Mac installer
#
#   curl -fsSL https://raw.githubusercontent.com/techyminati/NTFS4Mac/main/install.sh | bash
#
# Shows the disclaimer and the CipherOS License 2.0 and only continues when
# you accept them. Then it downloads the latest prebuilt release (built by
# GitHub Actions), and only when there is none it builds from source: from
# the checkout it was started in, or from a fresh clone. Finally it runs
# "sudo ntfs4mac install" to turn on plug and play.
#
# Everything is downloaded and built in a fresh temporary folder, and that
# folder is the only thing this script ever deletes.
#
# Non-interactive installs: NTFS4MAC_ACCEPT_LICENSE=yes accepts the terms.
# NTFS4MAC_FROM_SOURCE=1 skips the release and builds (make install uses it).
set -euo pipefail

REPO="${NTFS4MAC_REPO:-techyminati/NTFS4Mac}"
ASSET="ntfs4mac-macos.tar.gz"
MIN_MACOS="15.4"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }

# Downloads robustly. One of GitHub's download servers is unreachable from
# some networks, and plain curl waits 30 to 45 seconds on it before trying
# the next one. A short connect timeout skips it within a few seconds.
fetch() {	# fetch <url> <file> [bar]
	local show="-sS"
	if [ "${3:-}" = bar ] && [ -t 2 ]; then
		show="--progress-bar"
	fi
	curl -fL "$show" --connect-timeout 6 --retry 3 --retry-delay 1 \
		--retry-connrefused -o "$2" "$1"
}
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

# running from a source checkout?
here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd -P || true)"
if [ -n "$here" ] && [ -f "$here/src/core/n4m.h" ] && [ -f "$here/Makefile" ]; then
	SRC_HERE="$here"
else
	SRC_HERE=""
fi

# ---- disclaimer and license, nothing happens before they are accepted ----

if [ -n "$SRC_HERE" ]; then
	cp "$SRC_HERE/LICENSE" "$WORK/LICENSE"
else
	curl -fsSL -o "$WORK/LICENSE" \
		"${NTFS4MAC_LICENSE_URL:-https://raw.githubusercontent.com/$REPO/main/LICENSE}" ||
		die "could not download the license terms. Nothing was installed."
fi
[ -s "$WORK/LICENSE" ] || die "the license terms are empty. Nothing was installed."

cat <<'EOF'

==========================================================================
                               DISCLAIMER
==========================================================================

  NTFS4Mac comes with no warranty of any kind. It doesn't touch your
  Mac's own warranty, but it does write to your drives, and that part
  is on you.

  We are not responsible for lost files, corrupted partitions, dead
  disks, angry Windows PCs, missed deadlines, thermonuclear war, or you
  getting fired because the only copy of your presentation was on that
  drive.

  Please do some research if you have any concerns about this software
  before installing it! YOU are choosing to install it, and if you point
  the finger at us for messing up your drives, we will laugh at you.

  NTFS4Mac is free software. If you paid anyone for it, you were
  scammed: ask for your money back and report the seller.

  Back up anything you care about, and always eject before unplugging.

==========================================================================
                    LICENSE: CipherOS License 2.0
==========================================================================

EOF
fold -s -w 78 "$WORK/LICENSE"
echo
echo "=========================================================================="
echo

if [ "${NTFS4MAC_ACCEPT_LICENSE:-}" = yes ]; then
	info "terms accepted with NTFS4MAC_ACCEPT_LICENSE=yes"
else
	# works with curl | bash too: the answer comes from the terminal
	if ! (exec </dev/tty) 2>/dev/null; then
		die "there is no terminal to ask you to accept the terms. Nothing was installed. (Run it in Terminal, or set NTFS4MAC_ACCEPT_LICENSE=yes.)"
	fi
	printf 'Do you accept the disclaimer and the CipherOS License 2.0?\n'
	printf 'Type "yes" to accept and install, anything else cancels: '
	answer=""
	read -r answer </dev/tty || true
	case "$(printf '%s' "$answer" | tr '[:upper:]' '[:lower:]')" in
	yes) ;;
	*)
		echo "Not accepted. Nothing was installed."
		exit 1
		;;
	esac
fi
echo

# ---- get ntfs4mac --------------------------------------------------------

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

# 1. the latest release
BIN=""
if [ "${NTFS4MAC_FROM_SOURCE:-0}" != 1 ]; then
	# the release info also carries the file's sha256, which saves a
	# second trip to the download server
	tag="" asset_url="" digest=""
	if [ -z "${NTFS4MAC_RELEASE_URL:-}" ] && fetch \
			"https://api.github.com/repos/$REPO/releases/latest" \
			"$WORK/release.json" 2>/dev/null; then
		tag="$(plutil -extract tag_name raw -o - "$WORK/release.json" 2>/dev/null || true)"
		i=0
		while name="$(plutil -extract "assets.$i.name" raw -o - "$WORK/release.json" 2>/dev/null)"; do
			if [ "$name" = "$ASSET" ]; then
				asset_url="$(plutil -extract "assets.$i.browser_download_url" raw -o - "$WORK/release.json" 2>/dev/null || true)"
				digest="$(plutil -extract "assets.$i.digest" raw -o - "$WORK/release.json" 2>/dev/null || true)"
				break
			fi
			i=$((i + 1))
		done
	fi
	url="${asset_url:-${NTFS4MAC_RELEASE_URL:-https://github.com/$REPO/releases/latest/download}/$ASSET}"
	info "downloading the latest release${tag:+ ($tag)}"
	if fetch "$url" "$WORK/$ASSET" bar; then
		want=""
		case "$digest" in sha256:*) want="${digest#sha256:}" ;; esac
		if [ -z "$want" ] && fetch "$url.sha256" "$WORK/$ASSET.sha256"; then
			want="$(awk '{ print $1; exit }' "$WORK/$ASSET.sha256")"
		fi
		got="$(shasum -a 256 "$WORK/$ASSET" | awk '{ print $1 }')"
		[ -n "$want" ] && [ "$want" = "$got" ] ||
			die "the download is damaged (checksum mismatch), please try again"
		tar -xzf "$WORK/$ASSET" -C "$WORK"
		BIN="$WORK/ntfs4mac/ntfs4mac"
		if ! "$BIN" version >/dev/null 2>&1; then
			info "the release does not run on this Mac, building instead"
			BIN=""
		fi
	else
		info "no release available, building from source instead"
	fi
fi

# 2. build from source
if [ -z "$BIN" ]; then
	if [ -n "$SRC_HERE" ]; then
		info "building the source in $SRC_HERE"
		build_from "$SRC_HERE"
	else
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
info "Installed $("$BIN" version | head -1)"
info "Drives that are plugged in right now get remounted read-write in a moment."
info "To remove NTFS4Mac later: sudo ntfs4mac uninstall"
