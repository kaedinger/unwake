// handover.cpp - in-place upgrade: the old worker hands its FUSE connection and
// its state to a new worker of the new version, over a socket the supervisor set
// up between them. The old one decides: it sends "bye" (the new one serves from
// then on, the old one ends) or "abort" (the old one serves on, the new one
// exits). Nothing is lost either way: while neither reads, requests wait in the
// kernel, and so do change events.
//
//   new -> old  "ready <min> <max>"        formats the new one reads
//   old         pauses the request loop, change events and harvester
//   old -> new  "state <nfds> <format>"    + the memfd with the state
//   old -> new  "fds"                      + up to FDS_PER_MSG fds, until all are sent
//   new -> old  "ok" | "fail <why>"
//   old -> new  "bye" | "abort"
#include "handover.h"

#include <dirent.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

static constexpr size_t FDS_PER_MSG = 250;
// how long one side waits for the other
static constexpr int ANSWER_MS = 60000;    // a step: getting ready, taking the state in, confirming
static constexpr int STATE_MS  = 120000;   // the old one finishing its requests and writing the state
static constexpr int FDS_MS    = 30000;    // the next batch of open files
static constexpr int PAUSE_MS  = 15000;    // requests in progress, before the old one gives up and serves on

static bool send_msg(int sock, const std::string& text, const int* fds = nullptr, size_t n = 0) {
    struct iovec iov = {(void*)text.data(), text.size()};
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    std::vector<char> cbuf;
    if (n) {
        cbuf.resize(CMSG_SPACE(sizeof(int) * n));
        mh.msg_control = cbuf.data();
        mh.msg_controllen = cbuf.size();
        auto c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * n);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    }
    ssize_t r;
    do r = sendmsg(sock, &mh, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == (ssize_t)text.size();
}

// false on timeout, error or a closed socket
static bool recv_msg(int sock, std::string& text, std::vector<int>& fds, int timeout_ms) {
    auto deadline = now_ms() + timeout_ms;
    for (;;) {
        struct pollfd p = {sock, POLLIN, 0};
        int left = (int)std::max<int64_t>(0, deadline - now_ms());
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        break;
    }
    char buf[512];
    std::vector<char> cbuf(CMSG_SPACE(sizeof(int) * FDS_PER_MSG));
    struct iovec iov = {buf, sizeof buf};
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf.data();
    mh.msg_controllen = cbuf.size();
    ssize_t n;
    do n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC); while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    text.assign(buf, (size_t)n);
    for (auto c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* p = (const int*)CMSG_DATA(c);
        fds.insert(fds.end(), p, p + k);
    }
    if (mh.msg_flags & MSG_CTRUNC) return false;
    return true;
}

static bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// ---------------------------------------------------------------------------
// Old worker
// ---------------------------------------------------------------------------
bool handover_send(int sock, struct fuse_session* se, const HoMount& m) {
    std::string msg;
    std::vector<int> fds;
    if (!recv_msg(sock, msg, fds, ANSWER_MS) || !starts(msg, "ready ")) {
        log_err("Upgrade: the new Unwake didn't get ready" + (msg.empty() ? std::string() : " (" + msg + ")") + " - this one serves on");
        return false;
    }
    unsigned lo = 0, hi = 0;
    sscanf(msg.c_str() + 6, "%u %u", &lo, &hi);
    if (HANDOVER_FORMAT < lo || HANDOVER_FORMAT > hi) {
        log_err("Upgrade: the new Unwake can't take over from this version (state format " + std::to_string(HANDOVER_FORMAT) +
                ", it reads " + std::to_string(lo) + "-" + std::to_string(hi) + ") - restart Unwake to switch");
        send_msg(sock, "abort");
        return false;
    }

    auto t0 = now_ms();
    harvester_stop();
    events_pause();
    auto resume = [&] {
        loop_resume();
        events_resume();
        if (g_caching) harvester_start();
    };
    if (!loop_pause(PAUSE_MS)) {
        log_err("Upgrade: requests still in progress after " + fmt_secs(PAUSE_MS / 1000) + " - this one serves on, try again later");
        send_msg(sock, "abort");
        resume();
        return false;
    }
    auto t1 = now_ms();

    HoWriter w;
    w.str(UNWAKE_VERSION);
    struct fuse_session_handover fh;
    if (fuse_session_handover_export(se, &fh) != 0) {
        log_err("Upgrade: the FUSE session isn't initialized - this one serves on");
        send_msg(sock, "abort");
        resume();
        return false;
    }
    w.bytes(&fh, sizeof fh);
    w.fd(fuse_session_fd(se));
    w.fd(g_shfs_fd);
    w.u32((uint32_t)m.id); w.u32(m.major); w.u32(m.minor); w.i64(m.started);
    branches_handover_out(w);
    fs_handover_out(w);
    events_handover_out(w);
    {
        std::shared_lock<std::shared_mutex> l(g_tree.mu);
        tree_handover_out(w);
    }
    auto t2 = now_ms();

    int mfd = memfd_create("unwake-state", MFD_CLOEXEC);
    bool ok = mfd >= 0;
    for (size_t off = 0; ok && off < w.buf.size();) {
        ssize_t n = write(mfd, w.buf.data() + off, w.buf.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else off += (size_t)n;
    }
    ok = ok && send_msg(sock, "state " + std::to_string(w.fds.size()) + " " + std::to_string(HANDOVER_FORMAT), &mfd, 1);
    if (mfd >= 0) close(mfd);
    for (size_t i = 0; ok && i < w.fds.size(); i += FDS_PER_MSG)
        ok = send_msg(sock, "fds", w.fds.data() + i, std::min(FDS_PER_MSG, w.fds.size() - i));
    msg.clear();
    fds.clear();
    if (ok) ok = recv_msg(sock, msg, fds, ANSWER_MS);
    for (int f : fds) close(f);
    if (!ok || msg != "ok") {
        log_err("Upgrade: the new Unwake couldn't take over" + (msg.empty() ? std::string(" (no answer)") : " (" + msg + ")") +
                " - this one serves on");
        send_msg(sock, "abort");
        resume();
        return false;
    }
    if (!send_msg(sock, "bye")) {   // it can't know it may start: it will exit on its own
        log_err("Upgrade: lost the new Unwake at the last step - this one serves on");
        resume();
        return false;
    }
    log_info("Upgrade: handed over to the new Unwake - " + std::to_string(w.buf.size() / 1024) + " KB of state, " +
             std::to_string(w.fds.size()) + " fds; paused " + std::to_string(now_ms() - t1) + " ms (waiting for requests " +
             std::to_string(t1 - t0) + " ms, writing the state " + std::to_string(t2 - t1) + " ms)");
    loop_finish();
    return true;
}

// ---------------------------------------------------------------------------
// New worker
// ---------------------------------------------------------------------------
static bool fail(int sock, const std::string& why) {
    log_err("Upgrade: can't take over - " + why);
    send_msg(sock, "fail " + why);
    return false;
}

bool handover_receive(int sock, struct fuse_session* se, HoMount& m, const std::string& test_mode) {
    if (!send_msg(sock, "ready " + std::to_string(HANDOVER_FORMAT_MIN) + " " + std::to_string(HANDOVER_FORMAT))) {
        log_err("Upgrade: no connection to the running Unwake");
        return false;
    }
    std::string msg;
    std::vector<int> fds;
    if (!recv_msg(sock, msg, fds, STATE_MS) || !starts(msg, "state ") || fds.size() != 1) {
        log_err("Upgrade: the running Unwake didn't send its state" + (msg.empty() ? std::string() : " (" + msg + ")"));
        for (int f : fds) close(f);
        return false;
    }
    int mfd = fds[0];
    size_t nfds = 0;
    unsigned format = 0;
    sscanf(msg.c_str() + 6, "%zu %u", &nfds, &format);
    HoReader r;
    r.format = format;
    while (r.fds.size() < nfds) {
        std::vector<int> got;
        if (!recv_msg(sock, msg, got, FDS_MS) || msg != "fds") {
            for (int f : got) close(f);
            close(mfd);
            return fail(sock, "fds cut short");
        }
        r.fds.insert(r.fds.end(), got.begin(), got.end());
    }
    struct stat st;
    void* map = MAP_FAILED;
    if (fstat(mfd, &st) == 0 && st.st_size > 0)
        map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, mfd, 0);
    close(mfd);
    if (map == MAP_FAILED) return fail(sock, "can't read the state");
    r.p = (const char*)map;
    r.end = r.p + st.st_size;
    std::string from;
    try {
        if (format < HANDOVER_FORMAT_MIN || format > HANDOVER_FORMAT) throw std::runtime_error("state format " + std::to_string(format));
        from = r.str();
        struct fuse_session_handover fh;
        r.bytes(&fh, sizeof fh);
        int fuse_fd = r.fd();
        g_shfs_fd = r.fd();
        m.id = (int)r.u32(); m.major = r.u32(); m.minor = r.u32(); m.started = r.i64();
        branches_handover_in(r);
        fs_handover_in(r);
        events_handover_in(r);
        {
            std::unique_lock<std::shared_mutex> l(g_tree.mu);
            tree_handover_in(r);
        }
        if (r.p != r.end) throw std::runtime_error("state longer than expected");
#if UNWAKE_TEST_HOOKS
        if (test_mode == "refuse") throw std::runtime_error("refused (test)");
        if (test_mode == "crash") abort();
#endif
        if (fuse_session_handover_import(se, fuse_fd, &fh) != 0) throw std::runtime_error("libfuse refused the session");
        // the fds the state didn't claim (none, normally)
    } catch (const std::exception& e) {
        munmap(map, (size_t)st.st_size);
        for (int f : r.fds) close(f);
        return fail(sock, e.what());
    }
    munmap(map, (size_t)st.st_size);
    if (!send_msg(sock, "ok")) return false;
    msg.clear();
    fds.clear();
    if (!recv_msg(sock, msg, fds, ANSWER_MS) || msg != "bye") {
        log_err("Upgrade: the running Unwake " + (msg == "abort" ? std::string("serves on") : "didn't confirm") + " - this one exits");
        return false;
    }
    log_info("Upgrade: took over from " + from + " - " + std::to_string(g_tree.dirs) + " folders, " +
             std::to_string(fs_open_handles()) + " open files and folders");
    return true;
}

// ---------------------------------------------------------------------------
// Supervisor <-> worker control socket: "upgrade" + the socket to the new worker
// ---------------------------------------------------------------------------
bool ctl_send_upgrade(int ctl, int sock) { return send_msg(ctl, "upgrade", &sock, 1); }

int ctl_poll_upgrade(int ctl) {
    if (ctl < 0) return -1;
    std::string msg;
    std::vector<int> fds;
    if (!recv_msg(ctl, msg, fds, 0)) return -1;
    if (msg == "upgrade" && fds.size() == 1) return fds[0];
    for (int f : fds) close(f);
    return -1;
}

// ---------------------------------------------------------------------------
// unwaked --bench-handover N: time writing and reading a synthetic tree of
// about N names (in 1/12 as many folders, like a real one) - the bulk of the pause
// ---------------------------------------------------------------------------
int handover_bench(uint64_t names) {
    g_tree.all = 0x3f;
    uint64_t dirs = std::max<uint64_t>(1, names / 12);
    std::vector<DirNode*> ds{&g_tree.root};
    auto xs = std::make_shared<Xattrs>(Xattrs{{"user.DOSATTRIB", std::string(28, 'x')}});
    uint64_t made = 0;
    for (uint64_t i = 0; made < names; i++) {
        DirNode* d = ds[i % ds.size()];
        bool dir = ds.size() < dirs && i % 3 == 0;
        char name[96];
        snprintf(name, sizeof name, "%s %08llu - some typical file name.mkv", dir ? "Folder" : "File", (unsigned long long)i);
        auto it = d->entries.emplace(name, Entry()).first;
        Entry& e = it->second;
        e.mask = 1u << (i % 5);
        e.has_attr = true;
        e.attr.mode = dir ? 040755 : 0100644;
        e.attr.size = i * 4096;
        e.attr.mtime = 1700000000 + (int64_t)i;
        e.is_dir = dir;
        e.dtype = dir ? DT_DIR : DT_REG;
        e.xattrs = xs;
        d->known = 0x3f;
        if (dir) {
            auto c = std::make_unique<DirNode>();
            c->parent = d;
            c->name = &it->first;
            ds.push_back(c.get());
            e.dir = std::move(c);
        }
        made++;
    }
    g_tree.entries = made;
    g_tree.dirs = ds.size();
    auto t0 = now_ms();
    HoWriter w;
    w.buf.reserve(made * 120);
    tree_handover_out(w);
    auto t1 = now_ms();
    HoReader r;
    r.p = w.buf.data();
    r.end = r.p + w.buf.size();
    {
        std::unique_lock<std::shared_mutex> l(g_tree.mu);
        tree_handover_in(r);
    }
    auto t2 = now_ms();
    printf("%llu names, %zu folders: state %zu MB, write %lld ms, read %lld ms\n", (unsigned long long)g_tree.entries.load(),
           ds.size(), w.buf.size() >> 20, (long long)(t1 - t0), (long long)(t2 - t1));
    return g_tree.entries == made ? 0 : 1;
}
