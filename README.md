# NTFS4Mac

**Use your Windows (NTFS) drives on your Mac like any other drive.** Plug it in, copy, paste, rename, delete. Done.

Free and open source. No kernel extensions, no macFUSE, nothing to disable in your Mac's security settings.

> **NTFS4Mac is free software.** If you paid for it, you were scammed. Get your money back and report the seller.

## Why I built this

I have a lot of drives formatted as NTFS. Old backups, external disks I share with my Windows PC, random big drives full of stuff. macOS can read NTFS, but it can't write to it. So every time I plugged one in, I had two choices: copy everything off and reformat the drive for the Mac, or live with it being read only.

I really didn't want to reformat all of them just to use them on a Mac. Some still go back and forth to Windows, and some are just too big to shuffle around. The paid NTFS drivers want money again every few macOS releases, and the free ones usually mean lowering your Mac's security so a kernel extension can load. I didn't want either.

And let's be honest about the real reason this exists: Apple's engineers have been too lazy to add NTFS writing in the more than 20 years macOS has been able to read it. Someone had to do it.

So I built NTFS4Mac: full NTFS read and write on a normal, fully secured Mac.

## Install

Open **Terminal** and paste this:

```sh
curl -fsSL https://raw.githubusercontent.com/techyminati/NTFS4Mac/main/install.sh | bash
```

It first shows a short disclaimer and the license, and only installs once you type `yes`. Then it asks for your Mac password once, to set up plug and play. That's the whole install, no restart needed.

Works on macOS 15.4 or newer, on Apple Silicon and Intel Macs.

## Use it

1. **Plug in your NTFS drive.**
2. It shows up in Finder like any other drive, and **you can write to it**.
3. When you're done, **eject it in Finder** and unplug it once it disappears.

That's it.

## Good to know

**It showed up but I can't write to it.** Windows didn't fully shut down last time it used the drive. Windows 10 and 11 have "Fast Startup" on by default, which hibernates instead of shutting down, and writing to a drive in that state can destroy data. So NTFS4Mac plays it safe and mounts it read only. Fix: plug it into the Windows PC, hold **Shift** while clicking **Shut down**, then try again on the Mac. (Turning off Fast Startup in Windows' Power Options fixes it for good.)

**Unplugged without ejecting?** Then the drive shows up read only next time, on purpose: NTFS4Mac marks a drive "in use" while it's mounted, just like Windows does. Plug it into Windows once, Windows checks it automatically, and it's writable on the Mac again.

**Is it safe?** NTFS4Mac uses the same NTFS engine Linux has relied on for over 15 years, never writes to a drive Windows left in a risky state, and makes sure only one program writes to a drive at a time. It's also tested hard.

**Things that work a bit differently:**
- Deleting files on the drive is immediate (there's no Trash on it).
- Finder tags and colors work and are stored invisibly inside the files, so Windows never sees extra `._` files.
- A few special Windows files (OneDrive "online only" files, files compressed by Windows' CompactOS, or encrypted with EFS) can't be opened. You get an error, never broken data.

**Want macOS to leave a drive alone?** Add its name to `/Library/Application Support/NTFS4Mac/ignore`.

**Uninstall:** open Terminal and run

```sh
sudo ntfs4mac uninstall
```

## More

- [Command line usage](docs/USAGE.md): mount or unmount by hand, read only mode, options
- [How it works](docs/HOW-IT-WORKS.md): the technical side, what's supported, limitations, roadmap
- [Troubleshooting](docs/TROUBLESHOOTING.md): when something doesn't work
- [Building and development](docs/DEVELOPMENT.md): build from source, run the tests, make releases

## Developers

- **Aryan Sinha** ([@techyminati](https://github.com/techyminati))

## Credits

- **ntfs-3g** by Tuxera and its many contributors does the heavy NTFS lifting.

## License

NTFS4Mac is licensed under the CipherOS License 2.0, see [LICENSE](LICENSE). The bundled ntfs-3g keeps its own open source licence.
