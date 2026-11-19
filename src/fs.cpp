// fs.cpp - the FUSE file system on /mnt/user
//
// Stacked on top of shfs: g_shfs_fd is an O_PATH handle to the shfs root taken
// before our mount covered it, and every passthrough call is relative to it
// (*at() calls, or /proc/self/fd/N/... where no *at() exists). Nothing here may
// ever resolve a path under /mnt/user by name: that would re-enter us.
//
// Metadata (lookup, getattr, readdir, getxattr, listxattr) is answered from the
// union tree when it can be; everything else goes to shfs and updates the tree.
// File data uses kernel FUSE passthrough when available, so Unwake adds no page
// cache of its own; reads of a file that lives on exactly one known disk go to
// that disk directly (DATA_PATH=direct), skipping shfs.
#include "unwaked.h"

#include <cstring>
#include <fstream>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

Stats             g_stats;
int               g_shfs_fd = -1;
std::atomic<bool> g_passthrough{false};
std::atomic<bool> g_caching{true};

static constexpr double ENTRY_TIMEOUT = 1.0;
static constexpr double ATTR_TIMEOUT  = 1.0;
static constexpr uint64_t FUSE_UNKNOWN_INO = 0xffffffff;

// ---------------------------------------------------------------------------
// What went to shfs lately: when a disk spins up, the log lists these
// ---------------------------------------------------------------------------
struct ShfsReq { int64_t ms; const char* op; std::string rel, to, note; Mask mask; std::string who; };
static std::mutex _req_mu;
static std::vector<ShfsReq> _reqs;      // ring
static size_t _req_next = 0;
static constexpr size_t REQ_RING = 512;
static constexpr int64_t REQ_WINDOW = 30000, REQ_WINDOW_WRITE = 120000;

// Who sent the request being handled on this thread (FUSE gives the caller's thread id),
// named only with debug logging: "qemu-img 1234, parent php-fpm 99"
static thread_local pid_t t_caller = 0;

template <auto F> struct Caller;
template <typename... A, void (*F)(fuse_req_t, A...)>
struct Caller<F> {
    static void call(fuse_req_t req, A... a) { t_caller = fuse_req_ctx(req)->pid; F(req, a...); }
};

static std::string proc_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

static std::string caller_name(pid_t tid) {
    if (tid <= 0) return {};
    static std::mutex mu;
    static std::unordered_map<pid_t, std::pair<int64_t, std::string>> cache;   // tid -> (when, name)
    auto now = now_ms();
    {
        std::lock_guard<std::mutex> l(mu);
        auto it = cache.find(tid);
        if (it != cache.end() && it->second.first > now - 5000) return it->second.second;
    }
    pid_t tgid = tid, ppid = 0;
    {
        std::ifstream f("/proc/" + std::to_string(tid) + "/status");
        std::string line;
        while (std::getline(f, line)) {
            if (!line.compare(0, 5, "Tgid:")) tgid = (pid_t)atoi(line.c_str() + 5);
            else if (!line.compare(0, 5, "PPid:")) { ppid = (pid_t)atoi(line.c_str() + 5); break; }
        }
    }
    std::string comm = proc_line("/proc/" + std::to_string(tgid) + "/comm");
    std::string who = (comm.empty() ? std::string("?") : comm) + " " + std::to_string(tgid);
    if (ppid > 0) {
        std::string pc = proc_line("/proc/" + std::to_string(ppid) + "/comm");
        who += ", parent " + (pc.empty() ? std::string("?") : pc) + " " + std::to_string(ppid);
    }
    std::lock_guard<std::mutex> l(mu);
    if (cache.size() > 1024) cache.clear();
    cache[tid] = {now, who};
    return who;
}

void note_shfs(const char* op, const std::string& rel, const std::string& to, const std::string& note, Mask mask) {
    std::string who = g_debug ? caller_name(t_caller) : std::string();
    std::lock_guard<std::mutex> l(_req_mu);
    ShfsReq r{now_ms(), op, rel, to, note, mask, std::move(who)};
    if (_reqs.size() < REQ_RING) _reqs.push_back(std::move(r));
    else _reqs[_req_next] = std::move(r);
    _req_next = (_req_next + 1) % REQ_RING;
}

static bool is_write(const char* op) {
    static const char* w[] = {"setattr", "mkdir", "rmdir", "unlink", "rename", "create", "mknod", "symlink",
                              "link", "setxattr", "removexattr", "open (write)"};
    for (auto x : w) if (!strcmp(op, x)) return true;
    return false;
}

// Disks shfs may touch for a name in dir: the ones holding dir. Walks down while the
// tree is sure of a level; where it isn't, the last sure level bounds it. All disks if
// the tree knows nothing. shfs looks a name up on every disk with its directory.
static Mask disks_of_dir(const std::string& dir) {
    if (!g_tree.per_branch || !g_caching) return g_tree.all;
    std::shared_lock<std::shared_mutex> l(g_tree.mu);
    Mask m = g_tree.all;
    DirNode* d = &g_tree.root;
    bool sure = (d->known & g_tree.all) == g_tree.all;
    for (auto& c : split_path(dir)) {
        if (!sure) break;
        auto it = d->entries.find(c);
        if (it == d->entries.end() || !it->second.mask) break;
        m = it->second.mask;                    // a sure level knows its entries' disks
        if (!it->second.dir) break;             // not read (a share passed to shfs): bounded by that
        d = it->second.dir.get();
        sure = tree_all_known(d);
    }
    return m;
}

static std::string dir_of(const std::string& rel);

std::vector<std::string> shfs_for(int b, size_t max, size_t& total, size_t& others) {
    std::vector<ShfsReq> v;
    auto now = now_ms();
    {
        std::lock_guard<std::mutex> l(_req_mu);
        for (auto& r : _reqs)
            if (r.ms >= now - (is_write(r.op) ? REQ_WINDOW_WRITE : REQ_WINDOW)) v.push_back(r);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.ms < b.ms; });
    Mask want = g_tree.per_branch && b >= 0 ? bit(b) : ~Mask(0);
    std::vector<const ShfsReq*> hit;
    others = 0;
    for (auto& r : v) {
        Mask m = r.mask;
        if (!m) {   // a share itself, or a listing: its own disks; anything else: its directory's
            bool own = !strcmp(r.op, "readdir") || r.rel.find('/') == std::string::npos;
            m = disks_of_dir(own ? r.rel : dir_of(r.rel));
            if (!r.to.empty()) m |= disks_of_dir(dir_of(r.to));
        }
        if (m & want) hit.push_back(&r);
        else if (r.ms >= now - REQ_WINDOW) others++;
    }
    total = hit.size();
    std::vector<std::string> out;
    for (size_t i = hit.size() > max ? hit.size() - max : 0; i < hit.size(); i++) {
        auto& r = *hit[i];
        char t[32];
        time_t s = r.ms / 1000;
        struct tm tm;
        localtime_r(&s, &tm);
        strftime(t, sizeof t, "%H:%M:%S", &tm);
        out.push_back(std::string(t) + " " + r.op + " " + (r.rel.empty() ? "/" : r.rel) +
                      (r.to.empty() ? "" : " -> " + r.to) + (r.note.empty() ? "" : " " + r.note) +
                      (r.who.empty() ? "" : " [" + r.who + "]"));
    }
    return out;
}

std::vector<std::string> shfs_recent(const std::string& dir, size_t max) {
    std::vector<ShfsReq> v;
    {
        std::lock_guard<std::mutex> l(_req_mu);
        for (auto& r : _reqs)
            if (dir_of(r.rel) == dir || r.rel == dir || (!r.to.empty() && dir_of(r.to) == dir)) v.push_back(r);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.ms < b.ms; });
    std::vector<std::string> out;
    for (size_t i = v.size() > max ? v.size() - max : 0; i < v.size(); i++) {
        auto& r = v[i];
        char t[32];
        time_t s = r.ms / 1000;
        struct tm tm;
        localtime_r(&s, &tm);
        strftime(t, sizeof t, "%m-%d %H:%M:%S", &tm);
        out.push_back(std::string(t) + " " + r.op + " " + (r.rel.empty() ? "/" : r.rel) +
                      (r.to.empty() ? "" : " -> " + r.to) + (r.note.empty() ? "" : " " + r.note) +
                      (r.who.empty() ? "" : " [" + r.who + "]"));
    }
    return out;
}

// An error for the client. Expected ones only in the debug log.
static void fail(fuse_req_t req, const char* op, const std::string& rel, int e) {
    if (e == EIO || e == ENOTCONN || e == ESTALE || e == EBADF || e == ENOMEM || e == EFAULT || e == EINVAL)
        log_warn(std::string(op) + " " + rel + ": " + strerror(e));
    else if (e != ENOENT && e != ENODATA && e != EEXIST && e != ENOTEMPTY)
        log_debug(std::string(op) + " " + rel + ": " + strerror(e));
    fuse_reply_err(req, e);
}

// ---------------------------------------------------------------------------
// Node table: FUSE node id <-> (parent, name)
// ---------------------------------------------------------------------------
struct Node {
    fuse_ino_t  parent = 0;
    std::string name;
    uint64_t    nlookup = 0;
    bool        unlinked = false;
    std::vector<int> wfds;    // open for writing: getattr asks the file itself
    // kernel passthrough: one backing file per inode, shared by all its opens (the
    // kernel refuses a second, different one with EIO)
    int      backing_id = 0;
    int      backing_fd = -1;   // our own open of the backing file: the truth for size and times
    uint32_t backing_refs = 0;
    bool     backing_read = false, backing_write = false;   // what the backing file allows
};
static std::mutex _nmu;
std::atomic<uint64_t> g_nodes_forgotten{0};
static std::unordered_map<fuse_ino_t, Node> _nodes;
static std::unordered_map<std::string, fuse_ino_t> _children;
static fuse_ino_t _next_ino = 2;

static std::string ckey(fuse_ino_t p, const std::string& n) { return std::to_string(p) + "/" + n; }

static void nodes_init() {
    std::lock_guard<std::mutex> l(_nmu);
    Node root;
    root.nlookup = 1;
    _nodes[FUSE_ROOT_ID] = root;
}

// relative path of a node ("" = root); false if unknown or unlinked
static bool path_of(fuse_ino_t ino, std::string& out) {
    std::lock_guard<std::mutex> l(_nmu);
    std::vector<const std::string*> parts;
    while (ino != FUSE_ROOT_ID) {
        auto it = _nodes.find(ino);
        if (it == _nodes.end() || it->second.unlinked) return false;
        parts.push_back(&it->second.name);
        ino = it->second.parent;
    }
    out.clear();
    for (auto p = parts.rbegin(); p != parts.rend(); ++p) {
        if (!out.empty()) out += '/';
        out += **p;
    }
    return true;
}

// node for parent/name, with one more lookup reference (for an entry reply)
static fuse_ino_t node_ref(fuse_ino_t parent, const std::string& name) {
    std::lock_guard<std::mutex> l(_nmu);
    auto k = ckey(parent, name);
    auto it = _children.find(k);
    fuse_ino_t ino;
    if (it != _children.end()) ino = it->second;
    else {
        ino = _next_ino++;
        Node n;
        n.parent = parent;
        n.name = name;
        _nodes[ino] = n;
        _children[k] = ino;
    }
    _nodes[ino].nlookup++;
    return ino;
}

static void node_forget(fuse_ino_t ino, uint64_t n) {
    std::lock_guard<std::mutex> l(_nmu);
    auto it = _nodes.find(ino);
    if (it == _nodes.end() || ino == FUSE_ROOT_ID) return;
    it->second.nlookup = it->second.nlookup > n ? it->second.nlookup - n : 0;
    if (it->second.nlookup) return;
    if (!it->second.unlinked) {
        auto c = _children.find(ckey(it->second.parent, it->second.name));
        if (c != _children.end() && c->second == ino) _children.erase(c);
    }
    _nodes.erase(it);
    g_nodes_forgotten++;
}

static void node_unlinked(fuse_ino_t parent, const std::string& name) {
    std::lock_guard<std::mutex> l(_nmu);
    auto c = _children.find(ckey(parent, name));
    if (c == _children.end()) return;
    auto it = _nodes.find(c->second);
    if (it != _nodes.end()) it->second.unlinked = true;
    _children.erase(c);
}

static void node_renamed(fuse_ino_t op, const std::string& on, fuse_ino_t np, const std::string& nn, bool exchange) {
    std::lock_guard<std::mutex> l(_nmu);
    auto a = _children.find(ckey(op, on));
    fuse_ino_t ia = a == _children.end() ? 0 : a->second;
    auto b = _children.find(ckey(np, nn));
    fuse_ino_t ib = b == _children.end() ? 0 : b->second;
    if (ia) _children.erase(ckey(op, on));
    if (ib) _children.erase(ckey(np, nn));
    if (ia) { _nodes[ia].parent = np; _nodes[ia].name = nn; _children[ckey(np, nn)] = ia; }
    if (ib) {
        if (exchange) { _nodes[ib].parent = op; _nodes[ib].name = on; _children[ckey(op, on)] = ib; }
        else _nodes[ib].unlinked = true;
    }
}

// The inode's backing for an open that needs read and/or write: the existing one
// (another reference) or a new one. 0: no passthrough for it; -EBUSY: the existing
// backing can't do what this open needs (it had to be read-only, now opened for writing).
static int node_backing_get(fuse_req_t req, fuse_ino_t ino, int fd, bool rd, bool wr) {
    std::lock_guard<std::mutex> l(_nmu);
    auto it = _nodes.find(ino);
    if (it == _nodes.end()) return 0;
    Node& n = it->second;
    if (n.backing_id) {
        if ((rd && !n.backing_read) || (wr && !n.backing_write)) return -EBUSY;
        n.backing_refs++;
        return n.backing_id;
    }
    // Back every regular file with a read-write open of it, whatever this first open
    // needs: the kernel keeps this one backing for all later opens of the file, and a
    // writer coming after a reader must be able to share it. Only where that can't be
    // had (read-only file system, a file being executed: ETXTBSY) the open's own fd.
    int bfd = -1;
    bool brd = rd, bwr = wr;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        bfd = open(("/proc/self/fd/" + std::to_string(fd)).c_str(), O_RDWR | O_CLOEXEC | O_LARGEFILE | O_NOCTTY);
        if (bfd >= 0) brd = bwr = true;
    }
    if (bfd < 0) bfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    int id = fuse_passthrough_open(req, bfd >= 0 ? bfd : fd);
    if (id <= 0) { if (bfd >= 0) close(bfd); return 0; }
    n.backing_id = id;
    n.backing_fd = bfd;
    n.backing_refs = 1;
    n.backing_read = brd;
    n.backing_write = bwr;
    return id;
}

static void node_backing_put(fuse_req_t req, fuse_ino_t ino) {
    int id = 0;
    {
        std::lock_guard<std::mutex> l(_nmu);
        auto it = _nodes.find(ino);
        if (it == _nodes.end() || !it->second.backing_id) return;
        Node& n = it->second;
        if (n.backing_refs && --n.backing_refs) return;
        id = n.backing_id;
        n.backing_id = 0;
        if (n.backing_fd >= 0) close(n.backing_fd);
        n.backing_fd = -1;
    }
    fuse_passthrough_close(req, id);
}

// while a file is open for writing, getattr asks the file itself: the backing file if
// there is one (writes through passthrough reach it, possibly straight on the disk,
// so shfs may not know the new size yet), else the writer's own fd
static int node_wfd(fuse_ino_t ino) {
    std::lock_guard<std::mutex> l(_nmu);
    auto it = _nodes.find(ino);
    if (it == _nodes.end() || it->second.wfds.empty()) return -1;
    return it->second.backing_fd >= 0 ? it->second.backing_fd : it->second.wfds.front();
}

static void node_wfd_add(fuse_ino_t ino, int fd, bool add) {
    std::lock_guard<std::mutex> l(_nmu);
    auto it = _nodes.find(ino);
    if (it == _nodes.end()) return;
    auto& v = it->second.wfds;
    if (add) v.push_back(fd);
    else v.erase(std::remove(v.begin(), v.end(), fd), v.end());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char* rp(const std::string& rel) { return rel.empty() ? "." : rel.c_str(); }
static std::string proc_path(const std::string& rel) {
    return "/proc/self/fd/" + std::to_string(g_shfs_fd) + (rel.empty() ? "" : "/" + rel);
}
static std::string dir_of(const std::string& rel) {
    auto s = rel.rfind('/');
    return s == std::string::npos ? "" : rel.substr(0, s);
}
static std::string base_of(const std::string& rel) {
    auto s = rel.rfind('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}
static bool caching_for(const std::string& rel) { return g_caching && tree_cacheable(rel); }

static void reply_entry(fuse_req_t req, fuse_ino_t parent, const std::string& name, const struct stat& st) {
    struct fuse_entry_param e = {};
    e.ino = node_ref(parent, name);
    e.attr = st;
    e.attr.st_ino = e.ino;
    e.attr_timeout = ATTR_TIMEOUT;
    e.entry_timeout = ENTRY_TIMEOUT;
    if (fuse_reply_entry(req, &e) != 0) node_forget(e.ino, 1);
}

static void reply_negative(fuse_req_t req) {
    struct fuse_entry_param e = {};
    e.entry_timeout = ENTRY_TIMEOUT;
    fuse_reply_entry(req, &e);
}

// 1 = known (a filled), 0 = known absent, -1 = unknown
static int cache_get(const std::string& rel, Attr& a) {
    if (rel.empty() || !caching_for(rel)) return -1;
    std::shared_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(dir_of(rel));
    if (!d) return -1;
    auto it = d->entries.find(base_of(rel));
    if (it != d->entries.end()) {
        if (!it->second.has_attr) return -1;
        a = it->second.attr;
        return 1;
    }
    return tree_servable(d) ? 0 : -1;
}

// Tests only: <run_dir>/test-slow-<what> holds milliseconds to wait at a point where a
// change can race the request (only with --branch); nothing in other builds
#if UNWAKE_TEST_HOOKS
static void test_pause(const char* what) {
    if (g_cfg.branch_args.empty()) return;
    std::ifstream f(g_cfg.run_dir + "/test-slow-" + what);
    int ms = 0;
    if (f >> ms && ms > 0) usleep(ms * 1000);
}
#else
static inline void test_pause(const char*) {}
#endif

// Record a stat learned from shfs: s was taken before asking. If anything changed along
// the path meanwhile, nothing is written: fanotify brings the truth.
static void cache_put(const std::string& rel, const struct stat& st, const PathSnap& s) {
    if (rel.empty() || !caching_for(rel)) return;
    auto dir = dir_of(rel);
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    if (!tree_snap_ok(dir, s, true)) return;
    DirNode* d = tree_find(dir);
    if (!d) d = tree_ensure(dir, 0);
    auto r = d->entries.try_emplace(base_of(rel));
    Entry& e = r.first->second;
    if (r.second) g_tree.entries++;
    e.attr.from(st);
    e.has_attr = true;
    e.attr_from = -1;
    e.is_dir = S_ISDIR(st.st_mode);
    e.dtype = (uint8_t)IFTODT(st.st_mode);
    if (r.second) touch(d); else touch_attr(d);
}

// New attributes of a name the tree has (after a write, chmod, truncate, ...). Never adds
// the name: a rename or unlink may have raced with the caller (FUSE sends RELEASE after
// close() returned, so a program can rename the file before we see it).
static void cache_update(const std::string& rel, const struct stat& st, const PathSnap& s) {
    if (rel.empty() || !caching_for(rel)) return;
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    if (!tree_snap_ok(dir_of(rel), s, true)) return;
    DirNode* d = tree_find(dir_of(rel));
    if (!d) return;
    auto it = d->entries.find(base_of(rel));
    if (it == d->entries.end()) return;
    Entry& e = it->second;
    e.attr.from(st);
    e.has_attr = true;
    e.attr_from = -1;
    e.is_dir = S_ISDIR(st.st_mode);
    e.dtype = (uint8_t)IFTODT(st.st_mode);
    touch_attr(d);
}

// Our own change through shfs removed a name (s: before the unlink). If the folder changed
// meanwhile (the name back already, from somewhere else), fanotify sorts it out.
static void cache_removed(const std::string& rel, const PathSnap& s) {
    if (rel.empty()) return;
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(dir_of(rel));
    if (!d) { g_untraced++; return; }
    if (!tree_snap_ok(dir_of(rel), s, false)) { touch(d); return; }
    auto it = d->entries.find(base_of(rel));
    if (it == d->entries.end()) { touch(d); return; }
    if (__builtin_popcountll(it->second.mask) > 1) {
        d->pt_complete = false;              // shfs may have removed only one copy
        touch(d);
        return;
    }
    tree_erase(d, base_of(rel));
}

static void cache_xattrs_unknown(const std::string& rel) {
    if (rel.empty()) return;
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(dir_of(rel));
    if (!d) { g_untraced++; return; }
    touch_attr(d);
    auto it = d->entries.find(base_of(rel));
    if (it != d->entries.end()) it->second.xattrs.reset();
}

// Our own rename through shfs (s1, s2: before it). If either folder changed meanwhile,
// the tree is left to fanotify.
static void cache_renamed(const std::string& from, const std::string& to, bool exchange,
                          const PathSnap& s1, const PathSnap& s2) {
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d1 = tree_find(dir_of(from));
    DirNode* d2 = tree_find(dir_of(to));
    if (!d1 || !d2) g_untraced++;
    bool ok = tree_snap_ok(dir_of(from), s1, false) && tree_snap_ok(dir_of(to), s2, false);
    if (exchange || !ok) {
        for (auto& p : {std::make_pair(d1, base_of(from)), std::make_pair(d2, base_of(to))})
            if (p.first) {
                if (exchange || !ok) touch(p.first);
                if (exchange) { tree_erase(p.first, p.second); p.first->pt_complete = false; }
            }
        return;
    }
    // fanotify may have applied this rename already (MOVED_FROM/TO read before we got
    // here): then the source is gone and the target is in place - leave it as it is
    auto nh = d1 ? d1->entries.extract(base_of(from)) : decltype(d1->entries.extract(""))();
    if (d1) touch(d1);
    if (nh.empty()) {
        if (d2) { touch(d2); if (!d2->entries.count(base_of(to))) d2->pt_complete = false; }
        return;
    }
    if (d2) tree_erase(d2, base_of(to));
    if (!d2) { tree_count_add(nh.mapped(), -1); return; }
    nh.key() = base_of(to);
    auto ins = d2->entries.insert(std::move(nh));
    Entry& e = ins.position->second;
    if (e.dir) { e.dir->parent = d2; e.dir->name = &ins.position->first; }
    touch(d2);
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
static void op_init(void*, struct fuse_conn_info* conn) {
    for (auto cap : {FUSE_CAP_SPLICE_READ, FUSE_CAP_SPLICE_WRITE, FUSE_CAP_SPLICE_MOVE,
                     FUSE_CAP_READDIRPLUS, FUSE_CAP_READDIRPLUS_AUTO, FUSE_CAP_PARALLEL_DIROPS,
                     FUSE_CAP_EXPORT_SUPPORT, FUSE_CAP_ATOMIC_O_TRUNC, FUSE_CAP_ASYNC_DIO})
        fuse_set_feature_flag(conn, cap);
    fuse_unset_feature_flag(conn, FUSE_CAP_WRITEBACK_CACHE);
    // no POSIX ACL evaluation in the kernel: like shfs, mode bits only
    if (fuse_set_feature_flag(conn, FUSE_CAP_PASSTHROUGH)) {
        g_passthrough = true;
        conn->max_backing_stack_depth = FUSE_BACKING_STACKED_UNDER;
    }
    conn->max_write = 1 << 20;
    log_info(std::string("File data: ") + (g_passthrough ? "kernel passthrough" : "through unwaked (kernel without FUSE passthrough)") +
             (g_cfg.direct_reads ? ", reads of files on one known disk go to that disk directly" : ", all through shfs"));
}

static void op_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    if (!strcmp(name, ".") || !strcmp(name, "..")) {     // NFS export
        fuse_ino_t target = parent;
        std::string rel = prel;
        if (!strcmp(name, "..")) {
            std::lock_guard<std::mutex> l(_nmu);
            target = parent == FUSE_ROOT_ID ? FUSE_ROOT_ID : _nodes[parent].parent;
            rel = dir_of(prel);
        }
        struct stat st;
        if (fstatat(g_shfs_fd, rp(rel), &st, AT_SYMLINK_NOFOLLOW) != 0) return (void)fuse_reply_err(req, errno);
        {
            std::lock_guard<std::mutex> l(_nmu);
            _nodes[target].nlookup++;
        }
        struct fuse_entry_param e = {};
        e.ino = target;
        e.attr = st;
        e.attr.st_ino = target;
        e.attr_timeout = ATTR_TIMEOUT;
        e.entry_timeout = ENTRY_TIMEOUT;
        if (fuse_reply_entry(req, &e) != 0) node_forget(target, 1);
        return;
    }
    auto rel = join_path(prel, name);
    Attr a;
    int c = cache_get(rel, a);
    if (c == 1) {
        g_stats.lookup_hit++;
        struct stat st;
        a.to(st, 0);
        return reply_entry(req, parent, name, st);
    }
    if (c == 0) { g_stats.lookup_neg++; return reply_negative(req); }
    g_stats.lookup_miss++;
    note_shfs("lookup", rel);
    if (caching_for(rel)) log_debug("lookup miss " + rel);   // shares not in RAM: no line per request
    auto snap = tree_snap(prel);
    struct stat st;
    int r = fstatat(g_shfs_fd, rel.c_str(), &st, AT_SYMLINK_NOFOLLOW);
    if (r != 0 && errno == ENOENT) {
        // a folder above was renamed meanwhile: the old path is gone, ask again by the new one
        std::string p2;
        if (path_of(parent, p2) && p2 != prel) {
            prel = p2;
            rel = join_path(prel, name);
            snap = tree_snap(prel);
            r = fstatat(g_shfs_fd, rel.c_str(), &st, AT_SYMLINK_NOFOLLOW);
        }
    }
    if (r != 0) {
        if (errno == ENOENT) return reply_negative(req);
        return (void)fuse_reply_err(req, errno);
    }
    test_pause("lookup");
    cache_put(rel, st, snap);
    reply_entry(req, parent, name, st);
}

static void op_forget(fuse_req_t req, fuse_ino_t ino, uint64_t n) {
    node_forget(ino, n);
    fuse_reply_none(req);
}

static void op_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data* f) {
    for (size_t i = 0; i < count; i++) node_forget(f[i].ino, f[i].nlookup);
    fuse_reply_none(req);
}

struct Handle {
    int  fd = -1;
    int  backing_id = 0;
    bool write = false;
    fuse_ino_t ino = 0;
};

// fi->fh is an id into these tables, not a pointer: after an in-place upgrade the
// kernel hands the new unwaked the values the old one gave out.
struct DirHandle;
static std::shared_mutex _hmu;
static std::unordered_map<uint64_t, Handle*> _handles;
static std::unordered_map<uint64_t, DirHandle*> _dhandles;
static uint64_t _next_fh = 1;

template <class T> static uint64_t fh_add(std::unordered_map<uint64_t, T*>& m, T* h) {
    std::unique_lock<std::shared_mutex> l(_hmu);
    uint64_t id = _next_fh++;
    m[id] = h;
    return id;
}
template <class T> static T* fh_get(std::unordered_map<uint64_t, T*>& m, const struct fuse_file_info* fi) {
    if (!fi || !fi->fh) return nullptr;
    std::shared_lock<std::shared_mutex> l(_hmu);
    auto it = m.find(fi->fh);
    return it == m.end() ? nullptr : it->second;
}
template <class T> static T* fh_take(std::unordered_map<uint64_t, T*>& m, uint64_t id) {
    std::unique_lock<std::shared_mutex> l(_hmu);
    auto it = m.find(id);
    if (it == m.end()) return nullptr;
    T* h = it->second;
    m.erase(it);
    return h;
}
static Handle* H(const struct fuse_file_info* fi) { return fh_get(_handles, fi); }
static int fd_of(const struct fuse_file_info* fi) { auto h = H(fi); return h ? h->fd : -1; }

static void op_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
    struct stat st;
    int fd = node_wfd(ino);          // being written: the file itself knows best
    if (fd < 0) fd = fd_of(fi);
    if (fd >= 0) {
        if (fstat(fd, &st) != 0) return (void)fuse_reply_err(req, errno);
    } else {
        std::string rel;
        if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
        Attr a;
        if (cache_get(rel, a) == 1) {
            g_stats.getattr_hit++;
            a.to(st, ino);
            return (void)fuse_reply_attr(req, &st, ATTR_TIMEOUT);
        }
        if (!rel.empty()) { g_stats.getattr_miss++; note_shfs("getattr", rel); }
        auto snap = tree_snap(dir_of(rel));
        int r = fstatat(g_shfs_fd, rp(rel), &st, AT_SYMLINK_NOFOLLOW);
        std::string rel2;
        if (r != 0 && errno == ENOENT && path_of(ino, rel2) && rel2 != rel) {   // a folder above was renamed
            rel = rel2;
            snap = tree_snap(dir_of(rel));
            r = fstatat(g_shfs_fd, rp(rel), &st, AT_SYMLINK_NOFOLLOW);
        }
        if (r != 0) return (void)fuse_reply_err(req, errno);
        test_pause("getattr");
        cache_put(rel, st, snap);
    }
    st.st_ino = ino;
    fuse_reply_attr(req, &st, ATTR_TIMEOUT);
}

static void op_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, struct fuse_file_info* fi) {
    std::string rel;
    int fd = fd_of(fi);
    if (!path_of(ino, rel) && fd < 0) return (void)fuse_reply_err(req, ENOENT);
    auto snap = tree_snap(dir_of(rel));
    int r = 0;
    if (to_set & FUSE_SET_ATTR_MODE)
        r = fd >= 0 ? fchmod(fd, attr->st_mode) : fchmodat(g_shfs_fd, rp(rel), attr->st_mode, 0);
    if (!r && (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))) {
        uid_t u = (to_set & FUSE_SET_ATTR_UID) ? attr->st_uid : (uid_t)-1;
        gid_t g = (to_set & FUSE_SET_ATTR_GID) ? attr->st_gid : (gid_t)-1;
        r = fd >= 0 ? fchown(fd, u, g) : fchownat(g_shfs_fd, rp(rel), u, g, AT_SYMLINK_NOFOLLOW);
    }
    if (!r && (to_set & FUSE_SET_ATTR_SIZE))
        r = fd >= 0 ? ftruncate(fd, attr->st_size) : truncate(proc_path(rel).c_str(), attr->st_size);
    if (!r && (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME))) {
        struct timespec ts[2];
        ts[0].tv_nsec = ts[1].tv_nsec = UTIME_OMIT;
        if (to_set & FUSE_SET_ATTR_ATIME_NOW) ts[0].tv_nsec = UTIME_NOW;
        else if (to_set & FUSE_SET_ATTR_ATIME) ts[0] = attr->st_atim;
        if (to_set & FUSE_SET_ATTR_MTIME_NOW) ts[1].tv_nsec = UTIME_NOW;
        else if (to_set & FUSE_SET_ATTR_MTIME) ts[1] = attr->st_mtim;
        r = fd >= 0 ? futimens(fd, ts) : utimensat(g_shfs_fd, rp(rel), ts, AT_SYMLINK_NOFOLLOW);
    }
    if (r) return fail(req, "setattr", rel, errno);
    note_shfs("setattr", rel);
    struct stat st;
    if ((fd >= 0 ? fstat(fd, &st) : fstatat(g_shfs_fd, rp(rel), &st, AT_SYMLINK_NOFOLLOW)) != 0)
        return (void)fuse_reply_err(req, errno);
    cache_update(rel, st, snap);
    st.st_ino = ino;
    fuse_reply_attr(req, &st, ATTR_TIMEOUT);
}

static void op_readlink(fuse_req_t req, fuse_ino_t ino) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    char buf[PATH_MAX + 1];
    note_shfs("readlink", rel);
    auto n = readlinkat(g_shfs_fd, rp(rel), buf, sizeof buf - 1);
    if (n < 0) return (void)fuse_reply_err(req, errno);
    buf[n] = 0;
    fuse_reply_readlink(req, buf);
}

// after creating a name through shfs as root: give it to the caller, record it, reply
static void created(fuse_req_t req, fuse_ino_t parent, const std::string& rel, const char* name, int fd,
                    const PathSnap& snap) {
    auto ctx = fuse_req_ctx(req);
    int r = fd >= 0 ? fchown(fd, ctx->uid, ctx->gid)
                    : fchownat(g_shfs_fd, rel.c_str(), ctx->uid, ctx->gid, AT_SYMLINK_NOFOLLOW);
    if (r != 0) log_warn("chown " + rel + ": " + strerror(errno));
    struct stat st;
    if ((fd >= 0 ? fstat(fd, &st) : fstatat(g_shfs_fd, rel.c_str(), &st, AT_SYMLINK_NOFOLLOW)) != 0) {
        fuse_reply_err(req, errno);
        return;
    }
    test_pause("create");
    cache_put(rel, st, snap);
    if (fd < 0) reply_entry(req, parent, name, st);
}

static void op_mknod(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, dev_t rdev) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto rel = join_path(prel, name);
    note_shfs("mknod", rel);
    auto snap = tree_snap(prel);
    if (mknodat(g_shfs_fd, rel.c_str(), mode, rdev) != 0) return (void)fuse_reply_err(req, errno);
    created(req, parent, rel, name, -1, snap);
}

static void op_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto rel = join_path(prel, name);
    note_shfs("mkdir", rel);
    auto snap = tree_snap(prel);
    if (mkdirat(g_shfs_fd, rel.c_str(), mode) != 0) return fail(req, "mkdir", rel, errno);
    created(req, parent, rel, name, -1, snap);
}

static void op_symlink(fuse_req_t req, const char* link, fuse_ino_t parent, const char* name) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto rel = join_path(prel, name);
    note_shfs("symlink", rel);
    auto snap = tree_snap(prel);
    if (symlinkat(link, g_shfs_fd, rel.c_str()) != 0) return (void)fuse_reply_err(req, errno);
    created(req, parent, rel, name, -1, snap);
}

static void op_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char* newname) {
    std::string rel, prel;
    if (!path_of(ino, rel) || !path_of(newparent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto nrel = join_path(prel, newname);
    note_shfs("link", rel, nrel);
    auto snap_new = tree_snap(prel), snap_old = tree_snap(dir_of(rel));
    if (linkat(g_shfs_fd, rel.c_str(), g_shfs_fd, nrel.c_str(), 0) != 0) return (void)fuse_reply_err(req, errno);
    struct stat st;
    if (fstatat(g_shfs_fd, nrel.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return (void)fuse_reply_err(req, errno);
    cache_put(nrel, st, snap_new);
    cache_update(rel, st, snap_old);                 // nlink changed
    // FUSE wants the existing inode back; the new name gets its own node on its next lookup
    {
        std::lock_guard<std::mutex> l(_nmu);
        _nodes[ino].nlookup++;
    }
    struct fuse_entry_param e = {};
    e.ino = ino;
    e.attr = st;
    e.attr.st_ino = ino;
    e.attr_timeout = ATTR_TIMEOUT;
    e.entry_timeout = 0;
    if (fuse_reply_entry(req, &e) != 0) node_forget(ino, 1);
}

static void remove_name(fuse_req_t req, fuse_ino_t parent, const char* name, int flags) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto rel = join_path(prel, name);
    note_shfs(flags ? "rmdir" : "unlink", rel);
    auto snap = tree_snap(prel);
    if (unlinkat(g_shfs_fd, rel.c_str(), flags) != 0) return fail(req, flags ? "rmdir" : "unlink", rel, errno);
    test_pause("unlink");
    cache_removed(rel, snap);
    node_unlinked(parent, name);
    fuse_reply_err(req, 0);
}
static void op_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) { remove_name(req, parent, name, 0); }
static void op_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) { remove_name(req, parent, name, AT_REMOVEDIR); }

static void op_rename(fuse_req_t req, fuse_ino_t parent, const char* name, fuse_ino_t newparent,
                      const char* newname, unsigned int flags) {
    std::string a, b;
    if (!path_of(parent, a) || !path_of(newparent, b)) return (void)fuse_reply_err(req, ENOENT);
    auto from = join_path(a, name), to = join_path(b, newname);
    note_shfs("rename", from, to);
    auto s1 = tree_snap(a), s2 = tree_snap(b);
    if (renameat2(g_shfs_fd, from.c_str(), g_shfs_fd, to.c_str(), flags) != 0) {
        int e = errno;
        // shfs rejects rename flags (RENAME_NOREPLACE etc.) with EINVAL, as it does without
        // us. Callers like Qt expect that and fall back to link+unlink, so it's not a warning.
        if (e == EINVAL && flags) {
            log_debug("rename " + from + " -> " + to + " flags " + std::to_string(flags) + ": " + strerror(e));
            return (void)fuse_reply_err(req, e);
        }
        return fail(req, "rename", from + " -> " + to, e);
    }
    test_pause("rename");
    cache_renamed(from, to, flags & RENAME_EXCHANGE, s1, s2);
    node_renamed(parent, name, newparent, newname, flags & RENAME_EXCHANGE);
    fuse_reply_err(req, 0);
}

// The disk a file can be read from directly: it is on exactly one branch, and
// every branch's slice of its directory is known (no copy hides elsewhere).
static int direct_branch(const std::string& rel) {
    if (!g_cfg.direct_reads || rel.empty() || !caching_for(rel) || !g_tree.per_branch) return -1;
    std::shared_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(dir_of(rel));
    if (!d || !tree_all_known(d)) return -1;
    auto it = d->entries.find(base_of(rel));
    if (it == d->entries.end() || __builtin_popcountll(it->second.mask) != 1) return -1;
    return __builtin_ctzll(it->second.mask);
}

static void opened(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi, int fd, bool create,
                   const struct fuse_entry_param* e) {
    auto h = new Handle;
    h->fd = fd;
    h->ino = ino;
    h->write = (fi->flags & O_ACCMODE) != O_RDONLY;
    if (g_passthrough) {
        int acc = fi->flags & O_ACCMODE;
        int id = node_backing_get(req, ino, fd, acc != O_WRONLY, acc != O_RDONLY);
        if (id == -EBUSY) {
            // the kernel takes only one backing file per inode, and this one can't write
            log_warn("Open for writing refused: the file is open elsewhere and could only be opened read-only (read-only file system or being executed): ino " + std::to_string(ino));
            close(fd);
            delete h;
            return (void)fuse_reply_err(req, EBUSY);
        }
        if (id > 0) { h->backing_id = id; fi->backing_id = id; }
        else {
            static std::atomic<int> warned{0};
            if (warned++ < 5) log_warn("Kernel passthrough refused a file (" + std::string(strerror(errno)) +
                                       ") - its data goes through unwaked");
        }
    }
    if (!h->backing_id && (fi->flags & O_DIRECT)) fi->direct_io = 1;
    fi->fh = fh_add(_handles, h);
    if (h->write) node_wfd_add(ino, fd, true);
    int r = create ? fuse_reply_create(req, e, fi) : fuse_reply_open(req, fi);
    if (r != 0) {
        fh_take(_handles, fi->fh);
        if (h->write) node_wfd_add(ino, fd, false);
        if (h->backing_id) node_backing_put(req, ino);
        close(fd);
        delete h;
    }
}

static void op_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    int flags = (fi->flags & ~(O_CREAT | O_EXCL | O_NOCTTY)) | O_CLOEXEC;
    int fd = -1, b = -1;
    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & O_TRUNC)) {
        b = direct_branch(rel);
        if (b >= 0) {
            fd = open((g_branches[b]->root + "/" + rel).c_str(), flags | O_NOFOLLOW);
            if (fd >= 0) g_stats.open_direct++;
        }
    }
    if (fd < 0) {
        note_shfs((flags & O_ACCMODE) == O_RDONLY && !(flags & O_TRUNC) ? "open" : "open (write)", rel);
        auto snap = tree_snap(dir_of(rel));
        fd = openat(g_shfs_fd, rel.c_str(), flags);
        std::string rel2;
        if (fd < 0 && errno == ENOENT && path_of(ino, rel2) && rel2 != rel) {   // a folder above was renamed
            rel = rel2;
            snap = tree_snap(dir_of(rel));
            fd = openat(g_shfs_fd, rel.c_str(), flags);
        }
        if (fd < 0) {
            int e = errno;
            if (e == ENOENT) cache_removed(rel, snap);   // shfs has it nowhere: the tree was wrong
            return fail(req, "open", rel, e);
        }
        g_stats.open_shfs++;
    } else note_shfs("open (direct)", rel, {}, {}, bit(b));
    opened(req, ino, fi, fd, false, nullptr);
}

static void op_create(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, struct fuse_file_info* fi) {
    std::string prel;
    if (!path_of(parent, prel)) return (void)fuse_reply_err(req, ENOENT);
    auto rel = join_path(prel, name);
    note_shfs("create", rel);
    auto snap = tree_snap(prel);
    int fd = openat(g_shfs_fd, rel.c_str(), (fi->flags & ~O_NOCTTY) | O_CREAT | O_CLOEXEC, mode);
    if (fd < 0) return fail(req, "create", rel, errno);
    auto ctx = fuse_req_ctx(req);
    if (fchown(fd, ctx->uid, ctx->gid) != 0) log_warn("chown " + rel + ": " + strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); return (void)fuse_reply_err(req, e); }
    test_pause("create");
    cache_put(rel, st, snap);
    struct fuse_entry_param e = {};
    e.ino = node_ref(parent, name);
    e.attr = st;
    e.attr.st_ino = e.ino;
    e.attr_timeout = ATTR_TIMEOUT;
    e.entry_timeout = ENTRY_TIMEOUT;
    g_stats.open_shfs++;
    opened(req, e.ino, fi, fd, true, &e);
}

static void op_read(fuse_req_t req, fuse_ino_t, size_t size, off_t off, struct fuse_file_info* fi) {
    struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
    buf.buf[0].flags = (enum fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    buf.buf[0].fd = fd_of(fi);
    buf.buf[0].pos = off;
    fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE);
}

static void op_write_buf(fuse_req_t req, fuse_ino_t, struct fuse_bufvec* in, off_t off, struct fuse_file_info* fi) {
    struct fuse_bufvec out = FUSE_BUFVEC_INIT(fuse_buf_size(in));
    out.buf[0].flags = (enum fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    out.buf[0].fd = fd_of(fi);
    out.buf[0].pos = off;
    auto r = fuse_buf_copy(&out, in, (enum fuse_buf_copy_flags)0);
    if (r < 0) fuse_reply_err(req, (int)-r);
    else fuse_reply_write(req, (size_t)r);
}

static void op_flush(fuse_req_t req, fuse_ino_t, struct fuse_file_info* fi) {
    int r = close(dup(fd_of(fi)));
    fuse_reply_err(req, r == 0 ? 0 : errno);
}

static void op_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
    auto h = fh_take(_handles, fi->fh);
    if (!h) return (void)fuse_reply_err(req, 0);
    if (h->write) {
        int sfd = node_wfd(ino);   // the backing file if any: what was written is there
        std::string rel;
        struct stat st;
        if (path_of(ino, rel)) {
            auto snap = tree_snap(dir_of(rel));
            if (fstat(sfd >= 0 ? sfd : h->fd, &st) == 0) {
                test_pause("release");     // tests: a rename overtakes this release
                cache_update(rel, st, snap);
            }
        }
        node_wfd_add(ino, h->fd, false);
    }
    if (h->backing_id) node_backing_put(req, ino);
    close(h->fd);
    delete h;
    fuse_reply_err(req, 0);
}

static void op_fsync(fuse_req_t req, fuse_ino_t, int datasync, struct fuse_file_info* fi) {
    int fd = fd_of(fi);
    fuse_reply_err(req, (datasync ? fdatasync(fd) : fsync(fd)) == 0 ? 0 : errno);
}

static void op_fallocate(fuse_req_t req, fuse_ino_t, int mode, off_t off, off_t len, struct fuse_file_info* fi) {
    fuse_reply_err(req, fallocate(fd_of(fi), mode, off, len) == 0 ? 0 : errno);
}

static void op_lseek(fuse_req_t req, fuse_ino_t, off_t off, int whence, struct fuse_file_info* fi) {
    off_t r = lseek(fd_of(fi), off, whence);
    if (r < 0) fuse_reply_err(req, errno);
    else fuse_reply_lseek(req, r);
}

static void op_copy_file_range(fuse_req_t req, fuse_ino_t, off_t off_in, struct fuse_file_info* fi_in,
                               fuse_ino_t, off_t off_out, struct fuse_file_info* fi_out, size_t len, int flags) {
    auto r = copy_file_range(fd_of(fi_in), &off_in, fd_of(fi_out), &off_out, len, flags);
    if (r < 0) fuse_reply_err(req, errno);
    else fuse_reply_write(req, (size_t)r);
}

// ---------------------------------------------------------------------------
// Directories: the listing is taken at opendir, from the tree when servable,
// otherwise from shfs (and then stored: born complete).
// ---------------------------------------------------------------------------
struct DEnt { std::string name; uint8_t dtype; Attr attr; bool has_attr; };
struct DirHandle { std::string rel; std::vector<DEnt> ents; };

static void op_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    auto dh = new DirHandle;
    dh->rel = rel;
    dh->ents.push_back({".", DT_DIR, {}, false});
    dh->ents.push_back({"..", DT_DIR, {}, false});
    bool cache = caching_for(rel);
    bool hit = false;
    PathSnap snap;
    if (cache) {
        {
            std::shared_lock<std::shared_mutex> l(g_tree.mu);
            DirNode* d = tree_find(rel);
            if (d && tree_servable(d)) {
                hit = true;
                dh->ents.reserve(d->entries.size() + 2);
                for (auto& kv : d->entries)
                    dh->ents.push_back({kv.first, kv.second.dtype, kv.second.attr, kv.second.has_attr});
            }
        }
        if (!hit) snap = tree_snap(rel);    // the folder is made in the tree only if the listing holds
    }
    if (hit) { g_stats.readdir_hit++; log_debug("readdir hit " + (rel.empty() ? "/" : rel)); }
    else {
        g_stats.readdir_miss++;
        note_shfs("readdir", rel);
        if (rel.empty() || caching_for(rel)) log_debug("readdir miss " + (rel.empty() ? "/" : rel));
        int fd = openat(g_shfs_fd, rp(rel), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR* dp = fd >= 0 ? fdopendir(fd) : nullptr;
        if (!dp) {
            int e = errno;
            if (fd >= 0) close(fd);
            delete dh;
            return fail(req, "opendir", rel, e);
        }
        while (auto de = readdir(dp)) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            dh->ents.push_back({de->d_name, de->d_type, {}, false});
        }
        closedir(dp);
        test_pause("opendir");
        if (cache) {
            std::unique_lock<std::shared_mutex> l(g_tree.mu);
            DirNode* d = tree_snap_ok(rel, snap, false) ? tree_ensure(rel, 0) : nullptr;
            if (d) {
                std::unordered_map<std::string, const DEnt*> now;
                for (size_t i = 2; i < dh->ents.size(); i++) now[dh->ents[i].name] = &dh->ents[i];
                std::vector<std::string> gone;
                for (auto& kv : d->entries) if (!now.count(kv.first)) gone.push_back(kv.first);
                for (auto& n : gone) tree_erase(d, n);
                for (auto& kv : now) {
                    auto r = d->entries.try_emplace(kv.first);
                    Entry& e = r.first->second;
                    if (r.second) g_tree.entries++;
                    if ((!e.has_attr || r.second) && kv.second->dtype != DT_UNKNOWN) {
                        e.dtype = kv.second->dtype;
                        e.is_dir = kv.second->dtype == DT_DIR;
                    }
                }
                d->pt_complete = true;
                touch(d);
                // what the tree knows already, for readdirplus
                for (size_t i = 2; i < dh->ents.size(); i++) {
                    auto& e = d->entries[dh->ents[i].name];
                    if (e.has_attr) { dh->ents[i].attr = e.attr; dh->ents[i].has_attr = true; }
                }
            }
        }
    }
    fi->fh = fh_add(_dhandles, dh);
    fi->cache_readdir = 0;
    if (fuse_reply_open(req, fi) != 0) delete fh_take(_dhandles, fi->fh);
}

static void do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi, bool plus) {
    auto dh = fh_get(_dhandles, fi);
    if (!dh) return (void)fuse_reply_err(req, EBADF);
    std::vector<char> buf(size);
    size_t pos = 0;
    for (size_t i = (size_t)off; i < dh->ents.size(); i++) {
        auto& de = dh->ents[i];
        size_t n;
        bool dot = de.name == "." || de.name == "..";
        if (!plus) {
            struct stat st = {};
            st.st_ino = FUSE_UNKNOWN_INO;
            st.st_mode = DTTOIF(de.dtype);
            n = fuse_add_direntry(req, buf.data() + pos, size - pos, de.name.c_str(), &st, (off_t)(i + 1));
            if (n > size - pos) break;
        } else {
            struct fuse_entry_param e = {};
            if (dot) {
                e.attr.st_ino = FUSE_UNKNOWN_INO;
                e.attr.st_mode = S_IFDIR;
            } else {
                auto rel = join_path(dh->rel, de.name);
                if (!de.has_attr) {
                    struct stat st;
                    auto snap = tree_snap(dh->rel);
                    note_shfs("getattr", rel);
                    if (fstatat(g_shfs_fd, rel.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;   // gone
                    cache_put(rel, st, snap);
                    de.attr.from(st);
                    de.has_attr = true;
                }
                e.ino = node_ref(ino, de.name);
                de.attr.to(e.attr, e.ino);
                e.attr_timeout = ATTR_TIMEOUT;
                e.entry_timeout = ENTRY_TIMEOUT;
            }
            n = fuse_add_direntry_plus(req, buf.data() + pos, size - pos, de.name.c_str(), &e, (off_t)(i + 1));
            if (n > size - pos) {
                if (e.ino) node_forget(e.ino, 1);
                break;
            }
        }
        pos += n;
    }
    fuse_reply_buf(req, buf.data(), pos);
}
static void op_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
    do_readdir(req, ino, size, off, fi, false);
}
static void op_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
    do_readdir(req, ino, size, off, fi, true);
}
static void op_releasedir(fuse_req_t req, fuse_ino_t, struct fuse_file_info* fi) {
    delete fh_take(_dhandles, fi->fh);
    fuse_reply_err(req, 0);
}
static void op_fsyncdir(fuse_req_t req, fuse_ino_t, int, struct fuse_file_info*) { fuse_reply_err(req, 0); }

// shfs reports the free space of the share a path is in (its pool or disks), the whole
// array only for /mnt/user itself. Asked at the share, not the full path: a deep path would
// make shfs look up every level, which Unwake otherwise answers from RAM. Not noted: apps
// poll this every few seconds, and the disks' counters are in RAM.
static void op_statfs(fuse_req_t req, fuse_ino_t ino) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    auto share = rel.substr(0, rel.find('/'));
    struct statvfs s;
    int r = share.empty() ? fstatvfs(g_shfs_fd, &s) : statvfs(proc_path(share).c_str(), &s);
    if (r != 0) return (void)fuse_reply_err(req, errno);
    fuse_reply_statfs(req, &s);
}

// ---------------------------------------------------------------------------
// Extended attributes: Samba reads them for every file it lists. A file's
// whole set is cached on first use (it's small for almost every file).
// ---------------------------------------------------------------------------
// the xattrs of rel, from the tree or read through shfs (and kept)
static int xattrs_of(const std::string& rel, Xattrs& out) {
    if (!rel.empty() && caching_for(rel)) {
        std::shared_lock<std::shared_mutex> l(g_tree.mu);
        DirNode* d = tree_find(dir_of(rel));
        if (d) {
            auto it = d->entries.find(base_of(rel));
            if (it != d->entries.end() && it->second.xattrs) {
                out = *it->second.xattrs;
                g_stats.xattr_hit++;
                return 0;
            }
        }
    }
    g_stats.xattr_miss++;
    note_shfs("xattr", rel);
    auto snap = tree_snap(dir_of(rel));
    int err = 0;
    auto xs = read_xattrs(proc_path(rel), &err);
    if (err) return err;
    if (!xs) {      // too big to keep: read them for this caller only
        out.clear();
        auto p = proc_path(rel);
        std::vector<char> names(65536);
        ssize_t n = llistxattr(p.c_str(), names.data(), names.size());
        if (n < 0) return errno;
        for (ssize_t i = 0; i < n;) {
            std::string name(&names[(size_t)i]);
            i += (ssize_t)name.size() + 1;
            std::vector<char> v(65536);
            ssize_t m = lgetxattr(p.c_str(), name.c_str(), v.data(), v.size());
            if (m >= 0) out.emplace_back(name, std::string(v.data(), (size_t)m));
        }
        return 0;
    }
    out = *xs;
    if (rel.empty() || !caching_for(rel)) return 0;
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    if (!tree_snap_ok(dir_of(rel), snap, true)) return 0;
    DirNode* d = tree_find(dir_of(rel));
    if (!d) return 0;
    auto it = d->entries.find(base_of(rel));
    if (it != d->entries.end()) { it->second.xattrs = xs; touch_attr(d); }
    return 0;
}

// shfs's system.LOCATION (first disk holding a path) and system.LOCATIONS (all of
// them, comma separated, shfs order: pools, then disk1, disk2, ...), answered from
// the tree when every branch's slice of the parent is known. The File Manager asks
// for this on every entry it lists; through shfs, that looks at the disks.
static bool location_from_tree(const std::string& rel, bool all, std::string& out) {
    // a share's own entry in the root is kept current even for shares passed to shfs
    bool top = rel.find('/') == std::string::npos;
    if (rel.empty() || !g_caching || (!top && !caching_for(rel)) || !g_tree.per_branch) return false;
    std::shared_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(dir_of(rel));
    if (!d) return false;
    bool complete = d == &g_tree.root ? (d->known & g_tree.all) == g_tree.all : tree_all_known(d);
    if (!complete) return false;
    auto it = d->entries.find(base_of(rel));
    if (it == d->entries.end() || !it->second.mask) return false;
    out.clear();
    for (size_t i = 0; i < g_branches.size() && i < (size_t)MAX_BRANCHES; i++) {
        if (!(it->second.mask & bit((int)i))) continue;
        if (!out.empty()) out += ',';
        out += g_branches[i]->name;
        if (!all) break;
    }
    return true;
}

static void op_getxattr(fuse_req_t req, fuse_ino_t ino, const char* name, size_t size) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    bool loc1 = !strcmp(name, "system.LOCATION"), locs = !strcmp(name, "system.LOCATIONS");
    std::string loc;
    if ((loc1 || locs) && location_from_tree(rel, locs, loc)) {
        g_stats.xattr_hit++;
        if (!size) return (void)fuse_reply_xattr(req, loc.size());
        if (size < loc.size()) return (void)fuse_reply_err(req, ERANGE);
        return (void)fuse_reply_buf(req, loc.data(), loc.size());
    }
    // Straight to shfs: the root of /mnt/user, and every system.* attribute but the POSIX
    // ACLs. shfs computes some of these on request without listing them (system.LOCATION,
    // system.LOCATIONS: the disks holding a file, used by the File Manager, the VM manager
    // and moves). The ACLs are stored and listed like user.* ones, so the cached set
    // answers them; Samba asks for system.posix_acl_default of every folder it lists.
    bool acl = !strcmp(name, "system.posix_acl_access") || !strcmp(name, "system.posix_acl_default");
    if (rel.empty() || (!acl && !strncmp(name, "system.", 7))) {
        if (!rel.empty()) note_shfs("xattr", rel, {}, name);
        std::vector<char> v(size ? size : 0);
        auto p = proc_path(rel);
        ssize_t n = rel.empty() ? getxattr(p.c_str(), name, size ? v.data() : nullptr, size)
                                : lgetxattr(p.c_str(), name, size ? v.data() : nullptr, size);
        if (n < 0) return (void)fuse_reply_err(req, errno);
        return size ? (void)fuse_reply_buf(req, v.data(), (size_t)n) : (void)fuse_reply_xattr(req, (size_t)n);
    }
    Xattrs xs;
    if (int e = xattrs_of(rel, xs)) return (void)fuse_reply_err(req, e);
    for (auto& x : xs) {
        if (x.first != name) continue;
        if (!size) return (void)fuse_reply_xattr(req, x.second.size());
        if (size < x.second.size()) return (void)fuse_reply_err(req, ERANGE);
        return (void)fuse_reply_buf(req, x.second.data(), x.second.size());
    }
    fuse_reply_err(req, ENODATA);
}

static void op_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    std::string names;
    if (rel.empty()) {
        std::vector<char> v(65536);
        ssize_t n = listxattr(proc_path("").c_str(), v.data(), v.size());
        if (n < 0) return (void)fuse_reply_err(req, errno);
        names.assign(v.data(), (size_t)n);
    } else {
        Xattrs xs;
        if (int e = xattrs_of(rel, xs)) return (void)fuse_reply_err(req, e);
        for (auto& x : xs) { names += x.first; names += '\0'; }
    }
    if (!size) return (void)fuse_reply_xattr(req, names.size());
    if (size < names.size()) return (void)fuse_reply_err(req, ERANGE);
    fuse_reply_buf(req, names.data(), names.size());
}

static void op_setxattr(fuse_req_t req, fuse_ino_t ino, const char* name, const char* value, size_t size, int flags) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    if (!rel.empty()) note_shfs("setxattr", rel, {}, name);
    auto p = proc_path(rel);
    int r = rel.empty() ? setxattr(p.c_str(), name, value, size, flags) : lsetxattr(p.c_str(), name, value, size, flags);
    cache_xattrs_unknown(rel);
    fuse_reply_err(req, r == 0 ? 0 : errno);
}

static void op_removexattr(fuse_req_t req, fuse_ino_t ino, const char* name) {
    std::string rel;
    if (!path_of(ino, rel)) return (void)fuse_reply_err(req, ENOENT);
    if (!rel.empty()) note_shfs("removexattr", rel, {}, name);
    auto p = proc_path(rel);
    int r = rel.empty() ? removexattr(p.c_str(), name) : lremovexattr(p.c_str(), name);
    cache_xattrs_unknown(rel);
    fuse_reply_err(req, r == 0 ? 0 : errno);
}

const struct fuse_lowlevel_ops* fs_ops() {
    static struct fuse_lowlevel_ops ops = [] {
        struct fuse_lowlevel_ops o = {};
        o.init = op_init;
        o.lookup = Caller<op_lookup>::call;
        o.forget = op_forget;
        o.forget_multi = op_forget_multi;
        o.getattr = Caller<op_getattr>::call;
        o.setattr = Caller<op_setattr>::call;
        o.readlink = Caller<op_readlink>::call;
        o.mknod = Caller<op_mknod>::call;
        o.mkdir = Caller<op_mkdir>::call;
        o.symlink = Caller<op_symlink>::call;
        o.link = Caller<op_link>::call;
        o.unlink = Caller<op_unlink>::call;
        o.rmdir = Caller<op_rmdir>::call;
        o.rename = Caller<op_rename>::call;
        o.open = Caller<op_open>::call;
        o.create = Caller<op_create>::call;
        o.read = op_read;
        o.write_buf = op_write_buf;
        o.flush = op_flush;
        o.release = op_release;
        o.fsync = op_fsync;
        o.fallocate = op_fallocate;
        o.lseek = op_lseek;
        o.copy_file_range = op_copy_file_range;
        o.opendir = Caller<op_opendir>::call;
        o.readdir = Caller<op_readdir>::call;
        o.readdirplus = Caller<op_readdirplus>::call;
        o.releasedir = op_releasedir;
        o.fsyncdir = op_fsyncdir;
        o.statfs = op_statfs;
        o.getxattr = Caller<op_getxattr>::call;
        o.listxattr = Caller<op_listxattr>::call;
        o.setxattr = Caller<op_setxattr>::call;
        o.removexattr = Caller<op_removexattr>::call;
        return o;
    }();
    nodes_init();
    return &ops;
}

// ---------------------------------------------------------------------------
// In-place upgrade: what the kernel holds ids of (nodes, open files, open
// folders), plus counters and the recent shfs requests. Called while no request
// is in progress.
// ---------------------------------------------------------------------------
#include "handover.h"
#include <set>

void fs_handover_out(HoWriter& w) {
    w.u8(g_passthrough);
    w.u8(g_caching);
    {
        std::lock_guard<std::mutex> l(_nmu);
        w.u64(_next_ino);
        w.u64(_nodes.size());
        for (auto& kv : _nodes) {
            const Node& n = kv.second;
            w.u64(kv.first);
            w.u64(n.parent);
            w.str(n.name);
            w.u64(n.nlookup);
            w.u8(n.unlinked);
            w.u32((uint32_t)n.wfds.size());
            for (int f : n.wfds) w.fd(f);
            w.u32((uint32_t)n.backing_id);
            w.fd(n.backing_fd);
            w.u32(n.backing_refs);
            w.u8(n.backing_read);
            w.u8(n.backing_write);
        }
    }
    {
        std::shared_lock<std::shared_mutex> l(_hmu);
        w.u64(_next_fh);
        w.u64(_handles.size());
        for (auto& kv : _handles) {
            const Handle& h = *kv.second;
            w.u64(kv.first);
            w.fd(h.fd);
            w.u32((uint32_t)h.backing_id);
            w.u8(h.write);
            w.u64(h.ino);
        }
        w.u64(_dhandles.size());
        for (auto& kv : _dhandles) {
            const DirHandle& d = *kv.second;
            w.u64(kv.first);
            w.str(d.rel);
            w.u64(d.ents.size());
            for (auto& e : d.ents) {
                w.str(e.name);
                w.u8(e.dtype);
                w.u8(e.has_attr);
                if (e.has_attr) attr_out(w, e.attr);
            }
        }
    }
    for (auto* c : {&g_stats.lookup_hit, &g_stats.lookup_neg, &g_stats.lookup_miss, &g_stats.getattr_hit,
                    &g_stats.getattr_miss, &g_stats.readdir_hit, &g_stats.readdir_miss, &g_stats.xattr_hit,
                    &g_stats.xattr_miss, &g_stats.open_direct, &g_stats.open_shfs})
        w.u64(c->load());
    {
        std::lock_guard<std::mutex> l(_req_mu);
        w.u64(_reqs.size());
        for (size_t i = 0; i < _reqs.size(); i++) {   // oldest first
            auto& q = _reqs[(_req_next + i) % _reqs.size()];
            w.i64(q.ms); w.str(q.op); w.str(q.rel); w.str(q.to); w.str(q.note); w.u64(q.mask);
        }
    }
}

// op names of shfs requests point at string literals; ones read back live here
static const char* intern_op(const std::string& s) {
    static std::mutex m;
    static std::set<std::string> pool;
    std::lock_guard<std::mutex> l(m);
    return pool.insert(s).first->c_str();
}

void fs_handover_in(HoReader& r) {
    g_passthrough = r.u8();
    g_caching = r.u8();
    {
        std::lock_guard<std::mutex> l(_nmu);
        _nodes.clear();
        _children.clear();
        _next_ino = r.u64();
        uint64_t n = r.u64();
        _nodes.reserve(n);
        for (uint64_t i = 0; i < n; i++) {
            fuse_ino_t ino = r.u64();
            Node nd;
            nd.parent = r.u64();
            nd.name = r.str();
            nd.nlookup = r.u64();
            nd.unlinked = r.u8();
            uint32_t k = r.u32();
            for (uint32_t j = 0; j < k; j++) nd.wfds.push_back(r.fd());
            nd.backing_id = (int)r.u32();
            nd.backing_fd = r.fd();
            nd.backing_refs = r.u32();
            nd.backing_read = r.u8();
            nd.backing_write = r.u8();
            if (ino != FUSE_ROOT_ID && !nd.unlinked) _children[ckey(nd.parent, nd.name)] = ino;
            _nodes[ino] = std::move(nd);
        }
        if (!_nodes.count(FUSE_ROOT_ID)) throw std::runtime_error("no root node");
    }
    {
        std::unique_lock<std::shared_mutex> l(_hmu);
        _handles.clear();
        _dhandles.clear();
        _next_fh = r.u64();
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = r.u64();
            auto h = new Handle;
            h->fd = r.fd();
            h->backing_id = (int)r.u32();
            h->write = r.u8();
            h->ino = r.u64();
            _handles[id] = h;
        }
        n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = r.u64();
            auto d = new DirHandle;
            d->rel = r.str();
            uint64_t k = r.u64();
            d->ents.reserve(k);
            for (uint64_t j = 0; j < k; j++) {
                DEnt e;
                e.name = r.str();
                e.dtype = r.u8();
                e.has_attr = r.u8();
                if (e.has_attr) e.attr = attr_in(r);
                d->ents.push_back(std::move(e));
            }
            _dhandles[id] = d;
        }
    }
    for (auto* c : {&g_stats.lookup_hit, &g_stats.lookup_neg, &g_stats.lookup_miss, &g_stats.getattr_hit,
                    &g_stats.getattr_miss, &g_stats.readdir_hit, &g_stats.readdir_miss, &g_stats.xattr_hit,
                    &g_stats.xattr_miss, &g_stats.open_direct, &g_stats.open_shfs})
        c->store(r.u64());
    {
        std::lock_guard<std::mutex> l(_req_mu);
        _reqs.clear();
        _req_next = 0;
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            ShfsReq q;
            q.ms = r.i64(); q.op = intern_op(r.str()); q.rel = r.str(); q.to = r.str(); q.note = r.str(); q.mask = r.u64();
            if (_reqs.size() < REQ_RING) _reqs.push_back(std::move(q));
        }
        _req_next = _reqs.size() % REQ_RING;
    }
}

// names the kernel currently holds an inode for (it forgets them under memory pressure)
size_t fs_node_count() {
    std::lock_guard<std::mutex> l(_nmu);
    return _nodes.size();
}

size_t fs_open_handles() {
    std::shared_lock<std::shared_mutex> l(_hmu);
    return _handles.size() + _dhandles.size();
}
