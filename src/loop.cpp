// loop.cpp - the FUSE request loop, with a pause that never loses a request
//
// libfuse's fuse_session_loop_mt can't be paused: a thread that has just read a
// request when the session exits drops it (its caller would hang for good). An
// in-place upgrade has to stop taking requests, let the ones in progress finish
// and hand the connection over, so the loop is ours: worker threads that read and
// process one request at a time, more of them when all are busy (up to max),
// fewer when many idle. To pause, every thread parks before its next read; one
// blocked in read() is woken with a signal (EINTR, nothing read, nothing lost).
#include "unwaked.h"

#include <condition_variable>
#include <csignal>
#include <cstring>
#include <set>
#include <thread>

static struct fuse_session* _se = nullptr;
static std::mutex _m;
static std::condition_variable _cv;
static std::set<pthread_t> _reading;   // threads in (or about to enter) read(): only these get the signal,
                                       // never one processing a request (its shfs calls would see EINTR)
static int  _threads = 0, _avail = 0, _parked = 0, _max = 64, _max_idle = 8;
static bool _pause = false, _done = false;
static int  _err = 0;

static const int SIG_WAKE = SIGRTMIN + 1;
static void on_wake(int) {}

static void spawn_locked();

static void worker() {
    struct fuse_buf fbuf = {};
    std::unique_lock<std::mutex> l(_m);
    for (;;) {
        while (_pause && !_done) {
            _parked++;
            _cv.notify_all();
            _cv.wait(l);
            _parked--;
        }
        if (_done) break;
        _avail++;
        _reading.insert(pthread_self());
        l.unlock();
        int r = fuse_session_receive_buf(_se, &fbuf);
        l.lock();
        _reading.erase(pthread_self());
        _avail--;
        if (r == -EINTR || r == -EAGAIN) continue;
        if (r <= 0) {   // unmounted, aborted or exited: everyone stops
            if (r < 0 && r != -ENODEV) _err = r;
            _done = true;
            fuse_session_exit(_se);
            for (auto t : _reading) pthread_kill(t, SIG_WAKE);
            _cv.notify_all();
            break;
        }
        if (_avail == 0 && _threads < _max) spawn_locked();
        l.unlock();
        fuse_session_process_buf(_se, &fbuf);
        l.lock();
        if (_avail >= _max_idle && _threads > 1) break;
    }
    _threads--;
    _cv.notify_all();
    l.unlock();
    free(fbuf.mem);
}

static void spawn_locked() {
    _threads++;
    std::thread(worker).detach();
}

int loop_run(struct fuse_session* se, int max_threads, int max_idle) {
    struct sigaction sa = {};
    sa.sa_handler = on_wake;     // no SA_RESTART: a blocked read() returns EINTR
    sigaction(SIG_WAKE, &sa, nullptr);
    std::unique_lock<std::mutex> l(_m);
    _se = se;
    _max = max_threads;
    _max_idle = max_idle;
    _done = false;
    _err = 0;
    spawn_locked();
    _cv.wait(l, [] { return _done && _threads == 0; });
    _se = nullptr;
    return _err;
}

bool loop_pause(int timeout_ms) {
    auto deadline = now_ms() + timeout_ms;
    std::unique_lock<std::mutex> l(_m);
    _pause = true;
    for (;;) {
        if (_done) return false;
        if (_parked == _threads) return true;
        // a thread may be just about to enter read(): signal until it's parked.
        // Under the lock, so no thread can leave _reading (and exit) meanwhile.
        for (auto t : _reading) pthread_kill(t, SIG_WAKE);
        if (now_ms() > deadline) {
            _pause = false;
            _cv.notify_all();
            return false;
        }
        _cv.wait_for(l, std::chrono::milliseconds(5));
    }
}

void loop_resume() {
    std::lock_guard<std::mutex> l(_m);
    _pause = false;
    _cv.notify_all();
}

// for the hand-over: the old unwaked is done with the connection, its threads end
void loop_finish() {
    std::lock_guard<std::mutex> l(_m);
    _done = true;
    _pause = false;
    _cv.notify_all();
}
