// handover.h - state of a running unwaked, written by the old worker and read by
// the new one during an in-place upgrade.
//
// Plain little-endian fields, written one by one (never a struct's raw bytes, so a
// changed struct layout can't silently break it). File descriptors are not in the
// state: they travel next to it over the socket, and the state refers to them by
// their index in that list.
#pragma once
#include "unwaked.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Bump on any change to what is written. A new unwaked reads every format from
// HANDOVER_FORMAT_MIN up to HANDOVER_FORMAT; an older one is never asked to.
constexpr uint32_t HANDOVER_FORMAT     = 1;
constexpr uint32_t HANDOVER_FORMAT_MIN = 1;

struct HoWriter {
    std::string buf;
    std::vector<int> fds;                       // index -> fd of this process
    std::unordered_map<int, uint32_t> fd_index;

    void u8(uint8_t v)   { buf.push_back((char)v); }
    void u32(uint32_t v) { buf.append((const char*)&v, 4); }
    void u64(uint64_t v) { buf.append((const char*)&v, 8); }
    void i64(int64_t v)  { u64((uint64_t)v); }
    void str(const std::string& s) { u32((uint32_t)s.size()); buf.append(s); }
    void bytes(const void* p, size_t n) { u32((uint32_t)n); buf.append((const char*)p, n); }
    // an fd: its index in the list sent along (the same fd twice gets one index)
    void fd(int f) {
        if (f < 0) { u32(UINT32_MAX); return; }
        auto it = fd_index.find(f);
        if (it == fd_index.end()) {
            it = fd_index.emplace(f, (uint32_t)fds.size()).first;
            fds.push_back(f);
        }
        u32(it->second);
    }
};

struct HoReader {
    const char* p;
    const char* end;
    std::vector<int> fds;      // index -> fd received in this process
    uint32_t format = 0;

    void need(size_t n) { if ((size_t)(end - p) < n) throw std::runtime_error("state cut short"); }
    uint8_t  u8()  { need(1); return (uint8_t)*p++; }
    uint32_t u32() { need(4); uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
    uint64_t u64() { need(8); uint64_t v; memcpy(&v, p, 8); p += 8; return v; }
    int64_t  i64() { return (int64_t)u64(); }
    std::string str() { uint32_t n = u32(); need(n); std::string s(p, n); p += n; return s; }
    void bytes(void* out, size_t n) {
        if (u32() != n) throw std::runtime_error("field size differs");
        need(n); memcpy(out, p, n); p += n;
    }
    int fd() {
        uint32_t i = u32();
        if (i == UINT32_MAX) return -1;
        if (i >= fds.size()) throw std::runtime_error("fd index out of range");
        return fds[i];
    }
};

// each part writes and reads its own state
void fs_handover_out(HoWriter& w);         // nodes, open files and folders, stats, recent shfs requests
void fs_handover_in(HoReader& r);
void tree_handover_out(HoWriter& w);       // the union tree (lock held)
void tree_handover_in(HoReader& r);        // (lock held, tree empty)
void branches_handover_out(HoWriter& w);   // harvest state, exclusions
void branches_handover_in(HoReader& r);
void events_handover_out(HoWriter& w);     // the fanotify group and its marks
void events_handover_in(HoReader& r);
void events_pause();                       // reader thread stopped, pending re-stats done; the group stays
void events_resume();                      // reader thread runs again (also after events_handover_in)

inline void attr_out(HoWriter& w, const Attr& a) {
    w.u32(a.mode); w.u32(a.uid); w.u32(a.gid); w.u32(a.nlink);
    w.u64(a.size); w.u64(a.blocks);
    w.i64(a.atime); w.i64(a.mtime); w.i64(a.ctime);
    w.u32(a.atime_ns); w.u32(a.mtime_ns); w.u32(a.ctime_ns);
    w.u32(a.rdev); w.u32(a.blksize);
}
inline Attr attr_in(HoReader& r) {
    Attr a;
    a.mode = r.u32(); a.uid = r.u32(); a.gid = r.u32(); a.nlink = r.u32();
    a.size = r.u64(); a.blocks = r.u64();
    a.atime = r.i64(); a.mtime = r.i64(); a.ctime = r.i64();
    a.atime_ns = r.u32(); a.mtime_ns = r.u32(); a.ctime_ns = r.u32();
    a.rdev = r.u32(); a.blksize = r.u32();
    return a;
}

// the mount the connection belongs to, and when Unwake started serving it
struct HoMount { int id = -1; unsigned major = 0, minor = 0; int64_t started = 0; };

size_t fs_open_handles();
// old worker: hand over on the socket. true: done, the loop ends and the worker exits
// (EXIT_HANDED_OVER); false: nothing changed, it serves on.
bool handover_send(int sock, struct fuse_session* se, const HoMount& m);
// new worker: take over. true: the session, node table, tree and change events are
// in place and the old worker has let go; start serving.
bool handover_receive(int sock, struct fuse_session* se, HoMount& m, const std::string& test_mode);
constexpr int EXIT_HANDED_OVER = 3;
int handover_bench(uint64_t names);   // --bench-handover N
bool ctl_send_upgrade(int ctl, int sock);   // supervisor: ask the worker to hand over on sock
int  ctl_poll_upgrade(int ctl);             // worker: the socket of an upgrade request, or -1
