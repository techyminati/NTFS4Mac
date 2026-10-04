# How NTFS4Mac works

## The big picture

macOS ships with a very solid NFS client built into the kernel. NTFS4Mac runs a small NFS server on your Mac that only listens on `127.0.0.1` and only serves the one drive it mounted. macOS mounts it like a network share, but nothing ever leaves your machine. FUSE-T uses the same trick.

```
 Finder and your apps
          |
 macOS NFS client (built into macOS)
          |   127.0.0.1 only
 ntfs4mac local NFS server
          |
 NTFS4Mac engine -> libntfs-3g -> /dev/rdiskN (your drive)
```

This needs no kernel extension, no macFUSE and no changes to your Mac's security settings.

The NTFS work itself is done by **libntfs-3g**, the engine behind ntfs-3g, which Linux distributions have used for over 15 years. NTFS4Mac's engine (`src/core`) wraps it with a small thread safe API, and the NFS server (`src/nfs`) translates between that API and the macOS kernel.

The engine is separate from the NFS part on purpose. A native **FSKit** driver (Apple's official way to do user space file systems, which makes the drive a real local volume) is planned on top of the same engine. FSKit drivers must be signed with a paid Apple Developer account, which is why the NFS route came first: it works on any Mac today.

## Behaving like Windows

- Names are case-insensitive but keep their case, like on Windows. `Photo.JPG` and `photo.jpg` are the same file, and a case-only rename works.
- Accented names work whether an app sends them composed or decomposed (NFC or NFD). New names are stored composed, the way Windows writes them.
- Characters Windows can't store in names (`: * ? " < > | \`, control characters, a trailing space or dot) are stored as Unicode private use characters, the same mapping Apple's SMB client and Windows' Services for Mac use. Windows can still open those files and they come back with the right names on the Mac.
- `.DS_Store` and other names starting with a dot get the Windows hidden attribute, so they don't clutter Windows Explorer. Renaming keeps a hidden flag you set on Windows.
- The Windows "read-only" attribute shows up as a file without write permission (and `chmod u+w` clears it).
- Windows system folders in the root (`$RECYCLE.BIN`, `System Volume Information`, ...) are hidden from listings but still there.

## Mac metadata

NFSv3 has no extended attributes, so macOS normally stores Finder tags, colors, the "downloaded from the internet" flag and so on in a separate `._name` file next to every file. NTFS4Mac catches those and stores the bytes in an NTFS alternate data stream named `com.apple.AppleDouble` on the file itself instead. Windows doesn't show it, it moves along with renames, and no `._` files litter your drives.

## Links and special files

Reading understands everything found on NTFS: native Windows symlinks, directory junctions, WSL symlinks and old Interix symlinks. Absolute Windows targets like `C:\Users\me` are rewritten relative to the volume root (`../../Users/me`) so they work wherever the drive is mounted.

New symlinks with a relative target are stored as real Windows symlinks (directory symlinks when the target is a folder), so Windows follows them too. Absolute Mac paths have no Windows equivalent and are stored as WSL symlinks. Hard links, sparse files, NTFS compressed files and huge files all work.

## Keeping your data safe

- If Windows is hibernated (Fast Startup) or left its journal dirty (crash, drive pulled out), NTFS4Mac mounts read only and says why. It does not wipe the journal unless you explicitly ask with `--reset-journal`.
- Files whose data lives somewhere NTFS4Mac can't decode (Windows CompactOS/WOF compression, Data Deduplication, EFS encryption) return an error, never garbage.
- Rename never drops the file being replaced until the new name is in place. If something fails half way it is kept under a hidden temporary name.
- Data written to a file goes to the disk right away. The drive's own cache is flushed every couple of seconds while writing and always when unmounting.
- Like ntfs-3g, NTFS4Mac doesn't journal its own changes. Always eject before unplugging.

## Speed

Measured on a MacBook's internal SSD through the whole stack (`cp`, macOS NFS client, NTFS4Mac, NTFS on a disk image), timed until the data was really flushed to disk: about **380 MB/s writing** and **720 MB/s reading**. With real drives the drive itself is the limit: a USB hard disk does 100 to 150 MB/s, a USB SSD a lot more.

## Current limitations

- The drive shows up in Finder like a network volume. Deleting is immediate (no Trash) and Spotlight doesn't index it.
- Windows CompactOS (WOF) compressed and deduplicated files can't be read yet. Regular NTFS compression works.
- EFS encrypted files can't be read (the keys live in Windows).
- Unix permissions (`chmod`) are mostly ignored, only the write bit is kept (as the Windows read-only attribute).
- Mac metadata written for a file that doesn't exist yet ends up as a real (hidden on Windows) `._name` file. That's rare.

## Roadmap

- [x] Auto-mount NTFS drives read-write when they're plugged in
- [x] Mac metadata stored in NTFS alternate data streams (no `._` files)
- [ ] Native FSKit driver (real local volume: Trash, Spotlight, Disk Utility)
- [ ] Reading Windows CompactOS (WOF) compressed files
- [ ] `chmod` support using WSL style permission metadata
- [ ] Format and repair NTFS from Disk Utility
