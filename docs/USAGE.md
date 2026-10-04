# Using NTFS4Mac from the command line

Most people never need this page: after installing, NTFS drives just mount read-write when you plug them in. These commands are for when you want more control.

## Commands

```sh
ntfs4mac list                         # show NTFS drives and how they are mounted
sudo ntfs4mac mount disk4s1           # mount one read-write
sudo ntfs4mac unmount disk4s1         # unmount it safely
sudo ntfs4mac unmount disk4s1 --eject # ...and eject the drive so you can unplug it
ntfs4mac info disk4s1                 # name, NTFS version, size, free space
sudo ntfs4mac install                 # turn on plug and play (auto-mount)
sudo ntfs4mac uninstall               # turn it off and remove NTFS4Mac
```

`disk4s1` is the partition name. `ntfs4mac list` shows it, so does `diskutil list`.

```
$ ntfs4mac list
DEVICE     NAME                           SIZE  STATE                  MOUNTED AT
disk4s1    Backup 2019                  2.0 TB  read-only (macOS)      /Volumes/Backup 2019

$ sudo ntfs4mac mount disk4s1
Mounted read-write at /Volumes/Backup 2019
```

`mount` takes over from the read-only mount macOS made. When you unmount, wait for "safe to unplug" (or for the drive to disappear from Finder) before pulling it out.

## Mount options

```sh
sudo ntfs4mac mount disk4s1 --read-only        # look but don't touch, nothing is written
sudo ntfs4mac mount disk4s1 ~/mnt/win          # mount somewhere other than /Volumes
sudo ntfs4mac mount disk4s1 --foreground       # stay in the terminal and print the log
sudo ntfs4mac mount disk4s1 --remove-hiberfile # see below, loses the hibernated Windows session
sudo ntfs4mac mount disk4s1 --reset-journal    # see below, riskier
```

`ntfs4mac mount` also works on disk image files, which is handy for testing:

```sh
ntfs4mac mount ~/test.img ~/mnt/test
```

## Unmount options

- `--eject` also ejects the whole drive afterwards
- `--force` unmounts even if some app still has files open (that app may lose unsaved changes)

## When it mounts read only

NTFS4Mac only writes to drives Windows left in a clean state:

- **Windows is hibernated.** Windows 10 and 11 have **Fast Startup** on by default, which hibernates instead of really shutting down. Writing to a hibernated drive can destroy data. Fix it by booting Windows and shutting down while holding Shift, or turn Fast Startup off in Power Options. If you are sure you don't need that Windows session, `--remove-hiberfile` deletes the hibernation file and mounts read-write.
- **Windows didn't close the drive cleanly** (it crashed, or the drive was unplugged while in use). The NTFS journal still holds unfinished changes only Windows can replay. Fix it by plugging the drive into Windows and running `chkdsk X: /f`. If that's not possible, `--reset-journal` throws the journal away and mounts read-write anyway. ntfs-3g does that by default, NTFS4Mac doesn't, because it's the riskier choice.

## Plug and play details

`sudo ntfs4mac install`:

- copies `ntfs4mac` to `/usr/local/bin/ntfs4mac`
- adds a LaunchDaemon at `/Library/LaunchDaemons/com.ntfs4mac.automount.plist`
- creates `/Library/Application Support/NTFS4Mac/ignore`

When macOS is about to mount an NTFS drive read only, the daemon steps in and mounts it read-write instead. If that fails, macOS mounts it read only as usual, so a drive never just goes missing. Drives already plugged in when the daemon starts are picked up too. Ejecting in Finder closes the volume cleanly and ejects the drive.

To have macOS keep handling a drive (for example a Boot Camp partition), put its volume name or volume UUID in `/Library/Application Support/NTFS4Mac/ignore`, one per line. `diskutil info disk4s1` shows the UUID.

`sudo ntfs4mac uninstall` removes all three again. Drives mounted at that moment stay mounted until you eject them.

## Logs

- `/var/log/ntfs4mac.log` for anything run with sudo and for the plug and play daemon
- `~/Library/Logs/ntfs4mac.log` when you mount an image without sudo
