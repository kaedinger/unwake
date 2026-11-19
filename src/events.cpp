// events.cpp - fanotify: the authoritative change stream for every branch
//
// One fanotify group with a FAN_MARK_FILESYSTEM mark per mounted file system at
// or below a branch root (one per disk on XFS/btrfs, one per dataset on ZFS).
// Directory-entry events update the tree right away; attribute and content
// changes are collected and the entry re-read from its disk a second later
// (the disk is up: it was just written).
#include "unwaked.h"

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/fanotify.h>
#include <sys/statfs.h>
#include <thread>
#include <unistd.h>

std::atomic<uint64_t> g_events{0};
static int _unplaced = 0;   // events of an unknown fsid in a row

static int  _fan = -1;
static std::thread _thr;
static std::atomic<bool> _stop{false};

struct Mark { int branch; std::string path; std::string fstype; dev_t dev = 0; };
static bool _scanned_once = false;
static std::atomic<bool> _broken{false};

// file systems a disk or pool can carry, and fanotify can watch
static bool disk_fs(const std::string& t) {
    return t == "xfs" || t == "btrfs" || t == "zfs" || t == "ext4" || t == "ext3" || t == "ext2" ||
           t == "reiserfs" || t == "f2fs" || t == "bcachefs";
}

static void events_broken(const std::string& why) {
    if (_broken.exchange(true)) return;
    log_err(why + " - caching is off until Unwake is restarted");
    g_caching = false;
}
static std::map<std::string, Mark> _marks;          // mount path -> mark
static std::mutex _marks_mu;

// FAN_CLOSE_NOWRITE: a read changes a file's atime without any other event (relatime: at
// most once a day per file); re-reading the file after it was closed keeps atime current.
// Merged events (several closes in one) are fine: only the latest atime matters.
static constexpr uint64_t EVENTS = FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO |
                                   FAN_ATTRIB | FAN_MODIFY | FAN_CLOSE_NOWRITE | FAN_ONDIR;

// Every mount point at or below a branch root, from mountinfo (no disk I/O).
static std::vector<Mark> wanted_marks() {
    std::vector<Mark> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, parent, devno, root, mp, tok, fstype;
        ss >> id >> parent >> devno >> root >> mp;
        while (ss >> tok && tok != "-") {}
        ss >> fstype;
        // mountinfo escapes blanks as \040
        std::string p;
        for (size_t i = 0; i < mp.size(); i++) {
            if (mp[i] == '\\' && i + 3 < mp.size()) { p += (char)strtol(mp.substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
            else p += mp[i];
        }
        std::string rel;
        int b = branch_of_path(p, rel);
        if (b >= 0) {
            Mark m{b, p, fstype};
            struct stat st;
            if (stat(p.c_str(), &st) == 0) m.dev = st.st_dev;
            out.push_back(m);
        }
    }
    return out;
}

void events_rescan_mounts() {
    if (_fan < 0) return;
    auto want = wanted_marks();
    std::vector<std::string> uncached, forget;
    {
        std::lock_guard<std::mutex> l(_marks_mu);
        std::set<std::string> now;
        for (auto& w : want) {
            std::string rel;
            branch_of_path(w.path, rel);
            if (!disk_fs(w.fstype)) {
                // overlay, tmpfs, fuse, ...: shfs shows what's in it, but we'd never hear of changes
                if (rel.empty()) { events_broken(w.path + " is " + w.fstype); continue; }
                uncached.push_back(rel);
                continue;
            }
            now.insert(w.path);
            if (_marks.count(w.path)) continue;
            if (fanotify_mark(_fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, EVENTS, AT_FDCWD, w.path.c_str()) != 0) {
                g_branches[w.branch]->untracked = true;
                events_broken("Can't watch " + w.path + " (" + w.fstype + ": " + strerror(errno) + ")");
                continue;
            }
            log_info("Watching " + w.path + " (" + w.fstype + ")");
            _marks[w.path] = w;
            // a dataset mounted after the start: what we knew below its mount point was the folder under it
            if (_scanned_once && !rel.empty()) forget.push_back(rel);
        }
        for (auto it = _marks.begin(); it != _marks.end();) {
            if (now.count(it->first)) { ++it; continue; }
            log_info("No longer mounted: " + it->first);
            std::string rel;
            if (branch_of_path(it->first, rel) >= 0 && !rel.empty()) forget.push_back(rel);
            it = _marks.erase(it);
        }
        _scanned_once = true;
    }
    std::sort(uncached.begin(), uncached.end());
    auto old = std::atomic_load(&g_cfg.uncached_paths);
    if (*old != uncached) {
        for (auto& u : uncached)
            if (!std::binary_search(old->begin(), old->end(), u)) {
                log_info("Not cached: " + u + " (a mount whose changes can't be watched)");
                forget.push_back(u);
            }
        for (auto& u : *old)
            if (!std::binary_search(uncached.begin(), uncached.end(), u)) forget.push_back(u);
        std::atomic_store(&g_cfg.uncached_paths, std::shared_ptr<const std::vector<std::string>>(
                                                     std::make_shared<std::vector<std::string>>(uncached)));
    }
    if (!forget.empty()) {
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        for (auto& f : forget) tree_forget(f);
    }
}

bool events_dev_watched(dev_t dev) {
    std::lock_guard<std::mutex> l(_marks_mu);
    for (auto& kv : _marks) if (kv.second.dev == dev) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Applying changes
// ---------------------------------------------------------------------------
static void drop_subtree(Entry& e) {
    if (!e.dir) return;
    for (auto& kv : e.dir->entries) tree_count_add(kv.second, -1);
    g_tree.dirs--;
    e.dir.reset();
}

struct Pending { int b; std::string dir, name; bool xattrs; };   // xattrs: may have changed too
static std::map<std::string, Pending> _pending;
static int64_t _pending_since = 0;

static void restat_later(int b, const std::string& dir, const std::string& name, bool xattrs = true) {
    if (_pending.empty()) _pending_since = now_ms();
    auto r = _pending.try_emplace(std::to_string(b) + ":" + dir + "/" + name, Pending{b, dir, name, xattrs});
    if (!r.second) r.first->second.xattrs |= xattrs;
}

static void apply_added(int b, const std::string& rel_dir, const std::string& name,
                        const struct stat& st, bool created, std::shared_ptr<const Xattrs> xs) {
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(rel_dir);
    if (!d) { g_untraced++; return; }
    bool servable = tree_servable(d);
    auto r = d->entries.try_emplace(name);
    Entry& e = r.first->second;
    if (r.second) g_tree.entries++;
    bool had_b = e.mask & bit(b);   // our own rename through shfs: contents unchanged
    e.mask |= bit(b);
    if (attr_from_branch_wins(e, b)) {
        e.attr.from(st);
        e.has_attr = true;
        e.attr_from = (int8_t)b;
        e.xattrs = xs;
    }
    e.is_dir = S_ISDIR(st.st_mode);
    e.dtype = (uint8_t)IFTODT(st.st_mode);
    if (!created && !had_b && e.dir) drop_subtree(e);   // moved in with unknown contents
    if (created && r.second && e.is_dir && servable && !e.dir) {
        // a new, empty directory that exists nowhere else: complete from birth
        auto nd = std::make_unique<DirNode>();
        nd->parent = d;
        nd->name = &r.first->first;
        nd->pt_complete = true;
        nd->known = bit(b);
        e.dir = std::move(nd);
        g_tree.dirs++;
    }
    touch(d, dbit(b));
}

static void apply_removed(int b, const std::string& rel_dir, const std::string& name, bool moved) {
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(rel_dir);
    if (!d) { g_untraced++; return; }
    auto it = d->entries.find(name);
    if (it == d->entries.end()) { touch(d, dbit(b)); return; }   // a reader may have seen it: it must not add it back
    Entry& e = it->second;
    bool source = e.has_attr && (e.attr_from >= 0 ? e.attr_from == b : e.mask && b == __builtin_ctzll(e.mask));
    e.mask &= ~bit(b);
    if (moved) drop_subtree(e);
    touch(d, dbit(b));
    if (e.mask) {                                     // still on another disk
        if (source) {
            // the copy we showed is gone (the Mover moved it): show the first remaining one -
            // read now if that disk spins, else ask shfs on the next request
            e.has_attr = false;
            e.xattrs.reset();
            int first = __builtin_ctzll(e.mask);
            if (first < (int)g_branches.size() && g_branches[first]->up) restat_later(first, rel_dir, name);
        }
        return;
    }
    bool sure = tree_all_known(d);
    tree_erase(d, name);
    if (!sure) {                                      // maybe still elsewhere: ask shfs next time
        d->pt_complete = false;
        log_debug("not sure " + name + " is gone from every disk - " + (rel_dir.empty() ? "/" : rel_dir) + " is listed by shfs next time");
    }
}

// attribute / content changes, re-read a second later

static void flush_pending() {
    auto todo = std::move(_pending);
    _pending.clear();
    for (auto& kv : todo) {
        auto& p = kv.second;
        // after a read only (atime): a file read from the page cache doesn't need its disk;
        // re-reading it then could, so not while the disk sleeps (pools are always up)
        if (!p.xattrs && !g_branches[p.b]->up) continue;
        std::string abs = g_branches[p.b]->root + (p.dir.empty() ? "" : "/" + p.dir) + "/" + p.name;
        struct stat st;
        if (lstat(abs.c_str(), &st) != 0) continue;    // gone: a delete event follows
        // the disk is up: it was just used. After a read only atime changed: xattrs as they are
        std::shared_ptr<const Xattrs> xs;
        if (p.xattrs) xs = read_xattrs(abs);
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        DirNode* d = tree_find(p.dir);
        if (!d) { g_untraced++; continue; }
        touch_attr(d, dbit(p.b));                        // a reader's older stat must not land after this
        auto it = d->entries.find(p.name);
        if (it == d->entries.end()) continue;
        Entry& e = it->second;
        if (!attr_from_branch_wins(e, p.b)) continue;   // another disk's copy is the one shown
        e.attr.from(st);
        e.has_attr = true;
        e.attr_from = (int8_t)p.b;
        if (p.xattrs) e.xattrs = xs;
    }
}

// ---------------------------------------------------------------------------
// Reading events
// ---------------------------------------------------------------------------
static void handle(const struct fanotify_event_metadata* ev, std::map<std::string, int>& mfds) {
    g_events++;
    if (ev->mask & FAN_Q_OVERFLOW) {
        log_warn("fanotify queue overflow - the cache starts over");
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        tree_clear_all();
        for (size_t i = 0; i < g_branches.size(); i++) harvester_restart((int)i);
        return;
    }
    auto* info = (const struct fanotify_event_info_fid*)(ev + 1);
    if ((const char*)info >= (const char*)ev + ev->event_len) { log_debug("event without info"); return; }
    if (info->hdr.info_type != FAN_EVENT_INFO_TYPE_DFID_NAME) {
        log_debug("event info type " + std::to_string(info->hdr.info_type));
        return;
    }
    auto* fh = (struct file_handle*)info->handle;
    const char* name = (const char*)(fh->f_handle + fh->handle_bytes);
    // a folder's own change (chmod, chown, xattrs, ACLs) comes as the folder itself, name "."
    bool self = !*name || !strcmp(name, ".");
    if (self && !(ev->mask & (FAN_ATTRIB | FAN_MODIFY))) return;

    // which mark: same fsid as the statfs of the mount
    int dfd = -1;
    bool known_fs = false;
    int err = 0;
    {
        std::lock_guard<std::mutex> l(_marks_mu);
        for (auto& kv : _marks) {
            auto it = mfds.find(kv.first);
            if (it == mfds.end()) {
                int fd = open(kv.first.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);   // open_by_handle_at rejects O_PATH
                it = mfds.emplace(kv.first, fd).first;
            }
            if (it->second < 0) continue;
            struct statfs sfs;
            if (fstatfs(it->second, &sfs) != 0 || memcmp(&sfs.f_fsid, &info->fsid, sizeof sfs.f_fsid)) continue;
            known_fs = true;
            dfd = open_by_handle_at(it->second, fh, O_PATH | O_CLOEXEC);
            if (dfd >= 0) break;
            err = errno;
        }
    }
    if (!known_fs) {
        // a mount was just added or removed (rescan follows), or something we can't place
        if (++_unplaced > 100) events_broken("Change events from a file system we can't place");
        return;
    }
    if (dfd < 0) {
        if (err == ESTALE || err == ENOENT) return;    // the folder is gone already; its own event follows
        events_broken(std::string("Can't resolve a change event (") + strerror(err) + ")");
        return;
    }
    _unplaced = 0;
    char link[64], abs[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", dfd);
    auto n = readlink(link, abs, sizeof abs - 1);
    if (n <= 0) { close(dfd); return; }
    abs[n] = 0;
    std::string rel;
    int b = branch_of_path(abs, rel);
    if (b < 0 || !tree_cacheable(rel)) { close(dfd); return; }
    if (self) {
        auto slash = rel.rfind('/');
        if (!rel.empty()) restat_later(b, slash == std::string::npos ? "" : rel.substr(0, slash), rel.substr(slash + 1));
        close(dfd);
        return;
    }

    if (ev->mask & (FAN_CREATE | FAN_MOVED_TO)) {
        struct stat st;
        if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
            apply_added(b, rel, name, st, ev->mask & FAN_CREATE, read_xattrs(std::string(abs) + "/" + name));
    }
    if (ev->mask & (FAN_DELETE | FAN_MOVED_FROM)) {
        // gone from this disk unless it is back already (rename over, mover copy)
        struct stat st;
        if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            apply_removed(b, rel, name, ev->mask & FAN_MOVED_FROM);
    }
    if (ev->mask & (FAN_ATTRIB | FAN_MODIFY)) restat_later(b, rel, name);
    else if (ev->mask & FAN_CLOSE_NOWRITE) restat_later(b, rel, name, false);   // read: atime
    // a name came or went: the folder's own mtime and ctime changed, with no event of its own
    if ((ev->mask & (FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO)) && !rel.empty()) {
        auto slash = rel.rfind('/');
        restat_later(b, slash == std::string::npos ? "" : rel.substr(0, slash), rel.substr(slash + 1), false);
    }
    close(dfd);
    if (g_debug) log_debug("event " + std::string(abs) + "/" + name + " mask 0x" +
                           [&] { char x[20]; snprintf(x, sizeof x, "%llx", (unsigned long long)ev->mask); return std::string(x); }());
}

static void loop() {
    alignas(struct fanotify_event_metadata) char buf[65536];
    int64_t last_scan = now_ms();
    while (!_stop) {
        struct pollfd pfd = {_fan, POLLIN, 0};
        int r = poll(&pfd, 1, 500);
        if (r < 0 && errno != EINTR) log_err(std::string("fanotify poll: ") + strerror(errno));
        if (r > 0) {
            std::map<std::string, int> mfds;    // mount fds for this batch only: never pin a mount
            for (;;) {
                ssize_t len = read(_fan, buf, sizeof buf);
                if (len < 0 && errno != EAGAIN) log_err(std::string("fanotify read: ") + strerror(errno));
                if (len <= 0) break;
                for (auto* ev = (struct fanotify_event_metadata*)buf; FAN_EVENT_OK(ev, len); ev = FAN_EVENT_NEXT(ev, len))
                    handle(ev, mfds);
            }
            for (auto& kv : mfds) if (kv.second >= 0) close(kv.second);
        }
        if (!_pending.empty() && now_ms() - _pending_since >= 1000) flush_pending();
        if (now_ms() - last_scan >= 30000) {
            last_scan = now_ms();
            events_rescan_mounts();
        }
    }
}

bool events_start() {
    _fan = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_UNLIMITED_QUEUE | FAN_REPORT_DFID_NAME,
                         O_RDONLY | O_LARGEFILE);
    if (_fan < 0) {
        log_err(std::string("fanotify_init: ") + strerror(errno) + " - caching is off");
        return false;
    }
    events_rescan_mounts();
    _stop = false;
    _thr = std::thread(loop);
    return true;
}

void events_stop() {
    _stop = true;
    if (_thr.joinable()) _thr.join();
    if (_fan >= 0) close(_fan);
    _fan = -1;
    _marks.clear();
}

// ---------------------------------------------------------------------------
// In-place upgrade: the group fd and its marks go over as they are, so no
// change is missed: while neither unwaked reads, events queue in the kernel
// (FAN_UNLIMITED_QUEUE). Re-stats waiting for their second go along too.
// ---------------------------------------------------------------------------
#include "handover.h"

void events_pause() {
    _stop = true;
    if (_thr.joinable()) _thr.join();
}

void events_resume() {
    if (_fan < 0 || _thr.joinable()) return;
    _stop = false;
    _thr = std::thread(loop);
}

void events_handover_out(HoWriter& w) {
    w.fd(_fan);
    w.u8(_scanned_once);
    w.u8(_broken);
    w.u64(g_events.load());
    {
        std::lock_guard<std::mutex> l(_marks_mu);
        w.u64(_marks.size());
        for (auto& kv : _marks) {
            w.str(kv.first);
            w.u32((uint32_t)kv.second.branch);
            w.str(kv.second.fstype);
            w.u64((uint64_t)kv.second.dev);
        }
    }
    auto un = std::atomic_load(&g_cfg.uncached_paths);
    w.u64(un->size());
    for (auto& u : *un) w.str(u);
    w.u64(_pending.size());
    for (auto& kv : _pending) { w.u32((uint32_t)kv.second.b); w.str(kv.second.dir); w.str(kv.second.name); }
}

void events_handover_in(HoReader& r) {
    _fan = r.fd();
    _scanned_once = r.u8();
    _broken = r.u8();
    g_events = r.u64();
    {
        std::lock_guard<std::mutex> l(_marks_mu);
        _marks.clear();
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            Mark m;
            m.path = r.str();
            m.branch = (int)r.u32();
            m.fstype = r.str();
            m.dev = (dev_t)r.u64();
            if (m.branch < 0 || (size_t)m.branch >= g_branches.size()) throw std::runtime_error("mark of an unknown disk");
            _marks[m.path] = m;
        }
    }
    auto un = std::make_shared<std::vector<std::string>>();
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) un->push_back(r.str());
    std::atomic_store(&g_cfg.uncached_paths, std::shared_ptr<const std::vector<std::string>>(un));
    n = r.u64();
    if (n) _pending_since = now_ms();
    for (uint64_t i = 0; i < n; i++) {
        int b = (int)r.u32();
        auto dir = r.str();
        auto name = r.str();
        if (b >= 0 && (size_t)b < g_branches.size()) restat_later(b, dir, name);   // xattrs too: not handed over
    }
}
