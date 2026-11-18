# Unwake - Directory Caching for Unraid User Shares

Unwake keeps every folder of your user shares in RAM, so browsing them doesn't wake sleeping disks. Not for a while after the last browse, not until a backup pushes the kernel's caches out - for good, kept current as files change. The main goal is fewer disk spin-ups; faster folder listings come for free.

It replaces Dynamix Cache Directories, and it's the brother of [Unspin](https://github.com/kaedinger/unspin), which has the same goal for file data: Unspin keeps reading files from waking your disks, Unwake keeps browsing from doing so.

## Why

To list a folder of a user share, `shfs` asks every disk that may hold a part of it. If that disk's folder isn't in the kernel's caches any more, the disk spins up - for a folder listing, for a file name that doesn't exist (Windows asks for `desktop.ini` and `Thumbs.db` everywhere), or for the File Manager asking where a share lives. Cache Directories fights this by re-reading every folder over and over, so the kernel keeps them. A backup or a big copy still pushes them out, and the re-reading itself keeps disks busy.

## How It Works

Unwake runs `unwaked`, a FUSE daemon that sits **on top of** `/mnt/user` - shfs stays mounted underneath - and answers from its own tree in RAM:

- **From RAM:** folder listings (`ls`, SMB, NFS, File Manager), name lookups (including names that don't exist), file attributes, extended attributes (Samba's `user.DOSATTRIB`, `system.LOCATION`)
- **From the disk, as before:** reading and writing file data
- **Through shfs, as before:** creating, renaming, deleting

**Filling the tree.** Unwake reads a disk's folders only while that disk spins anyway, at idle I/O priority, and never wakes a disk to do so. A disk that sleeps when Unwake starts is read the next time something else spins it up.

**Keeping it current.** Unwake watches every disk and pool with **fanotify**. Every change - through Unwake, straight on `/mnt/diskN`, by the Mover, Unbalanced, a container or Unspin - updates the tree right away. Nothing is re-read on a timer.

**What stays with shfs.** Shares that live on an SSD pool only can't wake a disk, so they're passed to shfs untouched. You can untick any share to pass it to shfs as well.

### Safe by design

- **shfs stays where it is.** Stopping Unwake takes it off `/mnt/user` at once; whatever still uses it keeps working until it lets go.
- **If `unwaked` dies**, its supervisor takes the mount away immediately, so `/mnt/user` is plain shfs again, and restarts it.
- **On array stop**, Unwake gets out of the way within 10 seconds.
- **Updates switch in place.** The running version hands its FUSE connection, its tree and every open file and folder to the new one. Containers, VMs, SMB clients and shells carry on without a restart; they see a pause of about a second. If the new version can't take over, the old one simply keeps running.

### File data and kernel passthrough

Unwake isn't in the way of file data: it hands every opened file to the kernel (**FUSE passthrough**, Linux 6.9+), which then reads and writes it directly - no copy through the daemon. A file that lives on exactly one known disk is opened on that disk, skipping shfs.

One thing worth knowing: the kernel takes **one backing file per file**, shared by everything that has it open. So Unwake opens that backing file **for reading and writing**, even when the first program only reads - otherwise a program that wants to write a file while another one reads it (qBittorrent finishing a download that Plex is scanning, Word saving a document that Explorer's preview holds open) would be refused. That changes nothing on the disk, but:

- a program that tries to *execute* that same file via `/mnt/diskN` or `/mnt/cache` while it's open through `/mnt/user` gets "text file busy"
- tools that count file opens on the disks (Unspin) see one extra open when a file is first opened
- where a read-write open isn't possible (read-only file system, a file being executed), the backing is read-only, and a program opening that file for writing while it's open elsewhere gets "device or resource busy"

## Features

- **Folders in RAM, for good** - nothing pushes them out of memory
- **Never wakes a disk to read it** - folders are read while a disk spins anyway
- **Event-driven** - no polling, no re-scans
- **All Unraid file systems** - XFS, btrfs, ZFS (datasets are followed as they come and go), on array disks and pools
- **In-place updates** - same mount, same open files; containers and VMs don't notice
- **Shares in RAM** - every share listed with a tick box
- **Spin-up diagnostics** - when a disk spins up, the log lists what Unwake passed to shfs that could have reached that disk
- **Diagnostics in one file** - `rc.unwake diag`
- **Live log viewer**

## Screenshots

*(coming)*

## Installation

**Via Community Applications (recommended):** search for **Unwake** and click Install.

**Manual:**

```
plugin install https://github.com/kaedinger/unwake/releases/latest/download/unwake.plg
```

Then turn off Dynamix Cache Directories under *Settings → Folder Caching*, and enable Unwake under *Settings → Utilities → Unwake*. If Cache Directories left `vfs_cache_pressure` at a low value, the settings page offers to set it back to 100.

## Requirements

- Unraid 7.3.2 or newer
- RAM: between about 270 and 460 bytes per file and folder name, depending on how many of them the kernel currently holds (a nightly backup through `/mnt/user` touches them all). 1.4 million names: 380 to 650 MB
- No external dependencies - single C++ binary

## Links

- **GitHub:** https://github.com/kaedinger/unwake
- **Issues / feature requests:** https://github.com/kaedinger/unwake/issues
- **License:** GPLv2

## AI disclaimer / History

*(Florian: your words here, like in Unspin's post. The README says so far: Unwake came out of Unspin's disk wake-up tracking - once it showed who woke the disks, most of the wake-ups turned out to be folder listings and name lookups that Cache Directories couldn't keep in memory. Unwake was designed and written with Claude (Anthropic) in Claude Code, and tested on your own server.)*
