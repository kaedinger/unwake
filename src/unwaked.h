// unwaked - directory cache for Unraid user shares
//
// One header for the whole daemon: logging and config (util.cpp), the union
// directory tree (tree.cpp), branches, spin state and the harvester
// (branches.cpp), fanotify (events.cpp), the FUSE file system (fs.cpp) and the
// supervisor (main.cpp).
#pragma once

#define FUSE_USE_VERSION 317
#include <fuse_lowlevel.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Logging (util.cpp)
// ---------------------------------------------------------------------------
void log_open(const std::string& file);
void log_info (const std::string& m);
void log_warn (const std::string& m);
void log_err  (const std::string& m);
void log_debug(const std::string& m);
extern std::atomic<bool> g_debug;

// ---------------------------------------------------------------------------
// Config (util.cpp)
// ---------------------------------------------------------------------------
struct Config {
    // fixed paths, overridable on the command line for tests
    std::string mountpoint = "/mnt/user";
    std::string disks_ini  = "/var/local/emhttp/disks.ini";
    std::string var_ini    = "/var/local/emhttp/var.ini";
    std::string cfg_file   = "/boot/config/plugins/unwake/unwake.cfg";
    std::string log_file   = "/var/log/unwake.log";
    std::string run_dir    = "/var/run/unwake";
    std::string shares_dir = "/boot/config/shares";   // Unraid's share settings
    std::vector<std::string> branch_args;   // --branch name=path[,rotational]: no disks.ini

    // from unwake.cfg
    bool        debug        = false;
    std::atomic<bool> harvest{true};
    std::atomic<bool> direct_reads{true};   // DATA_PATH=direct: reads of files on one known disk skip shfs
    std::atomic<bool> return_ram{true};     // RETURN_RAM=yes: hand freed memory back after the kernel forgot many names
    std::vector<std::string> excluded_explicit, included_explicit;   // EXCLUDED_SHARES, INCLUDED_SHARES
    // effective set, sorted: explicit exclusions plus shares that live on SSD pools only.
    // Replaced as a whole; readers take a snapshot (std::atomic_load)
    std::shared_ptr<const std::vector<std::string>> excluded_shares = std::make_shared<std::vector<std::string>>();
    // union paths below a mount fanotify can't watch (overlay, tmpfs, ...): never cached
    std::shared_ptr<const std::vector<std::string>> uncached_paths = std::make_shared<std::vector<std::string>>();
    long        log_max_mb   = 16;
    long        log_trim_mb  = 2;
};
extern Config g_cfg;
std::vector<std::string> load_config();   // returns warnings
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string join_path(const std::string& dir, const std::string& name);   // "" + "a" = "a"
std::string human_count(uint64_t n);
std::string fmt_secs(long s);
int64_t     now_ms();

// ---------------------------------------------------------------------------
// Union directory tree (tree.cpp)
// ---------------------------------------------------------------------------
// Branch = one file system shfs merges into /mnt/user: an array disk or a pool.
// A Mask has one bit per branch index. At most 64 branches; beyond that the
// per-branch knowledge is switched off and only passthrough listings are served.
using Mask = uint64_t;
constexpr int MAX_BRANCHES = 64;
inline Mask bit(int b) { return Mask(1) << b; }

struct Attr {
    uint32_t mode = 0, uid = 0, gid = 0, nlink = 0;
    uint64_t size = 0, blocks = 0;
    int64_t  atime = 0, mtime = 0, ctime = 0;
    uint32_t atime_ns = 0, mtime_ns = 0, ctime_ns = 0;
    uint32_t rdev = 0;
    uint32_t blksize = 4096;
    void from(const struct stat& st);
    void to(struct stat& st, uint64_t ino) const;
};

struct DirNode;
using Xattrs = std::vector<std::pair<std::string, std::string>>;

struct Entry {
    Attr     attr;
    Mask     mask      = 0;      // branches known to hold this name
    bool     has_attr  = false;
    int8_t   attr_from = -1;     // branch the attr was read from, -1 = from shfs
    bool     is_dir    = false;  // valid even without attr (from d_type)
    uint8_t  dtype     = 0;      // DT_* for readdir without attr
    std::shared_ptr<const Xattrs> xattrs;   // known xattrs (empty = none), null = unknown
    std::unique_ptr<DirNode> dir;      // cached listing below this name, if any
};

// Should a stat read from branch b replace e's attr? A name on several branches
// shows the attr of the first one in shfs's order (pools, then disk1, disk2, ...);
// what shfs reported is replaced by a stat of that first branch only. Call with b's
// bit already in e.mask.
inline bool attr_from_branch_wins(const Entry& e, int b) {
    if (!e.has_attr) return true;
    if (e.attr_from >= 0) return b <= e.attr_from;
    return e.mask && b == __builtin_ctzll(e.mask);
}

struct DirNode {
    DirNode*           parent = nullptr;
    const std::string* name   = nullptr;   // key of this dir in parent->entries
    std::unordered_map<std::string, Entry> entries;
    Mask     known       = 0;      // branches whose slice of this dir is fully known
    bool     pt_complete = false;  // union listing complete (from shfs), kept current since
    uint64_t gen         = 0;      // bumped by every change, to detect races with readers
    Entry*   self_entry() const;   // this dir's entry in its parent (null for the root)
};

struct Tree {
    std::shared_mutex mu;
    DirNode root;
    Mask    all = 0;              // indexes of branches in use
    bool    per_branch = true;    // false: more than MAX_BRANCHES branches
    std::atomic<uint64_t> dirs{1}, entries{0};
};
extern Tree g_tree;

std::vector<std::string> split_path(const std::string& rel);
DirNode* tree_find(const std::string& rel);                      // lock held
DirNode* tree_ensure(const std::string& rel, Mask present);      // lock held (exclusive)
// the xattrs of a path (no symlinks followed); null if unreadable or bigger than
// what's worth keeping. "none" is one shared empty set, so it costs no memory.
std::shared_ptr<const Xattrs> read_xattrs(const std::string& path, int* err = nullptr);
bool     tree_servable(const DirNode* d);                        // lock held
bool     tree_all_known(const DirNode* d);                       // every branch slice known
bool     tree_cacheable(const std::string& rel);                 // excluded shares
void     tree_erase(DirNode* d, const std::string& name);        // lock held (exclusive)
void     tree_count_add(const Entry& e, int sign);
void     tree_clear_branch(int b);                                // lock held (exclusive)
void     tree_clear_all();                                        // lock held (exclusive)
void     tree_forget(const std::string& rel);                     // lock held (exclusive): drop what's known below rel

// ---------------------------------------------------------------------------
// Branches, spin state and harvester (branches.cpp)
// ---------------------------------------------------------------------------
struct Branch {
    std::string name;                    // "disk1", "cache"
    std::string root;                    // "/mnt/disk1"
    std::vector<std::string> devices;    // "sdb", ... (pool members)
    bool        rotational = true;
    std::atomic<bool> up{false};         // spinning (or not a spinning disk)
    std::atomic<bool> harvested{false};  // full walk done, slices known
    std::atomic<bool> harvesting{false};
    std::atomic<bool> untracked{false};  // a mount below it couldn't be watched
    std::atomic<int64_t> harvest_ms{0};  // duration of the last full walk
    std::atomic<int64_t> harvested_at{0};
    std::atomic<uint64_t> harvest_dirs{0}, harvest_entries{0};
};
extern std::vector<std::unique_ptr<Branch>> g_branches;
bool  branches_load();                   // disks.ini or --branch; false if none
void  spin_poll();                       // refresh Branch::up
int   branch_of_path(const std::string& abs, std::string& rel);   // -1 if none
void  harvester_start();
void  harvester_stop();
void  harvester_restart(int b);          // re-walk later (after a lost event)
bool  array_stopping();                  // var.ini says the array is going down
void  apply_exclusions();                // effective excluded shares from config + share settings
void  recompute_exclusions();            // SSD-pool-only shares: passed through unless an array disk holds them
std::string pool_only_json();            // {"share": {"known": 0|1, "on": ["disk2"]}}

// ---------------------------------------------------------------------------
// fanotify (events.cpp)
// ---------------------------------------------------------------------------
bool  events_start();                    // false: fanotify unavailable, no caching
void  events_stop();
void  events_rescan_mounts();            // pick up new ZFS datasets etc.
bool  events_dev_watched(dev_t dev);     // a file system we get change events for
extern std::atomic<uint64_t> g_events;

// ---------------------------------------------------------------------------
// FUSE (fs.cpp)
// ---------------------------------------------------------------------------
struct Stats {
    std::atomic<uint64_t> lookup_hit{0}, lookup_neg{0}, lookup_miss{0};
    std::atomic<uint64_t> getattr_hit{0}, getattr_miss{0};
    std::atomic<uint64_t> readdir_hit{0}, readdir_miss{0};
    std::atomic<uint64_t> xattr_hit{0}, xattr_miss{0};
    std::atomic<uint64_t> open_direct{0}, open_shfs{0};
};
extern Stats g_stats;
extern int   g_shfs_fd;                  // O_PATH fd of shfs, under our mount
extern std::atomic<bool> g_passthrough;  // kernel FUSE passthrough in use
extern std::atomic<bool> g_caching;      // cache in use (off while draining)
const struct fuse_lowlevel_ops* fs_ops();
size_t fs_node_count();   // names the kernel holds an inode for
extern std::atomic<uint64_t> g_nodes_forgotten;   // names the kernel let go of, ever

// request loop (loop.cpp): runs until the session ends; returns 0 or -errno
int  loop_run(struct fuse_session* se, int max_threads, int max_idle);
bool loop_pause(int timeout_ms);   // true: no request in progress, none being read
void loop_resume();
void loop_finish();                // after a hand-over: the threads end, loop_run returns
// requests passed to shfs (they may wake a disk), kept briefly to explain a spin-up
// to: a rename's target; note: shown after the path; mask: the disks it reached, 0 = work it out
void note_shfs(const char* op, const std::string& rel, const std::string& to = {}, const std::string& note = {},
               Mask mask = 0);
// requests that may have reached branch b: writes from the last 2 minutes (the journal
// reaches the disk up to a minute later), everything else from the last 30 s
std::vector<std::string> shfs_for(int b, size_t max, size_t& total, size_t& others);
