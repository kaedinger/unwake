// branches.cpp - the disks behind /mnt/user, their spin state, and the harvester
//
// Branches come from emhttpd's disks.ini: every mounted array data disk and
// every pool, in the order shfs uses them (pools first). The harvester walks a
// branch's own mount point (never /mnt/user) while that disk is spinning
// anyway, so the cache fills without ever waking a disk.
#include "unwaked.h"

#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

std::vector<std::unique_ptr<Branch>> g_branches;
static bool _spin_known = false;   // the first poll only records the state

static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> read_ini(const std::string& file) {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> out;
    std::ifstream f(file);
    std::string line, sec;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.size() >= 2 && line.front() == '[') {
            sec = line.substr(1, line.size() - 2);
            if (sec.size() >= 2 && sec.front() == '"') sec = sec.substr(1, sec.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto v = line.substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        out[sec][line.substr(0, eq)] = v;
    }
    return out;
}

static std::vector<std::string> mount_points() {
    std::vector<std::string> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, parent, devno, root, mp;
        ss >> id >> parent >> devno >> root >> mp;
        out.push_back(mp);
    }
    return out;
}

static bool is_mounted(const std::vector<std::string>& mps, const std::string& p) {
    for (auto& m : mps) if (m == p) return true;
    return false;
}

bool branches_load() {
    g_branches.clear();
    if (!g_cfg.branch_args.empty()) {
        // tests: --branch name=path[,ssd]
        for (auto& a : g_cfg.branch_args) {
            auto eq = a.find('=');
            if (eq == std::string::npos) continue;
            auto b = std::make_unique<Branch>();
            b->name = a.substr(0, eq);
            auto rest = split(a.substr(eq + 1), ',');
            if (rest.empty()) continue;
            b->root = rest[0];
            b->rotational = !(rest.size() > 1 && rest[1] == "ssd");
            g_branches.push_back(std::move(b));
        }
    } else {
        auto ini = read_ini(g_cfg.disks_ini);
        auto mps = mount_points();
        std::vector<std::unique_ptr<Branch>> pools, disks;
        // pools: "cache", "cache2", ...: extra members carry no mount point
        std::map<std::string, Branch*> by_name;
        std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> rows(ini.begin(), ini.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
            return std::stoi("0" + a.second["idx"]) < std::stoi("0" + b.second["idx"]);
        });
        for (auto& r : rows) {
            auto& kv = r.second;
            auto type = kv["type"];
            if (type != "Data" && type != "Cache") continue;
            auto mp = kv["fsMountpoint"];
            if (mp.empty() || !is_mounted(mps, mp)) continue;
            auto b = std::make_unique<Branch>();
            b->name = kv["name"];
            b->root = mp;
            b->rotational = kv["rotational"] != "0";
            if (!kv["device"].empty()) b->devices.push_back(kv["device"]);
            by_name[b->name] = b.get();
            (type == "Cache" ? pools : disks).push_back(std::move(b));
        }
        for (auto& r : rows) {
            auto& kv = r.second;
            if (kv["type"] != "Cache" || !kv["fsMountpoint"].empty() || kv["device"].empty()) continue;
            auto base = kv["name"];
            while (!base.empty() && isdigit((unsigned char)base.back())) base.pop_back();
            auto it = by_name.find(base);
            if (it != by_name.end()) {
                it->second->devices.push_back(kv["device"]);
                if (kv["rotational"] != "0") it->second->rotational = true;
            }
        }
        // shfs: pools first, then disk1..N
        std::sort(disks.begin(), disks.end(), [](auto& a, auto& b) {
            return std::stoi("0" + a->name.substr(4)) < std::stoi("0" + b->name.substr(4));
        });
        for (auto& p : pools) g_branches.push_back(std::move(p));
        for (auto& d : disks) g_branches.push_back(std::move(d));
    }
    g_tree.all = 0;
    g_tree.per_branch = g_branches.size() <= (size_t)MAX_BRANCHES;
    if (g_tree.per_branch)
        for (size_t i = 0; i < g_branches.size(); i++) g_tree.all |= bit((int)i);
    spin_poll();
    _spin_known = true;
    return !g_branches.empty();
}

// ---------------------------------------------------------------------------
// Spin state: disks.ini "spundown" per device, re-read when it changes. A
// branch counts as up only when all of its devices are. Tests list spun-down
// branch names in <run_dir>/test-spundown instead.
// ---------------------------------------------------------------------------
static struct timespec _ini_mtime = {0, 0};

// A disk that spun up: say what Unwake passed to shfs just before that may have reached
// it. If nothing, Unwake didn't let it through - Unspin's Disk Wake-ups names who did.
static void spin_changed(Branch& b, bool up) {
    if (!b.rotational || !_spin_known) return;
    if (!up) { log_debug(b.name + " spun down"); return; }
    int idx = -1;
    for (size_t i = 0; i < g_branches.size(); i++) if (g_branches[i].get() == &b) idx = (int)i;
    size_t total = 0, others = 0;
    auto reqs = shfs_for(idx, 12, total, others);
    std::string other = others ? " (" + std::to_string(others) + " for other disks only)" : "";
    if (reqs.empty()) {
        log_info(b.name + " spun up - Unwake passed nothing for it to shfs in the 30 s before (writes: 2 min)" + other);
        return;
    }
    std::string list;
    for (auto& r : reqs) list += "\n    " + r;
    log_info(b.name + " spun up - Unwake passed " + std::to_string(total) + " request(s) for it to shfs in the 30 s before (writes: 2 min)" +
             (total > reqs.size() ? ", the last " + std::to_string(reqs.size()) : std::string()) + other + ":" + list);
}

void spin_poll() {
    if (!g_cfg.branch_args.empty()) {
#if UNWAKE_TEST_HOOKS
        std::ifstream f(g_cfg.run_dir + "/test-spundown");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto down = split(all, '\n');
#else
        std::vector<std::string> down;
#endif
        for (auto& b : g_branches) {
            bool up = std::find(down.begin(), down.end(), b->name) == down.end();
            if (up != b->up.exchange(up)) spin_changed(*b, up);
        }
        return;
    }
    struct stat st;
    if (stat(g_cfg.disks_ini.c_str(), &st) != 0) return;
    if (st.st_mtim.tv_sec == _ini_mtime.tv_sec && st.st_mtim.tv_nsec == _ini_mtime.tv_nsec) return;
    _ini_mtime = st.st_mtim;
    auto ini = read_ini(g_cfg.disks_ini);
    std::unordered_map<std::string, bool> dev_down;
    for (auto& r : ini) {
        auto& kv = r.second;
        if (!kv["device"].empty()) dev_down[kv["device"]] = kv["spundown"] == "1";
    }
    for (auto& b : g_branches) {
        bool up = true;
        for (auto& d : b->devices) {
            auto it = dev_down.find(d);
            if (it != dev_down.end() && it->second) up = false;
        }
        if (up != b->up.exchange(up)) spin_changed(*b, up);
    }
}

// Going down: fsState was "Started" and is now "Stopping" or "Stopped". Values
// seen during the start (before "Started") never count.
bool array_stopping() {
    static bool seen_started = false;
    if (!g_cfg.branch_args.empty()) return false;
    auto ini = read_ini(g_cfg.var_ini);
    for (auto& r : ini) {
        auto it = r.second.find("fsState");
        if (it == r.second.end()) continue;
        if (it->second == "Started") { seen_started = true; return false; }
        return seen_started && (it->second == "Stopping" || it->second == "Stopped");
    }
    return false;
}

// Shares whose settings put their files on an SSD pool only (primary storage a
// pool, nothing behind it or the Mover moving towards the pool: shareUseCache
// "only" or "prefer"). Such a share can't wake a disk - unless it has leftovers on
// the array (an older setup, a Mover that never finished, "prefer" overflowing to
// the array when the pool was full): then shfs checks those disks on every listing.
// So: passed to shfs while no array disk holds a folder of it, kept in RAM as soon
// as one does. An array disk counts once its top level was read (harvest, or a
// change event there). INCLUDED_SHARES / EXCLUDED_SHARES override all of this.
static std::vector<std::string> _pool_only;          // candidates, from the share settings
static std::mutex _pool_mu;
static std::map<std::string, std::pair<bool, std::vector<std::string>>> _pool_state;   // share -> (all disks known, disks holding it)

void apply_exclusions() {
    _pool_only.clear();
    DIR* dp = opendir(g_cfg.shares_dir.c_str());
    while (dp) {
        auto de = readdir(dp);
        if (!de) break;
        std::string f = de->d_name;
        if (f.size() < 5 || f.compare(f.size() - 4, 4, ".cfg")) continue;
        auto share = f.substr(0, f.size() - 4);
        auto ini = read_ini(g_cfg.shares_dir + "/" + f);
        auto& kv = ini[""];
        auto use = kv["shareUseCache"], pool = kv["shareCachePool"];
        if (use != "only" && use != "prefer") continue;
        if (pool.empty()) pool = "cache";
        for (auto& b : g_branches)
            if (b->name == pool && !b->rotational) _pool_only.push_back(share);
    }
    if (dp) closedir(dp);
    recompute_exclusions();
}

void recompute_exclusions() {
    auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    std::vector<std::string> ex = g_cfg.excluded_explicit;
    std::map<std::string, std::pair<bool, std::vector<std::string>>> state;
    {
        std::shared_lock<std::shared_mutex> l(g_tree.mu);
        for (auto& share : _pool_only) {
            bool known = true;
            std::vector<std::string> on;
            auto it = g_tree.root.entries.find(share);
            for (size_t i = 0; i < g_branches.size() && i < (size_t)MAX_BRANCHES; i++) {
                if (!g_branches[i]->rotational) continue;
                if (!(g_tree.root.known & bit((int)i))) known = false;
                if (it != g_tree.root.entries.end() && (it->second.mask & bit((int)i))) on.push_back(g_branches[i]->name);
            }
            state[share] = {known, on};
            if (on.empty() && !has(g_cfg.included_explicit, share)) ex.push_back(share);
        }
    }
    std::sort(ex.begin(), ex.end());
    ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
    {
        std::lock_guard<std::mutex> l(_pool_mu);
        _pool_state = state;
    }
    auto old = std::atomic_load(&g_cfg.excluded_shares);
    if (*old == ex) return;
    std::vector<std::string> changed, now_cached;
    for (auto& s : *old) if (!std::binary_search(ex.begin(), ex.end(), s)) { changed.push_back(s); now_cached.push_back(s); }
    for (auto& s : ex) if (!std::binary_search(old->begin(), old->end(), s)) changed.push_back(s);
    std::string list;
    for (auto& s : ex) list += (list.empty() ? "" : ", ") + s;
    log_info("Passed to shfs, not kept in RAM: " + (list.empty() ? std::string("(none)") : list));
    for (auto& s : now_cached) {
        auto& on = state[s].second;
        std::string disks;
        for (auto& d : on) disks += (disks.empty() ? "" : ", ") + d;
        if (!on.empty()) log_info(s + " lives on an SSD pool, but " + disks + (on.size() == 1 ? " holds" : " hold") + " folders of it - kept in RAM");
    }
    std::atomic_store(&g_cfg.excluded_shares, std::shared_ptr<const std::vector<std::string>>(std::make_shared<std::vector<std::string>>(ex)));
    {
        // events below an excluded share were skipped: what's known about it is stale either way
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        for (auto& s : changed) tree_forget(s);
    }
    if (!now_cached.empty())
        for (size_t i = 0; i < g_branches.size(); i++) harvester_restart((int)i);
}

std::string pool_only_json() {
    std::lock_guard<std::mutex> l(_pool_mu);
    std::string o = "{";
    for (auto& kv : _pool_state) {
        if (o.size() > 1) o += ",";
        o += "\"" + kv.first + "\":{\"known\":" + (kv.second.first ? "1" : "0") + ",\"on\":[";
        for (size_t i = 0; i < kv.second.second.size(); i++) o += (i ? ",\"" : "\"") + kv.second.second[i] + "\"";
        o += "]}";
    }
    return o + "}";
}

int branch_of_path(const std::string& abs, std::string& rel) {
    for (size_t i = 0; i < g_branches.size(); i++) {
        auto& root = g_branches[i]->root;
        if (abs == root) { rel.clear(); return (int)i; }
        if (abs.size() > root.size() && abs.compare(0, root.size(), root) == 0 && abs[root.size()] == '/') {
            rel = abs.substr(root.size() + 1);
            return (int)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Harvester: one thread per branch, idle I/O priority. Walks the branch while
// it is up, merges each directory's slice into the union tree and marks it
// known. A directory already known for this branch isn't read again (fanotify
// keeps it current); its children are taken from the tree.
// ---------------------------------------------------------------------------
static std::atomic<bool>        _h_stop{false};
static std::vector<std::thread> _h_threads;
static std::mutex               _h_mu;
static std::condition_variable  _h_cv;

void harvester_restart(int b) {
    if (b < 0 || b >= (int)g_branches.size()) return;
    g_branches[b]->harvested = false;
    _h_cv.notify_all();
}

// Reads one directory of a branch. false: the directory is gone or unreadable.
bool read_slice(const std::string& abs, std::vector<Item>& out) {
    int fd = open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOATIME);
    if (fd < 0) fd = open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR* dp = fdopendir(fd);
    if (!dp) { close(fd); return false; }
    while (auto de = readdir(dp)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(dp), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        Item it;
        it.name = de->d_name;
        it.attr.from(st);
        it.is_dir = S_ISDIR(st.st_mode);
        it.dtype = (uint8_t)IFTODT(st.st_mode);
        it.dev = st.st_dev;
        // the inode was just read: inline xattrs cost nothing more (Samba asks for them per file)
        it.xattrs = read_xattrs(abs + "/" + it.name);
        out.push_back(std::move(it));
    }
    closedir(dp);
    return true;
}

static void walk(int b) {
    Branch& br = *g_branches[b];
    Mask m = bit(b);
    int64_t t0 = now_ms();
    uint64_t ndirs = 0, nentries = 0, nread = 0;
    std::vector<std::string> stack{""};
    std::unordered_map<std::string, int> tries;     // folders that changed while read
    br.harvesting = true;
    log_info("Harvest of " + br.name + " (" + br.root + ") started");
    while (!stack.empty()) {
        if (_h_stop || !br.up || !g_caching || !g_cfg.harvest) {
            br.harvesting = false;
            log_info("Harvest of " + br.name + " paused after " + human_count(ndirs) + " directories" +
                     (!br.up ? " - the disk spun down" : ""));
            return;
        }
        auto rel = stack.back();
        stack.pop_back();
        if (!tree_cacheable(rel)) continue;

        bool known;
        std::vector<std::string> subdirs;
        {
            std::unique_lock<std::shared_mutex> l(g_tree.mu);
            DirNode* d = rel.empty() ? &g_tree.root : tree_ensure(rel, m, dbit(b));
            known = d->known & m;
            d->dirty &= ~dbit(b);   // a change on this disk from now on means: read again
            if (known) {
                for (auto& kv : d->entries)
                    if ((kv.second.mask & m) && kv.second.is_dir) subdirs.push_back(join_path(rel, kv.first));
            }
        }
        if (!known) {
            std::vector<Item> items;
            if (!read_slice(br.root + (rel.empty() ? "" : "/" + rel), items)) continue;
#if UNWAKE_TEST_HOOKS
            if (!g_cfg.branch_args.empty()) {     // tests only: "<ms> <folder>" in test-slow-harvest
                std::ifstream f(g_cfg.run_dir + "/test-slow-harvest");
                int ms = 0;
                std::string at;
                if (f >> ms >> at && at == rel) usleep(ms * 1000);
            }
#endif
            nread++;
            std::unique_lock<std::shared_mutex> l(g_tree.mu);
            DirNode* d = rel.empty() ? &g_tree.root : tree_find(rel);
            if (!d) continue;                       // removed meanwhile
            if (d->dirty & dbit(b)) {               // this disk changed here while we read: again, a few times
                if (++tries[rel] <= 5) {
                    log_debug("harvest of " + br.name + ": " + (rel.empty() ? "/" : rel) + " changed while read, reading again");
                    stack.push_back(rel);
                } else {
                    // keeps changing: left to shfs (and fanotify) for now, not marked known
                    log_debug("harvest of " + br.name + ": " + (rel.empty() ? "/" : rel) + " keeps changing, left out");
                }
                continue;
            }
            std::unordered_map<std::string, bool> seen;
            for (auto& it : items) {
                auto r = d->entries.try_emplace(it.name);
                Entry& e = r.first->second;
                if (r.second) g_tree.entries++;
                e.mask |= m;
                e.is_dir = it.is_dir;
                e.dtype = it.dtype;
                if (attr_from_branch_wins(e, b)) {
                    e.attr = it.attr; e.has_attr = true; e.attr_from = (int8_t)b;
                    e.xattrs = it.xattrs;
                }
                seen[it.name] = true;
            }
            std::vector<std::string> gone;
            for (auto& kv : d->entries)
                if ((kv.second.mask & m) && !seen.count(kv.first)) gone.push_back(kv.first);
            for (auto& n : gone) {
                Entry& e = d->entries[n];
                e.mask &= ~m;
                if (!e.mask) {                      // a change we never saw
                    tree_erase(d, n, dbit(b));
                    d->pt_complete = false;
                }
            }
            d->known |= m;
            touch(d, dbit(b));  // readers that asked shfs before must not write over this
            for (auto& it : items)   // never into a mount we get no change events for
                if (it.is_dir && events_dev_watched(it.dev)) subdirs.push_back(join_path(rel, it.name));
            nentries += items.size();
        }
        ndirs++;
        // depth first, in name order
        std::sort(subdirs.rbegin(), subdirs.rend());
        for (auto& s : subdirs) stack.push_back(std::move(s));
    }
    br.harvesting = false;
    br.harvested = true;
    br.harvest_ms = now_ms() - t0;
    br.harvested_at = time(nullptr);
    br.harvest_dirs = ndirs;
    br.harvest_entries = nentries;
    log_info("Harvest of " + br.name + " done: " + human_count(ndirs) + " directories, " +
             human_count(nread) + " read from the disk, in " + fmt_secs((long)(br.harvest_ms / 1000)));
}

static void harvest_thread(int b) {
    // idle I/O class: the disk serves everyone else first
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)syscall(SYS_gettid), (3 << 13) /* IDLE */);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    Branch& br = *g_branches[b];
    while (!_h_stop) {
        {
            std::unique_lock<std::mutex> l(_h_mu);
            _h_cv.wait_for(l, std::chrono::seconds(3));
        }
        if (_h_stop) break;
        if (!g_cfg.harvest || br.harvested || !br.up || !g_caching || br.untracked) continue;
        walk(b);
    }
}

void harvester_start() {
    verify_start();
    _h_stop = false;
    for (size_t i = 0; i < g_branches.size() && i < (size_t)MAX_BRANCHES; i++)
        _h_threads.emplace_back(harvest_thread, (int)i);
}

void harvester_stop() {
    verify_stop();
    _h_stop = true;
    _h_cv.notify_all();
    for (auto& t : _h_threads) t.join();
    _h_threads.clear();
}

// ---------------------------------------------------------------------------
// In-place upgrade: the disks must be the same ones in the same order (the
// tree's masks are by index); what each one's harvest has done goes along.
// ---------------------------------------------------------------------------
#include "handover.h"

void branches_handover_out(HoWriter& w) {
    w.u64(g_branches.size());
    for (auto& b : g_branches) {
        w.str(b->name);
        w.str(b->root);
        w.u8(b->harvested);
        w.u8(b->untracked);
        w.i64(b->harvest_ms);
        w.i64(b->harvested_at);
        w.u64(b->harvest_dirs);
        w.u64(b->harvest_entries);
    }
    auto ex = std::atomic_load(&g_cfg.excluded_shares);
    w.u64(ex->size());
    for (auto& s : *ex) w.str(s);
}

void branches_handover_in(HoReader& r) {
    uint64_t n = r.u64();
    if (n != g_branches.size()) throw std::runtime_error("number of disks and pools differs");
    for (auto& b : g_branches) {
        auto name = r.str(), root = r.str();
        if (name != b->name || root != b->root)
            throw std::runtime_error("disk " + b->name + " was " + name + " in the running Unwake");
        b->harvested = r.u8();
        b->untracked = r.u8();
        b->harvest_ms = r.i64();
        b->harvested_at = r.i64();
        b->harvest_dirs = r.u64();
        b->harvest_entries = r.u64();
    }
    auto ex = std::make_shared<std::vector<std::string>>();
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) ex->push_back(r.str());
    std::atomic_store(&g_cfg.excluded_shares, std::shared_ptr<const std::vector<std::string>>(ex));
}
