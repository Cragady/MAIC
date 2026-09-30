#include "maic/session.hpp"

#include "maic/paths.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <climits>

#include <algorithm>
#include <cerrno>
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
        SessionInfo info;
        info.path = e.path();
        info.id = e.path().stem().string();
        info.home = e.path().parent_path().lexically_relative(sessions_dir()).string();
        // 20260930-003735-tui-3234642
        size_t second_dash = info.id.find('-', info.id.find('-') + 1);
        info.started = info.id.substr(0, second_dash);
        size_t third_dash = info.id.find('-', second_dash + 1);
        info.kind = info.id.substr(second_dash + 1, third_dash == std::string::npos ? std::string::npos : third_dash - second_dash - 1);
        std::ifstream in(e.path());
        for (std::string line; std::getline(in, line);) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_object()) continue;
            std::string type = j.value("type", "");
            if (type == "start") {
                if (info.workspace.empty()) info.workspace = j.value("workspace", "");
                info.opened_in = j.value("workspace", "");
                info.host = j.value("host", "");
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
            }
        }
        if (info.opened_in.empty()) info.opened_in = info.workspace;
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
        for (const auto& s : list_sessions()) {
            if (fs::equivalent(s.path, id_or_path, ec)) return s;
        }
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

void load_into(LoadedSession& out, const fs::path& path, size_t limit, int depth) {
    if (depth > 32) throw std::runtime_error("session fork chain too deep at " + path.string());
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read " + path.string());
    size_t n = 0;
    for (std::string line; std::getline(in, line) && n < limit; ++n) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        std::string type = j.value("type", "");
        if (type == "resumed_from") {
            // The parent may have been rehomed since: fall back to finding it by id.
            fs::path parent = j.value("path", "");
            std::error_code ec;
            if (!fs::is_regular_file(parent, ec)) {
                auto found = find_session(j.value("id", parent.stem().string()));
                if (!found) throw std::runtime_error("the session this one was resumed from is gone: " + parent.string());
                parent = found->path;
            }
            load_into(out, parent, j.value("records", size_t(0)), depth + 1);
        } else if (type == "start") {
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
            std::string name = j.value("tool", "");
            auto args = j.value("arguments", nlohmann::json::object());
            std::string summary = name == "run_shell" ? "$ " + args.value("command", "") : name + " " + args.value("path", "");
            out.transcript.push_back({"tool_call", summary});
            out.transcript.push_back({"tool_result", j.value("result", ""), j.value("ok", true)});
        } else if (type == "context") {
            out.transcript.push_back({"notice", j.value("text", "")});
        }
    }
    if (depth == 0) out.records = n;
}

}  // namespace

LoadedSession load_session(const fs::path& path) {
    LoadedSession out;
    load_into(out, path, ~size_t(0), 0);
    return out;
}

void SessionLog::write(const std::string& type, nlohmann::json data) {
    data["type"] = type;
    data["time"] = now("%Y-%m-%dT%H:%M:%S%z");
    std::lock_guard lock(mu_);
    out_ << data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    out_.flush();
}

}  // namespace maic
