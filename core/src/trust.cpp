#include "maic/trust.hpp"

#include "maic/harness.hpp"
#include "maic/lua.hpp"
#include "maic/paths.hpp"
#include "maic/session.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <istream>
#include <iterator>
#include <ostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

extern char** environ;

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::mutex g_mu;  // the session answers, the config and the verifier

// This process's own answers: --trust, "not now", and what settle_trust fixed at start.
std::map<std::string, bool>& session_answers() {
    static std::map<std::string, bool> answers;
    return answers;
}

TrustConfig& config() {
    static TrustConfig c;
    return c;
}

StepUpVerifier& verifier() {
    static StepUpVerifier v;
    return v;
}

std::optional<bool> session_answer(const fs::path& dir) {
    std::lock_guard lock(g_mu);
    auto it = session_answers().find(dir.string());
    if (it == session_answers().end()) return std::nullopt;
    return it->second;
}

void set_session_answer(const fs::path& dir, bool trusted) {
    std::lock_guard lock(g_mu);
    session_answers()[dir.string()] = trusted;
}

// --trust=sandbox and the like: this process's Lua level for a directory, over the remembered one.
std::map<std::string, LuaTier>& session_lua() {
    static std::map<std::string, LuaTier> levels;
    return levels;
}

fs::path home_path() {
    const char* home = std::getenv("HOME");
    std::error_code ec;
    return home && *home ? fs::weakly_canonical(home, ec) : fs::path();
}

fs::path absolute_dir(const fs::path& dir) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    if (p.has_filename() || p == p.root_path()) return p;
    return p.parent_path();  // a trailing slash
}

std::string expand_home(std::string path) {
    if (!path.empty() && path[0] == '~') path = home_path().string() + path.substr(1);
    return path;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string sha256(const std::vector<std::string>& parts) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    for (const auto& s : parts) EVP_DigestUpdate(ctx, s.data(), s.size());
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_DigestFinal_ex(ctx, d, &n);
    EVP_MD_CTX_free(ctx);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < n; ++i) out += digits[d[i] >> 4], out += digits[d[i] & 15];
    return out;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string rel(const fs::path& dir, const fs::path& f) {
    return f.lexically_relative(dir).string();
}

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// git, never through a shell, with no GIT_* from the environment and nothing from the repository's own config
// that could run a program (fsmonitor, hooks, a pager, signature checks). Local only. nullopt when it fails.
std::optional<std::string> git(const fs::path& dir, const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    for (const char* a : {"git", "--no-pager", "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null", "-c", "log.showSignature=false"}) argv.emplace_back(a);
    if (!dir.empty()) {
        argv.push_back("-C");
        argv.push_back(dir.string());
    }
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> av;
    for (auto& a : argv) av.push_back(a.data());
    av.push_back(nullptr);
    std::vector<std::string> envs = {"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0"};
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "GIT_", 4) != 0) envs.push_back(*e);
    }
    std::vector<char*> ev;
    for (auto& e : envs) ev.push_back(e.data());
    ev.push_back(nullptr);
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return std::nullopt;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, "git", &fa, nullptr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        return std::nullopt;
    }
    std::string text;
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) > 0;) text.append(buf, static_cast<size_t>(n));
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    return text;
}

bool work_tree(const fs::path& dir) {
    static const std::vector<std::string> args = {"rev-parse", "--is-inside-work-tree"};
    auto r = git(dir, args);
    return r && trim(*r) == "true";
}

std::string git_head(const fs::path& dir) {
    return trim(git(dir, {"rev-parse", "-q", "--verify", "HEAD"}).value_or(""));
}

bool git_tracked(const fs::path& dir, const std::string& file) {
    return git(dir, {"ls-files", "--error-unmatch", "--", file}).has_value();
}

std::string git_blob(const fs::path& dir, const std::string& rev, const std::string& file) {
    if (rev.empty()) return "";
    return trim(git(dir, {"rev-parse", "-q", "--verify", rev + ":./" + file}).value_or(""));
}

// The user's identities: trust_identities from the global settings, else the global git config's user.email.
// Never a repository's own config.
std::set<std::string> identities() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(g_mu);
        ids = config().identities;
    }
    if (ids.empty()) {
        if (auto email = git("", {"config", "--global", "--get", "user.email"}); email && !trim(*email).empty()) ids.push_back(trim(*email));
    }
    std::set<std::string> out;
    for (const auto& i : ids) out.insert(lower(i));
    return out;
}

json read_store() {
    std::ifstream in(trust_path());
    if (!in) return json::object();
    json j = json::parse(in, nullptr, false);
    return j.is_object() ? j : json::object();
}

void write_private(const fs::path& path, const std::string& text, bool append) {
    fs::create_directories(path.parent_path());
    fs::path target = append ? path : fs::path(path.string() + ".tmp");
    int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0600);
    if (fd < 0) throw std::runtime_error("can't write " + target.string());
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::fchmod(fd, 0600);
    ok = ::close(fd) == 0 && ok;
    if (!ok) throw std::runtime_error("can't write " + target.string());
    if (!append) fs::rename(target, path);
}

void write_store(const json& j) {
    write_private(trust_path(), j.dump(2) + "\n", false);
}

json store_entry(const fs::path& dir) {
    json e = read_store().value("dirs", json::object()).value(dir.string(), json());
    return e.is_object() ? e : json();
}

std::string level_for(const fs::path& dir, const json& entry) {
    if (entry.is_object() && valid_trust_level(entry.value("level", ""))) return entry["level"];
    std::lock_guard lock(g_mu);
    for (const auto& [path, level] : config().levels) {
        if (absolute_dir(expand_home(path)) == dir) return level;
    }
    return config().strictness;
}

// A settings file as data, for judging what it can do: always in the sandbox, whatever the directory's level,
// since a changed file has not been approved yet.
json settings_json(const fs::path& file, const fs::path& dir) {
    if (file.extension() == ".lua") return eval_lua_data_file(file, dir, LuaTier::Sandbox, lua_data_limits().memory_mb);
    std::ifstream in(file);
    return json::parse(in, nullptr, true, true);
}

// What a project directory can do, for the relaxed tier: its permission entries, the keys that loosen the
// harness, the providers it defines, its instruction files and its tools.
json capabilities(const ProjectDir& p) {
    json caps = {{"allow", json::array()}, {"ask", json::array()}, {"deny", json::array()}, {"mode", ""}, {"harness", ""}, {"tripwire", ""},
                 {"allow_isolated", false}, {"dumb_auto_ok", false}, {"providers", json::object()}, {"error", ""},
                 {"instructions", json::array()}, {"lua_tools", json::array()}, {"script_tools", json::object()}};
    for (const auto& f : p.settings) {
        json j;
        try {
            j = settings_json(f, p.dir);
        } catch (const std::exception& e) {
            caps["error"] = rel(p.dir, f) + ": " + e.what();
            continue;
        }
        if (!j.is_object()) continue;
        auto add = [&](const char* kind, const json& list, const std::string& prefix) {
            if (!list.is_array()) return;
            for (const auto& x : list) {
                if (x.is_string()) caps[kind].push_back(prefix + x.get<std::string>());
            }
        };
        if (j.contains("permission") && j["permission"].is_object()) {
            for (const char* kind : {"allow", "ask", "deny"}) add(kind, j["permission"].value(kind, json::array()), "");
        }
        if (j.contains("allow")) add("allow", j["allow"], "run_shell:");
        for (const char* key : {"mode", "harness", "tripwire"}) {
            if (j.contains(key) && j[key].is_string()) caps[key] = j[key];
        }
        for (const char* key : {"allow_isolated", "dumb_auto_ok"}) {
            if (j.value(key, json()).is_boolean() && j[key].get<bool>()) caps[key] = true;
        }
        if (j.contains("providers") && j["providers"].is_object()) {
            for (const auto& [name, pj] : j["providers"].items()) caps["providers"][name] = pj.dump();
        }
    }
    for (const char* kind : {"allow", "ask", "deny"}) {
        std::set<std::string> s(caps[kind].begin(), caps[kind].end());
        caps[kind] = s;
    }
    for (const auto& f : p.instructions) caps["instructions"].push_back(rel(p.dir, f));
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.dir / ".maic" / "tools", ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".lua") caps["lua_tools"].push_back(e.path().filename().string());
        if (e.is_directory(ec) && fs::is_regular_file(e.path() / "tool.json", ec)) {
            std::ifstream in(e.path() / "tool.json");
            json m = json::parse(in, nullptr, false);
            json shape = m.is_object() ? json{{"run", m.value("run", json())}, {"reads", m.value("reads", json())}, {"writes", m.value("writes", json())}} : json("unreadable");
            caps["script_tools"][e.path().filename().string()] = shape.dump();
        }
    }
    return caps;
}

int mode_rank(const std::string& m) {
    if (m == "plan") return 0;
    if (m == "manual") return 1;
    if (m == "auto-read") return 2;
    if (m == "edit") return 3;
    return 4;  // auto, or anything unknown
}

// What `now` can do that `old` could not.
std::vector<std::string> widenings(const json& old_caps, const json& now) {
    json old = old_caps.is_object() ? old_caps : json::object();
    std::vector<std::string> out;
    auto set = [](const json& j, const char* key) {
        std::set<std::string> s;
        for (const auto& x : j.value(key, json::array())) {
            if (x.is_string()) s.insert(x.get<std::string>());
        }
        return s;
    };
    for (const auto& x : set(now, "allow")) {
        if (!set(old, "allow").count(x)) out.push_back("a new permission.allow entry: " + x);
    }
    for (const char* kind : {"ask", "deny"}) {
        for (const auto& x : set(old, kind)) {
            if (!set(now, kind).count(x)) out.push_back(std::string("a removed permission.") + kind + " entry: " + x);
        }
    }
    std::string om = old.value("mode", ""), nm = now.value("mode", "");
    if (om != nm && (nm.empty() || (om.empty() ? mode_rank(nm) > 1 : mode_rank(nm) > mode_rank(om)))) out.push_back("mode " + (om.empty() ? "unset" : om) + " -> " + (nm.empty() ? "unset" : nm));
    if (now.value("harness", "") == "dumb" && old.value("harness", "") != "dumb") out.push_back("harness = \"dumb\" (no reviewer)");
    std::string ot = old.value("tripwire", ""), nt = now.value("tripwire", "");
    if (nt != ot && !nt.empty() && nt != "machine") out.push_back("tripwire = \"" + nt + "\"");
    for (const char* key : {"allow_isolated", "dumb_auto_ok"}) {
        if (now.value(key, false) && !old.value(key, false)) out.push_back(std::string(key) + " = true");
    }
    json op = old.value("providers", json::object()), np = now.value("providers", json::object());
    for (const auto& [name, v] : np.items()) {
        if (!op.contains(name) || op[name] != v) out.push_back("provider " + name + " is new or changed");
    }
    if (!now.value("error", "").empty() && now.value("error", "") != old.value("error", "")) out.push_back("settings that do not load: " + now.value("error", ""));
    for (const char* key : {"instructions", "lua_tools"}) {
        for (const auto& x : set(now, key)) {
            if (!set(old, key).count(x)) out.push_back(std::string(key[0] == 'i' ? "a new instruction file: " : "a new tool: .maic/tools/") + x);
        }
    }
    json os = old.value("script_tools", json::object()), ns = now.value("script_tools", json::object());
    for (const auto& [name, v] : ns.items()) {
        if (!os.contains(name)) out.push_back("a new tool: .maic/tools/" + name + "/");
        else if (os[name] != v) out.push_back("tool " + name + "'s manifest changed its run, reads or writes");
    }
    return out;
}

// Why the standard tier asks again about the changed files; empty when every change is the user's own.
std::vector<std::string> not_own(const ProjectDir& p, const json& entry, const std::vector<fs::path>& changed) {
    if (!work_tree(p.dir)) return {"not a git working tree, so your own edits can't be told from anyone else's"};
    json g = entry.value("git", json());
    if (!g.is_object()) return {"it was not a git working tree when it was trusted"};
    std::string old = g.value("head", ""), now = git_head(p.dir);
    std::set<std::string> was_untracked;
    for (const auto& f : g.value("untracked", json::array())) was_untracked.insert(f.get<std::string>());
    std::set<std::string> ids = identities();
    std::vector<std::string> out;
    for (const auto& path : changed) {
        std::string file = rel(p.dir, path);
        std::error_code ec;
        if (fs::exists(path, ec) && !git_tracked(p.dir, file)) {
            if (!was_untracked.count(file)) out.push_back("a new untracked file: " + file);
            continue;  // an untracked file it had then: only a local edit changes it
        }
        if (git_blob(p.dir, old, file) == git_blob(p.dir, now, file)) continue;  // the commits did not change it: the edit is uncommitted
        if (old.empty() || now.empty()) {
            out.push_back("commits changed " + file + " since it was trusted");
            continue;
        }
        if (!git(p.dir, {"merge-base", "--is-ancestor", old, now})) {
            out.push_back("the history moved (a checkout or reset) and changed " + file);
            continue;
        }
        auto log = git(p.dir, {"log", "--full-history", "--format=%ae", old + ".." + now, "--", file});
        if (!log || trim(*log).empty()) {
            out.push_back("commits changed " + file + " that could not be read");
            continue;
        }
        std::istringstream lines(*log);
        for (std::string email; std::getline(lines, email);) {
            if (!email.empty() && !ids.count(lower(email))) {
                out.push_back("a commit by " + email + " changed " + file);
                break;
            }
        }
    }
    return out;
}

// The hash over every file (path and content, sorted) and each file's own, by its path inside the directory.
std::pair<std::string, json> contents(const ProjectDir& p) {
    std::vector<std::string> parts;
    json files = json::object();
    for (const auto& f : p.files()) {
        std::string text = read_all(f);
        parts.insert(parts.end(), {f.string(), std::string(1, '\0'), text, std::string(1, '\0')});
        files[rel(p.dir, f)] = sha256({text});
    }
    return {sha256(parts), files};
}

// The record trust.json keeps for a trusted directory: its strictness tier and Lua level are `level` and `lua`
// when given, else what `old` had (the Lua level defaults to full: "trust it" means as yourself).
json record(const ProjectDir& p, const json& old, const std::string& level, const std::string& lua) {
    auto [hash, files] = contents(p);
    json e = {{"state", "trusted"}, {"hash", hash}, {"files", files}, {"caps", capabilities(p)}, {"at", utc_now()}};
    std::string keep_level = !level.empty() ? level : old.is_object() ? old.value("level", "") : "";
    if (!keep_level.empty()) e["level"] = keep_level;
    std::string keep_lua = !lua.empty() ? lua : old.is_object() ? old.value("lua", "full") : "full";
    e["lua"] = parse_lua_tier(keep_lua) ? keep_lua : "full";
    if (work_tree(p.dir)) {
        json untracked = json::array();
        for (const auto& f : p.files()) {
            if (!git_tracked(p.dir, rel(p.dir, f))) untracked.push_back(rel(p.dir, f));
        }
        e["git"] = {{"head", git_head(p.dir)}, {"untracked", untracked}};
    }
    return e;
}

void store_record(const fs::path& dir, json entry) {
    json store = read_store();
    if (!store.contains("dirs") || !store["dirs"].is_object()) store["dirs"] = json::object();
    store["dirs"][dir.string()] = std::move(entry);
    write_store(store);
}

void forget(const fs::path& dir) {
    json store = read_store();
    if (store.contains("dirs") && store["dirs"].is_object() && store["dirs"].contains(dir.string())) {
        store["dirs"].erase(dir.string());
        write_store(store);
    }
}

void require_local(Origin origin) {
    if (origin == Origin::Remote) throw std::runtime_error("a remote request cannot grant or refuse trust without step-up verification (POST /api/trust)");
}

// PATH as typed (relative to the workspace, ~ expanded), checked to be a directory that can be a project.
fs::path project_arg(const fs::path& workspace, const std::string& path) {
    std::string p = expand_home(path);
    fs::path dir = absolute_dir(fs::path(p).is_absolute() ? fs::path(p) : workspace / p);
    if (never_project(dir)) throw std::runtime_error(dir.string() + " is never a project (it is $HOME or /); your own settings and instructions go in ~/.config/maic/");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error(dir.string() + " is not a directory");
    return dir;
}

std::string trust_name(Trust t) {
    switch (t) {
        case Trust::Trusted: return "trusted";
        case Trust::Changed: return "changed since it was trusted";
        case Trust::NotNow: return "untrusted this session";
        case Trust::Never: return "never trusted";
        case Trust::Unknown: return "not trusted yet";
    }
    return "";
}

std::string file_list(const fs::path& dir, const std::vector<fs::path>& files) {
    std::string s;
    for (const auto& f : files) s += (s.empty() ? "" : ", ") + rel(dir, f);
    return s;
}

}  // namespace

std::vector<fs::path> ProjectDir::files() const {
    std::vector<fs::path> all = settings;
    all.insert(all.end(), instructions.begin(), instructions.end());
    all.insert(all.end(), tools.begin(), tools.end());
    std::sort(all.begin(), all.end());
    return all;
}

void set_trust_config(TrustConfig c) {
    std::lock_guard lock(g_mu);
    config() = std::move(c);
}

bool valid_trust_level(const std::string& level) {
    return level == "strict" || level == "standard" || level == "relaxed";
}

fs::path trust_path() {
    return state_dir() / "trust.json";
}

fs::path trust_audit_path() {
    return state_dir() / "trust-audit.log";
}

bool never_project(const fs::path& dir) {
    fs::path d = absolute_dir(dir);
    return d == d.root_path() || d == home_path();
}

ProjectDir project_dir(const fs::path& dir) {
    ProjectDir p;
    p.dir = absolute_dir(dir);
    std::error_code ec;
    for (const char* name : {"settings.lua", "settings.json", "settings.local.lua", "settings.local.json"}) {
        if (fs::is_regular_file(p.dir / ".maic" / name, ec)) p.settings.push_back(p.dir / ".maic" / name);
    }
    for (const char* name : {"MAIC.md", "AGENTS.md"}) {
        if (fs::is_regular_file(p.dir / name, ec)) p.instructions.push_back(p.dir / name);
    }
    fs::path tools = p.dir / ".maic" / "tools";
    p.tool_dir = fs::is_directory(tools, ec);
    if (p.tool_dir) {
        for (auto it = fs::recursive_directory_iterator(tools, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) p.tools.push_back(it->path());
        }
        std::sort(p.tools.begin(), p.tools.end());
    }
    return p;
}

std::vector<fs::path> config_chain(const fs::path& workspace) {
    TrustConfig c;
    {
        std::lock_guard lock(g_mu);
        c = config();
    }
    fs::path home = home_path();
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::path d = absolute_dir(workspace); !d.empty(); d = d.parent_path()) {
        if (d == home || d == d.root_path()) break;
        out.push_back(d);
        auto r = d.lexically_relative(home);
        if (home.empty() || r.empty() || *r.begin() == "..") break;  // outside $HOME: the workspace alone
        if (c.bound == "home") continue;
        bool root = false;
        for (const auto& m : c.project_markers) root = root || fs::exists(d / m, ec);
        if (root) break;  // the project root: nothing above it is read
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<ProjectDir> project_dirs(const fs::path& workspace) {
    std::vector<ProjectDir> out;
    for (const auto& d : config_chain(workspace)) {
        if (ProjectDir p = project_dir(d); !p.empty()) out.push_back(std::move(p));
    }
    return out;
}

TrustStatus trust_status(const ProjectDir& p) {
    json entry = store_entry(p.dir);
    TrustStatus s{Trust::Unknown, level_for(p.dir, entry), {}, {}};
    if (never_project(p.dir)) return s.trust = Trust::Never, s;
    if (auto a = session_answer(p.dir)) return s.trust = *a ? Trust::Trusted : Trust::NotNow, s;
    if (!entry.is_object()) return s;
    if (entry.value("state", "") == "never") return s.trust = Trust::Never, s;
    if (entry.value("state", "") != "trusted") return s;
    auto [hash, files] = contents(p);
    if (hash == entry.value("hash", "")) return s.trust = Trust::Trusted, s;
    json old = entry.value("files", json::object());
    for (const auto& [name, h] : files.items()) {
        if (!old.contains(name) || old[name] != h) s.changed.push_back(p.dir / name);
    }
    for (const auto& [name, h] : old.items()) {
        if (!files.contains(name)) s.changed.push_back(p.dir / name);
    }
    std::sort(s.changed.begin(), s.changed.end());
    if (s.level == "strict") s.reasons = {"strict: every change is asked about"};
    else if (s.level == "relaxed") {
        s.reasons = widenings(entry.value("caps", json()), capabilities(p));
        // A fully trusted directory's settings.lua runs as you: its code can't be judged as data.
        for (const auto& f : s.changed) {
            if (f.extension() == ".lua" && f.parent_path() == p.dir / ".maic" && trust_lua_tier(p.dir) == LuaTier::Full) {
                s.reasons.push_back(rel(p.dir, f) + " changed, and it runs with full Lua (trusted fully)");
            }
        }
    }
    else s.reasons = not_own(p, entry, s.changed);
    s.trust = s.reasons.empty() ? Trust::Trusted : Trust::Changed;
    return s;
}

bool trusted(const fs::path& dir) {
    fs::path d = absolute_dir(dir);
    if (never_project(d)) return false;
    if (auto a = session_answer(d)) return *a;
    return trust_status(project_dir(d)).trust == Trust::Trusted;
}

void trust_dir(const ProjectDir& p, Origin origin, const std::string& level, const std::string& lua) {
    require_local(origin);
    if (never_project(p.dir)) throw std::runtime_error(p.dir.string() + " is never a project (it is $HOME or /)");
    if (!level.empty() && !valid_trust_level(level)) throw std::runtime_error("--level takes strict, standard or relaxed");
    if (!lua.empty() && !parse_lua_tier(lua)) throw std::runtime_error("--lua takes full, sandbox or restricted");
    store_record(p.dir, record(p, store_entry(p.dir), level, lua));
}

LuaTier trust_lua_tier(const fs::path& dir) {
    fs::path d = absolute_dir(dir);
    {
        std::lock_guard lock(g_mu);
        if (auto it = session_lua().find(d.string()); it != session_lua().end()) return it->second;
    }
    json e = store_entry(d);
    return parse_lua_tier(e.is_object() ? e.value("lua", "full") : "full").value_or(LuaTier::Full);
}

void never_trust(const fs::path& dir, Origin origin) {
    require_local(origin);
    fs::path d = absolute_dir(dir);
    store_record(d, {{"state", "never"}, {"at", utc_now()}});
}

void untrust(const fs::path& dir) {
    fs::path d = absolute_dir(dir);
    forget(d);
    set_session_answer(d, false);
}

void trust_for_session(const fs::path& dir, LuaTier lua) {
    set_session_answer(absolute_dir(dir), true);
    std::lock_guard lock(g_mu);
    session_lua()[absolute_dir(dir).string()] = lua;
}

void not_now(const fs::path& dir) {
    set_session_answer(absolute_dir(dir), false);
}

std::vector<std::string> settle_trust(const fs::path& workspace) {
    std::vector<std::string> notes;
    for (const auto& p : project_dirs(workspace)) {
        if (session_answer(p.dir)) continue;
        TrustStatus s = trust_status(p);
        if (s.trust == Trust::Trusted && !s.changed.empty()) {
            store_record(p.dir, record(p, store_entry(p.dir), "", ""));
            notes.push_back(std::string(s.level == "relaxed" ? "edits that widen nothing" : "your own edits") + " passed in " + p.dir.string() + ": " +
                            file_list(p.dir, s.changed) + " (" + tier_hint(p.dir, s.level) + ")");
        }
        set_session_answer(p.dir, s.trust == Trust::Trusted);
    }
    return notes;
}

std::vector<ProjectDir> trust_to_ask(const fs::path& workspace) {
    std::vector<ProjectDir> out;
    for (auto& p : project_dirs(workspace)) {
        Trust t = trust_status(p).trust;
        if (t == Trust::Unknown || t == Trust::Changed) out.push_back(std::move(p));
    }
    return out;
}

std::string describe_project(const ProjectDir& p, const std::string& indent) {
    std::string out;
    if (!p.settings.empty()) out += indent + "settings:     " + file_list(p.dir, p.settings) + "\n";
    if (!p.instructions.empty()) out += indent + "instructions: " + file_list(p.dir, p.instructions) + "\n";
    if (p.tool_dir) {
        std::vector<fs::path> top;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(p.dir / ".maic" / "tools", ec)) top.push_back(e.path());
        std::sort(top.begin(), top.end());
        out += indent + "tools:        " + (top.empty() ? std::string(".maic/tools/ (empty)") : file_list(p.dir, top)) + " (" + std::to_string(p.tools.size()) + " file" +
               (p.tools.size() == 1 ? "" : "s") + ")\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

std::string lua_hint(const fs::path& dir) {
    std::string cmd = "`maic trust " + dir.string() + " --lua ";
    switch (trust_lua_tier(dir)) {
        case LuaTier::Full: return "Lua full: its settings.lua runs as you; " + cmd + "sandbox` runs it in a child process that cannot reach the system";
        case LuaTier::Sandbox: return "Lua sandbox: its settings.lua runs in a child process that cannot reach the system; " + cmd + "full` runs it as you";
        case LuaTier::Restricted: return "Lua restricted: its settings.lua runs in a restricted state in MAIC's own process; " + cmd + "sandbox` for the child process";
    }
    return "";
}

std::string tier_hint(const fs::path& dir, const std::string& level) {
    std::string cmd = "`maic trust " + dir.string() + " --level ";
    if (level == "strict") return "tier strict: every change is asked about; " + cmd + "standard` lets your own edits pass";
    if (level == "relaxed") return "tier relaxed: only changes that widen what it can do are asked about; " + cmd + "standard` or " + cmd + "strict` to be asked about more";
    return "tier standard; " + cmd + "relaxed` to stop asking about your own edits, " + cmd + "strict` to be asked about every change";
}

std::vector<std::string> trust_notices(const fs::path& workspace) {
    std::vector<std::string> out;
    fs::path ws = absolute_dir(workspace);
    if (never_project(ws)) {
        if (ProjectDir p = project_dir(ws); !p.empty()) {
            out.push_back(ws.string() + " is " + (ws == ws.root_path() ? "/" : "$HOME") + ", never a project: its files are ignored\n" + describe_project(p, "  ") +
                          "\nyour own settings, instructions and tools go in ~/.config/maic/");
        }
        return out;
    }
    for (const auto& p : project_dirs(ws)) {
        TrustStatus s = trust_status(p);
        if (s.trust == Trust::Trusted) continue;
        std::string line = "untrusted (" + trust_name(s.trust) + "): " + p.dir.string() + "\n" + describe_project(p, "  ");
        if (!s.changed.empty()) line += "\n  changed:      " + file_list(p.dir, s.changed);
        for (const auto& r : s.reasons) line += "\n  asks because: " + r;
        line += "\nits settings are not applied, its instructions not given to the model, its tools not loaded. :trust (or maic trust " + p.dir.string() + ") trusts it; " +
                tier_hint(p.dir, s.level);
        out.push_back(line);
    }
    return out;
}

std::string grant_trust(const fs::path& workspace, const std::string& path, Origin origin, const std::string& level, const std::string& lua) {
    require_local(origin);
    if (!level.empty() && !valid_trust_level(level)) throw std::runtime_error("--level takes strict, standard or relaxed");
    if (!lua.empty() && !parse_lua_tier(lua)) throw std::runtime_error("--lua takes full, sandbox or restricted");
    std::vector<ProjectDir> dirs;
    if (!path.empty()) {
        ProjectDir p = project_dir(project_arg(workspace, path));
        if (p.empty()) throw std::runtime_error("nothing to trust in " + p.dir.string() + ": no .maic/settings.*, MAIC.md, AGENTS.md or .maic/tools/");
        dirs.push_back(std::move(p));
    } else {
        for (auto& p : project_dirs(workspace)) {
            if (!level.empty() || !lua.empty() || trust_status(p).trust != Trust::Trusted) dirs.push_back(std::move(p));
        }
        if (dirs.empty()) {
            std::string out = "nothing to trust here:";
            for (const auto& p : project_dirs(workspace)) out += "\n  " + p.dir.string() + "  (" + trust_name(trust_status(p).trust) + ")";
            if (project_dirs(workspace).empty()) out += " no directory from under $HOME down to " + absolute_dir(workspace).string() + " holds .maic/, MAIC.md or AGENTS.md";
            return out;
        }
    }
    std::string out;
    for (const auto& p : dirs) {
        trust_dir(p, origin, level, lua);
        set_session_answer(p.dir, true);  // a file edited later in this session does not undo what was just granted
        out += (out.empty() ? "" : "\n") + std::string("trusted ") + p.dir.string() + "\n" + describe_project(p, "  ") + "\n  " + lua_hint(p.dir) + "\n  " +
               tier_hint(p.dir, trust_status(p).level);
    }
    return out + "\nsettings and tools apply when MAIC next starts there; instructions from the next turn";
}

std::string revoke_trust(const fs::path& workspace, const std::string& path) {
    std::vector<fs::path> dirs;
    if (!path.empty()) dirs.push_back(project_arg(workspace, path));
    else {
        for (const auto& p : project_dirs(workspace)) dirs.push_back(p.dir);
    }
    if (dirs.empty()) return "no project directory here to untrust";
    std::string out;
    for (const auto& d : dirs) {
        untrust(d);
        out += (out.empty() ? "" : "\n") + std::string("untrusted ") + d.string();
    }
    return out + "\nasked about again when MAIC next starts there";
}

std::string trust_listing() {
    json dirs = read_store().value("dirs", json::object());
    if (dirs.empty()) return "no directory is trusted (" + trust_path().string() + ")";
    std::string out;
    for (const auto& [dir, e] : dirs.items()) {
        if (e.value("state", "") == "never") {
            out += (out.empty() ? "" : "\n") + std::string("never    ") + dir + "  (" + e.value("at", "") + ")";
            continue;
        }
        TrustStatus s = trust_status(project_dir(dir));
        std::string line = "trusted  " + dir + "  (" + s.level + ", Lua " + lua_tier_name(trust_lua_tier(dir)) + ", " + e.value("at", "") + ")";
        if (s.trust == Trust::Changed) line += "\n         changed since: " + file_list(dir, s.changed);
        else if (!s.changed.empty()) line += "\n         changed, passes its tier: " + file_list(dir, s.changed);
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

std::string trust_command(const std::string& command, const std::vector<std::string>& args, const fs::path& workspace) {
    std::string path, level, lua;
    bool list = false;
    for (size_t i = 0; i < args.size(); ++i) {
        bool takes = command == "trust" && (args[i] == "--level" || args[i] == "--lua");
        if (args[i] == "--list" || args[i] == "-l") list = true;
        else if (takes) {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] == "--level" ? "--level takes strict, standard or relaxed" : "--lua takes full, sandbox or restricted");
            (args[i] == "--level" ? level : lua) = args[i + 1];
            ++i;
        } else if (args[i].rfind("--level=", 0) == 0 && command == "trust") level = args[i].substr(8);
        else if (args[i].rfind("--lua=", 0) == 0 && command == "trust") lua = args[i].substr(6);
        else if (path.empty() && (args[i].empty() || args[i][0] != '-')) path = args[i];
        else throw std::runtime_error("usage: " + command + (command == "trust" ? " [PATH] [--lua full|sandbox|restricted] [--level strict|standard|relaxed] | trust --list" : " [PATH]"));
    }
    if (list) return trust_listing();
    if (command == "untrust") return revoke_trust(workspace, path);
    return grant_trust(workspace, path, Origin::Local, level, lua);
}

void ask_trust(const fs::path& workspace, std::istream& in, std::ostream& out) {
    for (const auto& p : trust_to_ask(workspace)) {
        TrustStatus s = trust_status(p);
        out << "\nMAIC: " << (s.trust == Trust::Changed ? "a trusted project directory changed" : "a project directory you have not trusted") << "\n  " << p.dir.string() << "\n"
            << describe_project(p, "  ") << "\n";
        if (s.trust == Trust::Unknown && !list_sessions(p.dir).empty()) out << "  MAIC used this directory before this check existed.\n";
        if (!s.changed.empty()) out << "  changed:      " << file_list(p.dir, s.changed) << "\n";
        for (const auto& r : s.reasons) out << "  asks because: " << r << "\n";
        out << "  " << tier_hint(p.dir, s.level) << "\n"
            << "Untrusted, its settings are not applied, its instructions are not given to the model and its tools are not loaded.\n"
            << "  [t] trust fully: its Lua runs as you\n"
            << "  [s] trust sandboxed: its Lua runs in a child process that cannot reach the system\n"
            << "  [n] not now (untrusted this session)   [v] never (remember)  > " << std::flush;
        std::string answer;
        if (!std::getline(in, answer)) answer.clear();
        answer = lower(trim(answer));
        if (answer == "t" || answer == "trust" || answer == "y" || answer == "yes" || answer == "s" || answer == "sandbox") {
            bool sandboxed = answer == "s" || answer == "sandbox";
            trust_dir(p, Origin::Local, "", sandboxed ? "sandbox" : "full");
            out << "trusted " << (sandboxed ? "sandboxed" : "fully") << ": " << p.dir.string() << "\n";
        } else if (answer == "v" || answer == "never") {
            never_trust(p.dir, Origin::Local);
            out << "never: " << p.dir.string() << " stays untrusted (maic trust " << p.dir.string() << " changes that)\n";
        } else {
            not_now(p.dir);
            out << "not now: untrusted for this session\n";
        }
    }
}

void set_step_up_verifier(StepUpVerifier v) {
    std::lock_guard lock(g_mu);
    verifier() = std::move(v);
}

std::string remote_trust_change(const std::string& device, const std::string& proof, const std::string& action, const std::string& path, const std::string& level,
                                const std::string& lua) {
    auto audit = [&](const std::string& outcome) {
        write_private(trust_audit_path(), utc_now() + " device=" + device + " action=" + action + " path=" + path + (level.empty() ? "" : " level=" + level) +
                                              (lua.empty() ? "" : " lua=" + lua) + " " + outcome + "\n", true);
    };
    auto refuse = [&](const std::string& why) {
        audit("refused: " + why);
        throw std::runtime_error(why);
    };
    StepUpVerifier v;
    {
        std::lock_guard lock(g_mu);
        v = verifier();
    }
    if (!v) refuse("step-up verification is not available until accounts land (docs/design/accounts.md)");
    if (proof.empty()) refuse("a step_up proof is required to change trust from a remote device");
    if (!v(device, proof)) refuse("step-up verification failed");
    if (!fs::path(path).is_absolute()) refuse("path must be absolute");
    fs::path dir = absolute_dir(path);
    if (never_project(dir)) refuse(dir.string() + " is never a project (it is $HOME or /)");
    if (!level.empty() && !valid_trust_level(level)) refuse("level must be strict, standard or relaxed");
    if (!lua.empty() && !parse_lua_tier(lua)) refuse("lua must be full, sandbox or restricted");
    std::string done;
    if (action == "trust") {
        ProjectDir p = project_dir(dir);
        if (p.empty()) refuse("nothing to trust in " + dir.string());
        store_record(dir, record(p, store_entry(dir), level, lua));
        done = "trusted " + dir.string() + " (Lua " + lua_tier_name(trust_lua_tier(dir)) + ")";
    } else if (action == "untrust") {
        forget(dir);
        done = "untrusted " + dir.string();
    } else if (action == "never") {
        store_record(dir, {{"state", "never"}, {"at", utc_now()}});
        done = "never trusted " + dir.string();
    } else if (action == "level" || action == "lua") {
        json e = store_entry(dir);
        if (!e.is_object() || e.value("state", "") != "trusted") refuse(dir.string() + " is not trusted");
        std::string value = action == "level" ? level : lua;
        if (value.empty()) refuse(action + " is required");
        e[action] = value;
        store_record(dir, e);
        done = dir.string() + (action == "level" ? " is now tier " : " now runs its settings Lua ") + value;
    } else {
        refuse("action must be trust, untrust, never, level or lua");
    }
    audit("done");
    return done;
}

}  // namespace maic
