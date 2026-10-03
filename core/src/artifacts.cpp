#include "maid/artifacts.hpp"

#include "maid/lua.hpp"
#include "maid/paths.hpp"
#include "maid/session.hpp"
#include "maid/settings.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <regex>
#include <stdexcept>

namespace maid {

namespace fs = std::filesystem;

namespace {

bool is_old(const fs::directory_entry& e, std::optional<std::chrono::hours> older_than) {
    if (!older_than) return true;
    std::error_code ec;
    auto mtime = e.symlink_status(ec).type() == fs::file_type::symlink ? fs::file_time_type::clock::now()
                                                                        : e.last_write_time(ec);
    return !ec && fs::file_time_type::clock::now() - mtime > *older_than;
}

// A session's kept outputs (<id>.d beside <id>.jsonl, side_dir) go with their session, never by their own age.
bool is_old_or_its_session(const fs::directory_entry& e, std::optional<std::chrono::hours> older_than) {
    fs::path dir = e.path().parent_path();
    std::error_code ec;
    if (fs::path session = dir.parent_path() / (dir.stem().string() + ".jsonl"); dir.extension() == ".d" && fs::is_regular_file(session, ec)) {
        return is_old(fs::directory_entry(session), older_than);
    }
    return is_old(e, older_than);
}

// Visits every file (and symlink, as itself) under an artifact without following links.
void each_file(const fs::path& root, const std::function<void(const fs::directory_entry&)>& fn) {
    std::error_code ec;
    auto st = fs::symlink_status(root, ec);
    if (ec || !fs::exists(st)) return;
    if (!fs::is_directory(st)) {
        fn(fs::directory_entry(root));
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto type = it->symlink_status(ec).type();
        if (type == fs::file_type::regular || type == fs::file_type::symlink) fn(*it);
    }
}

void require_safe(const fs::path& path) {
    fs::path home = fs::weakly_canonical(std::getenv("HOME"));
    fs::path p = fs::weakly_canonical(path);
    auto rel = p.lexically_relative(home);
    if (p == home || rel.empty() || *rel.begin() == ".." || p.is_relative()) {
        throw std::runtime_error("refusing to clean " + path.string() + ": only paths inside your home directory");
    }
}

// diction's log directory as diction itself resolves it without flags: DICTION_LOG_DIR, then `log_dir` in
// diction.lua beside settings.lua (or, while that does not exist, in the old ~/.config/diction/config.toml), then
// diction-logs/ next to the document (here: the current directory).
Artifact diction_logs() {
    auto expand = [](std::string p) { return !p.empty() && p[0] == '~' ? std::string(std::getenv("HOME")) + p.substr(1) : p; };
    if (const char* env = std::getenv("DICTION_LOG_DIR"); env && *env) {
        return {"diction", "logs", "diction's session logs (raw, scribe, session, taptest), from DICTION_LOG_DIR", expand(env)};
    }
    fs::path config = settings_path().parent_path() / "diction.lua";
    std::string log_dir;
    std::error_code ec;
    if (fs::exists(config, ec)) {
        try {
            LuaDataLimits limits = lua_data_limits();  // global_lua, from the settings main() read first
            nlohmann::json cfg = eval_lua_data_file(config, fs::current_path(), limits.tier, limits.memory_mb);
            if (cfg.value("log_dir", nlohmann::json()).is_string()) log_dir = cfg["log_dir"].get<std::string>();
        } catch (const std::exception&) {
            // diction names the error when it runs; with the file broken it logs next to the document, as here
        }
    } else {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        fs::path toml = (xdg && *xdg ? fs::path(xdg) : fs::path(std::getenv("HOME")) / ".config") / "diction" / "config.toml";
        if (fs::exists(toml, ec)) config = toml;
        std::ifstream in(toml);
        static const std::regex key(R"re(^\s*log_dir\s*=\s*["']([^"']+)["'])re");
        std::smatch m;
        for (std::string line; log_dir.empty() && std::getline(in, line);) {
            if (std::regex_search(line, m, key)) log_dir = m[1].str();
        }
    }
    if (!log_dir.empty()) {
        return {"diction", "logs", "diction's session logs (raw, scribe, session, taptest), from log_dir in " + config.string(), expand(log_dir)};
    }
    return {"diction", "logs", "diction's session logs, next to the document: diction-logs/ here (log_dir in " + config.string() + " collects them in one place)",
            fs::current_path() / "diction-logs"};
}

}  // namespace

std::vector<Artifact> list_artifacts(const std::vector<ServiceDef>& services) {
    std::vector<Artifact> out = {
        {"maid", "sessions", "agent session transcripts (JSONL)", sessions_dir()},
        {"maid", "service-logs", "stdout/stderr of services MAID started", state_dir() / "logs"},
        diction_logs(),
    };
    for (const auto& s : services) {
        for (const auto& a : s.artifacts) {
            out.push_back({s.name, a.name, a.description, a.path});
        }
    }
    for (auto& a : out) {
        std::error_code ec;
        fs::path real = fs::weakly_canonical(a.path, ec);
        if (!ec && real != a.path && fs::exists(a.path, ec)) a.resolved = real.string();
    }
    return out;
}

ArtifactUsage measure(const Artifact& artifact, std::optional<std::chrono::hours> older_than) {
    ArtifactUsage u;
    each_file(artifact.path, [&](const fs::directory_entry& e) {
        if (!is_old_or_its_session(e, older_than)) return;
        std::error_code ec;
        if (e.symlink_status(ec).type() == fs::file_type::regular) u.bytes += e.file_size(ec);
        ++u.files;
    });
    return u;
}

ArtifactUsage clean(const Artifact& artifact, std::optional<std::chrono::hours> older_than) {
    require_safe(artifact.path);
    ArtifactUsage removed;
    std::vector<fs::path> victims;
    each_file(artifact.path, [&](const fs::directory_entry& e) {
        if (!is_old_or_its_session(e, older_than)) return;
        std::error_code ec;
        if (e.symlink_status(ec).type() == fs::file_type::regular) removed.bytes += e.file_size(ec);
        victims.push_back(e.path());
    });
    for (const auto& v : victims) {
        std::error_code ec;
        if (fs::remove(v, ec)) ++removed.files;
    }
    // Drop directories the cleaning emptied, deepest first; keep the artifact's own directory.
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(artifact.path, ec))) {
        std::vector<fs::path> dirs;
        for (auto it = fs::recursive_directory_iterator(artifact.path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->symlink_status(ec).type() == fs::file_type::directory) dirs.push_back(it->path());
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            if (fs::is_empty(*it, ec)) fs::remove(*it, ec);
        }
    }
    return removed;
}

}  // namespace maid
