// verify.cpp - the verifier: compares the tree with the disks, slowly, while they spin
//
// One thread per disk or pool, at idle I/O priority. It walks the folders the tree
// knows for that disk, up to 50 per second (VERIFY_RATE), only while the disk spins
// anyway and its folders were read once (harvested) - and on a spinning disk only while
// something else uses it too, so it never keeps a disk from spinning down - and compares each folder's
// slice in RAM with the disk: names, type, mode, owner, links, size, the three
// times, xattrs. A difference only counts if a second look 5 s later shows the same
// and the folder didn't change in between (a change on its way through fanotify
// isn't a difference). Each difference is repaired in RAM and written to its own
// log (unwake-verify.log), with the requests Unwake passed to shfs for that folder.
//
// On with VERIFY=yes (not on the settings page); a beta build verifies regardless.
#include "unwaked.h"

#include <algorithm>
#include <condition_variable>
#include <dirent.h>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

static constexpr int64_t CONFIRM_MS = 5000;           // second look after a difference
static constexpr int64_t RETRY_MS = 60000;            // a folder that couldn't be checked or repaired: again then
static constexpr int64_t IDLE_MS = 60000;             // a disk nobody else used for this long: let it spin down
static constexpr off_t   LOG_MAX = 4 << 20, LOG_KEEP = 1 << 20;

// per disk, the current pass only (or the last one, until the next starts)
enum { V_WAITING = 0, V_RUNNING = 1, V_DONE = 2 };
struct VState {
    std::atomic<int>      state{V_WAITING};
    std::atomic<uint64_t> checked{0}, total{0};
    std::atomic<uint64_t> found{0}, repaired{0}, pending{0};   // differences; pending: repair retried
    std::atomic<int64_t>  done_at{0};                           // the pass ended (epoch s)
};
static std::unique_ptr<VState[]> _st;
static size_t _nst = 0;
static std::vector<std::thread> _threads;
static std::atomic<bool> _stop{false};
static std::mutex _mu;                    // for _cv, the log and the test hook
static std::condition_variable _cv;

bool verify_on() { return g_cfg.verify || strstr(UNWAKE_VERSION, "-beta") != nullptr; }

std::string verify_json(int b) {
    if (b < 0 || (size_t)b >= _nst) return "{}";
    auto& s = _st[b];
    std::ostringstream o;
    static const char* names[] = {"waiting", "running", "done"};
    o << "{\"state\":\"" << names[s.state] << "\",\"checked\":" << s.checked << ",\"total\":" << s.total
      << ",\"found\":" << s.found << ",\"repaired\":" << s.repaired << ",\"pending\":" << s.pending
      << ",\"done_at\":" << s.done_at << "}";
    return o.str();
}

// ---------------------------------------------------------------------------
// The verify log: its own file, cut to its newest part when it grows too big
// ---------------------------------------------------------------------------
static void vlog(const std::string& text) {
    std::lock_guard<std::mutex> l(_mu);
    struct stat st;
    if (stat(g_cfg.verify_log.c_str(), &st) == 0 && st.st_size > LOG_MAX)
        cut_to_tail(g_cfg.verify_log, LOG_KEEP, "(older entries cut)\n");
    std::ofstream out(g_cfg.verify_log, std::ios::app);
    out << fmt_time(time(nullptr)) << " " << text << "\n";
}

// ---------------------------------------------------------------------------
// Comparing one folder slice
// ---------------------------------------------------------------------------
struct InRam {
    bool shown;                                   // the attr shown is this disk's
    bool has_attr;
    Attr attr;
    bool is_dir;
    std::shared_ptr<const Xattrs> xattrs;         // null: not known
};
struct Diff { std::string name, what; bool ghost = false, missing = false, attr = false; };

static std::string ts(int64_t s, uint32_t ns) {
    char b[48];
    snprintf(b, sizeof b, "%lld.%09u", (long long)s, ns);
    return b;
}

static std::string xattr_names(const Xattrs& x) {
    std::string o;
    for (auto& kv : x) o += (o.empty() ? "" : ",") + kv.first;
    return o.empty() ? "none" : o;
}

// the tree's view of rel on branch b; false: not a folder whose slice for b is known
static bool snapshot(int b, const std::string& rel, std::map<std::string, InRam>& out, uint64_t& gen,
                     std::vector<std::string>* subdirs) {
    Mask m = bit(b);
    std::shared_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(rel);
    if (!d || !(d->known & m)) return false;
    gen = d->gen;
    out.clear();
    for (auto& kv : d->entries) {
        const Entry& e = kv.second;
        if (!(e.mask & m)) continue;
        bool shown = e.has_attr && (e.attr_from == b || (e.attr_from < 0 && __builtin_ctzll(e.mask) == b));
        out[kv.first] = InRam{shown, e.has_attr, e.attr, e.is_dir, shown ? e.xattrs : nullptr};
        if (subdirs && e.is_dir && e.dir) subdirs->push_back(join_path(rel, kv.first));
    }
    return true;
}

static std::vector<Diff> compare(const std::map<std::string, InRam>& ram, const std::vector<Item>& disk) {
    std::vector<Diff> out;
    std::set<std::string> on_disk;
    for (auto& it : disk) {
        on_disk.insert(it.name);
        auto r = ram.find(it.name);
        if (r == ram.end()) { out.push_back({it.name, "missing - on the disk, not in RAM", false, true}); continue; }
        const InRam& e = r->second;
        if (e.is_dir != it.is_dir) { out.push_back({it.name, std::string("type - RAM ") + (e.is_dir ? "folder" : "file") +
                                                  ", disk " + (it.is_dir ? "folder" : "file"), false, false, true}); continue; }
        if (!e.shown) continue;                   // another disk's copy is the one shown
        const Attr &a = e.attr, &d = it.attr;
        std::string w;
        auto f = [&](const char* k, const std::string& x, const std::string& y) {
            if (x != y) w += std::string(w.empty() ? "" : ", ") + k + " RAM " + x + " disk " + y;
        };
        f("mode", std::to_string(a.mode), std::to_string(d.mode));
        f("uid", std::to_string(a.uid), std::to_string(d.uid));
        f("gid", std::to_string(a.gid), std::to_string(d.gid));
        f("nlink", std::to_string(a.nlink), std::to_string(d.nlink));
        if (!it.is_dir) f("size", std::to_string(a.size), std::to_string(d.size));
        f("rdev", std::to_string(a.rdev), std::to_string(d.rdev));
        f("mtime", ts(a.mtime, a.mtime_ns), ts(d.mtime, d.mtime_ns));
        f("ctime", ts(a.ctime, a.ctime_ns), ts(d.ctime, d.ctime_ns));
        f("atime", ts(a.atime, a.atime_ns), ts(d.atime, d.atime_ns));
        if (e.xattrs && it.xattrs) {
            auto x = *e.xattrs, y = *it.xattrs;
            std::sort(x.begin(), x.end());
            std::sort(y.begin(), y.end());
            if (x != y) w += std::string(w.empty() ? "" : ", ") + "xattrs RAM " + xattr_names(x) + " disk " + xattr_names(y) +
                             (xattr_names(x) == xattr_names(y) ? " (values differ)" : "");
        }
        if (!w.empty()) out.push_back({it.name, w, false, false, true});
    }
    for (auto& kv : ram)
        if (!on_disk.count(kv.first)) out.push_back({kv.first, "ghost - in RAM, not on the disk", true});
    return out;
}

static void clear_known(DirNode* d, Mask m) {
    d->known &= ~m;
    for (auto& kv : d->entries) {
        kv.second.mask &= ~m;
        if (kv.second.dir) clear_known(kv.second.dir.get(), m);
    }
}

// what the disk says, into the tree; false if the folder changed meanwhile
static bool repair(int b, const std::string& rel, uint64_t gen, const std::vector<Diff>& diffs,
                   const std::vector<Item>& disk) {
    Mask m = bit(b);
    std::unique_lock<std::shared_mutex> l(g_tree.mu);
    DirNode* d = tree_find(rel);
    if (!d || d->gen != gen || !(d->known & m)) return false;
    std::map<std::string, const Item*> items;
    for (auto& it : disk) items[it.name] = &it;
    for (auto& df : diffs) {
        if (df.ghost) {
            auto it = d->entries.find(df.name);
            if (it == d->entries.end()) continue;
            it->second.mask &= ~m;
            if (!it->second.mask) tree_erase(d, df.name);
            else if (it->second.dir) clear_known(it->second.dir.get(), m);
            continue;
        }
        auto ii = items.find(df.name);
        if (ii == items.end()) continue;
        const Item& item = *ii->second;
        auto r = d->entries.try_emplace(df.name);
        Entry& e = r.first->second;
        if (r.second) g_tree.entries++;
        e.mask |= m;
        if (e.is_dir != item.is_dir && e.dir) { tree_count_add(e, -1); g_tree.entries++; e.dir.reset(); }
        e.is_dir = item.is_dir;
        e.dtype = item.dtype;
        if (attr_from_branch_wins(e, b)) {
            e.attr = item.attr;
            e.has_attr = true;
            e.attr_from = (int8_t)b;
            e.xattrs = item.xattrs;
        }
    }
    d->gen++;
    return true;
}

// ---------------------------------------------------------------------------
// Tests only: plant a wrong entry ("ghost|drop|size <disk> <path>" lines in
// <run_dir>/test-plant); the verifier must find and repair it
// ---------------------------------------------------------------------------
#if UNWAKE_TEST_HOOKS
static void test_plant() {
    if (g_cfg.branch_args.empty()) return;
    std::lock_guard<std::mutex> lk(_mu);
    auto path = g_cfg.run_dir + "/test-plant";
    std::ifstream f(path);
    if (!f) return;
    std::string op, disk, rel;
    while (f >> op >> disk >> rel) {
        int b = -1;
        for (size_t i = 0; i < g_branches.size(); i++) if (g_branches[i]->name == disk) b = (int)i;
        if (b < 0) continue;
        std::string dir = path_dir(rel), name = path_base(rel);
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        DirNode* d = tree_find(dir);
        if (!d) continue;
        if (op == "ghost") {
            auto r = d->entries.try_emplace(name);
            if (r.second) g_tree.entries++;
            r.first->second.mask |= bit(b);
            r.first->second.has_attr = true;
            r.first->second.attr_from = (int8_t)b;
            r.first->second.attr.mode = S_IFREG | 0644;
            r.first->second.dtype = DT_REG;
        } else if (op == "drop") {
            tree_erase(d, name);
        } else if (op == "size") {
            auto it = d->entries.find(name);
            if (it != d->entries.end()) it->second.attr.size += 1;
        }
    }
    f.close();
    unlink(path.c_str());
}
#endif

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------
// The disk's I/O count (reads + writes completed, all its devices); 0 if unknown
static uint64_t dev_ios(const Branch& b) {
    uint64_t t = 0;
    for (auto& d : b.devices) {
        std::ifstream f("/sys/block/" + d + "/stat");
        uint64_t rd = 0, x = 0, wr = 0;
        if (f >> rd >> x >> x >> x >> wr) t += rd + wr;
    }
    return t;
}

// folders a pass of branch b will check: those the tree knows for it (as the walk goes)
static uint64_t count_known(const DirNode* d, Mask m) {
    if (!(d->known & m)) return 0;
    uint64_t n = 1;
    for (auto& kv : d->entries)
        if ((kv.second.mask & m) && kv.second.is_dir && kv.second.dir) n += count_known(kv.second.dir.get(), m);
    return n;
}

static bool pause_for(int64_t ms) {
    std::unique_lock<std::mutex> l(_mu);
    _cv.wait_for(l, std::chrono::milliseconds(ms), [] { return _stop.load(); });
    return !_stop;
}

// NONE: no difference; UNSURE: differences that didn't hold still for a second look;
// REPAIRED / FAILED: n confirmed differences, repaired or not (the folder changed meanwhile)
enum Outcome { SKIPPED, NONE, UNSURE, REPAIRED, FAILED };   // SKIPPED: not a folder known for this disk

static Outcome check(int b, const std::string& rel, std::vector<std::string>* subdirs, uint64_t& n, bool retry) {
    Branch& br = *g_branches[b];
    std::map<std::string, InRam> ram;
    uint64_t gen = 0;
    n = 0;
    if (!snapshot(b, rel, ram, gen, subdirs)) return SKIPPED;
    std::string abs = br.root + (rel.empty() ? "" : "/" + rel);
    std::vector<Item> disk;
    if (!read_slice(abs, disk)) return NONE;      // gone meanwhile: its own events say so
    auto first = compare(ram, disk);
    if (first.empty()) return NONE;

    // a second look: only what is still different, unchanged, and without a change in between
    if (!pause_for(CONFIRM_MS) || !br.up) return UNSURE;
    uint64_t gen2 = 0;
    if (!snapshot(b, rel, ram, gen2, nullptr)) return UNSURE;
    disk.clear();
    if (!read_slice(abs, disk)) return UNSURE;
    auto second = compare(ram, disk);
    std::set<std::pair<std::string, std::string>> was;
    for (auto& d : first) was.insert({d.name, d.what});
    std::vector<Diff> sure;
    for (auto& d : second) if (was.count({d.name, d.what})) sure.push_back(d);
    if (sure.empty()) return UNSURE;
    n = sure.size();
    bool ok = repair(b, rel, gen2, sure, disk);
    std::string where = br.name + " " + (rel.empty() ? "/" : rel);
    std::string text;
    for (auto& d : sure) text += "\n    \"" + d.name + "\": " + d.what;
    auto reqs = shfs_recent(rel, 10);
    if (!reqs.empty()) {
        text += "\n    last requests Unwake passed to shfs in this folder:";
        for (auto& r : reqs) text += "\n      " + r;
    }
    std::string state = ok ? "repaired" : "not repaired yet (the folder changed meanwhile), retried in a minute";
    vlog(where + ": " + std::to_string(n) + " difference(s)" + (retry ? " (retry)" : "") + ", " + state + text);
    if (ok) log_warn("Verifier: " + std::to_string(n) + " difference(s) in " + where + " repaired - see " + g_cfg.verify_log);
    return ok ? REPAIRED : FAILED;
}

static void verify_thread(int b) {
    background_priority();
    Branch& br = *g_branches[b];
    VState& s = _st[b];
    std::vector<std::string> stack;
    bool in_pass = false;
    std::string now_token;                  // tests only (test-verify-now)
    struct Retry { std::string rel; int64_t due; uint64_t n; };
    std::vector<Retry> retries;             // confirmed differences whose repair has to wait
    // A spinning disk is only read while something else uses it too: our own reads would
    // keep it from spinning down. Its I/O count moving between our reads is someone else.
    bool watch_io = br.rotational && !br.devices.empty();
    uint64_t last_io = dev_ios(br);
    int64_t other_at = 0;                   // when someone else last used the disk
    while (!_stop) {
        long rate = std::max(1L, g_cfg.verify_rate.load());
        if (!pause_for(std::max<int64_t>(1, 1000 / rate))) break;
#if UNWAKE_TEST_HOOKS
        test_plant();
#endif
        bool can = verify_on() && g_caching && br.up && br.harvested && !br.untracked && g_cfg.harvest;
        if (watch_io) {
            uint64_t io = dev_ios(br);
            if (io != last_io) { other_at = now_ms(); last_io = io; }
            if (now_ms() - other_at > IDLE_MS) can = false;
        }
        if (!can) {
            if (in_pass || !s.done_at) s.state = V_WAITING;
            continue;
        }
        // repairs that had to wait, a minute later, until they succeed
        int64_t now = now_ms();
        auto r = std::find_if(retries.begin(), retries.end(), [&](const Retry& x) { return x.due <= now; });
        if (r != retries.end()) {
            uint64_t n = 0;
            Outcome o = check(b, r->rel, nullptr, n, true);
            if (watch_io) last_io = dev_ios(br);        // that was us
            if (o == FAILED || o == UNSURE) r->due = now_ms() + RETRY_MS;
            else {                          // repaired now, or right by itself meanwhile
                s.repaired += r->n;
                s.pending -= r->n;
                retries.erase(r);
            }
            continue;
        }
        if (!in_pass) {
            bool now_please = false;
#if UNWAKE_TEST_HOOKS
            // tests only: a new token in <run_dir>/test-verify-now starts the next pass at once
            if (!g_cfg.branch_args.empty()) {
                std::ifstream f(g_cfg.run_dir + "/test-verify-now");
                std::string tok;
                if (f >> tok && tok != now_token) { now_token = tok; now_please = true; }
            }
#endif
            if (s.done_at && time(nullptr) < s.done_at + g_cfg.verify_interval && !now_please) { s.state = V_DONE; continue; }
            stack.assign(1, "");
            s.checked = 0;
            {
                std::shared_lock<std::shared_mutex> l(g_tree.mu);
                s.total = count_known(&g_tree.root, bit(b));
            }
            s.found = s.pending.load();     // still being repaired from the last pass
            s.repaired = 0;
            in_pass = true;
            log_debug("verify " + br.name + ": pass started, " + std::to_string(s.total.load()) + " folders");
        }
        s.state = V_RUNNING;
        auto rel = stack.back();
        stack.pop_back();
        if (tree_cacheable(rel)) {
            std::vector<std::string> subdirs;
            uint64_t n = 0;
            Outcome o = check(b, rel, &subdirs, n, false);
            if (watch_io) last_io = dev_ios(br);        // that was us
            if (o == REPAIRED) { s.found += n; s.repaired += n; }
            else if (o == FAILED) { s.found += n; s.pending += n; retries.push_back({rel, now_ms() + RETRY_MS, n}); }
            if (o != SKIPPED) s.checked++;
            std::sort(subdirs.rbegin(), subdirs.rend());
            for (auto& d : subdirs) stack.push_back(std::move(d));
        }
        if (stack.empty()) {
            s.total = s.checked.load();     // folders made or removed during the pass
            br.harvest_dirs = s.total.load();   // the disk's folder count, as of this pass
            in_pass = false;
            log_debug("verify " + br.name + ": pass done, " + std::to_string(s.checked.load()) + " folders, " +
                      std::to_string(s.found.load()) + " difference(s)");
            s.done_at = time(nullptr);
            s.state = V_DONE;
        }
    }
}

void verify_start() {
    if (!_threads.empty()) return;
    if (_nst != g_branches.size()) {
        _nst = g_branches.size();
        _st.reset(new VState[_nst]);
    }
    _stop = false;
    for (size_t i = 0; i < _nst; i++) { _st[i].state = V_WAITING; _st[i].done_at = 0; _st[i].pending = 0; }
    for (size_t i = 0; i < g_branches.size() && i < (size_t)MAX_BRANCHES; i++) _threads.emplace_back(verify_thread, (int)i);
}

void verify_stop() {
    {
        std::lock_guard<std::mutex> l(_mu);
        _stop = true;
    }
    _cv.notify_all();
    for (auto& t : _threads) t.join();
    _threads.clear();
}
