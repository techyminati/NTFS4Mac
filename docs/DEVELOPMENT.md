# Building and hacking on NTFS4Mac

## Building from source

You need the Xcode command line tools and a few build tools from Homebrew:

```sh
xcode-select --install
brew install autoconf automake libtool

git clone --recursive https://github.com/techyminati/NTFS4Mac.git
cd NTFS4Mac
make          # builds libntfs-3g, the engine and build/ntfs4mac
make test     # runs the test suite against throwaway NTFS images
```

The result is `build/ntfs4mac`, a universal (Apple Silicon + Intel) binary. `./install.sh` from inside the checkout builds it and sets up plug and play, or run `sudo build/ntfs4mac install` yourself.

`install.sh` shows the disclaimer and the CipherOS License 2.0 and stops unless you type `yes`. For unattended installs set `NTFS4MAC_ACCEPT_LICENSE=yes`. `NTFS4MAC_SKIP_SETUP=1` does everything except the final `sudo ntfs4mac install` (handy for testing the installer).

## Tests

Please test changes before trusting them with real data.

- **`make test`** creates a throwaway NTFS image and hammers the engine: creating, writing, reading back, renames of every kind (case-only renames, replacing existing files and folders, moving folders), hard links, symlinks, alternate data streams, Unicode and Windows-illegal names, truncating, files over 4 GiB, timestamps, a 700 file folder listed page by page, a 64 MiB file, remounting to check everything stuck, read only mounts and hibernated Windows volumes. Afterwards the image is checked with the independent `ntfsfix`, `ntfsls` and `ntfscat` tools.
- **`tests/automount-test.sh`** tests plug and play end to end without root: it builds a disk image that looks like a Windows USB stick, "plugs it in" with the daemon running in a test mode that only ever touches that image, writes to it, ejects it, and checks a drive that was already mounted read only gets taken over.

Both scripts only work inside their own fresh temporary folders and refuse to format anything that isn't the image they just created.

For real drives: use a spare one first, then check it on Windows with `chkdsk X:`.

## Releases

```sh
make dist
```

creates `dist/ntfs4mac-macos.tar.gz` and its `.sha256`. Upload both to a GitHub release, `install.sh` downloads them from the latest release so users don't need Xcode. The tarball carries `LICENSE` and ntfs-3g's own license text as `COPYING.ntfs-3g`.

## License

NTFS4Mac's own code is under the CipherOS License 2.0 (see `LICENSE`): free for personal use, evaluation, testing, research and study, commercial or third party use only with written permission. ntfs-3g in `vendor/ntfs-3g` and the patches to it in `patches/ntfs-3g` stay under ntfs-3g's own license.

## Project layout

```
src/core/        the NTFS engine, a thread safe API over libntfs-3g (n4m.h)
src/nfs/         local NFSv3 + MOUNT server (RFC 1813)
src/cli/         the ntfs4mac command, plug and play daemon, DiskArbitration helpers
tests/           engine tests, auto-mount test, test image helper
docs/            these docs
vendor/ntfs-3g/  upstream ntfs-3g (git submodule, untouched)
patches/ntfs-3g/ small fixes we apply on top of upstream at build time
scripts/         build helpers
install.sh       the one line installer
```

## Notes on libntfs-3g

- Never have the same inode open twice. Closing an inode syncs its names into the parent folder's index, so close children through `ntfs_inode_close_in_dir()` or close the folder first, the way `vendor/ntfs-3g/src/lowntfs-3g.c` does.
- `ntfs_delete()` closes both inodes it is given, even when it fails.
- libntfs-3g is not thread safe, every engine call takes the volume lock.
- `patches/ntfs-3g/0001-readdir-resume-bitmap-offset.patch` fixes an upstream bug: resuming a directory listing in the middle of a large folder checked the wrong bit of the index bitmap and could skip files. ntfs-3g's own driver never resumes listings, NFS and FSKit do.
