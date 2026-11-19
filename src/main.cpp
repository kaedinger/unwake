// main.cpp - supervisor, worker lifecycle, status file
//
// unwaked runs as two processes:
//   supervisor: forks the worker and waits. If the worker dies, its dead mount
//               would leave /mnt/user answering "Transport endpoint is not
//               connected"; the supervisor detaches it at once, so new access
//               reaches shfs again, raises an Unraid notification and starts a
//               new worker (at most 3 times an hour).
//   worker:     takes an O_PATH handle to shfs, mounts itself on top of
//               /mnt/user and serves it.
//
// Stopping (SIGTERM): the worker detaches its mount (lazy unmount), so every new
// access goes to shfs right away, and keeps serving whatever still uses it
// (open files, a VM, containers started while it ran) until the last user is
// gone. SIGUSR1 (array stop) does the same but gives up after 10 seconds and
// aborts the connection, so Unraid can unmount shfs. A draining worker also
// stops by itself when var.ini says the array is going down.
//
// unwaked never calls libfuse's unmount: after the detach, the mount on top of
// /mnt/user is shfs again, and that must never be touched.
#include "unwaked.h"

#include <malloc.h>
#include "handover.h"

#include <algorithm>
#include <csignal>
#include <execinfo.h>
#include <cstring>
#include <fcntl.h>
#include <exception>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static volatile sig_atomic_t _sig_stop = 0, _sig_array = 0, _sig_reload = 0;
static std::string _version = UNWAKE_VERSION;

struct TopMount { int id = -1; std::string fstype, source; unsigned major = 0, minor = 0; };

// the mount currently on top at path (last in mountinfo)
static TopMount top_mount(const std::string& path) {
    TopMount t;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, parent, devno, root, mp, opts, tok;
        ss >> id >> parent >> devno >> root >> mp >> opts;
        if (mp != path) continue;
        while (ss >> tok && tok != "-") {}
        TopMount m;
        m.id = std::stoi(id);
        ss >> m.fstype >> m.source;
        sscanf(devno.c_str(), "%u:%u", &m.major, &m.minor);
        t = m;
    }
    return t;
}

static void write_file(const std::string& path, const std::string& s) {
    auto tmp = path + ".tmp";
    { std::ofstream o(tmp, std::ios::trunc); o << s; }
    rename(tmp.c_str(), path.c_str());
}

static void notify(const std::string& subject, const std::string& desc, const char* importance) {
    const char* n = "/usr/local/emhttp/webGui/scripts/notify";
    if (access(n, X_OK) != 0) return;
    std::string cmd = std::string(n) + " -e Unwake -s '" + subject + "' -d '" + desc + "' -i " + importance + " >/dev/null 2>&1 &";
    if (system(cmd.c_str())) {}
}

static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

static long rss_kb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0, res = 0;
    f >> pages >> res;
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

// Heap memory malloc has freed but keeps for reuse: the anonymous resident memory minus
// what is allocated (mallinfo2's own free count also holds pages already given back).
// Includes the threads' stacks, a few MB.
static long kept_free_kb() {
    static long kept = 0;
    static int64_t at = 0;
    int64_t now = now_ms();
    if (at && now - at < 30000) return kept;     // mallinfo2 walks the free chunks under the arena locks
    at = now;
    long anon = 0;
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line))
            if (!line.compare(0, 8, "RssAnon:")) { anon = atol(line.c_str() + 8); break; }
    }
    struct mallinfo2 mi = mallinfo2();
    long used = (long)((mi.uordblks + mi.hblkhd) / 1024);
    kept = anon > used ? anon - used : 0;
    return kept;
}

// RETURN_RAM=yes: once the kernel has forgotten many names (after the nightly backup's
// inodes aged out of its cache), give the freed memory back to the system. Not while
// it's still forgetting, and at most every 5 minutes; malloc_trim takes some ms.
static void trim_ram() {
    static uint64_t at_trim = 0, seen = 0;
    static int64_t last_trim = 0, last_change = 0;
    uint64_t f = g_nodes_forgotten;
    int64_t now = now_ms();
    if (f != seen) { seen = f; last_change = now; return; }
    if (f - at_trim < 100000 || now - last_change < 10000 || now - last_trim < 300000) return;
    long before = rss_kb();
    malloc_trim(0);
    log_debug("Returned freed RAM: " + std::to_string((before - rss_kb()) / 1024) + " MB, after the kernel forgot " +
              std::to_string(f - at_trim) + " names");
    at_trim = f;
    last_trim = now;
}

static void write_status(const std::string& state, time_t started) {
    std::ostringstream o;
    auto n = [&](const char* k, uint64_t v) { o << jstr(k) << ":" << v << ","; };
    o << "{" << jstr("state") << ":" << jstr(state) << "," << jstr("version") << ":" << jstr(_version) << ",";
    n("pid", (uint64_t)getpid());
    n("started", (uint64_t)started);
    n("updated", (uint64_t)time(nullptr));
    n("caching", g_caching ? 1 : 0);
    n("passthrough", g_passthrough ? 1 : 0);
    n("handover", HANDOVER_FORMAT);   // takes an in-place upgrade (rc.unwake upgrade)
    n("dirs", g_tree.dirs);
    n("entries", g_tree.entries);
    n("inodes", fs_node_count());
    n("rss_kb", (uint64_t)rss_kb());
    n("kept_free_kb", (uint64_t)kept_free_kb());
    n("events", g_events);
    n("lookup_hit", g_stats.lookup_hit); n("lookup_neg", g_stats.lookup_neg); n("lookup_miss", g_stats.lookup_miss);
    n("getattr_hit", g_stats.getattr_hit); n("getattr_miss", g_stats.getattr_miss);
    n("readdir_hit", g_stats.readdir_hit); n("readdir_miss", g_stats.readdir_miss);
    n("xattr_hit", g_stats.xattr_hit); n("xattr_miss", g_stats.xattr_miss);
    n("open_direct", g_stats.open_direct); n("open_shfs", g_stats.open_shfs);
    o << jstr("pool_only") << ":" << pool_only_json() << ",";
    o << jstr("branches") << ":[";
    for (size_t i = 0; i < g_branches.size(); i++) {
        auto& b = *g_branches[i];
        if (i) o << ",";
        o << "{" << jstr("name") << ":" << jstr(b.name) << "," << jstr("root") << ":" << jstr(b.root) << ","
          << jstr("up") << ":" << (b.up ? 1 : 0) << "," << jstr("rotational") << ":" << (b.rotational ? 1 : 0) << ","
          << jstr("harvested") << ":" << (b.harvested ? 1 : 0) << "," << jstr("harvesting") << ":" << (b.harvesting ? 1 : 0) << ","
          << jstr("untracked") << ":" << (b.untracked ? 1 : 0) << "," << jstr("harvest_ms") << ":" << b.harvest_ms << ","
          << jstr("harvested_at") << ":" << b.harvested_at << "," << jstr("dirs") << ":" << b.harvest_dirs << "}";
    }
    o << "]}\n";
    write_file(g_cfg.run_dir + "/status.json", o.str());
}

// ---------------------------------------------------------------------------
// Crash report: signal, load address and a raw backtrace into the log, so it
// can be resolved against the released binary (addr2line -e unwaked ADDR-BASE).
// Only async-signal-safe calls in the handler; backtrace() is primed beforehand.
// ---------------------------------------------------------------------------
static char _crash_log[512];
static char _crash_head[160];
static char _altstack[65536];

static void crash_handler(int sig) {
    int fd = open(_crash_log, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGABRT ? "SIGABRT" :
                           sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "signal";
        if (write(fd, "\n[ERROR] unwaked crashed: ", 26) < 0) {}
        if (write(fd, name, strlen(name)) < 0) {}
        if (write(fd, _crash_head, strlen(_crash_head)) < 0) {}
        void* frames[48];
        int n = backtrace(frames, 48);
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler() {
    snprintf(_crash_log, sizeof _crash_log, "%s", g_cfg.log_file.c_str());
    unsigned long base = 0;
    if (FILE* m = fopen("/proc/self/maps", "r")) { if (fscanf(m, "%lx", &base) != 1) base = 0; fclose(m); }
    snprintf(_crash_head, sizeof _crash_head, " (unwaked %s, load address 0x%lx) - backtrace:\n", _version.c_str(), base);
    void* prime[2];
    backtrace(prime, 2);
    stack_t ss = {};
    ss.ss_sp = _altstack;
    ss.ss_size = sizeof _altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa = {};
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) sigaction(s, &sa, nullptr);
    std::set_terminate([] {
        log_err("unwaked: uncaught exception");
        abort();
    });
}

static void on_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) _sig_stop = 1;
    else if (sig == SIGUSR1) { _sig_stop = 1; _sig_array = 1; }
    else if (sig == SIGHUP) _sig_reload = 1;
}

static void install_signals() {
    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    for (int s : {SIGTERM, SIGINT, SIGUSR1, SIGHUP}) sigaction(s, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
static int  _ctl_fd = -1;      // control socket from the supervisor (upgrade requests)
static int  _adopt_fd = -1;    // new worker of an upgrade: socket to the old one
static bool _handed_over = false;

// a new unwaked of an in-place upgrade: take the running connection over
static struct fuse_session* adopt(struct fuse_args* args, TopMount& mine, time_t& started) {
    if (!branches_load()) { log_err("Upgrade: no disks or pools found - not taking over"); return nullptr; }
    auto se = fuse_session_new(args, fs_ops(), sizeof(struct fuse_lowlevel_ops), nullptr);
    if (!se) { log_err("fuse_session_new failed"); return nullptr; }
    std::string test;   // tests only: make the take-over fail
#if UNWAKE_TEST_HOOKS
    if (!g_cfg.branch_args.empty()) { std::ifstream f(g_cfg.run_dir + "/test-adopt"); f >> test; }
#endif
    HoMount m;
    if (!handover_receive(_adopt_fd, se, m, test)) { fuse_session_destroy(se); return nullptr; }
    close(_adopt_fd);
    _adopt_fd = -1;
    mine.id = m.id; mine.major = m.major; mine.minor = m.minor; mine.fstype = "fuse.unwake";
    started = (time_t)m.started;
    apply_exclusions();   // the tree is in place: comes out as the old one had it
    events_resume();
    return se;
}

// a fresh start: mount on top of shfs
static struct fuse_session* mount_new(struct fuse_args* args, TopMount& mine) {
    auto below = top_mount(g_cfg.mountpoint);
    if (below.fstype == "fuse.unwake") {
        log_err(g_cfg.mountpoint + " already has Unwake on top - not stacking a second one");
        return nullptr;
    }
    if (g_cfg.branch_args.empty() && below.fstype != "fuse.shfs") {
        log_err(g_cfg.mountpoint + " is " + (below.fstype.empty() ? "not mounted" : below.fstype) +
                ", not the user share file system (shfs) - not starting");
        return nullptr;
    }
    g_shfs_fd = open(g_cfg.mountpoint.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (g_shfs_fd < 0) {
        log_err("open " + g_cfg.mountpoint + ": " + strerror(errno));
        return nullptr;
    }
    if (!branches_load()) {
        log_warn("No disks or pools found - passing everything through, no caching");
        g_caching = false;
    } else {
        for (auto& b : g_branches)
            log_info("Branch " + b->name + ": " + b->root + (b->rotational ? "" : " (no spin-down)") +
                     (b->up ? "" : " - spun down"));
        apply_exclusions();
        if (!g_tree.per_branch) log_warn("More than 64 disks and pools: caching only directories read through shfs");
        if (!events_start()) g_caching = false;
    }
    auto se = fuse_session_new(args, fs_ops(), sizeof(struct fuse_lowlevel_ops), nullptr);
    if (!se) { log_err("fuse_session_new failed"); return nullptr; }
    if (fuse_session_mount(se, g_cfg.mountpoint.c_str()) != 0) {
        log_err("Mounting on " + g_cfg.mountpoint + " failed - shfs stays as it is");
        fuse_session_destroy(se);
        return nullptr;
    }
    mine = top_mount(g_cfg.mountpoint);
    if (mine.fstype != "fuse.unwake") {
        log_err("Mounted, but the top mount of " + g_cfg.mountpoint + " is " + mine.fstype + " - stopping");
        fuse_session_destroy(se);
        return nullptr;
    }
    log_info("Mounted on top of " + g_cfg.mountpoint + " (mount " + std::to_string(mine.id) + ")");
    return se;
}

static int worker() {
    time_t started = time(nullptr);
    install_signals();
    signal(SIGUSR2, SIG_IGN);
    umask(0);   // the caller's mode arrives with its umask applied; ours must not cut it further
    install_crash_handler();
    for (auto& w : load_config()) log_warn(w);
    bool adopting = _adopt_fd >= 0;
    log_info("unwaked " + _version + (adopting ? " taking over " + g_cfg.mountpoint + " from the running Unwake" : " starting on " + g_cfg.mountpoint) +
             " (pid " + std::to_string(getpid()) + ")");

    std::string opts = "-oallow_other,default_permissions,fsname=unwake,subtype=unwake,noatime";
    const char* argv[] = {"unwaked", opts.c_str(), nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(2, (char**)argv);
    TopMount mine;
    auto se = adopting ? adopt(&args, mine, started) : mount_new(&args, mine);
    if (!se) return adopting ? 4 : 1;
    {   // mount id (the supervisor detaches only this one) and device (diag finds who still uses it)
        // never stat() our own mount here: the request loop isn't running yet, it would hang
        unsigned long dev = (unsigned long)makedev(mine.major, mine.minor);
        write_file(g_cfg.run_dir + "/mount." + std::to_string(getpid()), std::to_string(mine.id) + " " + std::to_string(dev) + "\n");
    }
    write_status("running", started);
    write_file(g_cfg.run_dir + "/ready", std::to_string(getpid()) + "\n");
    if (g_caching) harvester_start();

    // housekeeping: spin state, status file, signals, array stop, upgrade requests
    std::atomic<bool> loop_done{false};
    std::thread house([&] {
        std::string state = "running";
        int64_t detached_at = 0;
        bool aborted = false;
        int tick = 0;
        while (!loop_done) {
            usleep(250 * 1000);
            tick++;
            if (!detached_at && !_sig_stop) {
                int ho = ctl_poll_upgrade(_ctl_fd);
                if (ho >= 0) {
                    log_info("Upgrade: a new Unwake is taking over");
                    HoMount m;
                    m.id = mine.id; m.major = mine.major; m.minor = mine.minor; m.started = started;
                    bool done = handover_send(ho, se, m);
                    close(ho);
                    if (done) { _handed_over = true; return; }
                }
            }
            if (_sig_reload) {
                _sig_reload = 0;
                auto before = *std::atomic_load(&g_cfg.excluded_shares);
                for (auto& w : load_config()) log_warn(w);
                apply_exclusions();
                log_info("Settings reloaded");
                if (before != *std::atomic_load(&g_cfg.excluded_shares) && g_caching) {
                    // a share that comes back missed its changes while excluded: start over
                    log_info("Excluded shares changed - the cache starts over");
                    std::unique_lock<std::shared_mutex> l(g_tree.mu);
                    tree_clear_all();
                    for (size_t i = 0; i < g_branches.size(); i++) harvester_restart((int)i);
                }
            }
            if (g_cfg.return_ram && tick % 4 == 0) trim_ram();
            bool stopping_array = array_stopping();
            if ((_sig_stop || stopping_array) && !detached_at) {
                auto t = top_mount(g_cfg.mountpoint);
                if (t.id == mine.id) {
                    if (umount2(g_cfg.mountpoint.c_str(), MNT_DETACH) != 0)
                        log_err("Detaching " + g_cfg.mountpoint + ": " + strerror(errno));
                    else log_info("Detached from " + g_cfg.mountpoint + " (pid " + std::to_string(getpid()) + ", " +
                                  (stopping_array ? "array stops" : _sig_array ? "array stop" : "stop") +
                                  ") - new access goes to shfs, waiting for open files to close");
                } else {
                    log_info("Not on top of " + g_cfg.mountpoint + " any more (mount " + std::to_string(t.id) + " is)");
                }
                detached_at = now_ms();
                state = "draining";
                g_caching = false;
                harvester_stop();
                events_stop();
                {
                    std::unique_lock<std::shared_mutex> l(g_tree.mu);
                    tree_clear_all();
                }
            }
            if (detached_at && !aborted && (_sig_array || stopping_array) && now_ms() - detached_at > 10000) {
                // array stop: Unraid must be able to unmount shfs; cut the stragglers off
                log_warn("Still in use 10s after detaching - closing the connection for the array stop");
                char p[128];
                snprintf(p, sizeof p, "/sys/fs/fuse/connections/%u/abort", mine.minor);
                int fd = open(p, O_WRONLY);
                if (fd >= 0) { if (write(fd, "1", 1) < 0) {} close(fd); }
                fuse_session_exit(se);
                aborted = true;
            }
            if (!detached_at && tick % 8 == 0) { spin_poll(); if (g_caching) recompute_exclusions(); }
            // a detached unwaked stays quiet: a new one may run already and own the status files
            if (!detached_at && tick % 8 == 0) write_status(state, started);
        }
    });

    int r = loop_run(se, 64, 8);
    loop_done = true;
    house.join();
    if (_handed_over) {
        // the new unwaked has the connection, the change events and the status files
        harvester_stop();
        events_stop();      // closes our copy of the group; the marks live on in the new one
        fuse_session_destroy(se);
        close(g_shfs_fd);
        unlink((g_cfg.run_dir + "/mount." + std::to_string(getpid())).c_str());
        log_info("unwaked " + _version + " handed over and stopped (pid " + std::to_string(getpid()) + ")");
        return EXIT_HANDED_OVER;
    }
    if (g_caching) { harvester_stop(); events_stop(); }
    fuse_session_destroy(se);       // closes /dev/fuse; never fuse_session_unmount()
    close(g_shfs_fd);
    log_info("unwaked stopped (pid " + std::to_string(getpid()) + ")" + std::string(r ? " (" + std::string(strerror(-r)) + ")" : ""));
    unlink((g_cfg.run_dir + "/mount." + std::to_string(getpid())).c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Supervisor
//
// In-place upgrade (SIGUSR2, after the plugin installed a new binary): it starts
// the new binary as a second worker with a socket to the old one and asks the
// old one, over its control socket, to hand over. When the old worker exits with
// EXIT_HANDED_OVER, the new one is the worker; the supervisor then replaces
// itself with the new binary too (same pid, the worker stays its child).
// ---------------------------------------------------------------------------
static pid_t _child = 0, _pending = 0;    // the worker; a new one taking over
static int   _child_ctl = -1, _pending_ctl = -1;
static volatile sig_atomic_t _sig_upgrade = 0;
static std::string _exe;                  // our binary's path (the new one after an update)
static std::vector<std::string> _args;    // the options we were started with

static void forward(int sig) {
    if (_child > 0) kill(_child, sig);
    if (_pending > 0 && sig != SIGHUP) kill(_pending, sig);   // it has no handler yet
    if (sig == SIGTERM || sig == SIGUSR1 || sig == SIGINT) _sig_stop = 1;
}
static void on_usr2(int) { _sig_upgrade = 1; }

static void exec_self(std::vector<std::string> extra) {
    std::vector<std::string> a{_exe};
    a.insert(a.end(), _args.begin(), _args.end());
    a.insert(a.end(), extra.begin(), extra.end());
    std::vector<char*> av;
    for (auto& x : a) av.push_back((char*)x.c_str());
    av.push_back(nullptr);
    execv(_exe.c_str(), av.data());
}

static void start_upgrade() {
    if (_child <= 0 || _pending > 0 || _child_ctl < 0) return;
    if (access(_exe.c_str(), X_OK) != 0) { log_err("Upgrade: " + _exe + " is missing"); return; }
    int ho[2], ctl[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ho) != 0) { log_err(std::string("socketpair: ") + strerror(errno)); return; }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctl) != 0) { close(ho[0]); close(ho[1]); return; }
    pid_t p = fork();
    if (p == 0) {
        for (int s : {SIGTERM, SIGINT, SIGUSR1, SIGHUP, SIGUSR2}) signal(s, SIG_DFL);
        fcntl(ho[1], F_SETFD, 0);
        fcntl(ctl[1], F_SETFD, 0);
        exec_self({"--no-supervisor", "--adopt-fd", std::to_string(ho[1]), "--ctl-fd", std::to_string(ctl[1])});
        _exit(127);
    }
    close(ho[1]);
    close(ctl[1]);
    if (p < 0) { close(ho[0]); close(ctl[0]); log_err(std::string("fork: ") + strerror(errno)); return; }
    if (!ctl_send_upgrade(_child_ctl, ho[0])) log_err("Upgrade: the running worker didn't take the request");
    close(ho[0]);
    _pending = p;
    _pending_ctl = ctl[0];
    log_info("Upgrade: started " + _exe + " (pid " + std::to_string(p) + ") to take over from pid " + std::to_string(_child));
}

// after a hand-over: become the new binary, keeping the worker as our child
static void reexec_supervisor() {
    sigset_t all, old;
    sigemptyset(&all);
    for (int s : {SIGTERM, SIGINT, SIGUSR1, SIGHUP, SIGUSR2}) sigaddset(&all, s);
    sigprocmask(SIG_BLOCK, &all, &old);     // held until the new one has its handlers
    fcntl(_child_ctl, F_SETFD, 0);
    exec_self({"--supervise", std::to_string(_child), "--ctl-fd", std::to_string(_child_ctl)});
    fcntl(_child_ctl, F_SETFD, FD_CLOEXEC);
    sigprocmask(SIG_SETMASK, &old, nullptr);
    log_err("Upgrade: the supervisor couldn't switch to " + _exe + " (" + strerror(errno) + ") - it carries on as it is");
}

static int supervisor(pid_t adopt_child) {
    struct sigaction sa = {};
    sa.sa_handler = forward;
    for (int s : {SIGTERM, SIGINT, SIGUSR1, SIGHUP}) sigaction(s, &sa, nullptr);
    sa.sa_handler = on_usr2;
    sigaction(SIGUSR2, &sa, nullptr);
    if (adopt_child > 0) {      // re-exec after an upgrade: the worker runs already
        _child = adopt_child;
        _child_ctl = _ctl_fd;
        fcntl(_child_ctl, F_SETFD, FD_CLOEXEC);
        log_info("Upgrade: supervisor runs " + _version + " now");
    }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    std::vector<time_t> restarts;
    for (;;) {
        if (_child <= 0) {
            int ctl[2];
            if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctl) != 0) ctl[0] = ctl[1] = -1;
            _child = fork();
            if (_child < 0) { log_err(std::string("fork: ") + strerror(errno)); return 1; }
            if (_child == 0) {
                signal(SIGTERM, SIG_DFL); signal(SIGUSR1, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGINT, SIG_DFL);
                if (ctl[0] >= 0) close(ctl[0]);
                _ctl_fd = ctl[1];
                _exit(worker());
            }
            if (ctl[1] >= 0) close(ctl[1]);
            _child_ctl = ctl[0];
        }
        int st = 0;
        pid_t got;
        for (;;) {
            if (_sig_upgrade) { _sig_upgrade = 0; start_upgrade(); }
            got = waitpid(-1, &st, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got > 0 && got == _pending) {
                log_err("Upgrade: the new Unwake (pid " + std::to_string(got) + ") exited without taking over" +
                        (WIFSIGNALED(st) ? std::string(" (signal ") + strsignal(WTERMSIG(st)) + ")" : " (exit code " + std::to_string(WEXITSTATUS(st)) + ")"));
                _pending = 0;
                close(_pending_ctl);
                _pending_ctl = -1;
                continue;
            }
            if (got > 0 && got == _child && WIFEXITED(st) && WEXITSTATUS(st) == EXIT_HANDED_OVER && _pending > 0) {
                log_info("Upgrade: pid " + std::to_string(_pending) + " is the worker now");
                _child = _pending;
                close(_child_ctl);
                _child_ctl = _pending_ctl;
                _pending = 0;
                _pending_ctl = -1;
                if (!_sig_stop) reexec_supervisor();
                continue;
            }
            if (got > 0 && got != _child) continue;     // not ours (shouldn't happen)
            break;
        }
        if (got < 0) { log_err(std::string("waitpid: ") + strerror(errno)); return 1; }
        pid_t child = _child;
        _child = 0;
        if (_child_ctl >= 0) { close(_child_ctl); _child_ctl = -1; }
        if (_pending > 0) { kill(_pending, SIGKILL); }   // its old worker is gone: nothing to take over
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 1) return 1;     // didn't start: nothing mounted
        // crashed: the dead mount answers ENOTCONN to everyone - take it away now
        std::string why = WIFSIGNALED(st) ? std::string("signal ") + strsignal(WTERMSIG(st))
                                          : "exit code " + std::to_string(WEXITSTATUS(st));
        // only its own mount: after a restart, a newer Unwake may be on top
        auto idfile = g_cfg.run_dir + "/mount." + std::to_string(child);
        int mine = -1;
        { std::ifstream f(idfile); f >> mine; }
        unlink(idfile.c_str());
        auto t = top_mount(g_cfg.mountpoint);
        if (mine >= 0 && t.id == mine) {
            if (umount2(g_cfg.mountpoint.c_str(), MNT_DETACH) == 0)
                log_err("unwaked died (" + why + ") - detached its mount, " + g_cfg.mountpoint + " is plain shfs again");
            else
                log_err("unwaked died (" + why + ") and its mount could not be detached: " + strerror(errno));
        } else log_err("unwaked died (" + why + ")");
        {   // its ready file, unless a newer Unwake owns it already
            std::ifstream f(g_cfg.run_dir + "/ready");
            pid_t r = 0;
            if (f >> r && r == child) unlink((g_cfg.run_dir + "/ready").c_str());
        }
        auto now = time(nullptr);
        restarts.erase(std::remove_if(restarts.begin(), restarts.end(), [&](time_t x) { return now - x > 3600; }), restarts.end());
        if (_sig_stop || array_stopping() || restarts.size() >= 3) {
            notify("unwake stopped", "unwaked died (" + why + "). User shares work without it. Containers or VMs "
                   "started while it ran may need a restart. See /var/log/unwake.log.", "alert");
            return 2;
        }
        notify("unwake restarted", "unwaked died (" + why + ") and was restarted. Containers or VMs started "
               "while it ran may need a restart. See /var/log/unwake.log.", "warning");
        restarts.push_back(now);
        sleep(2);
    }
}

static void usage() {
    fprintf(stderr,
        "unwaked %s - directory cache for Unraid user shares\n"
        "usage: unwaked [options]\n"
        "  --mountpoint PATH   user share mount (default /mnt/user)\n"
        "  --config FILE       settings (default /boot/config/plugins/unwake/unwake.cfg)\n"
        "  --log FILE          log file (default /var/log/unwake.log)\n"
        "  --run-dir DIR       status and ready files (default /var/run/unwake)\n"
        "  --disks-ini FILE    emhttpd disks.ini\n"
        "  --var-ini FILE      emhttpd var.ini\n"
        "  --branch NAME=PATH[,ssd]   use these branches instead of disks.ini (tests)\n"
        "  --no-supervisor     run the worker only\n"
        "  --handover-formats  print the state formats this version can take over from\n"
        "In-place upgrade (internal): --adopt-fd FD, --ctl-fd FD, --supervise PID\n", _version.c_str());
}

static std::string self_exe() {
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return "/usr/local/sbin/unwaked";
    std::string p(b, (size_t)n);
    const std::string del = " (deleted)";
    if (p.size() > del.size() && p.compare(p.size() - del.size(), del.size(), del) == 0) p.resize(p.size() - del.size());
    return p;
}

int main(int argc, char** argv) {
    bool supervise = true;
    pid_t supervise_child = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) { usage(); exit(64); } return argv[++i]; };
        auto keep = [&](const std::string& v) { _args.push_back(a); _args.push_back(v); return v; };
        if (a == "--mountpoint") g_cfg.mountpoint = keep(val());
        else if (a == "--config") g_cfg.cfg_file = keep(val());
        else if (a == "--log") g_cfg.log_file = keep(val());
        else if (a == "--run-dir") g_cfg.run_dir = keep(val());
        else if (a == "--disks-ini") g_cfg.disks_ini = keep(val());
        else if (a == "--var-ini") g_cfg.var_ini = keep(val());
        else if (a == "--branch") g_cfg.branch_args.push_back(keep(val()));
        else if (a == "--shares-dir") g_cfg.shares_dir = keep(val());
        else if (a == "--no-supervisor") supervise = false;
        else if (a == "--adopt-fd") _adopt_fd = std::stoi(val());
        else if (a == "--ctl-fd") _ctl_fd = std::stoi(val());
        else if (a == "--supervise") supervise_child = std::stoi(val());
        else if (a == "--version") { printf("%s\n", _version.c_str()); return 0; }
        else if (a == "--handover-formats") { printf("%u %u\n", HANDOVER_FORMAT_MIN, HANDOVER_FORMAT); return 0; }
        else if (a == "--bench-handover") return handover_bench(std::stoull(val()));
        else { usage(); return 64; }
    }
    // descriptors inherited from whoever started us (a file on an old Unwake mount
    // would keep that one alive for as long as we run) - except the upgrade sockets
    {
        int lo = 3;
        std::vector<int> keepfds;
        for (int f : {_adopt_fd, _ctl_fd}) if (f >= 3) keepfds.push_back(f);
        std::sort(keepfds.begin(), keepfds.end());
        for (int f : keepfds) {
            if (f > lo) syscall(SYS_close_range, (unsigned)lo, (unsigned)f - 1, 0U);
            lo = f + 1;
        }
        syscall(SYS_close_range, (unsigned)lo, ~0U, 0U);
    }
    _exe = self_exe();
    mkdir(g_cfg.run_dir.c_str(), 0755);
    log_open(g_cfg.log_file);
    if (supervise_child > 0) return supervisor(supervise_child);
    return supervise ? supervisor(0) : worker();
}
