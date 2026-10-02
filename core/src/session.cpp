#include <signal.h>
#include "maic/session.hpp"

#include "maic/full_output.hpp"
#include "maic/harness.hpp"
#include "maic/paths.hpp"
#include "maic/tools.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <stdexcept>
#include <string_view>

namespace maic {

namespace fs = std::filesystem;

namespace {

std::string now(const char* format) {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), format, std::localtime(&t));
    return buf;
}

}  // namespace

fs::path sessions_dir() {
    return state_dir() / "sessions";
}

std::string project_home_name(const fs::path& workspace) {
    std::error_code ec;
    std::string s = fs::weakly_canonical(workspace, ec).string();
    for (char& c : s) {
        if (c == '/' || c == ' ') c = '-';
    }
    return s.empty() ? "-" : s;
}

fs::path runtime_sessions_dir() {
    if (const char* rt = std::getenv("XDG_RUNTIME_DIR"); rt && *rt) return fs::path(rt) / "maic" / "sessions";
    return fs::path("/tmp") / ("maic-" + std::to_string(getuid())) / "sessions";
}

fs::path sessions_home(const std::string& home) {
    if (home.rfind("project:", 0) == 0) return sessions_dir() / "projects" / project_home_name(home.substr(8));
    if (home.empty() || home == "general") return sessions_dir() / "general";
    if (home.find("..") != std::string::npos || home[0] == '/') throw std::runtime_error("a session home is a name under " + sessions_dir().string());
    return sessions_dir() / home;
}

void SessionLog::create(const std::string& kind, const fs::path& home) {
    fs::create_directories(home);
    std::error_code ec;
    fs::permissions(home.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::permissions(home, fs::perms::owner_all, fs::perm_options::replace, ec);
    // time-kind-pid; a second session from the same process in the same second gets a sequence suffix.
    std::string base = now("%Y%m%d-%H%M%S") + "-" + kind + "-" + std::to_string(getpid());
    int fd = -1;
    for (int seq = 0; seq < 100 && fd < 0; ++seq) {
        path_ = home / (base + (seq ? "-" + std::to_string(seq) : "") + ".jsonl");
        fd = open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) {
        throw std::runtime_error("can't create session log " + path_.string() + ": " + std::strerror(errno));
    }
    close(fd);
    out_.open(path_, std::ios::app);
}

SessionLog::SessionLog(const std::string& kind, const fs::path& home) {
    create(kind, home);
}

SessionLog::SessionLog(Fork, const fs::path& parent, size_t records, const std::string& kind, const fs::path& home) {
    if (!fs::is_regular_file(parent)) throw std::runtime_error("no session at " + parent.string());
    create(kind, home);
    write("resumed_from", {{"id", parent.stem().string()}, {"path", fs::weakly_canonical(parent).string()}, {"records", records}});
}

SessionLog::SessionLog(Reopen, const fs::path& path) : path_(path) {
    // A move this session was in the middle of when MAIC stopped is finished first; it may have left the file elsewhere.
    recovered_ = recover_relocations(path.stem().string());
    if (!fs::is_regular_file(path_)) {
        if (auto found = find_session(path.stem().string()); found && !recovered_.empty()) path_ = found->path;
    }
    if (!fs::is_regular_file(path_)) throw std::runtime_error("no session at " + path.string());
    out_.open(path_, std::ios::app);
    if (!out_) throw std::runtime_error("can't append to " + path_.string());
}

namespace {

std::string preview(const std::string& s) {
    std::string one;
    for (char c : s) one += (c == '\n' || c == '\t') ? ' ' : c;
    return one.size() > 72 ? one.substr(0, 69) + "..." : one;
}

}  // namespace

// Reads the listing fields of one transcript.
SessionInfo read_session_info(const fs::path& path) {
    SessionInfo info;
    info.path = path;
    info.id = path.stem().string();
    info.home = path.parent_path().lexically_relative(sessions_dir()).string();
    // 20260930-003735-tui-3234642
    size_t second_dash = info.id.find('-', info.id.find('-') + 1);
    info.started = info.id.substr(0, second_dash);
    size_t third_dash = info.id.find('-', second_dash + 1);
    info.kind = info.id.substr(second_dash + 1, third_dash == std::string::npos ? std::string::npos : third_dash - second_dash - 1);
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        std::string type = j.value("type", "");
        if (type == "start") {
            if (info.workspace.empty()) info.workspace = j.value("workspace", "");
            info.opened_in = j.value("workspace", "");
            info.host = j.value("host", "");
            info.model = j.value("model", info.model);
            info.pid = j.value("pid", info.pid);
            info.delegated_from = j.value("parent", info.delegated_from);
            info.agent = j.value("agent", j.value("profile", info.agent));
            ++info.opens;
        } else if (type == "resumed_from") {
            info.parent = fs::path(j.value("path", "")).stem().string();
            info.parent_records = j.value("records", 0);
            if (info.workspace.empty()) {
                // A fork inherits the parent's workspace until it records its own.
                std::ifstream pin(j.value("path", ""));
                for (std::string pl; std::getline(pin, pl);) {
                    auto pj = nlohmann::json::parse(pl, nullptr, false);
                    if (pj.is_object() && pj.value("type", "") == "start") {
                        info.workspace = pj.value("workspace", "");
                        break;
                    }
                }
            }
        } else if (type == "user") {
            if (info.first_prompt.empty()) info.first_prompt = preview(j.value("text", ""));
            ++info.turns;
        } else if (type == "title") {
            info.title = j.value("text", "");
        }
    }
    if (info.opened_in.empty()) info.opened_in = info.workspace;
    return info;
}

fs::path side_dir(const fs::path& session_file) {
    return session_file.parent_path() / (session_file.stem().string() + ".d");
}

std::vector<fs::path> sub_sessions_of(const fs::path& path) {
    std::vector<fs::path> out;
    std::error_code ec;
    std::string id = path.stem().string();
    for (const auto& e : fs::directory_iterator(path.parent_path(), ec)) {
        if (e.path().extension() != ".jsonl" || e.path().stem().string().find("-sub-") == std::string::npos) continue;
        SessionInfo info = read_session_info(e.path());
        if (info.kind == "sub" && info.delegated_from == id) out.push_back(e.path());
    }
    return out;
}

std::vector<SessionInfo> list_sessions(const std::optional<fs::path>& workspace) {
    std::vector<SessionInfo> out;
    std::error_code ec;
    std::string want = workspace ? fs::weakly_canonical(*workspace, ec).string() : "";
    // Files from before homes existed sit directly in sessions/; they belong in general/.
    for (const auto& e : fs::directory_iterator(sessions_dir(), ec)) {
        if (e.path().extension() == ".jsonl") {
            fs::create_directories(sessions_home("general"));
            fs::rename(e.path(), sessions_home("general") / e.path().filename(), ec);
        }
    }
    for (auto it = fs::recursive_directory_iterator(sessions_dir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& e = *it;
        // sessions/.backups/<id>/<stamp>.jsonl are trans-fairy-write's copies (docs/cai.md), not sessions.
        if (e.is_directory(ec) && e.path().filename().string().rfind(".", 0) == 0) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.path().extension() != ".jsonl") continue;
        SessionInfo info = read_session_info(e.path());
        if (!want.empty() && info.workspace != want && info.opened_in != want) continue;
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    return out;
}

std::optional<SessionInfo> find_session(const std::string& id_or_path) {
    std::error_code ec;
    if (fs::is_regular_file(id_or_path, ec)) {
        // A path is taken as is, listed or not (temporary transcripts in the runtime directory are never listed).
        for (const auto& s : list_sessions()) {
            if (fs::equivalent(s.path, id_or_path, ec)) return s;
        }
        SessionInfo info = read_session_info(fs::absolute(id_or_path));
        info.home = "(unlisted)";
        return info;
    }
    std::optional<SessionInfo> found;
    for (const auto& s : list_sessions()) {
        if (s.id == id_or_path) return s;
        if (s.id.rfind(id_or_path, 0) == 0) {
            if (found) return std::nullopt;  // ambiguous prefix
            found = s;
        }
    }
    return found;
}

size_t count_records(const fs::path& path) {
    std::ifstream in(path);
    size_t n = 0;
    for (std::string line; std::getline(in, line);) ++n;
    return n;
}

fs::path backup_session(const fs::path& path) {
    fs::path root = sessions_dir() / ".backups", dir = root / path.stem();
    for (const auto& d : {root, dir}) {
        if (mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) throw std::runtime_error("can't create " + d.string() + ": " + std::strerror(errno));
    }
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", std::gmtime(&t));
    fs::path dest;
    int fd = -1;
    for (int n = 0; fd < 0; ++n) {
        dest = dir / (std::string(stamp) + (n ? "-" + std::to_string(n) : "") + ".jsonl");
        fd = open(dest.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) throw std::runtime_error("can't create " + dest.string() + ": " + std::strerror(errno));
    }
    close(fd);
    std::ifstream src(path, std::ios::binary);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    out << src.rdbuf();
    if (!src || !out.flush()) throw std::runtime_error("could not copy " + path.string() + " to " + dest.string());
    return dest;
}

bool is_maic_session(const fs::path& path) {
    std::ifstream in(path);
    bool signature = false;
    for (std::string line; std::getline(in, line);) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        std::string type = j.value("type", "");
        if (type == "cai" || type == "rewritten") continue;
        for (const char* k : {"uuid", "parentUuid", "message", "isMeta", "sessionId"}) {
            if (j.contains(k)) return false;
        }
        signature = signature || (type == "start" && j.contains("workspace")) || (type == "msg" && j.contains("role")) ||
                    ((type == "user" || type == "assistant") && j.contains("text")) || (type == "tool" && j.contains("tool")) ||
                    (type == "resumed_from" && j.contains("records"));
    }
    return signature;
}

namespace {

// The file a `resumed_from` record points at. The parent may have been rehomed since: fall back to its id.
fs::path pointer_target(const nlohmann::json& j) {
    fs::path parent = j.value("path", "");
    std::error_code ec;
    if (fs::is_regular_file(parent, ec)) return parent;
    auto found = find_session(j.value("id", parent.stem().string()));
    if (!found) throw std::runtime_error("the session this one was resumed from is gone: " + parent.string());
    return found->path;
}

void walk(const fs::path& path, size_t limit, int depth, const std::function<void(const nlohmann::json&)>& fn, size_t& top) {
    if (depth > 32) throw std::runtime_error("session fork chain too deep at " + path.string());
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read " + path.string());
    size_t n = 0;
    for (std::string line; std::getline(in, line) && n < limit; ++n) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        if (j.value("type", "") != "resumed_from") fn(j);
        else walk(pointer_target(j), j.value("records", size_t(0)), depth + 1, fn, top);
    }
    if (depth == 0) top = n;
}

std::string tool_summary(const nlohmann::json& j) {
    std::string name = j.value("tool", "");
    auto args = j.value("arguments", nlohmann::json::object());
    return name == "run_shell" ? "$ " + args.value("command", "") : name + " " + args.value("path", "");
}

// Seconds since the epoch from a record's `time` (local ISO 8601 with the offset); -1 when it does not parse.
long record_time(const nlohmann::json& j) {
    std::string t = j.value("time", "");
    std::tm tm{};
    const char* rest = strptime(t.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (!rest) return -1;
    long offset = 0;
    if ((rest[0] == '+' || rest[0] == '-') && std::strlen(rest) >= 5) {
        offset = (std::atoi(std::string(rest + 1, 2).c_str()) * 3600 + std::atoi(std::string(rest + 3, 2).c_str()) * 60) * (rest[0] == '-' ? -1 : 1);
    }
    return static_cast<long>(timegm(&tm)) - offset;
}

}  // namespace

size_t walk_records(const fs::path& path, size_t limit, const std::function<void(const nlohmann::json&)>& fn) {
    size_t top = 0;
    walk(path, limit, 0, fn, top);
    return top;
}

LoadedSession load_session(const fs::path& path, size_t records) {
    LoadedSession out;
    out.records = walk_records(path, records, [&](const nlohmann::json& j) {
        std::string type = j.value("type", "");
        if (type == "start") {
            out.model = j.value("model", out.model);
            out.mode = j.value("mode", out.mode);
        } else if (type == "msg") {
            out.messages.push_back(message_from_json(j));
        } else if (type == "reset") {
            out.messages.clear();  // compaction re-dumps the history; the transcript stays
        } else if (type == "compact") {
            out.transcript.push_back({"notice", "compacted (" + j.value("stage", "") + ")" + (j.contains("summary") ? ":\n" + j.value("summary", "") : "")});
        } else if (type == "clear") {
            out.messages.clear();
            out.transcript.clear();
        } else if (type == "user") {
            out.transcript.push_back({"user", j.value("text", "")});
            out.model = j.value("model", out.model);
            out.mode = j.value("mode", out.mode);
        } else if (type == "assistant") {
            out.transcript.push_back({"assistant", j.value("text", "")});
        } else if (type == "tool") {
            out.transcript.push_back({"tool_call", tool_summary(j)});
            out.transcript.push_back({"tool_result", j.value("result", ""), j.value("ok", true), j.value("full_output", nlohmann::json())});
        } else if (type == "context") {
            out.transcript.push_back({"notice", j.value("text", "")});
        } else if (type == "steer") {
            std::string note = j.value("note", "");
            out.transcript.push_back({"notice", "↯ " + j.value("action", "") + (j.value("trigger", "") == "ban" ? " (a ban's steer)" : "") + (note.empty() ? "" : ": " + note)});
        }
    });
    return out;
}

SessionStats session_stats(const fs::path& path) {
    SessionStats s;
    auto touched = [&](const std::string& p) {
        if (!p.empty() && std::find(s.files.begin(), s.files.end(), p) == s.files.end()) s.files.push_back(p);
    };
    s.records = walk_records(path, ~size_t(0), [&](const nlohmann::json& j) {
        std::string type = j.value("type", "");
        if (s.first_time.empty()) s.first_time = j.value("time", "");
        s.last_time = j.value("time", s.last_time);
        if (type == "user") ++s.turns;
        else if (type == "assistant") ++s.replies;
        else if (type == "tool") {
            ++s.tool_calls;
            if (!j.value("ok", true)) ++s.tool_errors;
            std::string name = j.value("tool", "");
            ++s.tools[name];
            for (const char* writer : {"write_file", "edit_file", "multi_edit", "apply_patch", "move_file", "copy_file", "delete_file", "make_dir"}) {
                if (name != writer) continue;
                auto args = j.value("arguments", nlohmann::json::object());
                touched(args.value("path", ""));
                touched(args.value("to", ""));
            }
        } else if (type == "usage") {
            s.input_tokens += j.value("input", 0);
            s.output_tokens += j.value("output", 0);
            s.context = j.value("context", s.context);
        } else if (type == "normalized") s.normalized[j.value("provider", "?") + " " + j.value("rule", "?")] += j.value("count", 1);
        else if (type == "compact") ++s.compactions[j.value("stage", "?")];
        else if (type == "clear") ++s.clears;
        else if (type == "undo") {
            ++s.undos;
            touched(j.value("path", ""));
        }
    });
    return s;
}

SessionTiming session_timing(const fs::path& path) {
    SessionTiming t;
    long turn_start = -1, previous = -1, last = -1;
    auto close_turn = [&] {
        if (!t.turns.empty() && turn_start >= 0 && last > turn_start) t.turns.back().seconds = last - turn_start;
    };
    walk_records(path, ~size_t(0), [&](const nlohmann::json& j) {
        std::string type = j.value("type", "");
        long at = record_time(j);
        if (type == "user") {
            close_turn();
            t.turns.push_back({preview(j.value("text", "")), 0, 0});
            turn_start = at;
        } else if (type == "tool") {
            if (!t.turns.empty()) ++t.turns.back().tool_calls;
            t.tools.push_back({preview(tool_summary(j)), previous >= 0 && at > previous ? at - previous : 0, j.value("ok", true)});
        }
        if (at < 0) return;
        std::string role = type == "msg" ? j.value("role", "") : "";
        if (role == "user") return;  // the prompt's own message record: the next turn, not the end of this one
        last = at;
        // The tool message is written with the tool record, so it is not where the call started.
        if (role != "tool") previous = at;
    });
    close_turn();
    return t;
}

namespace {

// The records of `path` from line `first` on (up to `limit`) that belong to the conversation: everything but the
// file's own identity (start, title, imported_from) and system prompts, which a new session gets from its own
// head. A pointer is replaced by the parent's records, so a fork is copied as the whole conversation it holds.
std::vector<nlohmann::json> conversation_records(const fs::path& path, size_t first, size_t limit = ~size_t(0)) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::vector<nlohmann::json> out;
    size_t n = 0;
    for (std::string line; std::getline(in, line) && n < limit; ++n) {
        if (n < first) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        std::string type = j.value("type", "");
        if (type == "resumed_from") {
            auto parent = conversation_records(pointer_target(j), 0, j.value("records", size_t(0)));
            out.insert(out.end(), parent.begin(), parent.end());
            continue;
        }
        if (type == "start" || type == "title" || type == "imported_from") continue;
        if (type == "msg" && j.value("role", "") == "system") continue;
        out.push_back(std::move(j));
    }
    return out;
}

void append_records(SessionLog& log, std::vector<nlohmann::json> records) {
    for (auto& j : records) {
        std::string type = j["type"];
        j.erase("type");
        log.write(type, std::move(j));
    }
}

// A note the model reads as a system message and the transcript shows as a notice.
void note(SessionLog& log, const std::string& text) {
    log.write("msg", message_to_json({"system", text}));
    log.write("context", {{"text", text}});
}

}  // namespace

fs::path inject_note(const fs::path& parent, size_t records, const std::string& role, const std::string& text, const fs::path& home) {
    if (role != "user" && role != "system") throw std::runtime_error("an injected note is a user or a system message, not '" + role + "'");
    if (text.empty()) throw std::runtime_error("nothing to inject");
    SessionLog log = SessionLog::fork(parent, records, "inject", home);
    log.write("inject", {{"role", role}});
    log.write("msg", message_to_json({role, text}));
    log.write("context", {{"text", "injected " + role + " note (" + now("%Y-%m-%d") + "):\n" + text}});
    return log.path();
}

fs::path graft_session(const fs::path& onto, size_t records, const fs::path& graft, const fs::path& home) {
    if (!fs::is_regular_file(graft)) throw std::runtime_error("no session at " + graft.string());
    size_t messages = 0;
    for (const auto& m : load_session(graft).messages) messages += m.role != "system";
    if (messages == 0) throw std::runtime_error(graft.stem().string() + " has no conversation to graft");
    std::vector<nlohmann::json> copied = conversation_records(graft, 0);
    SessionLog log = SessionLog::fork(onto, records, "graft", home);
    log.write("graft", {{"id", graft.stem().string()}, {"path", fs::weakly_canonical(graft).string()}, {"messages", messages}});
    note(log, "The next " + std::to_string(messages) + " messages were grafted from session " + graft.stem().string() + " on " + now("%Y-%m-%d") +
                  ": they took place separately and are not this conversation's own history.");
    append_records(log, std::move(copied));
    return log.path();
}

fs::path compose_session(const fs::path& source, size_t first, const std::string& root, const fs::path& home) {
    if (!fs::is_regular_file(source)) throw std::runtime_error("no session at " + source.string());
    std::vector<nlohmann::json> suffix = conversation_records(source, first);
    if (suffix.empty()) throw std::runtime_error(source.stem().string() + " has " + std::to_string(count_records(source)) + " records; nothing after " + std::to_string(first));
    LoadedSession head = load_session(source, first);
    SessionInfo info = read_session_info(source);
    SessionLog log("compose", home);
    char host[256] = "";
    gethostname(host, sizeof(host) - 1);
    log.write("start", {{"workspace", info.workspace}, {"model", head.model}, {"mode", head.mode.empty() ? "manual" : head.mode}, {"host", host}, {"pid", getpid()}});
    log.write("compose", {{"id", source.stem().string()}, {"path", fs::weakly_canonical(source).string()}, {"from", first}, {"records", suffix.size()}});
    for (const auto& m : head.messages) {
        if (m.role != "system") continue;
        log.write("msg", message_to_json(m));
        break;
    }
    if (!root.empty()) {
        log.write("msg", message_to_json({"user", root}));
        log.write("context", {{"text", "root (composed, not typed):\n" + root}});
    }
    note(log, "This conversation continues session " + source.stem().string() + " from its record " + std::to_string(first) + " on, composed on " +
                  now("%Y-%m-%d") + "; what came before the cut is not present. Do not infer what the missing context said.");
    append_records(log, std::move(suffix));
    return log.path();
}

bool session_running(const SessionInfo& info) {
    if (info.pid <= 0) return false;
    char host[256] = "";
    gethostname(host, sizeof(host) - 1);
    if (!info.host.empty() && info.host != host) return false;
    if (kill(static_cast<pid_t>(info.pid), 0) != 0) return false;
    std::ifstream cmd("/proc/" + std::to_string(info.pid) + "/cmdline");
    std::string line((std::istreambuf_iterator<char>(cmd)), std::istreambuf_iterator<char>());
    return line.find("maic") != std::string::npos;
}

std::optional<std::string> session_lock_reason(const SessionInfo& info) {
    std::ifstream in(info.path.string() + ".tripped");
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

namespace {

// For a result whose whole output was kept: the label, its size and the command that shows it, between `open` and `close`.
std::string kept_note(const TranscriptEntry& t, const std::string& open, const std::string& close) {
    if (!t.full_output.is_object() || !t.full_output.contains("path") || !t.full_output["path"].is_string()) return "";
    fs::path rel = t.full_output["path"].get<std::string>();
    std::string id = rel.begin() != rel.end() ? rel.begin()->stem().string() : "";
    std::string note = std::string("[") + kFullOutputLabel + "] " + std::to_string(t.full_output.value("bytes", size_t(0))) + " bytes";
    if (t.full_output.value("dropped", size_t(0))) note += " (" + std::to_string(t.full_output.value("dropped", size_t(0))) + " more dropped over full_output_max_mb)";
    return open + note + ": maic sessions output " + id + " " + rel.stem().string() + close;
}

}  // namespace

std::string export_markdown(const SessionInfo& info, const LoadedSession& session, bool tool_details) {
    std::string out = "# " + (info.title.empty() ? (info.first_prompt.empty() ? info.id : info.first_prompt) : info.title) + "\n\n";
    out += "session `" + info.id + "`, started " + info.started + " in `" + info.workspace + "`";
    if (!session.model.empty()) out += ", model `" + session.model + "`";
    out += "\n\n";
    for (const auto& t : session.transcript) {
        if (t.type == "user") out += "## User\n\n" + t.text + "\n\n";
        else if (t.type == "assistant") out += "## Assistant\n\n" + t.text + "\n\n";
        else if (t.type == "tool_call" && tool_details) out += "**Tool:** `" + t.text + "`\n\n";
        else if (t.type == "tool_result" && tool_details) out += "```\n" + (t.text.size() > 4000 ? t.text.substr(0, 4000) + "\n…" : t.text) + "\n```\n\n" + kept_note(t, "_", "_\n\n");
        else if (t.type == "notice") out += "_" + t.text + "_\n\n";
    }
    return out;
}

std::string render_text(const LoadedSession& session, size_t from, size_t to, bool tools) {
    std::string out;
    size_t turn = 0;
    for (const auto& t : session.transcript) {
        if (t.type == "user") ++turn;
        if (turn < from || (to && turn > to)) continue;
        if (t.type == "user") out += "[user]\n" + t.text + "\n\n";
        else if (t.type == "assistant") out += "[assistant]\n" + t.text + "\n\n";
        else if (t.type == "notice") out += "[notice] " + t.text + "\n\n";
        else if (tools && t.type == "tool_call") out += "[tool] " + t.text + "\n";
        else if (tools && t.type == "tool_result") out += std::string(t.ok ? "[result]\n" : "[result, error]\n") + t.text + "\n\n" + kept_note(t, "", "\n\n");
    }
    return out;
}

namespace {

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

std::string read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Appends `data` to `file` and fsyncs it.
bool append_synced(const fs::path& file, std::string_view data) {
    int fd = open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write_all(fd, data) && fsync(fd) == 0;
    close(fd);
    return ok;
}

// rename(2), or when the two are on different filesystems (or `copy`), a copy to <to>.moving that is fsynced and
// renamed into place before the source goes. On failure the source is untouched and nothing is left at `to`.
void move_session_file(const fs::path& from, const fs::path& to, bool copy) {
    if (!copy) {
        if (rename(from.c_str(), to.c_str()) == 0) return;
        if (errno != EXDEV) throw std::runtime_error("can't move " + from.string() + " to " + to.string() + ": " + std::strerror(errno));
    }
    fs::path moving = to.string() + ".moving";
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) throw std::runtime_error("can't read " + from.string() + ": " + std::strerror(errno));
    int out = open(moving.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        int err = errno;
        close(in);
        throw std::runtime_error("can't create " + moving.string() + ": " + std::strerror(err));
    }
    bool ok = true;
    char buf[1 << 16];
    for (ssize_t n; ok && (n = read(in, buf, sizeof(buf))) != 0;) {
        if (n < 0) ok = errno == EINTR;
        else ok = write_all(out, std::string_view(buf, static_cast<size_t>(n)));
    }
    ok = ok && fsync(out) == 0;
    close(in);
    close(out);
    if (!ok || rename(moving.c_str(), to.c_str()) != 0) {
        int err = errno;
        unlink(moving.c_str());
        throw std::runtime_error("can't copy " + from.string() + " to " + to.string() + ": " + std::strerror(err));
    }
    if (unlink(from.c_str()) != 0) {
        int err = errno;
        unlink(to.c_str());
        throw std::runtime_error("can't remove " + from.string() + " after copying it: " + std::strerror(err));
    }
}

// A session's side directory to beside its new place, when it has one.
std::error_code move_side_dir(const fs::path& from_session, const fs::path& to_session) {
    std::error_code ec;
    if (!fs::is_directory(side_dir(from_session), ec)) return {};
    return move_path(side_dir(from_session), side_dir(to_session));
}

void make_home(const fs::path& dir) {
    fs::create_directories(dir);
    std::error_code ec;
    fs::permissions(dir.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
}

}  // namespace

fs::path SessionLog::relocate(const fs::path& dest_dir, const std::string& reason) {
    make_home(dest_dir);
    std::error_code ec;
    fs::path from, to, pending;
    {
        std::lock_guard lock(mu_);
        if (pending_fd_ >= 0) throw std::runtime_error("the session is already being moved");
        from = path_;
        to = dest_dir / from.filename();
        if (fs::exists(from.string() + ".tripped", ec)) throw std::runtime_error("the session's tripwire lock is set beside it; :unlock first");
        if (fs::exists(to, ec)) throw std::runtime_error(to.string() + " already exists");
        pending = to.string() + ".pending";
        pending_fd_ = open(pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
        if (pending_fd_ < 0) throw std::runtime_error("can't create " + pending.string() + ": " + std::strerror(errno));
        out_.close();
    }
    std::vector<fs::path> children = sub_sessions_of(from);
    if (while_relocating) while_relocating();
    std::string failure;
    try {
        move_session_file(from, to, relocate_by_copy);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    {
        std::lock_guard lock(mu_);
        fs::path at = failure.empty() ? to : from;
        bool appended = append_synced(at, read_bytes(pending));
        close(pending_fd_);
        pending_fd_ = -1;
        if (appended) unlink(pending.c_str());
        else failure += (failure.empty() ? "" : "; ") + std::string("the records written meanwhile are in ") + pending.string() + " until the session is opened again";
        path_ = at;
        out_.open(path_, std::ios::app);
    }
    if (!failure.empty()) throw std::runtime_error(failure);
    if (auto err = move_side_dir(from, to)) throw std::runtime_error("moved the session, but not its kept outputs in " + side_dir(from).string() + ": " + err.message());
    for (const auto& c : children) {
        if (auto err = move_path(c, dest_dir / c.filename())) throw std::runtime_error("moved the session, but not its subagent " + c.stem().string() + ": " + err.message());
        if (auto err = move_side_dir(c, dest_dir / c.filename())) throw std::runtime_error("moved the subagent " + c.stem().string() + ", but not its kept outputs: " + err.message());
    }
    write("rehomed", {{"from", from.string()}, {"to", to.string()}, {"reason", reason}});
    return to;
}

std::vector<RehomeMove> plan_rehome(const std::vector<RehomeTarget>& targets, const std::string& home) {
    std::vector<SessionInfo> all = list_sessions();
    std::vector<std::string> problems;
    std::vector<RehomeMove> plan;
    std::set<std::string> planned;
    std::error_code ec;
    for (const auto& t : targets) {
        std::optional<SessionInfo> named;
        if (fs::is_regular_file(t.id, ec)) {
            named = find_session(t.id);
        } else {
            std::vector<const SessionInfo*> matches;
            for (const auto& s : all) {
                if (s.id == t.id) {
                    matches = {&s};
                    break;
                }
                if (s.id.rfind(t.id, 0) == 0) matches.push_back(&s);
            }
            if (matches.size() == 1) named = *matches[0];
            else if (matches.empty()) problems.push_back("no session matching '" + t.id + "' (maic sessions)");
            else {
                std::string ids;
                for (const auto* m : matches) ids += "\n      " + m->id + "  [" + m->home + "]";
                problems.push_back("'" + t.id + "' matches " + std::to_string(matches.size()) + " sessions; give more of the id:" + ids);
            }
        }
        if (!named) continue;
        fs::path dir = home == "project" ? sessions_home("project:" + named->workspace) : sessions_home(home);
        std::vector<SessionInfo> moving;
        if (t.subagents != Subagents::Only) moving.push_back(*named);
        if (t.subagents != Subagents::Stay) {
            // Its subagents wherever they are, and theirs.
            std::vector<std::string> parents = {named->id};
            std::set<std::string> seen = {named->id};
            while (!parents.empty()) {
                std::string parent = parents.back();
                parents.pop_back();
                for (const auto& s : all) {
                    if (s.kind != "sub" || s.delegated_from != parent || !seen.insert(s.id).second) continue;
                    moving.push_back(s);
                    parents.push_back(s.id);
                }
            }
        }
        for (const auto& s : moving) {
            if (!planned.insert(s.path.string()).second) continue;
            fs::path to = dir / s.path.filename();
            if (fs::equivalent(s.path.parent_path(), dir, ec)) {
                plan.push_back({s, s.path});
                continue;
            }
            if (session_running(s)) problems.push_back(s.id + " is running (pid " + std::to_string(s.pid) + "): a live session moves only by :init inside it; wait until it ends");
            else if (session_lock_reason(s)) problems.push_back(s.id + " has its tripwire lock set beside it; `maic unlock session " + s.id + "` first");
            else if (fs::exists(to, ec)) problems.push_back(to.string() + " already exists");
            else if (fs::exists(side_dir(to), ec)) problems.push_back(side_dir(to).string() + " already exists");
            plan.push_back({s, to});
        }
    }
    if (!problems.empty()) {
        std::string text = "nothing moved:";
        for (const auto& p : problems) text += "\n  " + p;
        throw std::runtime_error(text);
    }
    return plan;
}

void rehome_session(const RehomeMove& move) {
    if (move.to == move.session.path) return;
    make_home(move.to.parent_path());
    move_session_file(move.session.path, move.to, false);
    if (auto err = move_side_dir(move.session.path, move.to)) {
        throw std::runtime_error("moved " + move.session.id + " to " + move.to.string() + ", but not its kept outputs in " + side_dir(move.session.path).string() + ": " + err.message());
    }
    nlohmann::json record = {{"type", "rehomed"}, {"time", now("%Y-%m-%dT%H:%M:%S%z")}, {"from", move.session.path.string()}, {"to", move.to.string()}, {"reason", "rehome"}};
    if (!append_synced(move.to, record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n')) {
        throw std::runtime_error("moved " + move.session.id + " to " + move.to.string() + ", but could not add its rehomed record");
    }
}

std::vector<std::string> recover_relocations(const std::string& id) {
    std::map<std::string, std::vector<fs::path>> files;  // by file name: <id>.jsonl, .pending and .moving
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(sessions_dir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (it->is_directory(ec) && name.rfind(".", 0) == 0) {
            it.disable_recursion_pending();
            continue;
        }
        if (!id.empty() && name.rfind(id + ".jsonl", 0) != 0) continue;
        if (name.find(".jsonl") != std::string::npos) files[name].push_back(it->path());
    }
    std::vector<std::string> notices;
    auto home_of = [](const fs::path& p) { return p.parent_path().lexically_relative(sessions_dir()).string() + "/"; };
    for (const auto& [name, leftovers] : files) {
        bool pending = name.size() > 14 && name.compare(name.size() - 14, 14, ".jsonl.pending") == 0;
        bool moving = name.size() > 13 && name.compare(name.size() - 13, 13, ".jsonl.moving") == 0;
        if (!pending && !moving) continue;
        std::string sid = name.substr(0, name.find(".jsonl"));
        for (const auto& leftover : leftovers) {
            fs::path dir = leftover.parent_path(), target = dir / (sid + ".jsonl");
            std::optional<fs::path> source;
            if (auto f = files.find(sid + ".jsonl"); f != files.end()) {
                for (const auto& p : f->second) {
                    if (p.parent_path() != dir) source = p;
                }
            }
            fs::path live = fs::exists(target, ec) ? target : source.value_or(fs::path());
            if (!live.empty() && session_running(read_session_info(live))) continue;  // its own process is moving it now
            if (moving) {
                if (!source) {
                    notices.push_back("an unfinished copy of session " + sid + " is at " + leftover.string() + " and no original was found; left as it is");
                    continue;
                }
                fs::remove(leftover, ec);
                notices.push_back("discarded an unfinished copy of session " + sid + " in " + home_of(leftover) + " (a move was interrupted; the original in " + home_of(*source) + " is intact)");
                continue;
            }
            if (source && fs::exists(target, ec)) {
                // Copied and renamed into place, but the source was never removed: the copy is the same file.
                if (read_bytes(target) != read_bytes(*source)) {
                    notices.push_back("session " + sid + " is in both " + home_of(*source) + " and " + home_of(target) + " and they differ; left as they are, with " + leftover.string());
                    continue;
                }
                fs::remove(target, ec);
                live = *source;
            }
            if (live.empty()) {
                notices.push_back("records of session " + sid + " from an interrupted move are in " + leftover.string() + ", but the session file is gone; left as it is");
                continue;
            }
            // A crash can come after the records were appended and before their file was removed: skip what the
            // session already ends with.
            std::string held = read_bytes(leftover), file = read_bytes(live);
            size_t k = std::min(held.size(), file.size());
            while (k > 0 && file.compare(file.size() - k, k, held, 0, k) != 0) --k;
            if (!append_synced(live, std::string_view(held).substr(k))) {
                notices.push_back("could not add the records of session " + sid + " in " + leftover.string() + " back to " + live.string());
                continue;
            }
            fs::remove(leftover, ec);
            size_t added = static_cast<size_t>(std::count(held.begin() + static_cast<long>(k), held.end(), '\n'));
            notices.push_back("session " + sid + " was being moved when MAIC stopped: " + std::to_string(added) + " record" + (added == 1 ? "" : "s") +
                              " written meanwhile were added back to it, in " + home_of(live));
        }
    }
    return notices;
}

InitMove init_move_check(const fs::path& path, const std::vector<nlohmann::json>& records, const fs::path& workspace, bool recorded,
                         size_t outside_reads_allowed) {
    InitMove m;
    std::error_code ec;
    fs::path ws = fs::weakly_canonical(workspace, ec);
    fs::path home = sessions_home("project:" + ws.string());
    if (!recorded) {
        m.reason = "it is not recorded (--no-record): it lives in the runtime directory and is gone at logout";
        return m;
    }
    if (fs::weakly_canonical(path.parent_path(), ec) == fs::weakly_canonical(home, ec)) {
        m.reason = "it is already in " + home.lexically_relative(sessions_dir()).string() + "/";
        return m;
    }
    auto outside = [&](const fs::path& p) {
        fs::path rel = p.lexically_relative(ws);
        return rel.empty() || *rel.begin() == "..";
    };
    std::set<std::string> reads, writes;
    // Paths are resolved against the workspace in effect when the call was made, then judged against this one.
    std::optional<Harness> at;
    at.emplace(ws);
    for (const auto& j : records) {
        std::string type = j.value("type", "");
        if (type == "start") at.emplace(j.value("workspace", ws.string()));
        else if (type == "workspace") at.emplace(j.value("to", ws.string()));
        if (type != "tool" || !j.value("ok", true)) continue;
        std::vector<Action> actions;
        try {
            actions = tool_actions(*at, j.value("tool", ""), j.value("arguments", nlohmann::json::object()));
        } catch (const std::exception&) {
            // Not a built-in (a Lua or script tool: its actions are listed below) or arguments it never ran with.
        }
        for (const auto& sub : j.value("actions", nlohmann::json::array())) {
            if (!sub.is_object()) continue;
            std::string d = sub.value("decision", ""), a = sub.value("action", "");
            if (d == "deny" || d == "trip" || sub.value("approval", "") == "no") continue;
            for (auto [prefix, kind] : {std::pair<const char*, Action::Kind>{"writes ", Action::Kind::Write}, {"reads ", Action::Kind::Read},
                                        {"write_file ", Action::Kind::Write}, {"read_file ", Action::Kind::Read}, {"list_dir ", Action::Kind::Read}}) {
                if (a.rfind(prefix, 0) != 0) continue;
                std::string p = a.substr(std::strlen(prefix));
                p = p.substr(0, p.find(" (declared by "));
                actions.push_back({kind, at->resolve(p), ""});
            }
        }
        for (const auto& a : actions) {
            if (a.kind == Action::Kind::Shell) {
                if (!a.workdir.empty() && outside(a.workdir)) writes.insert(a.workdir.string());
            } else if (outside(a.path)) {
                (a.kind == Action::Kind::Write ? writes : reads).insert(a.path.string());
            }
        }
    }
    m.outside_reads = reads.size();
    m.outside_writes = writes.size();
    if (m.outside_writes > 0 || m.outside_reads > outside_reads_allowed) {
        m.verdict = InitMove::Ask;
        m.reason = "it read " + std::to_string(m.outside_reads) + " and wrote " + std::to_string(m.outside_writes) + " files outside the project";
    } else {
        m.verdict = InitMove::Move;
        m.reason = "it worked here throughout";
    }
    return m;
}

InitMove init_move_check(const fs::path& path, const fs::path& workspace, bool recorded, size_t outside_reads_allowed) {
    std::vector<nlohmann::json> records;
    auto keep = [&](const nlohmann::json& j) { records.push_back(j); };
    if (recorded) {
        walk_records(path, ~size_t(0), keep);
        for (const auto& c : sub_sessions_of(path)) walk_records(c, ~size_t(0), keep);
    }
    return init_move_check(path, records, workspace, recorded, outside_reads_allowed);
}

bool SessionLog::recorded() const {
    auto rel = path().lexically_relative(runtime_sessions_dir());
    return rel.empty() || *rel.begin() == "..";
}

void SessionLog::write(const std::string& type, nlohmann::json data) {
    data["type"] = type;
    if (!data.contains("time")) data["time"] = now("%Y-%m-%dT%H:%M:%S%z");
    std::string line = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
    std::lock_guard lock(mu_);
    if (pending_fd_ >= 0) {
        write_all(pending_fd_, line);
        return;
    }
    out_ << line;
    out_.flush();
}

}  // namespace maic
