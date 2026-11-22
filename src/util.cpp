// util.cpp - logging, config and small helpers
#include "unwaked.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    if (cut_to_tail(_log_file, g_cfg.log_trim_mb * 1024 * 1024) && _log) {
        fclose(_log);
        _log = fopen(_log_file.c_str(), "a");
    }
}

static void vlog(const char* level, const std::string& m) {
    time_t now = time(nullptr);
    std::string ts = fmt_time(now);
    std::lock_guard<std::mutex> l(_log_mu);
    FILE* f = _log ? _log : stderr;
    fprintf(f, "%s [%s] %s\n", ts.c_str(), level, m.c_str());
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

std::string path_dir(const std::string& rel) {
    auto s = rel.rfind('/');
    return s == std::string::npos ? "" : rel.substr(0, s);
}

std::string path_base(const std::string& rel) {
    auto s = rel.rfind('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}

std::string human_count(uint64_t n) {
    char b[32];
    if (n >= 10000000) snprintf(b, sizeof b, "%.0fM", n / 1e6);
    else if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 10000) snprintf(b, sizeof b, "%.0fk", n / 1e3);
    else snprintf(b, sizeof b, "%llu", (unsigned long long)n);
    return b;
}

// "1h 5m", "2h", "3m 10s", "2m", "45s"
std::string fmt_secs(long s) {
    auto part = [](long n, const char* unit) { return std::to_string(n) + unit; };
    if (s >= 3600) return part(s / 3600, "h") + (s % 3600 >= 60 ? " " + part(s % 3600 / 60, "m") : "");
    if (s >= 60) return part(s / 60, "m") + (s % 60 ? " " + part(s % 60, "s") : "");
    return part(s, "s");
}

std::string fmt_time(time_t t, const char* fmt) {
    struct tm tm;
    localtime_r(&t, &tm);
    char b[64];
    strftime(b, sizeof b, fmt, &tm);
    return b;
}

static std::string unquote(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

std::unordered_map<std::string, IniSection> read_ini(const std::string& file) {
    std::unordered_map<std::string, IniSection> out;
    std::ifstream f(file);
    std::string line, sec;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { sec = unquote(line.substr(1, line.size() - 2)); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[sec][trim(line.substr(0, eq))] = unquote(trim(line.substr(eq + 1)));
    }
    return out;
}

std::vector<MountInfo> mountinfo() {
    std::vector<MountInfo> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, parent, devno, root, mp, tok;
        ss >> id >> parent >> devno >> root >> mp;
        MountInfo m;
        m.id = atoi(id.c_str());
        sscanf(devno.c_str(), "%u:%u", &m.major, &m.minor);
        for (size_t i = 0; i < mp.size(); i++) {   // blanks and the like come as \040
            if (mp[i] == '\\' && i + 3 < mp.size()) { m.mountpoint += (char)strtol(mp.substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
            else m.mountpoint += mp[i];
        }
        while (ss >> tok && tok != "-") {}
        ss >> m.fstype >> m.source;
        out.push_back(std::move(m));
    }
    return out;
}

long proc_status(const std::string& file, const char* key) {
    std::ifstream f(file);
    std::string line;
    size_t n = strlen(key);
    while (std::getline(f, line))
        if (!line.compare(0, n, key)) return atol(line.c_str() + n);
    return 0;
}

bool cut_to_tail(const std::string& file, off_t keep, const std::string& marker) {
    struct stat st;
    if (stat(file.c_str(), &st) != 0 || st.st_size <= keep) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(st.st_size - keep);
    std::string tail((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto nl = tail.find('\n');
    if (nl != std::string::npos) tail.erase(0, nl + 1);
    auto tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << marker << tail;
    }
    return rename(tmp.c_str(), file.c_str()) == 0;
}

void background_priority() {
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)syscall(SYS_gettid), (3 << 13) /* IOPRIO_CLASS_IDLE */);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
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
static IniSection read_kv(const std::string& file) { return read_ini(file)[""]; }

namespace {
using KV = IniSection;

// One setting: the saved value if it is valid, else the default. parse: value -> optional result
struct Settings {
    const KV& saved;
    const KV& defs;
    const std::string& defaults_file;
    std::vector<std::string>& warn;
    KV effective = {};   // the value of each setting in effect, as it would be saved
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
    bool yesno(const char* key) { return either(key, "yes", "no"); }
    // true for the first of the two values, false for the second
    bool either(const char* key, const char* first, const char* second) {
        std::string valid = std::string(first) + " or " + second;
        bool v = get(key, valid.c_str(), [&](const std::string& v) {
            return v == first ? std::optional<bool>(true) : v == second ? std::optional<bool>(false) : std::nullopt;
        });
        effective[key] = v ? first : second;
        return v;
    }
    long number(const char* key, long min) {
        std::string valid = "a number of at least " + std::to_string(min);
        long v = get(key, valid.c_str(), [min](const std::string& v) {
            char* end = nullptr;
            long n = strtol(v.c_str(), &end, 10);
            return !v.empty() && !*end && n >= min ? std::optional<long>(n) : std::nullopt;
        });
        effective[key] = std::to_string(v);
        return v;
    }
    std::vector<std::string> list(const char* key) {
        auto v = get(key, "", [](const std::string& v) { return std::optional<std::vector<std::string>>(split(v, ',')); });
        std::string joined;
        for (auto& x : v) joined += (joined.empty() ? "" : ",") + x;
        effective[key] = joined;
        return v;
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
    s.either("SERVICE", "enabled", "disabled");   // rc.unwake starts on it; checked here like the rest
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
    if (log_trim >= log_max) {   // the trim size must be smaller, or every new line cuts the log again
        log_trim = std::max(1L, log_max / 2);
        s.effective["LOG_TRIM_SIZE_MB"] = std::to_string(log_trim);
    }
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
    g_cfg.in_effect = s.effective;
    g_debug = debug;
    return true;
}
