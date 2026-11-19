// util.cpp - logging, config and small helpers
#include "unwaked.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

Config            g_cfg;
std::atomic<bool> g_debug{false};

// ---------------------------------------------------------------------------
// Logging: "2026-11-17 13:05:12 [INFO ] message", like Unspin. Trimmed to
// LOG_TRIM_SIZE_MB once it grows past LOG_MAX_SIZE_MB (checked once a minute).
// ---------------------------------------------------------------------------
static std::mutex  _log_mu;
static std::string _log_file;
static FILE*       _log = nullptr;
static time_t      _log_checked = 0;

void log_open(const std::string& file) {
    std::lock_guard<std::mutex> l(_log_mu);
    if (_log) fclose(_log);
    _log_file = file;
    _log = fopen(file.c_str(), "a");
}

static void log_trim_locked() {
    struct stat st;
    if (_log_file.empty() || stat(_log_file.c_str(), &st) != 0) return;
    if (st.st_size <= g_cfg.log_max_mb * 1024 * 1024) return;
    long keep = g_cfg.log_trim_mb * 1024 * 1024;
    std::ifstream in(_log_file, std::ios::binary);
    if (!in) return;
    in.seekg(st.st_size - keep);
    std::string tail((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto nl = tail.find('\n');
    if (nl != std::string::npos) tail.erase(0, nl + 1);
    auto tmp = _log_file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << tail;
    }
    if (rename(tmp.c_str(), _log_file.c_str()) == 0 && _log) {
        fclose(_log);
        _log = fopen(_log_file.c_str(), "a");
    }
}

static void vlog(const char* level, const std::string& m) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    std::lock_guard<std::mutex> l(_log_mu);
    FILE* f = _log ? _log : stderr;
    fprintf(f, "%s [%s] %s\n", ts, level, m.c_str());
    fflush(f);
    if (now - _log_checked >= 60) {
        _log_checked = now;
        log_trim_locked();
    }
}

void log_info (const std::string& m) { vlog("INFO ", m); }
void log_warn (const std::string& m) { vlog("WARN ", m); }
void log_err  (const std::string& m) { vlog("ERROR", m); }
void log_debug(const std::string& m) { if (g_debug) vlog("DEBUG", m); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, sep)) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::string join_path(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
}

std::string human_count(uint64_t n) {
    char b[32];
    if (n >= 10000000) snprintf(b, sizeof b, "%.0fM", n / 1e6);
    else if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(b, sizeof b, "%.0fk", n / 1e3);
    else snprintf(b, sizeof b, "%llu", (unsigned long long)n);
    return b;
}

std::string fmt_secs(long s) {
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%ldh %ldm", s / 3600, s % 3600 / 60);
    else if (s >= 60) snprintf(b, sizeof b, "%ldm %lds", s / 60, s % 60);
    else snprintf(b, sizeof b, "%lds", s);
    return b;
}

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// Config: unwake.cfg, KEY="value" lines as written by the settings page.
// Unknown or invalid values keep the previous value and are reported.
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, std::string> read_kv(const std::string& file) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        kv[trim(line.substr(0, eq))] = v;
    }
    return kv;
}

std::vector<std::string> load_config() {
    std::vector<std::string> warn;
    auto kv = read_kv(g_cfg.cfg_file);
    auto yesno = [&](const char* key, std::atomic<bool>& out) {
        auto it = kv.find(key);
        if (it == kv.end()) return;
        if (it->second == "yes") out = true;
        else if (it->second == "no") out = false;
        else warn.push_back(std::string(key) + "=\"" + it->second + "\" is not yes or no - kept " + (out ? "yes" : "no"));
    };
    auto number = [&](const char* key, long& out, long min) {
        auto it = kv.find(key);
        if (it == kv.end()) return;
        char* end = nullptr;
        long v = strtol(it->second.c_str(), &end, 10);
        if (it->second.empty() || *end || v < min)
            warn.push_back(std::string(key) + "=\"" + it->second + "\" is not a valid number - kept " + std::to_string(out));
        else out = v;
    };
    if (kv.count("LOG_LEVEL")) g_cfg.debug = kv["LOG_LEVEL"] == "debug";
    yesno("HARVEST", g_cfg.harvest);
    yesno("RETURN_RAM", g_cfg.return_ram);
    // not on the settings page (betas verify anyway): a missing key means the default
    g_cfg.verify = false;
    yesno("VERIFY", g_cfg.verify);
    {
        long r = 4;
        number("VERIFY_RATE", r, 1);
        g_cfg.verify_rate = r;
        long iv = 43200;
        number("VERIFY_INTERVAL", iv, 0);
        g_cfg.verify_interval = iv;
    }
    if (kv.count("DATA_PATH")) {
        if (kv["DATA_PATH"] == "direct") g_cfg.direct_reads = true;
        else if (kv["DATA_PATH"] == "shfs") g_cfg.direct_reads = false;
        else warn.push_back("DATA_PATH=\"" + kv["DATA_PATH"] + "\" is not direct or shfs - kept " +
                            (g_cfg.direct_reads ? "direct" : "shfs"));
    }
    g_cfg.excluded_explicit = split(kv["EXCLUDED_SHARES"], ',');
    g_cfg.included_explicit = split(kv["INCLUDED_SHARES"], ',');
    number("LOG_MAX_SIZE_MB", g_cfg.log_max_mb, 1);
    number("LOG_TRIM_SIZE_MB", g_cfg.log_trim_mb, 1);
    if (g_cfg.log_trim_mb >= g_cfg.log_max_mb) g_cfg.log_trim_mb = g_cfg.log_max_mb / 2 ? g_cfg.log_max_mb / 2 : 1;
    g_debug = g_cfg.debug;
    return warn;
}
