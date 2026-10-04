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

`install.sh` shows the disclaimer and the CipherOS License 2.0 and stops unless you type `yes`, then installs the latest release (building from source only when there is none). For unattended installs set `NTFS4MAC_ACCEPT_LICENSE=yes`. `NTFS4MAC_FROM_SOURCE=1` always builds instead of downloading (`make install` does that), and `NTFS4MAC_SKIP_SETUP=1` does everything except the final `sudo ntfs4mac install` (handy for testing the installer).

## Tests

Please test changes before trusting them with real data.

- **`make test`** creates a throwaway NTFS image and hammers the engine: creating, writing, reading back, renames of every kind (case-only renames, replacing existing files and folders, moving folders), hard links, symlinks, alternate data streams, Unicode and Windows-illegal names, truncating, files over 4 GiB, timestamps, a 700 file folder listed page by page, a 64 MiB file, remounting to check everything stuck, read only mounts and hibernated Windows volumes. Afterwards the image is checked with the independent `ntfsfix`, `ntfsls` and `ntfscat` tools.
- **`tests/automount-test.sh`** tests plug and play end to end without root: it builds a disk image that looks like a Windows USB stick, "plugs it in" with the daemon running in a test mode that only ever touches that image, writes to it, ejects it, and checks a drive that was already mounted read only gets taken over.

Both scripts only work inside their own fresh temporary folders and refuse to format anything that isn't the image they just created.

For real drives: use a spare one first, then check it on Windows with `chkdsk X:`.

## Releases

GitHub Actions (`.github/workflows/build.yml`) builds and tests every push and pull request. To publish a release:

1. bump the `VERSION` file and commit
2. tag and push: `git tag 0.1.0 && git push a main 0.1.0` (`v0.1.0` works too)

The workflow checks the tag matches `VERSION`, builds the universal binary, runs the tests and publishes a GitHub release with `ntfs4mac-macos.tar.gz` and its `.sha256`. `install.sh` always tries the latest release first, so users don't need Xcode. `make dist` builds the same tarball locally. It carries `LICENSE` and ntfs-3g's own license text as `COPYING.ntfs-3g`.

The repository has to be public for `install.sh` to download releases without logging in.

## Versions

`VERSION` holds the release number. The build stamps it into the binary together with `git describe` (for example `v0.1.0`, `v0.1.0-3-gabc1234` for later commits, `-dirty` with uncommitted changes) and the commit date. `ntfs4mac version` shows it, and every mount writes it to the log, so a log always says which build produced it. `ntfs4mac about` shows the developers, license and credits.

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
