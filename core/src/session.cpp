#include <signal.h>
#include "maic/session.hpp"

#include "maic/paths.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <climits>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

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
    if (!fs::is_regular_file(path)) throw std::runtime_error("no session at " + path.string());
    out_.open(path, std::ios::app);
    if (!out_) throw std::runtime_error("can't append to " + path.string());
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
            info.profile = j.value("profile", info.profile);
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
    for (const auto& e : fs::recursive_directory_iterator(sessions_dir(), ec)) {
        if (e.path().extension() != ".jsonl") continue;
        SessionInfo info = read_session_info(e.path());
        if (!want.empty() && info.workspace != want && info.opened_in != want) continue;
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    return out;
}

fs::path rehome_session(const SessionInfo& session, const std::string& home) {
    fs::path dir = home == "project" ? sessions_home("project:" + session.workspace) : sessions_home(home);
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
    fs::path target = dir / session.path.filename();
    if (fs::exists(target) && !fs::equivalent(target, session.path)) throw std::runtime_error(target.string() + " already exists");
    fs::rename(session.path, target);
    return target;
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
            out.transcript.push_back({"tool_result", j.value("result", ""), j.value("ok", true)});
        } else if (type == "context") {
            out.transcript.push_back({"notice", j.value("text", "")});
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
        } else if (type == "compact") ++s.compactions[j.value("stage", "?")];
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

std::string export_markdown(const SessionInfo& info, const LoadedSession& session, bool tool_details) {
    std::string out = "# " + (info.title.empty() ? (info.first_prompt.empty() ? info.id : info.first_prompt) : info.title) + "\n\n";
    out += "session `" + info.id + "`, started " + info.started + " in `" + info.workspace + "`";
    if (!session.model.empty()) out += ", model `" + session.model + "`";
    out += "\n\n";
    for (const auto& t : session.transcript) {
        if (t.type == "user") out += "## User\n\n" + t.text + "\n\n";
        else if (t.type == "assistant") out += "## Assistant\n\n" + t.text + "\n\n";
        else if (t.type == "tool_call" && tool_details) out += "**Tool:** `" + t.text + "`\n\n";
        else if (t.type == "tool_result" && tool_details) out += "```\n" + (t.text.size() > 4000 ? t.text.substr(0, 4000) + "\n…" : t.text) + "\n```\n\n";
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
        else if (tools && t.type == "tool_result") out += std::string(t.ok ? "[result]\n" : "[result, error]\n") + t.text + "\n\n";
    }
    return out;
}

void SessionLog::write(const std::string& type, nlohmann::json data) {
    data["type"] = type;
    if (!data.contains("time")) data["time"] = now("%Y-%m-%dT%H:%M:%S%z");
    std::lock_guard lock(mu_);
    out_ << data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    out_.flush();
}

}  // namespace maic
