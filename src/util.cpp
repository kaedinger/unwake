// util.cpp - logging, config and small helpers
#include "unwaked.h"

#include <algorithm>
#include <fstream>
#include <optional>
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
// Config: KEY="value" lines - unwake.cfg.default (shipped: every setting and its default)
// with unwake.cfg (as the settings page saved it) over it. An invalid saved value is
// reported and the default used.
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

namespace {
using KV = std::unordered_map<std::string, std::string>;

// One setting: the saved value if it is valid, else the default. parse: value -> optional result
struct Settings {
    const KV& saved;
    const KV& defs;
    const std::string& defaults_file;
    std::vector<std::string>& warn;
    bool ok = true;

    template <class Parse>
    auto get(const char* key, const char* valid, Parse parse) -> typename decltype(parse(""))::value_type {
        auto d = defs.find(key);
        auto def = d == defs.end() ? decltype(parse(""))() : parse(d->second);
        if (!def) {
            warn.push_back(std::string(key) + " has no valid default in " + defaults_file);
            ok = false;
            return {};
        }
        auto s = saved.find(key);
        if (s == saved.end()) return *def;
        if (auto v = parse(s->second)) return *v;
        warn.push_back(std::string(key) + "=\"" + s->second + "\" is not " + valid + " - using the default \"" + d->second + "\"");
        return *def;
    }
    bool yesno(const char* key) {
        return get(key, "yes or no", [](const std::string& v) {
            return v == "yes" ? std::optional<bool>(true) : v == "no" ? std::optional<bool>(false) : std::nullopt;
        });
    }
    // true for the first of the two values, false for the second
    bool either(const char* key, const char* first, const char* second) {
        std::string valid = std::string(first) + " or " + second;
        return get(key, valid.c_str(), [&](const std::string& v) {
            return v == first ? std::optional<bool>(true) : v == second ? std::optional<bool>(false) : std::nullopt;
        });
    }
    long number(const char* key, long min) {
        std::string valid = "a number of at least " + std::to_string(min);
        return get(key, valid.c_str(), [min](const std::string& v) {
            char* end = nullptr;
            long n = strtol(v.c_str(), &end, 10);
            return !v.empty() && !*end && n >= min ? std::optional<long>(n) : std::nullopt;
        });
    }
    std::vector<std::string> list(const char* key) {
        return get(key, "", [](const std::string& v) { return std::optional<std::vector<std::string>>(split(v, ',')); });
    }
};
}   // namespace

bool load_config(std::vector<std::string>& warn) {
    KV defs = read_kv(g_cfg.defaults_file);
    if (defs.empty()) {
        warn.push_back(g_cfg.defaults_file + " is missing or empty - reinstall the plugin");
        return false;
    }
    KV saved = read_kv(g_cfg.cfg_file);
    Settings s{saved, defs, g_cfg.defaults_file, warn};
    bool debug       = s.either("LOG_LEVEL", "debug", "info");
    bool harvest     = s.yesno("HARVEST");
    bool return_ram  = s.yesno("RETURN_RAM");
    bool verify      = s.yesno("VERIFY");
    long verify_rate = s.number("VERIFY_RATE", 1);
    long verify_iv   = s.number("VERIFY_INTERVAL", 0);
    bool direct      = s.either("DATA_PATH", "direct", "shfs");
    auto excluded    = s.list("EXCLUDED_SHARES");
    auto included    = s.list("INCLUDED_SHARES");
    long log_max     = s.number("LOG_MAX_SIZE_MB", 1);
    long log_trim    = s.number("LOG_TRIM_SIZE_MB", 1);
    if (!s.ok) return false;
    if (log_trim >= log_max) log_trim = std::max(1L, log_max / 2);   // as the settings page saves it
    g_cfg.debug = debug;
    g_cfg.harvest = harvest;
    g_cfg.return_ram = return_ram;
    g_cfg.verify = verify;
    g_cfg.verify_rate = verify_rate;
    g_cfg.verify_interval = verify_iv;
    g_cfg.direct_reads = direct;
    g_cfg.excluded_explicit = excluded;
    g_cfg.included_explicit = included;
    g_cfg.log_max_mb = log_max;
    g_cfg.log_trim_mb = log_trim;
    g_debug = debug;
    return true;
}
