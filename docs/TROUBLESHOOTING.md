# Troubleshooting

**See what happened.** The log has the details:

```sh
tail -50 /var/log/ntfs4mac.log
```

(`~/Library/Logs/ntfs4mac.log` if you mounted an image without sudo.)

**The drive mounted read only.** Windows left it in a state where writing could lose data (usually Fast Startup), or it was unplugged without ejecting last time. Plugging it into Windows once fixes both. See [When it mounts read only](USAGE.md#when-it-mounts-read-only).

**"already mounted by NTFS4Mac".** Only one NTFS4Mac can write to a drive at a time. It is mounted already, check `ntfs4mac list`.

**The drive doesn't show up at all.** Check that plug and play is on: `ls /Library/LaunchDaemons/com.ntfs4mac.automount.plist`. If it's missing, run `sudo ntfs4mac install`. Also check that the drive isn't listed in `/Library/Application Support/NTFS4Mac/ignore`. `ntfs4mac list` shows what NTFS4Mac sees, and `diskutil list` shows what macOS sees. BitLocker encrypted drives aren't supported.

**Finder says the drive is busy when ejecting.** Some app still has a file open. Quit it and try again, or force it with `sudo ntfs4mac unmount disk4s1 --force` (that app may lose unsaved changes).

**A mount got stuck** (Finder spins, "server not responding"). Run `sudo umount -f "/Volumes/Your Drive"`, then plug the drive in again. Please report it together with the log, that's a bug.

**Windows says the drive needs repair.** Let Windows fix it (`chkdsk X: /f`). If it happened after using NTFS4Mac, please report it with the log and what you did before.

**I want macOS to handle a drive itself.** Put its volume name or UUID in `/Library/Application Support/NTFS4Mac/ignore`, one per line.

**Turn it all off.** `sudo ntfs4mac uninstall`.
