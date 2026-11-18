// tree.cpp - the union directory tree
//
// One DirNode per cached directory of /mnt/user, keyed by name below its
// parent. A listing is servable (answered without touching shfs) when it is
// complete for the union of all branches:
//   - pt_complete: read from shfs (born complete) and kept current since by
//     fanotify and our own changes, or
//   - every branch's slice is known: harvested from that disk, or known absent
//     because a parent's slice on that disk is known and doesn't hold the name.
// Callers hold g_tree.mu (shared for reading, exclusive for changes).
#include "unwaked.h"

#include <dirent.h>
#include <sys/xattr.h>

Tree g_tree;

static constexpr size_t XATTR_KEEP_MAX = 4096;   // bigger xattr sets aren't kept
static const std::shared_ptr<const Xattrs> _no_xattrs = std::make_shared<Xattrs>();

std::shared_ptr<const Xattrs> read_xattrs(const std::string& path, int* err) {
    if (err) *err = 0;
    ssize_t n = llistxattr(path.c_str(), nullptr, 0);
    if (n < 0) {
        if (errno == ENOTSUP) return _no_xattrs;
        if (err) *err = errno;
        return nullptr;
    }
    if (n == 0) return _no_xattrs;
    std::vector<char> names((size_t)n + 1);
    n = llistxattr(path.c_str(), names.data(), names.size());
    if (n < 0) { if (err) *err = errno; return nullptr; }
    auto out = std::make_shared<Xattrs>();
    size_t total = 0;
    for (ssize_t i = 0; i < n;) {
        std::string name(&names[(size_t)i]);
        i += (ssize_t)name.size() + 1;
        ssize_t v = lgetxattr(path.c_str(), name.c_str(), nullptr, 0);
        if (v < 0) continue;
        std::string val((size_t)v, '\0');
        v = lgetxattr(path.c_str(), name.c_str(), val.data(), val.size());
        if (v < 0) continue;
        val.resize((size_t)v);
        total += name.size() + val.size();
        out->emplace_back(std::move(name), std::move(val));
    }
    if (out->empty()) return _no_xattrs;
    if (total > XATTR_KEEP_MAX) return nullptr;   // the caller has them, nobody keeps them
    return out;
}

void Attr::from(const struct stat& st) {
    mode = st.st_mode; uid = st.st_uid; gid = st.st_gid; nlink = (uint32_t)st.st_nlink;
    size = st.st_size; blocks = st.st_blocks; rdev = (uint32_t)st.st_rdev;
    blksize = (uint32_t)st.st_blksize;
    atime = st.st_atim.tv_sec; atime_ns = (uint32_t)st.st_atim.tv_nsec;
    mtime = st.st_mtim.tv_sec; mtime_ns = (uint32_t)st.st_mtim.tv_nsec;
    ctime = st.st_ctim.tv_sec; ctime_ns = (uint32_t)st.st_ctim.tv_nsec;
}

void Attr::to(struct stat& st, uint64_t ino) const {
    st = {};
    st.st_ino = ino;
    st.st_mode = mode; st.st_uid = uid; st.st_gid = gid; st.st_nlink = nlink;
    st.st_size = size; st.st_blocks = blocks; st.st_rdev = rdev; st.st_blksize = blksize;
    st.st_atim.tv_sec = atime; st.st_atim.tv_nsec = atime_ns;
    st.st_mtim.tv_sec = mtime; st.st_mtim.tv_nsec = mtime_ns;
    st.st_ctim.tv_sec = ctime; st.st_ctim.tv_nsec = ctime_ns;
}

Entry* DirNode::self_entry() const {
    if (!parent) return nullptr;
    auto it = parent->entries.find(*name);
    return it == parent->entries.end() ? nullptr : &it->second;
}

std::vector<std::string> split_path(const std::string& rel) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < rel.size()) {
        auto j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        if (j > i) out.emplace_back(rel.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

DirNode* tree_find(const std::string& rel) {
    DirNode* d = &g_tree.root;
    for (auto& c : split_path(rel)) {
        auto it = d->entries.find(c);
        if (it == d->entries.end() || !it->second.dir) return nullptr;
        d = it->second.dir.get();
    }
    return d;
}

void tree_count_add(const Entry& e, int sign) {
    g_tree.entries += sign;
    if (!e.dir) return;
    g_tree.dirs += sign;
    for (auto& kv : e.dir->entries) tree_count_add(kv.second, sign);
}

static DirNode* attach_dir(DirNode* parent, std::unordered_map<std::string, Entry>::iterator it) {
    auto d = std::make_unique<DirNode>();
    d->parent = parent;
    d->name   = &it->first;
    it->second.dir = std::move(d);
    g_tree.dirs++;
    return it->second.dir.get();
}

DirNode* tree_ensure(const std::string& rel, Mask present) {
    DirNode* d = &g_tree.root;
    for (auto& c : split_path(rel)) {
        auto it = d->entries.find(c);
        if (it == d->entries.end()) {
            it = d->entries.emplace(c, Entry()).first;
            it->second.is_dir = true;
            it->second.dtype  = DT_DIR;
            g_tree.entries++;
            d->gen++;
        }
        Entry& e = it->second;
        if ((e.mask & present) != present) { e.mask |= present; d->gen++; }
        if (!e.dir) attach_dir(d, it);
        d = e.dir.get();
    }
    return d;
}

// Is the slice of branch b of this directory known (present or known absent)?
static bool known_on(const DirNode* d, Mask b) {
    if (d->known & b) return true;
    for (const DirNode* c = d; c->parent; c = c->parent) {
        const Entry* e = c->self_entry();
        if (!e || (e->mask & b)) return false;          // c exists on b, its slice unknown
        if (c->parent->known & b) return true;          // parent's slice known, c absent there
    }
    return false;
}

bool tree_servable(const DirNode* d) {
    return d->pt_complete || tree_all_known(d);
}

bool tree_all_known(const DirNode* d) {
    if (!g_tree.per_branch || d == &g_tree.root) return false;   // the root only from shfs
    for (int i = 0; i < MAX_BRANCHES; i++) {
        Mask b = bit(i);
        if (!(g_tree.all & b)) continue;
        if (!known_on(d, b)) return false;
    }
    return g_tree.all != 0;
}

bool tree_cacheable(const std::string& rel) {
    if (rel.empty()) return true;
    auto slash = rel.find('/');
    auto share = rel.substr(0, slash);
    auto ex = std::atomic_load(&g_cfg.excluded_shares);
    if (std::binary_search(ex->begin(), ex->end(), share)) return false;
    auto un = std::atomic_load(&g_cfg.uncached_paths);
    for (auto& p : *un)
        if (rel.size() >= p.size() && rel.compare(0, p.size(), p) == 0 && (rel.size() == p.size() || rel[p.size()] == '/'))
            return false;
    return true;
}

void tree_erase(DirNode* d, const std::string& name) {
    auto it = d->entries.find(name);
    if (it == d->entries.end()) return;
    tree_count_add(it->second, -1);
    d->entries.erase(it);
    d->gen++;
}

static void clear_branch_rec(DirNode* d, Mask b) {
    d->known &= ~b;
    d->pt_complete = false;
    d->gen++;
    for (auto& kv : d->entries) {
        kv.second.mask &= ~b;
        if (kv.second.dir) clear_branch_rec(kv.second.dir.get(), b);
    }
}

void tree_clear_branch(int b) { clear_branch_rec(&g_tree.root, bit(b)); }

void tree_forget(const std::string& rel) {
    if (rel.empty()) { tree_clear_all(); return; }
    auto slash = rel.rfind('/');
    DirNode* d = tree_find(slash == std::string::npos ? "" : rel.substr(0, slash));
    if (!d) return;
    auto it = d->entries.find(slash == std::string::npos ? rel : rel.substr(slash + 1));
    if (it == d->entries.end() || !it->second.dir) return;
    for (auto& kv : it->second.dir->entries) tree_count_add(kv.second, -1);
    g_tree.dirs--;
    it->second.dir.reset();
    d->gen++;
}

void tree_clear_all() {
    g_tree.root.entries.clear();
    g_tree.root.known = 0;
    g_tree.root.pt_complete = false;
    g_tree.root.gen++;
    g_tree.dirs = 1;
    g_tree.entries = 0;
}

// ---------------------------------------------------------------------------
// In-place upgrade: the whole tree, depth first. An xattr set shared by several
// entries is written once and referred to by number after that.
// ---------------------------------------------------------------------------
#include "handover.h"

namespace {
struct XattrsOut { std::unordered_map<const Xattrs*, uint32_t> ids; };

enum : uint8_t { XA_UNKNOWN = 0, XA_NONE = 1, XA_NEW = 2, XA_SEEN = 3 };

void dir_out(HoWriter& w, const DirNode& d, XattrsOut& xo) {
    w.u64(d.known);
    w.u8(d.pt_complete);
    w.u64(d.gen);
    w.u64(d.entries.size());
    for (auto& kv : d.entries) {
        const Entry& e = kv.second;
        w.str(kv.first);
        w.u64(e.mask);
        w.u8(e.has_attr);
        if (e.has_attr) attr_out(w, e.attr);
        w.u8((uint8_t)e.attr_from);
        w.u8(e.is_dir);
        w.u8(e.dtype);
        if (!e.xattrs) w.u8(XA_UNKNOWN);
        else if (e.xattrs->empty()) w.u8(XA_NONE);
        else {
            auto it = xo.ids.find(e.xattrs.get());
            if (it != xo.ids.end()) { w.u8(XA_SEEN); w.u32(it->second); }
            else {
                w.u8(XA_NEW);
                xo.ids.emplace(e.xattrs.get(), (uint32_t)xo.ids.size());
                w.u32((uint32_t)e.xattrs->size());
                for (auto& x : *e.xattrs) { w.str(x.first); w.str(x.second); }
            }
        }
        w.u8(e.dir != nullptr);
        if (e.dir) dir_out(w, *e.dir, xo);
    }
}

void dir_in(HoReader& r, DirNode& d, std::vector<std::shared_ptr<const Xattrs>>& xs, uint64_t& dirs, uint64_t& entries) {
    d.known = r.u64();
    d.pt_complete = r.u8();
    d.gen = r.u64();
    uint64_t n = r.u64();
    d.entries.reserve(n);
    for (uint64_t i = 0; i < n; i++) {
        auto it = d.entries.emplace(r.str(), Entry()).first;
        Entry& e = it->second;
        entries++;
        e.mask = r.u64();
        e.has_attr = r.u8();
        if (e.has_attr) e.attr = attr_in(r);
        e.attr_from = (int8_t)r.u8();
        e.is_dir = r.u8();
        e.dtype = r.u8();
        switch (r.u8()) {
        case XA_UNKNOWN: break;
        case XA_NONE: e.xattrs = _no_xattrs; break;
        case XA_SEEN: {
            uint32_t id = r.u32();
            if (id >= xs.size()) throw std::runtime_error("xattr set out of range");
            e.xattrs = xs[id];
            break;
        }
        case XA_NEW: {
            auto x = std::make_shared<Xattrs>();
            uint32_t k = r.u32();
            for (uint32_t j = 0; j < k; j++) { auto name = r.str(); x->emplace_back(std::move(name), r.str()); }
            xs.push_back(x);
            e.xattrs = x;
            break;
        }
        default: throw std::runtime_error("bad xattr tag");
        }
        if (r.u8()) {
            auto c = std::make_unique<DirNode>();
            c->parent = &d;
            c->name = &it->first;
            dirs++;
            dir_in(r, *c, xs, dirs, entries);
            e.dir = std::move(c);
        }
    }
}
}  // namespace

void tree_handover_out(HoWriter& w) {
    w.u64(g_tree.all);
    w.u8(g_tree.per_branch);
    XattrsOut xo;
    dir_out(w, g_tree.root, xo);
}

void tree_handover_in(HoReader& r) {
    if (r.u64() != g_tree.all || r.u8() != (uint8_t)g_tree.per_branch)
        throw std::runtime_error("disks and pools differ from the running Unwake");
    tree_clear_all();
    std::vector<std::shared_ptr<const Xattrs>> xs;
    uint64_t dirs = 1, entries = 0;
    dir_in(r, g_tree.root, xs, dirs, entries);
    g_tree.dirs = dirs;
    g_tree.entries = entries;
}
