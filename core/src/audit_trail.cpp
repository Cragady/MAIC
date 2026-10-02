#include "maic/audit_trail.hpp"

#include "maic/lua.hpp"
#include "maic/paths.hpp"
#include "maic/settings.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utc(std::time_t t, const char* format) {
    char buf[64];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

// "2026-10-01T12:00:00Z", as maic-leak-audit writes index.json; 0 for anything else.
std::time_t parse_utc(const std::string& text) {
    std::tm tm{};
    const char* end = strptime(text.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return end && *end == '\0' ? timegm(&tm) : 0;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

// ("20261001", 2) for 20261001.2.jsonl; part 1 is 20261001.jsonl. Empty day for anything that is not a container.
std::pair<std::string, int> container(const std::string& name) {
    static const std::regex re(R"((\d{8})(?:\.(\d+))?\.jsonl)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return {"", 0};
    return {m[1].str(), m[2].matched ? std::stoi(m[2].str()) : 1};
}

}  // namespace

fs::path audit_settings_path() {
    return settings_path().parent_path() / "audit.lua";
}

long parse_duration(const std::string& text) {
    static const std::regex re(R"(([0-9]{1,9})([smhdw]?))");
    std::smatch m;
    if (!std::regex_match(text, m, re) || std::stol(m[1].str()) == 0) throw std::runtime_error("\"" + text + "\" is not a duration like \"30m\", \"12h\", \"1d\" or \"2w\"");
    long n = std::stol(m[1].str());
    switch (m[2].str().empty() ? 's' : m[2].str()[0]) {
        case 'm': return n * 60;
        case 'h': return n * 3600;
        case 'd': return n * 86400;
        case 'w': return n * 7 * 86400;
        default: return n;
    }
}

AuditSettings load_audit_settings() {
    AuditSettings a;
    fs::path path = audit_settings_path();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return a;
    LuaDataLimits limits = lua_data_limits();
    json j = eval_lua_data_file(path, fs::current_path(), limits.tier, limits.memory_mb);
    if (!j.is_object()) throw std::runtime_error(path.string() + ": must return a table (maic audit-trail init writes one)");
    auto fail = [&](const std::string& key, const std::string& what) { throw std::runtime_error(path.string() + ": " + key + " " + what); };
    auto boolean = [&](const char* key, bool& out) {
        if (!j.contains(key)) return;
        if (!j[key].is_boolean()) fail(key, "must be true or false");
        out = j[key].get<bool>();
    };
    auto count = [&](const char* key, int& out) {
        if (!j.contains(key)) return;
        if (!j[key].is_number_integer() || j[key].get<long long>() < 1 || j[key].get<long long>() > 1000000) fail(key, "must be a whole number of at least 1");
        out = j[key].get<int>();
    };
    auto text = [&](const char* key, std::string& out, std::initializer_list<const char*> allowed) {
        if (!j.contains(key)) return;
        if (!j[key].is_string() || j[key].get<std::string>().empty()) fail(key, "must be a string");
        out = j[key].get<std::string>();
        if (allowed.size() && std::none_of(allowed.begin(), allowed.end(), [&](const char* v) { return out == v; })) {
            std::string names;
            for (const char* v : allowed) names += (names.empty() ? "\"" : ", \"") + std::string(v) + "\"";
            fail(key, "must be one of " + names);
        }
    };
    auto duration = [&](const char* key, std::string& out, long& seconds) {
        text(key, out, {});
        try {
            seconds = parse_duration(out);
        } catch (const std::exception& e) {
            fail(key, e.what());
        }
    };
    for (const auto& [key, v] : j.items()) {
        static const std::vector<std::string> known = {"enabled", "every", "grace", "live_window", "stale_days", "order", "enforce", "start_services",
                                                       "judge", "judge_thinking", "judge_max_tokens", "file_mb", "live_mb", "chunk_mb", "archive"};
        if (std::find(known.begin(), known.end(), key) == known.end()) fail(key, "is not an audit trail setting (docs/audit-trail.md lists them)");
    }
    boolean("enabled", a.enabled);
    duration("every", a.every, a.every_seconds);
    count("grace", a.grace);
    duration("live_window", a.live_window, a.live_window_seconds);
    count("stale_days", a.stale_days);
    text("order", a.order, {"stale-first", "live-first", "oldest-first", "newest-first"});
    text("enforce", a.enforce, {"judge-and-hold", "scan-and-continue", "notify"});
    boolean("start_services", a.start_services);
    text("judge", a.judge, {});
    boolean("judge_thinking", a.judge_thinking);
    count("judge_max_tokens", a.judge_max_tokens);
    count("file_mb", a.file_mb);
    count("live_mb", a.live_mb);
    count("chunk_mb", a.chunk_mb);
    text("archive", a.archive, {});
    if (a.archive == "~" || a.archive.rfind("~/", 0) == 0) a.archive = std::string(std::getenv("HOME")) + a.archive.substr(1);
    if (a.archive != "off" && a.archive[0] != '/') fail("archive", "must be \"off\" or an absolute directory (~ expands)");
    return a;
}

bool write_default_audit_settings() {
    fs::path path = audit_settings_path();
    fs::create_directories(path.parent_path());
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return false;
        throw std::runtime_error("can't create " + path.string() + ": " + std::strerror(errno));
    }
    static const char* const text =
        "-- MAIC's audit trail (docs/audit-trail.md). This file is yours alone: no project can set or override any of\n"
        "-- it. Once maic-server has accounts, only an administrator configures it.\n"
        "return {\n"
        "  -- Off by default. On: every tool call of every session, recorded or not, appends one entry to\n"
        "  -- <state>/maic/audit-trail/: the call and the harness's decisions, never conversation text, typed feedback\n"
        "  -- or tool output.\n"
        "  enabled = false,\n"
        "  -- How often maic-leak-audit audits the trail: the systemd timer (maic audit-trail schedule install) and\n"
        "  -- the due check at every start. 30m, 12h, 1d, 2w.\n"
        "  every = \"1d\",\n"
        "  -- With the timer installed, a start holds only once the audit is grace x every overdue.\n"
        "  grace = 3,\n"
        "  -- Entries younger than this are live; older ones not yet retired are stale live.\n"
        "  live_window = \"1d\",\n"
        "  -- The retirement signal: an entry audited successfully at least once whose result has not changed for\n"
        "  -- this many days. With archive = \"off\" the signal deletes it; with a directory it moves it into a chunk.\n"
        "  stale_days = 14,\n"
        "  -- Which entries an audit judges first: stale-first, live-first, oldest-first, newest-first.\n"
        "  order = \"stale-first\",\n"
        "  -- When the audit is due and no scheduler ran it: judge-and-hold (audit with the local judge before the\n"
        "  -- session opens), scan-and-continue (the phase-1 scan, then go on) or notify (one line, nothing else).\n"
        "  enforce = \"judge-and-hold\",\n"
        "  -- judge-and-hold starts the judge's local llama.cpp service when it is down.\n"
        "  start_services = true,\n"
        "  -- The model that judges: a preset that resolves to this machine.\n"
        "  judge = \"qwen-9b\",\n"
        "  -- The judge thinks before its verdict: better calls on borderline cases (reached or only mentioned), at\n"
        "  -- the cost of time and tokens per candidate. maic-leak-audit --thinking on|off overrides it for a run.\n"
        "  judge_thinking = true,\n"
        "  -- The judge's reply budget per candidate, thinking included, so one candidate cannot run away.\n"
        "  judge_max_tokens = 2048,\n"
        "  -- A trail file past this many MB continues in a numbered part.\n"
        "  file_mb = 16,\n"
        "  -- The live trail past this many MB holds the next start for an audit.\n"
        "  live_mb = 256,\n"
        "  -- Archive chunks are split at this many MB.\n"
        "  chunk_mb = 256,\n"
        "  -- \"off\": retired entries are deleted. A directory (an external drive, a network share, an rclone mount):\n"
        "  -- retired entries move there as verified chunks instead. Moving old chunks on to cold storage (off-site)\n"
        "  -- is yours to run: maic audit-trail offsite DEST prints the commands.\n"
        "  archive = \"off\",\n"
        "}\n";
    bool ok = write_all(fd, text);
    close(fd);
    if (!ok) throw std::runtime_error("can't write " + path.string());
    return true;
}

fs::path audit_trail_dir() {
    return state_dir() / "audit-trail";
}

void append_audit_trail(json entry, int file_mb) {
    fs::path dir = audit_trail_dir();
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
    fs::path seq = dir / "seq";
    int lock = open(seq.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0) throw std::runtime_error("can't open " + seq.string() + ": " + std::strerror(errno));
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer{lock};
    while (flock(lock, LOCK_EX) != 0) {
        if (errno != EINTR) throw std::runtime_error("can't lock " + seq.string() + ": " + std::strerror(errno));
    }
    char buf[32] = {};
    ssize_t n = pread(lock, buf, sizeof(buf) - 1, 0);
    long long id = (n > 0 ? std::atoll(buf) : 0) + 1;
    std::time_t t = std::time(nullptr);
    entry["id"] = id;
    entry["time"] = utc(t, "%Y-%m-%dT%H:%M:%SZ");
    std::string line = entry.dump(-1, ' ', false, json::error_handler_t::replace) + '\n';
    // Today's container, or its highest part; a new part once this line would take it past file_mb.
    std::string day = utc(t, "%Y%m%d");
    int part = 1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        auto [d, p] = container(e.path().filename().string());
        if (d == day) part = std::max(part, p);
    }
    auto name = [&](int p) { return dir / (day + (p == 1 ? "" : "." + std::to_string(p)) + ".jsonl"); };
    uintmax_t size = fs::file_size(name(part), ec);
    if (!ec && size > 0 && size + line.size() > static_cast<uintmax_t>(file_mb) << 20) ++part;
    fs::path file = name(part);
    int fd = open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("can't open " + file.string() + ": " + std::strerror(errno));
    bool ok = write_all(fd, line);
    int err = errno;
    close(fd);
    if (!ok) throw std::runtime_error("can't append to " + file.string() + ": " + std::strerror(err));
    std::string last = std::to_string(id) + "\n";
    if (pwrite(lock, last.data(), last.size(), 0) != static_cast<ssize_t>(last.size()) || ftruncate(lock, static_cast<off_t>(last.size())) != 0) {
        throw std::runtime_error("can't update " + seq.string() + ": " + std::strerror(errno));
    }
}

AuditDue audit_due(const AuditSettings& s, std::time_t now) {
    AuditDue d;
    std::error_code ec;
    d.scheduled = fs::exists(systemd_user_dir() / "maic-leak-audit.timer", ec);
    fs::path dir = audit_trail_dir();
    std::time_t oldest = 0;
    bool any = false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string day = container(e.path().filename().string()).first;
        if (day.empty()) continue;
        any = true;
        d.live_bytes += e.file_size(ec);
        std::tm tm{};
        if (strptime(day.c_str(), "%Y%m%d", &tm)) oldest = oldest ? std::min(oldest, timegm(&tm)) : timegm(&tm);
    }
    if (!any) return d;
    std::ifstream in(dir / "index.json");
    json index = json::parse(in, nullptr, false);
    std::time_t last = 0, next = 0;
    if (index.is_object()) {
        if (index.contains("last_audit") && index["last_audit"].is_string()) last = parse_utc(index["last_audit"]);
        if (index.contains("next_audit_due") && index["next_audit_due"].is_string()) next = parse_utc(index["next_audit_due"]);
    }
    // Never audited: due once the oldest container is `every` old.
    std::time_t base = last ? last : oldest;
    if (!next) next = base + s.every_seconds;
    d.due = now >= next;
    d.overdue = now >= base + static_cast<std::time_t>(s.grace) * s.every_seconds;
    d.over_size = d.live_bytes > static_cast<uintmax_t>(s.live_mb) << 20;
    if (d.over_size) {
        d.why = "the live trail is " + std::to_string(d.live_bytes >> 20) + " MB, past live_mb (" + std::to_string(s.live_mb) + " MB)";
    } else if (d.overdue) {
        d.why = (last ? "the last audit was " + utc(last, "%Y-%m-%d %H:%M UTC") : std::string("the trail has never been audited")) + ", more than grace (" +
                std::to_string(s.grace) + ") x every (" + s.every + ") ago";
    } else if (d.due) {
        d.why = "an audit was due " + utc(next, "%Y-%m-%d %H:%M UTC") + " and no scheduler ran it";
    }
    return d;
}

fs::path systemd_user_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "systemd" / "user";
    return fs::path(std::getenv("HOME")) / ".config" / "systemd" / "user";
}

std::string render_unit(const std::string& template_text, const std::string& exec, const std::string& maic, const std::string& every) {
    std::string out = template_text;
    for (const auto& [mark, value] : {std::pair<std::string, std::string>{"@EXEC@", exec}, {"@MAIC@", maic}, {"@EVERY@", every}}) {
        for (size_t at = out.find(mark); at != std::string::npos; at = out.find(mark, at + value.size())) out.replace(at, mark.size(), value);
    }
    return out;
}

}  // namespace maic
