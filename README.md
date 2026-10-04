# NTFS4Mac

Read **and write** NTFS drives on your Mac. No kernel extension, no macFUSE, no turning off System Integrity Protection, no paid app.

```
$ ntfs4mac list
DEVICE     NAME                           SIZE  STATE                  MOUNTED AT
disk4s1    Backup 2019                  2.0 TB  read-only (macOS)      /Volumes/Backup 2019

$ sudo ntfs4mac mount disk4s1
Mounted read-write at /Volumes/Backup 2019
```

That's it. The drive shows up in Finder and you can copy, paste, rename and delete like on any other disk.

## Why I built this

I have a lot of drives formatted as NTFS. Old backups, external disks I share with my Windows PC, random big drives full of stuff. macOS can read NTFS just fine, but it can't write to it. So every time I plugged one in, I had two options: copy everything off and reformat the drive for the Mac, or live with it being read-only.

I really didn't want to reformat all of them just to use them on a Mac. Some of them still go back and forth to Windows machines, and some are just too big to shuffle around.

The existing fixes didn't feel right either. The paid NTFS drivers want money again every few macOS releases. The free ones mostly depend on macFUSE, and on Apple Silicon that has usually meant booting into Recovery and lowering your Mac's security so a kernel extension can load. I didn't want to do that to my Mac for a USB drive.

So NTFS4Mac was born: full NTFS read-write that works on a stock, fully secured Mac.

## What it does

- Mounts NTFS partitions **read-write**, Finder copy/paste/rename/delete all work
- Works on Apple Silicon and Intel, macOS 15.4 and newer (built and tested on macOS 26)
- Nothing to install into the system: no kext, no macFUSE, SIP stays on
- Uses **libntfs-3g** for the on-disk work, the same NTFS engine Linux distros have trusted for over 15 years
- Behaves like Windows does:
  - names are case-insensitive but keep their case (`Photo.JPG` and `photo.jpg` are the same file)
  - accented names work whether an app sends them composed or decomposed (NFC/NFD)
  - characters Windows can't store (`: * ? " < > | \`) are mapped the same way Apple's SMB client does it, so Windows can still open those files
  - `.DS_Store` and other dot files are marked hidden, so they don't clutter Windows Explorer
  - the Windows "read-only" attribute shows up as a file without write permission
- Windows symlinks, directory junctions and WSL symlinks all work. New symlinks are created as real Windows symlinks when possible
- Hard links, sparse files, NTFS-compressed files, huge files
- Safe with **Windows Fast Startup**: if Windows left the drive hibernated, NTFS4Mac mounts it read-only instead of risking your data, and tells you why

## How it works

macOS ships with a very solid NFS client built into the kernel. NTFS4Mac runs a small NFS server on your Mac that only listens on `127.0.0.1` and only serves the one drive you mounted. macOS mounts it like a network share, but nothing ever leaves your machine. (FUSE-T uses the same trick.)

```
 Finder and your apps
          |
 macOS NFS client (built into macOS)
          |   127.0.0.1 only
 ntfs4mac local NFS server
          |
 NTFS4Mac engine -> libntfs-3g -> /dev/rdiskN (your drive)
```

The engine (`src/core`) is separate from the NFS part on purpose. A native **FSKit** driver (Apple's official way to do user space file systems) is planned on top of the same engine. FSKit drivers have to be signed with a paid Apple Developer account, which is why the NFS route comes first: it works today on any Mac.

## Building

You need the Xcode command line tools and a few build tools from Homebrew:

```sh
xcode-select --install
brew install autoconf automake libtool

git clone --recursive https://github.com/<you>/NTFS4Mac.git
cd NTFS4Mac
make          # builds libntfs-3g, the engine and build/ntfs4mac
make test     # runs the test suite against throwaway NTFS images
```

Optionally put it on your PATH:

```sh
sudo cp build/ntfs4mac /usr/local/bin/
```

## Using it

Find your NTFS drives:

```sh
ntfs4mac list
```

Mount one read-write (it takes over from the read-only mount macOS made):

```sh
sudo ntfs4mac mount disk4s1
```

When you're done, unmount it before unplugging. Either eject it in Finder, or:

```sh
sudo ntfs4mac unmount disk4s1 --eject
```

Wait for "safe to unplug". NTFS4Mac closes the volume cleanly so Windows won't complain about it next time.

Other handy bits:

```sh
ntfs4mac info disk4s1                          # name, size, free space
sudo ntfs4mac mount disk4s1 --read-only        # look but don't touch
sudo ntfs4mac mount disk4s1 ~/mnt/win          # mount somewhere else
sudo ntfs4mac mount disk4s1 --foreground       # stay in the terminal with logs
```

Logs go to `/var/log/ntfs4mac.log`.

### "It mounted read-only, why?"

Windows didn't fully shut down. Windows 10 and 11 have **Fast Startup** turned on by default, which hibernates instead of shutting down, and writing to a hibernated NTFS drive can destroy data. Best fix: boot Windows, then shut down while holding Shift (or turn Fast Startup off in Power Options). If you know you don't need that Windows session, `--remove-hiberfile` deletes the hibernation file and mounts read-write anyway.

## Testing it safely

Please test before trusting it with the only copy of anything.

1. **`make test`** creates throwaway NTFS images and hammers them: creating, writing, reading back, renames of every kind (including case-only renames and replacing existing files), hard links, symlinks, Unicode and Windows-illegal names, truncating, timestamps, listing a 700 file folder page by page, a 64 MB file, remounting to make sure everything stuck, and finally checks the image with the independent `ntfsfix`, `ntfsls` and `ntfscat` tools.
2. **Then a spare drive.** Use a USB stick formatted as NTFS on Windows, or a drive whose data is backed up. Mount it, copy things on and off, rename, delete, eject.
3. **Then back on Windows.** Open the files, and run `chkdsk X:` to confirm the file system is clean.

## Things to know

NTFS4Mac is young. These are the current rough edges:

- The volume shows up in Finder like a network volume. Deleting is immediate (no Trash) and Spotlight doesn't index it.
- macOS extended attributes (Finder tags, "downloaded from the internet" flags) are stored as hidden `._name` files for now. Windows hides them. Storing them inside NTFS streams is on the list.
- Files compressed with Windows **CompactOS / WOF** or **Data Deduplication** can't be read yet. You get an error, never garbage data. Regular NTFS compression works fine.
- EFS encrypted files can't be read (the keys live in Windows).
- Unix permissions (`chmod`) are mostly ignored, only the write bit is kept (as the Windows read-only attribute).
- No auto-mount on plug in yet, you run `ntfs4mac mount` yourself.

## Roadmap

- [ ] Auto-mount NTFS drives read-write when they're plugged in (LaunchDaemon)
- [ ] Extended attributes stored in NTFS alternate data streams (no more `._` files)
- [ ] Native FSKit driver (real local volume: Trash, Spotlight, Disk Utility)
- [ ] Reading Windows CompactOS (WOF) compressed files
- [ ] `chmod` support using WSL style permission metadata
- [ ] Format and repair NTFS from Disk Utility

## Project layout

```
src/core/        the NTFS engine, a thread safe API over libntfs-3g
src/nfs/         local NFSv3 + MOUNT server (RFC 1813)
src/cli/         the ntfs4mac command, DiskArbitration helpers
tests/           engine test suite and runner
vendor/ntfs-3g/  upstream ntfs-3g (git submodule, untouched)
patches/ntfs-3g/ small fixes we apply on top of upstream at build time
scripts/         build helpers
```

## Credits

- **ntfs-3g** by Tuxera and its many contributors does the heavy NTFS lifting. NTFS4Mac carries one small fix on top of it (see `patches/ntfs-3g`): resuming a directory listing in the middle of a large folder could skip files, which matters for NFS and FSKit where listings arrive in pages.

## License

NTFS4Mac is licensed under the CipherOS License 2.0, see [LICENSE](LICENSE). The bundled ntfs-3g keeps its own open source licence.
