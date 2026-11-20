#!/usr/bin/env bash
# integration.sh - unwaked against real file systems, run as root (CI runner).
#
# Stand-ins for an Unraid box: three loop-mounted XFS images as disk1, disk2 and
# a pool, mergerfs over them as shfs at $T/mnt/user, and a second mergerfs
# ($T/ref) to compare against. "Would this have spun a disk up?" is answered by
# the block-layer read counters of the loop devices after the XFS buffer cache
# was dropped (drop_caches with vfs_cache_pressure=0: what reclaim does on the
# real box).
#
# Usage: sudo tests/integration.sh path/to/unwaked
set -uo pipefail

BIN="$(realpath "${1:-usr/local/sbin/unwaked}")"
T=/tmp/uwtest
FAILS=0
PASSES=0

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok()   { PASSES=$((PASSES + 1)); printf '  \033[32mok\033[0m   %s\n' "$*"; }
fail() { FAILS=$((FAILS + 1));  printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
check() { local what="$1"; shift; if "$@"; then ok "$what"; else fail "$what"; fi; }

cleanup() {
    pkill -f "$BIN" 2>/dev/null; sleep 1; pkill -9 -f "$BIN" 2>/dev/null
    for _ in 1 2 3; do for m in $(awk -v t="$T" '$2 ~ "^"t {print $2}' /proc/mounts | sort -r); do umount -l "$m" 2>/dev/null; done; done
    for l in $(losetup -a | grep "$T" | cut -d: -f1); do losetup -d "$l"; done
    rm -rf "$T"
}

setup() {
    cleanup
    mkdir -p "$T"/{img,mnt/user,ref,run}
    for d in cache disk1 disk2; do
        truncate -s 300M "$T/img/$d.img"
        mkfs.xfs -q "$T/img/$d.img"
        mkdir -p "$T/mnt/$d"
        mount -o loop "$T/img/$d.img" "$T/mnt/$d"
    done
    # a share on both disks, one on the pool, one excluded
    for d in disk1 disk2; do
        mkdir -p "$T/mnt/$d/media/shows" "$T/mnt/$d/media/movies/$d-only"
        for i in $(seq 1 40); do echo "$d $i" > "$T/mnt/$d/media/shows/ep-$d-$i.mkv"; done
        for i in $(seq 1 5); do mkdir -p "$T/mnt/$d/media/deep/a/b/c/d/e/f/g/$d-$i"; echo x > "$T/mnt/$d/media/deep/a/b/c/d/e/f/g/$d-$i/file"; done
    done
    echo both-disks > "$T/mnt/disk1/media/movies/same-name.txt"
    mkdir -p "$T/mnt/cache/docs/notes" "$T/mnt/cache/media/shows" "$T/mnt/disk1/excluded/x"
    echo pool > "$T/mnt/cache/media/shows/on-pool.mkv"
    for i in $(seq 1 20); do echo "note $i" > "$T/mnt/cache/docs/notes/n$i.txt"; done
    setfattr -n user.DOSATTRIB -v archive "$T/mnt/disk1/media/shows/ep-disk1-1.mkv"
    setfacl -d -m u:nobody:rx "$T/mnt/disk1/media/movies/disk1-only"     # a default ACL Samba reads per folder
    dd if=/dev/urandom of="$T/mnt/disk2/media/movies/big.bin" bs=1M count=20 status=none
    # a mount fanotify can't watch, below a disk (Docker overlay, Unassigned Devices, ...)
    mkdir -p "$T/mnt/disk1/media/tmpmnt"
    mount -t tmpfs tmpfs "$T/mnt/disk1/media/tmpmnt"
    echo t > "$T/mnt/disk1/media/tmpmnt/in-tmpfs"
    # Unraid share settings: docs lives on the (SSD) pool only
    mkdir -p "$T/shares"
    printf 'shareUseCache="only"\nshareCachePool="cache"\n' > "$T/shares/docs.cfg"
    printf 'shareUseCache="yes"\nshareCachePool="cache"\n' > "$T/shares/media.cfg"
    # appdata: pool-only by its settings, but with leftovers on disk2
    mkdir -p "$T/mnt/cache/appdata/app" "$T/mnt/disk2/appdata/old-leftover"
    printf 'shareUseCache="only"\nshareCachePool="cache"\n' > "$T/shares/appdata.cfg"
    chown -R 99:100 "$T/mnt/disk1/media" "$T/mnt/disk2/media" "$T/mnt/cache"
    sync

    local opts="allow_other,use_ino,cache.files=off,category.create=ff,minfreespace=0,dropcacheonclose=false"
    local branches="$T/mnt/cache:$T/mnt/disk1:$T/mnt/disk2"
    mergerfs -o "$opts" "$branches" "$T/mnt/user"
    mergerfs -o "$opts" "$branches" "$T/ref"
    cat > "$T/unwake.cfg" <<EOF
SERVICE="enabled"
LOG_LEVEL="debug"
HARVEST="yes"
DATA_PATH="direct"
EXCLUDED_SHARES="excluded"
EOF
    sysctl -q vm.vfs_cache_pressure=100   # drop_caches then also drops dentries, inodes and XFS buffers
}

start_unwake() {
    rm -f "$T/run/ready"
    "$BIN" --mountpoint "$T/mnt/user" --config "$T/unwake.cfg" --log "$T/unwake.log" --verify-log "$T/verify.log" --run-dir "$T/run" \
           --shares-dir "$T/shares" --branch "cache=$T/mnt/cache,ssd" --branch "disk1=$T/mnt/disk1" --branch "disk2=$T/mnt/disk2" &
    SUP=$!
    for _ in $(seq 1 50); do [ -f "$T/run/ready" ] && break; sleep 0.2; done
    [ -f "$T/run/ready" ]
}

worker_pid() { cat "$T/run/ready" 2>/dev/null; }
top_fstype() { awk -v m="$T/mnt/user" '$5 == m {t=$0} END {split(t, a, " - "); split(a[2], b, " "); print b[1]}' /proc/self/mountinfo; }
stat_json() { python3 -c "import json,sys; d=json.load(open('$T/run/status.json')); print(eval(sys.argv[1]))" "$1"; }
wait_harvest() {
    for _ in $(seq 1 100); do
        sleep 0.3
        [ "$(stat_json "all(b['harvested'] or not b['up'] for b in d['branches'])")" = True ] && return 0
    done
    return 1
}
reads() {   # reads of the loop devices behind the given disks
    local n=0 d dev
    for d in "$@"; do
        dev=$(findmnt -n -o SOURCE "$T/mnt/$d"); dev=${dev#/dev/}
        n=$((n + $(awk '{print $1}' "/sys/block/$dev/stat")))
    done
    echo $n
}
# XFS frees a buffer only once its LRU reference reaches 0: one pass isn't enough
drop() { sync; for _ in 1 2 3 4 5; do echo 3 > /proc/sys/vm/drop_caches; done; sleep 0.5; }
# a directory's size depends on which disk shfs takes it from: compared for files only
listing() { (cd "$1" && find . -path ./excluded -prune -o -printf '%P|%y|%m|%U|%G|' -a \( -type d -printf '\n' -o -printf '%s\n' \) | sort); }
same_listing() { diff <(listing "$T/ref") <(listing "$T/mnt/user") > "$T/diff.txt" || { head -30 "$T/diff.txt" | sed "s/^/    /"; return 1; }; }
browse() {   # what a client does: list, stat, read xattrs, probe names that don't exist
    # media/tmpmnt is a mount Unwake leaves to shfs on purpose: not part of this
    (cd "$T/mnt/user" && find media docs -path media/tmpmnt -prune -o -printf '%p %s %m\n' >/dev/null &&
     getfattr -R -d media/shows media/movies media/deep docs >/dev/null 2>&1;
     getfattr --no-dereference --absolute-names -n system.LOCATIONS media/* media/shows/* >/dev/null 2>&1;   # File Manager
     for p in media/shows/desktop.ini media/movies/Thumbs.db docs/notes/.DS_Store; do stat "$p" >/dev/null 2>&1; done; true)
}

trap cleanup EXIT
say "setup"
setup
check "mergerfs mounted as the shfs stand-in" test "$(top_fstype)" = "fuse.mergerfs"

say "start"
check "unwaked starts and mounts on top" start_unwake
check "top mount is fuse.unwake" test "$(top_fstype)" = "fuse.unwake"
grep -E 'File data|Branch|Watching' "$T/unwake.log" | sed 's/^/  log: /'
check "harvest of all branches completes" wait_harvest

say "smoke"
stat "$T/mnt/user/media" >/dev/null; ls "$T/mnt/user/media/shows" | head -3
getfattr -d "$T/mnt/user/media/shows/ep-disk1-1.mkv"; getfattr -d "$T/ref/media/shows/ep-disk1-1.mkv"
sleep 2.2; cat "$T/run/status.json"; tail -5 "$T/unwake.log"
check "free space of a share is shfs's for that share" test "$(stat -f -c '%b %a' "$T/mnt/user/docs/notes")" = "$(stat -f -c '%b %a' "$T/ref/docs/notes")"

say "a file open more than once at a time (one passthrough backing per file)"
F="$T/mnt/user/media/shows/twice.txt"
echo twice > "$F"
exec 8< "$F"
check "a second reader opens it while the first holds it" bash -c "grep -q twice '$F'"
check "and a third while two are open" bash -c "exec 9< '$F'; grep -q twice <&9 && grep -q twice '$F'"
exec 8<&-
exec 8>> "$F"
check "a reader while it's open for writing" bash -c "grep -q twice '$F'"
echo again >&8
check "the writer writes on, readers see it" bash -c "exec 9< '$F'; grep -q again <&9"
exec 8>&-
check "after the last close it opens fresh" bash -c "grep -q again '$F'"
exec 8< "$F"
check "a writer opens it while a reader holds it (read-write backing)" bash -c "echo third >> '$F'"
check "and the reader reads what was written" bash -c "grep -q third <&8"
exec 8<&-

say "detection"
check "caching stays on with a tmpfs mount below a disk" test "$(stat_json "d['caching']")" = 1
check "that mount is passed through" grep -q 'Not cached: media/tmpmnt' "$T/unwake.log"
check "SSD-pool-only share passed through" grep -q 'Passed to shfs, not kept in RAM: docs, excluded$' "$T/unwake.log"
check "SSD-pool-only share with leftovers on an array disk kept in RAM" grep -q 'appdata lives on an SSD pool, but disk2 holds folders of it - kept in RAM' "$T/unwake.log"
check "status reports why" test "$(stat_json "d['pool_only']['appdata']['on']")" = "['disk2']"

say "listings"
check "union listing equals shfs" same_listing
check "file on two disks listed once" test "$(ls "$T/mnt/user/media/movies" | grep -c same-name.txt)" = 1
check "file content" test "$(cat "$T/mnt/user/media/shows/on-pool.mkv")" = pool
check "big file reads intact" cmp -s "$T/mnt/disk2/media/movies/big.bin" "$T/mnt/user/media/movies/big.bin"
# POSIX ACLs come from the cached set, other system.* ones from shfs (shfs's system.LOCATIONS isn't listed)
check "missing ACL of a file answered as through shfs itself" bash -c "test \"\$(getfattr --absolute-names --only-values -n system.posix_acl_access '$T/mnt/user/media/shows/ep-disk1-1.mkv' 2>&1 | sed 's|$T/mnt/user|X|')\" = \"\$(getfattr --absolute-names --only-values -n system.posix_acl_access '$T/ref/media/shows/ep-disk1-1.mkv' 2>&1 | sed 's|$T/ref|X|')\""
check "system.LOCATIONS of a share from RAM" test "$(getfattr --absolute-names --only-values -n system.LOCATIONS "$T/mnt/user/media")" = "cache,disk1,disk2"
check "system.LOCATIONS of a file from RAM" test "$(getfattr --absolute-names --only-values -n system.LOCATIONS "$T/mnt/user/media/movies/same-name.txt")" = "disk1"
check "system.LOCATION is the first" test "$(getfattr --absolute-names --only-values -n system.LOCATION "$T/mnt/user/media/shows")" = "cache"
check "xattr visible" test "$(getfattr --only-values -n user.DOSATTRIB "$T/mnt/user/media/shows/ep-disk1-1.mkv")" = archive

say "no disk reads once cached (the point of Unwake)"
sleep 1.5   # the atime re-reads of the files read above are done (they'd read after the drop)
browse; browse
grep xfs_buf /proc/slabinfo | awk '{print "  xfs_buf before drop: " $2}'
drop
grep xfs_buf /proc/slabinfo | awk '{print "  xfs_buf after drop:  " $2}'
r0=$(reads disk1 disk2)
browse
r1=$(reads disk1 disk2)
check "browse after cache drop: 0 array reads (got $((r1 - r0)))" test "$r1" -eq "$r0"
drop
r0=$(reads disk1 disk2)
(cd "$T/ref" && find media -printf '%p %s\n' >/dev/null)
r1=$(reads disk1 disk2)
sleep 2.2   # status.json is written every 2 s
check "control: the same browse through shfs reads the disks ($((r1 - r0)) reads)" test "$r1" -gt "$r0"
echo "  counters: $(stat_json "{k: d[k] for k in ('lookup_hit','lookup_neg','lookup_miss','getattr_hit','getattr_miss','readdir_hit','readdir_miss','xattr_hit','xattr_miss','open_direct','open_shfs','dirs','entries','rss_kb')}")"
check "negative lookups answered from cache" test "$(stat_json "d['lookup_neg']")" -gt 0

say "changes outside Unwake (straight to a disk, like the mover)"
echo new > "$T/mnt/disk2/media/shows/oob-new.mkv"
mkdir -p "$T/mnt/disk2/media/shows/oob-dir/sub"; echo y > "$T/mnt/disk2/media/shows/oob-dir/sub/f"
rm "$T/mnt/disk1/media/shows/ep-disk1-2.mkv"
mv "$T/mnt/disk2/media/shows/ep-disk2-3.mkv" "$T/mnt/disk2/media/shows/renamed.mkv"
echo grown >> "$T/mnt/disk1/media/shows/ep-disk1-4.mkv"
cp "$T/mnt/cache/media/shows/on-pool.mkv" "$T/mnt/disk1/media/shows/on-pool.mkv"; rm "$T/mnt/cache/media/shows/on-pool.mkv"   # mover
rmdir "$T/mnt/disk1/media/deep/a/b/c/d/e/f/g/disk1-5" 2>/dev/null || rm -r "$T/mnt/disk1/media/deep/a/b/c/d/e/f/g/disk1-5"
sleep 2
echo "  events seen: $(stat_json "d['events']")"; grep -E 'open_by_handle|ERROR' "$T/unwake.log" | head -4 | sed 's/^/  log: /'
check "listing follows out-of-band changes" same_listing
check "moved file still readable" test "$(cat "$T/mnt/user/media/shows/on-pool.mkv")" = pool

say "changes through unwake"
U="$T/mnt/user/media"
check "create + write" bash -c "echo hello > '$U/shows/inband.txt'"
check "read back" test "$(cat "$U/shows/inband.txt")" = hello
check "owner is the caller" bash -c "setpriv --reuid=99 --regid=100 --clear-groups touch '$U/shows/by-nobody' && test \$(stat -c %u '$U/shows/by-nobody') = 99"
check "mkdir + rename dir" bash -c "mkdir -p '$U/newdir/inner' && echo z > '$U/newdir/inner/z' && mv '$U/newdir' '$U/moved-dir'"
check "rename file over another" bash -c "echo a > '$U/shows/a' && echo b > '$U/shows/b' && mv '$U/shows/a' '$U/shows/b' && test \$(cat '$U/shows/b') = a"
check "chmod" bash -c "chmod 600 '$U/shows/inband.txt' && test \$(stat -c %a '$U/shows/inband.txt') = 600"
check "truncate" bash -c "truncate -s 2 '$U/shows/inband.txt' && test \$(stat -c %s '$U/shows/inband.txt') = 2"
check "setfattr" bash -c "setfattr -n user.x -v 1 '$U/shows/inband.txt' && test \$(getfattr --only-values -n user.x '$U/shows/inband.txt') = 1"
check "append grows size while open" bash -c "exec 3>>'$U/shows/inband.txt'; echo more >&3; s=\$(stat -c %s '$U/shows/inband.txt'); exec 3>&-; test \$s -gt 2"
check "umask 000: file 666, folder 777" bash -c "umask 000; touch '$U/shows/m-file' && mkdir '$U/shows/m-dir' && test \$(stat -c %a '$U/shows/m-file') = 666 && test \$(stat -c %a '$U/shows/m-dir') = 777"
check "umask 022 still applies" bash -c "umask 022; touch '$U/shows/m-file2' && test \$(stat -c %a '$U/shows/m-file2') = 644"
check "change inside the unwatched mount shows up" bash -c "echo u > '$T/mnt/disk1/media/tmpmnt/later' && sleep 1.2 && test -e '$U/tmpmnt/later'"
check "unlink" bash -c "rm '$U/shows/by-nobody' && ! test -e '$U/shows/by-nobody'"
check "rmdir" bash -c "rm -r '$U/moved-dir' && ! test -e '$U/moved-dir'"
check "symlink" bash -c "ln -s inband.txt '$U/shows/sl' && test -L '$U/shows/sl' && test \$(readlink '$U/shows/sl') = inband.txt"
check "hardlink" bash -c "ln '$U/shows/b' '$U/shows/hl' && stat -c '%h %i %n' '$U/shows/b' '$U/shows/hl' '$T/ref/media/shows/b' && test \$(cat '$U/shows/hl') = a && test \$(stat -c %h '$U/shows/b') = 2"
# git's lock files: write a lot, close, rename at once - FUSE's RELEASE comes after the rename
# (test-slow-release holds each release 300 ms, so the rename always gets in between)
touch "$T/run/test-slow-release"
python3 - "$U/shows" <<'PY'
import os, sys
for i in range(20):
    p = f"{sys.argv[1]}/lk{i}"
    fd = os.open(p + ".lock", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    os.write(fd, os.urandom(200000)); os.close(fd)
    os.rename(p + ".lock", p)
PY
sleep 2
rm -f "$T/run/test-slow-release"
check "no ghost of a file renamed right after writing it" test "$(ls "$U/shows" | grep -c 'lk.*\.lock')" -eq 0
check "renamed files all there" test "$(ls "$U/shows" | grep -c '^lk[0-9]*$')" -eq 20
rm -f "$U"/shows/lk*
sleep 2
check "listing equals shfs after in-band changes" same_listing

say "xattrs come with the full read"
kill "$SUP"; wait "$SUP" 2>/dev/null
start_unwake && wait_harvest
drop
r0=$(reads disk1 disk2)
(cd "$T/mnt/user" && getfattr -R -d media/shows media/movies media/deep >/dev/null 2>&1; getfattr -n system.LOCATIONS docs appdata media >/dev/null 2>&1
 getfattr -n system.posix_acl_default media/movies/* media/shows media/deep/a/b/c/d/e/f/g/* >/dev/null 2>&1)
r1=$(reads disk1 disk2)
check "xattrs and ACLs of never-touched files and locations of passed-through shares: 0 array reads (got $((r1 - r0)))" test "$r1" -eq "$r0"
check "default ACL from RAM, as through shfs" test "$(getfacl -d --omit-header "$T/mnt/user/media/movies/disk1-only")" = "$(getfacl -d --omit-header "$T/ref/media/movies/disk1-only")"
check "no default ACL from RAM, as through shfs" test "$(getfacl -d --omit-header "$T/mnt/user/media/shows")" = "$(getfacl -d --omit-header "$T/ref/media/shows")"
setfacl -d -m u:nobody:rwx "$T/mnt/disk1/media/movies/disk1-only"; sleep 2
check "ACL changed straight on the disk shows up" bash -c "getfacl -d --omit-header '$T/mnt/user/media/movies/disk1-only' | grep -q 'user:nobody:rwx'"
chmod 750 "$T/mnt/disk1/media/movies/disk1-only"; sleep 2
check "folder mode changed straight on the disk shows up" test "$(stat -c %a "$T/mnt/user/media/movies/disk1-only")" = 750
check "harvested xattr value" test "$(getfattr --only-values -n user.DOSATTRIB "$T/mnt/user/media/shows/ep-disk1-1.mkv")" = archive
setfattr -n user.DOSATTRIB -v changed "$T/mnt/disk1/media/shows/ep-disk1-1.mkv"; sleep 2
check "xattr changed straight on the disk shows up" test "$(getfattr --only-values -n user.DOSATTRIB "$T/mnt/user/media/shows/ep-disk1-1.mkv")" = changed
# a folder's mtime changes with its contents, without an event of its own
D=media/movies/disk1-only
touch -d '2 days ago' "$T/mnt/disk1/$D"; sleep 2
echo new > "$T/mnt/disk1/$D/dirmtime-direct"; sleep 2
check "folder mtime after a create straight on the disk" test "$(stat -c %Y "$T/mnt/user/$D")" = "$(stat -c %Y "$T/mnt/disk1/$D")"
echo new > "$T/mnt/user/$D/dirmtime-inband"; sleep 2
# (shfs may create the new file, and the folder with it, on another disk: compare with shfs)
check "folder mtime after a create through Unwake" test "$(stat -c %Y.%Z "$T/mnt/user/$D")" = "$(stat -c %Y.%Z "$T/ref/$D")"
rm "$T/mnt/user/$D/dirmtime-inband" "$T/mnt/disk1/$D/dirmtime-direct"; sleep 2
check "folder mtime after deletes" test "$(stat -c %Y.%Z "$T/mnt/user/$D")" = "$(stat -c %Y.%Z "$T/ref/$D")"
# atime: a read changes it without any other event (relatime: once it's older than a day)
F=media/shows/ep-disk1-2.mkv
touch -a -d '3 days ago' "$T/mnt/disk1/$F"; sleep 2
a0=$(stat -c %X "$T/mnt/disk1/$F")
check "old atime from RAM" test "$(stat -c %X "$T/mnt/user/$F")" = "$a0"
for _ in 1 2 3 4 5; do cat "$T/mnt/user/$F" >/dev/null; done    # several closes, merged or not
sleep 2
check "a read updated the atime on the disk" test "$(stat -c %X "$T/mnt/disk1/$F")" -gt "$a0"
check "the new atime from RAM, as on the disk" test "$(stat -c %X "$T/mnt/user/$F")" = "$(stat -c %X "$T/mnt/disk1/$F")"
# a read through a read-write open closes as CLOSE_WRITE (as every passthrough read does)
touch -a -d '3 days ago' "$T/mnt/disk1/$F"; sleep 2
a0=$(stat -c %X "$T/mnt/disk1/$F")
check "old atime from RAM before a read-write read" test "$(stat -c %X "$T/mnt/user/$F")" = "$a0"
python3 -c 'import sys; f=open(sys.argv[1], "r+b"); f.read(); f.close()' "$T/mnt/disk1/$F"
sleep 2
check "a read-write read updated the atime on the disk" test "$(stat -c %X "$T/mnt/disk1/$F")" -gt "$a0"
check "the new atime from RAM after a read-write read" test "$(stat -c %X "$T/mnt/user/$F")" = "$(stat -c %X "$T/mnt/disk1/$F")"
# all disks read: a pool-only share passed to shfs is known not to be on disk2
echo disk2 > "$T/run/test-spundown"; sleep 2.5
n0=$(grep -c 'disk2 spun up' "$T/unwake.log")
cat "$T/mnt/user/docs/notes/n1.txt" >/dev/null
touch "$T/mnt/user/media/movies/disk2-only/who-test"
rm -f "$T/run/test-spundown"; sleep 2.5
check "spin-up logged" test "$(grep -c 'disk2 spun up' "$T/unwake.log")" -gt "$n0"
check "requests in a pool-only share aren't listed for an array disk" bash -c "! tac '$T/unwake.log' | sed '/disk2 spun up/q' | grep -q docs/notes/n1.txt"
check "debug log names the process behind a request" bash -c "tac '$T/unwake.log' | sed '/disk2 spun up/q' | grep -q 'create media/movies/disk2-only/who-test \\[touch [0-9]*, parent '"
rm -f "$T/mnt/user/media/movies/disk2-only/who-test"

say "verifier: the tree equals the disks, planted errors are found and repaired"
vall() { stat_json "all(b['verify']['state'] == 'done' for b in d['branches'] if b['harvested'])"; }
vsum() { stat_json "sum(b['verify']['$1'] for b in d['branches'])"; }
printf 'VERIFY="yes"\nVERIFY_RATE="1000"\n' >> "$T/unwake.cfg"; kill -HUP "$SUP"
for _ in $(seq 1 60); do sleep 0.5; [ "$(vall)" = True ] && break; done
check "a full pass on every disk" test "$(vall)" = True
check "no differences after all the tests above" test ! -s "$T/verify.log"
[ -s "$T/verify.log" ] && sed 's/^/  verify: /' "$T/verify.log" | head -40
sleep 2
check "the next pass waits (12 hours)" test "$(stat_json "sum(b['verify']['checked'] for b in d['branches'])")" -gt 0 -a "$(vall)" = True
printf 'ghost disk1 media/shows/planted-ghost\ndrop disk2 media/shows/ep-disk2-17.mkv\nsize disk1 media/shows/ep-disk1-5.mkv\n' > "$T/run/test-plant"
date +%s%N > "$T/run/test-verify-now"
for _ in $(seq 1 60); do sleep 0.5; [ "$(grep -cE '"(planted-ghost|ep-disk2-17.mkv|ep-disk1-5.mkv)"' "$T/verify.log" 2>/dev/null)" -ge 3 ] && [ "$(vall)" = True ] && break; done
sleep 2.5   # status.json is written every 2 s
sed 's/^/  verify: /' "$T/verify.log" | head -20
check "planted ghost found" grep -q '"planted-ghost": ghost' "$T/verify.log"
check "planted missing name found" grep -q '"ep-disk2-17.mkv": missing' "$T/verify.log"
check "planted wrong size found" grep -q '"ep-disk1-5.mkv": size RAM' "$T/verify.log"
check "repaired: listing equals shfs" same_listing
check "repaired: size as on the disk" test "$(stat -c %s "$T/mnt/user/media/shows/ep-disk1-5.mkv")" = "$(stat -c %s "$T/mnt/disk1/media/shows/ep-disk1-5.mkv")"
check "this pass on the status: 3 found, 3 repaired, none pending" test "$(vsum found).$(vsum repaired).$(vsum pending)" = "3.3.0"
stat_json "[(b['name'], b['verify']) for b in d['branches']]" | sed 's/^/  verify status: /'
rm -f "$T/run/test-verify-now"
sed -i '/^VERIFY/d' "$T/unwake.cfg"; kill -HUP "$SUP"; sleep 1

say "spun-down disk is never read"
kill "$SUP"; wait "$SUP" 2>/dev/null
echo disk2 > "$T/run/test-spundown"
drop
r0=$(reads disk2)
start_unwake
sleep 6
r1=$(reads disk2)
check "harvester leaves the sleeping disk alone (disk2 reads: $((r1 - r0)))" test "$r1" -eq "$r0"
check "while the awake disks are harvested" test "$(stat_json "[b['harvested'] for b in d['branches'] if b['name']=='disk1'][0]")" = 1
check "listing correct while disk2 sleeps (passthrough)" same_listing
rm -f "$T/run/test-spundown"
sleep 2.5
check "spin-up is logged with what Unwake passed to shfs before it" grep -q 'disk2 spun up - Unwake passed [0-9]* request' "$T/unwake.log"
grep -A3 'disk2 spun up' "$T/unwake.log" | head -4 | sed 's/^/  log: /'
wait_harvest
f1=r1; f2=r2; echo a > "$T/mnt/disk1/media/movies/disk1-only/$f1"; echo b > "$T/mnt/disk2/media/movies/disk2-only/$f2"; sleep 1
echo disk2 > "$T/run/test-spundown"; sleep 2.5
mv "$T/mnt/user/media/movies/disk1-only/$f1" "$T/mnt/user/media/movies/disk1-only/$f1.r"
mv "$T/mnt/user/media/movies/disk2-only/$f2" "$T/mnt/user/media/movies/disk2-only/$f2.r"
rm -f "$T/run/test-spundown"; sleep 2.5
last=$(grep -n 'disk2 spun up' "$T/unwake.log" | tail -1 | cut -d: -f1)
msg=$(tail -n +"$last" "$T/unwake.log" | awk 'NR==1||/^    /{print;next}{exit}')
echo "$msg" | sed 's/^/  log: /'
check "spin-up names the rename on that disk" grep -q "rename media/movies/disk2-only/$f2 " <<<"$msg"
check "but not the one on another disk" bash -c '! grep -q "disk1-only" <<<"$1"' _ "$msg"

say "in-place upgrade: same mount, open files, bind mounts, tree and events carry over"
mkdir -p "$T/mnt/disk1/media/big"
for i in $(seq 1 1500); do : > "$T/mnt/disk1/media/big/$(printf 'a-rather-long-file-name-so-that-one-readdir-buffer-holds-only-part-of-the-folder-%05d.txt' "$i")"; done
sleep 2
wait_harvest
OLDW=$(worker_pid)
MID=$(awk -v m="$T/mnt/user" '$5 == m {id=$1} END {print id}' /proc/self/mountinfo)
DIRS0=$(stat_json "d['dirs']")
echo "upgrade-read" > "$T/mnt/user/media/shows/ho-read.txt"; echo "a" > "$T/mnt/user/media/shows/ho-write.txt"
exec 6< "$T/mnt/user/media/shows/ho-read.txt"
exec 7>> "$T/mnt/user/media/shows/ho-write.txt"
mkdir -p "$T/bind" && mount --bind "$T/mnt/user/media" "$T/bind"          # what Docker does with a path mapping
(cd "$T/mnt/user/media/shows" && exec sleep 300) 6<&- 7>&- & CWDP=$!        # a shell inside a share
python3 - "$T/mnt/user/media/big" "$T/run/dirtest" 6<&- 7>&- <<'PY' &
import os, sys, time
it = os.scandir(sys.argv[1]); first = [next(it).name for _ in range(10)]
open(sys.argv[2] + ".ready", "w").close()
while not os.path.exists(sys.argv[2] + ".go"): time.sleep(0.05)
open(sys.argv[2] + ".out", "w").write(str(len(first) + len(list(it))))
PY
DIRP=$!
for _ in $(seq 1 50); do [ -f "$T/run/dirtest.ready" ] && break; sleep 0.1; done
BIG=$(ls -A "$T/mnt/user/media/big" | wc -l)
( while [ ! -f "$T/run/stress.stop" ]; do
    ls -lR "$T/mnt/user/media" >/dev/null 2>>"$T/stress.err" || echo "ls failed" >> "$T/stress.err"
    cat "$T/bind/shows/ho-read.txt" >/dev/null 2>>"$T/stress.err" || echo "cat failed" >> "$T/stress.err"
    stat "$T/mnt/user/media/shows/no-such-file" >/dev/null 2>&1
  done ) 6<&- 7>&- & STRESS=$!
sleep 0.5
H0=$(grep -c 'Harvest of' "$T/unwake.log")
kill -USR2 "$SUP"
echo "during" > "$T/mnt/disk1/media/shows/ho-during.txt"                     # a change on the disk while it switches
for _ in $(seq 1 100); do [ "$(worker_pid)" != "$OLDW" ] && [ -n "$(worker_pid)" ] && break; sleep 0.1; done
NEWW=$(worker_pid)
sleep 1
touch "$T/run/stress.stop"; wait "$STRESS"; touch "$T/run/dirtest.go"; wait "$DIRP"
grep 'Upgrade:' "$T/unwake.log" | sed 's/^/  log: /'
check "a new worker took over (pid $OLDW -> $NEWW)" test -n "$NEWW" -a "$NEWW" != "$OLDW"
check "the old worker is gone" bash -c "! kill -0 $OLDW 2>/dev/null"
check "the supervisor stays (same pid) and runs the new binary" bash -c "kill -0 $SUP && grep -q 'supervisor runs' '$T/unwake.log'"
check "same mount as before" test "$(awk -v m="$T/mnt/user" '$5 == m {id=$1} END {print id}' /proc/self/mountinfo)" = "$MID"
check "no errors while it switched" bash -c "! test -s '$T/stress.err'"
[ -s "$T/stress.err" ] && head -5 "$T/stress.err" | sed 's/^/    /'
check "a file open for reading reads on" bash -c "cat <&6 | grep -q upgrade-read"
echo "b" >&7
check "a file open for writing writes on" bash -c "grep -q '^b$' '$T/mnt/user/media/shows/ho-write.txt'"
check "a half-read folder lists on ($(cat "$T/run/dirtest.out") of $BIG names)" test "$(cat "$T/run/dirtest.out")" = "$BIG"
check "the bind mount (container) reaches the new worker" bash -c "ls '$T/bind/shows' >/dev/null && test \$(stat -c %d '$T/bind') = \$(stat -c %d '$T/mnt/user')"
check "the shell inside the share works on" ls "/proc/$CWDP/cwd/" >/dev/null
check "the tree came along ($DIRS0 folders)" test "$(stat_json "d['dirs']")" -ge "$DIRS0"
check "no disk was read again" test "$(grep -c 'Harvest of' "$T/unwake.log")" = "$H0"
check "a change made during the switch shows" test -f "$T/mnt/user/media/shows/ho-during.txt"
echo "after" > "$T/mnt/disk2/media/shows/ho-after.txt"; sleep 1
check "change events keep coming" test -f "$T/mnt/user/media/shows/ho-after.txt"
drop
r0=$(reads disk1 disk2)
ls -lR "$T/mnt/user/media/shows" "$T/mnt/user/media/movies" >/dev/null
r1=$(reads disk1 disk2)
check "listings after the switch from RAM: 0 disk reads (got $((r1 - r0)))" test "$r1" -eq "$r0"
check "listing equals shfs" same_listing
exec 6<&- 7>&-
kill "$CWDP" 2>/dev/null; wait "$CWDP" 2>/dev/null
umount "$T/bind"

W2=$(worker_pid)
echo "$SUP" > "$T/run/unwaked.pid"     # rc.unwake finds the supervisor here
out=$(UNWAKE_RUN="$T/run" UNWAKE_MNT="$T/mnt/user" UNWAKE_LOG="$T/unwake.log" UNWAKE_DAEMON="$BIN" bash etc/rc.d/rc.unwake upgrade)
echo "  rc.unwake upgrade: $out"
check "a second upgrade, through rc.unwake (the re-executed supervisor handles it)" bash -c "test \"\$(cat '$T/run/ready')\" != $W2 && grep -q '^Switched to' <<<'$out'"

for mode in refuse crash; do
    W=$(worker_pid)
    echo "$mode" > "$T/run/test-adopt"
    n0=$(grep -c 'without taking over' "$T/unwake.log")
    kill -USR2 "$SUP"
    for _ in $(seq 1 100); do [ "$(grep -c 'without taking over' "$T/unwake.log")" != "$n0" ] && break; sleep 0.1; done
    rm -f "$T/run/test-adopt"
    check "new one fails ($mode): the old one serves on" bash -c "test \"\$(cat '$T/run/ready')\" = $W && kill -0 $W && ls '$T/mnt/user/media/shows' >/dev/null"
done
sleep 1
check "and still takes requests after that" same_listing

say "stop with a file open: detach, keep serving it"
exec 4< "$T/mnt/user/media/shows/inband.txt"
kill "$SUP"
sleep 1
check "top mount is shfs again right away" test "$(top_fstype)" = "fuse.mergerfs"
check "worker still serves the open file" bash -c "cat <&4 >/dev/null"
exec 4<&-
for _ in $(seq 1 30); do kill -0 "$SUP" 2>/dev/null || break; sleep 0.2; done
check "worker exits once the last user is gone" bash -c "! kill -0 $SUP 2>/dev/null"

say "restart while the old one still serves an open file"
start_unwake
OLD=$SUP; OLDW=$(worker_pid)
exec 5< "$T/mnt/user/media/shows/inband.txt"
unshare -m --propagation private sleep 300 5<&- & NS1=$!     # a container started while the old one ran
for _ in $(seq 1 50); do [ "$(readlink /proc/$NS1/ns/mnt)" != "$(readlink /proc/self/ns/mnt)" ] && break; sleep 0.05; done
kill "$OLD"; sleep 1
check "new one starts on top of shfs" start_unwake
NEWW=$(worker_pid)
unshare -m --propagation private sleep 300 5<&- & NS2=$!     # one started after the restart
for _ in $(seq 1 50); do [ "$(readlink /proc/$NS2/ns/mnt)" != "$(readlink /proc/self/ns/mnt)" ] && break; sleep 0.05; done
sleep 0.5
users=$(UNWAKE_RUN="$T/run" UNWAKE_MNT="$T/mnt/user" bash etc/rc.d/rc.unwake users)
echo "$users" | sed 's/^/  users: /'
check "users names the old instance" grep -q "Detached Unwake (pid $OLDW" <<<"$users"
check "and the open file holding it" grep -q "pid $$ .*inband.txt" <<<"$users"
check "and the namespace started while it ran" grep -q "pid $NS1 .*own namespace" <<<"$users"
check "but not the one on the live instance" bash -c '! grep -q "pid $1 " <<<"$2"' _ "$NS2" "$users"
kill "$NS1" "$NS2"; wait "$NS1" "$NS2" 2>/dev/null
sleep 2.5
check "status belongs to the new one" test "$(stat_json "d['pid']")" = "$NEWW"
check "old one still serves its open file" bash -c "cat <&5 >/dev/null"
exec 5<&-
for _ in $(seq 1 30); do kill -0 "$OLD" 2>/dev/null || break; sleep 0.2; done
check "old one exits, new one keeps its ready file and mount" bash -c "! kill -0 $OLD 2>/dev/null && test -f '$T/run/ready' && test \"\$(cat '$T/run/ready')\" = $NEWW && test \"$(top_fstype)\" = fuse.unwake"
kill "$SUP"; wait "$SUP" 2>/dev/null

say "crash: the supervisor takes the dead mount away"
start_unwake
kill -9 "$(worker_pid)"
sleep 1.5
check "mount point works after the crash ($(top_fstype))" ls "$T/mnt/user/media" >/dev/null
for _ in $(seq 1 30); do [ -f "$T/run/ready" ] && break; sleep 0.2; done
check "restarted on top" test "$(top_fstype)" = "fuse.unwake"
kill "$SUP"; wait "$SUP" 2>/dev/null

say "crash report"
start_unwake
kill -SEGV "$(worker_pid)"
sleep 1.5
check "a crash leaves a backtrace in the log" grep -q 'unwaked crashed: SIGSEGV' "$T/unwake.log"
off=$(grep -A40 'unwaked crashed: SIGSEGV' "$T/unwake.log" | grep -o 'unwaked(+0x[0-9a-f]*)' | head -3 | grep -o '0x[0-9a-f]*' | tail -1)
echo "  resolved: $(addr2line -f -C -e "$BIN" "$off" | head -2 | tr '\n' ' ')"
check "the backtrace resolves against the binary" bash -c "addr2line -e '$BIN' '$off' | grep -q 'src/'"
for _ in $(seq 1 30); do [ "$(top_fstype)" = fuse.unwake ] && break; sleep 0.2; done
check "and it was restarted" test "$(top_fstype)" = "fuse.unwake"
kill "$SUP"; wait "$SUP" 2>/dev/null

say "diagnostics"
check "rc.unwake diag writes its file" bash -c "out=\$(bash etc/rc.d/rc.unwake diag) && f=\${out#Saved to } && grep -q '== Unwake diagnostics' \"\$f\" && grep -q 'vfs_cache_pressure' \"\$f\""

say "log"
grep -E 'ERROR|WARN' "$T/unwake.log" | head -20 | sed 's/^/  /'
cp "$T/unwake.log" /tmp/unwake-test.log 2>/dev/null

printf '\n%d passed, %d failed\n' "$PASSES" "$FAILS"
[ "$FAILS" -eq 0 ]
