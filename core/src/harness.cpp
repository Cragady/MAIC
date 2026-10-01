#include "maic/harness.hpp"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>

#include "maic/paths.hpp"
#include "maic/agent_def.hpp"

#include <cstdlib>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace maic {

namespace fs = std::filesystem;

namespace {

struct Pattern {
    std::regex re;
    const char* why;
};

bool helper_read_only(const std::string& command);

// Commands that trip the harness on sight. Checked even in plan mode, before any approval prompt.
const std::vector<Pattern>& trip_patterns() {
    static const std::vector<Pattern> patterns = [] {
        auto rx = [](const char* s) { return std::regex(s, std::regex::ECMAScript | std::regex::icase); };
        return std::vector<Pattern>{
            {rx(R"((^|[;&|`(\s])(sudo|su|doas|pkexec|run0)(\s|$))"), "privilege escalation"},
            {rx(R"(\brm\s+(-\S+\s+)*(/\*?|~/?\*?|\$HOME/?\*?)(\s|$))"), "rm on / or the home directory"},
            {rx(R"(\b(mkfs(\.\w+)?|wipefs|shred|fdisk|sfdisk|parted|blkdiscard)\b)"), "disk-destroying tool"},
            {rx(R"(\bdd\b.*\bof=/dev/)"), "dd onto a device"},
            {rx(R"(>\s*/dev/(sd|nvme|hd|vd|mmcblk))"), "write to a raw disk"},
            {rx(R"(:\s*\(\s*\)\s*\{.*\|.*&\s*\}\s*;)"), "fork bomb"},
            {rx(R"(\b(chmod|chown|chgrp)\s+(-\S+\s+)*-R\b.*\s(/|~|\$HOME)(\s|$))"), "recursive permission change on / or home"},
            {rx(R"(\b(curl|wget)\b.*\|\s*(sudo\s+)?(ba|z|da|k)?sh\b)"), "piping a download into a shell"},
            {rx(R"(\bcrontab\s+(?!-l\b)\S)"), "changing scheduled jobs"},
            {rx(R"(\bsystemctl\s+(--user\s+)?(enable|disable|mask|unmask|start|stop|restart|reload|daemon-reload|edit|link|set-default|isolate|poweroff|reboot|halt)\b)"), "changing system services"},
            {rx(R"(\b(reboot|poweroff|shutdown|halt|init\s+[06])\b)"), "shutting the machine down"},
            {rx(R"(\.ssh\b|\.gnupg\b|sudoers|/var/lib/maic|maic-lock)"), "touching credentials or the harness"},
        };
    }();
    return patterns;
}

bool under(const fs::path& p, const fs::path& root) {
    auto pi = p.begin();
    for (auto ri = root.begin(); ri != root.end(); ++ri, ++pi) {
        if (ri->empty()) {
            continue;  // trailing slash
        }
        if (pi == p.end() || *pi != *ri) {
            return false;
        }
    }
    return true;
}

bool under_any(const fs::path& p, const std::vector<fs::path>& roots) {
    for (const auto& r : roots) {
        if (under(p, r)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> split_words(const std::string& segment) {
    std::istringstream in(segment);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    return words;
}

bool read_only_segment(const std::string& segment) {
    static const std::set<std::string> programs = {
        "ls", "cat", "head", "tail", "grep", "egrep", "fgrep", "rg", "ag", "find", "fd", "tree", "wc", "file",
        "stat", "du", "df", "pwd", "echo", "printf", "which", "type", "whoami", "id", "uname", "date", "sort",
        "uniq", "cut", "tr", "sed", "jq", "diff", "cmp", "md5sum", "sha1sum", "sha256sum", "basename", "dirname",
        "realpath", "readlink", "nl", "column", "xxd", "hexdump", "strings", "true", "false", "test", "[", "git",
        "printenv", "hostname", "nproc", "lscpu", "free", "uptime", "ps", "pgrep", "tac", "rev", "seq", "expr",
        "bat", "env", "command",
    };
    static const std::set<std::string> git_read = {"status", "log", "diff", "show", "blame", "ls-files", "rev-parse",
                                                   "describe", "shortlog", "grep", "remote", "branch", "tag", "stash",
                                                   "config", "ls-tree", "cat-file", "rev-list", "reflog", "worktree",
                                                   "submodule", "check-ignore", "for-each-ref", "merge-base", "name-rev",
                                                   "count-objects", "var", "diff-tree", "show-ref", "symbolic-ref"};
    // Toolchains whose only looking-only invocation is printing their version or help: `python3 -c ...` runs code.
    static const std::set<std::string> toolchains = {
        "python", "python3", "node", "npm", "npx", "bun", "deno", "cargo", "rustc", "cmake", "make", "ninja", "meson",
        "gcc", "g++", "cc", "c++", "clang", "clang++", "go", "java", "javac", "ruby", "perl", "pip", "pip3", "uv", "tsc",
        "bash", "zsh", "sh", "docker", "clang-format", "clang-tidy", "ctest",
    };
    auto words = split_words(segment);
    if (words.empty()) return false;
    const std::string& prog = words[0];
    if (toolchains.count(prog)) {
        if (prog == "go") return words.size() == 2 && (words[1] == "version" || words[1] == "help" || words[1] == "env");
        if (prog == "java" && words.size() == 2 && words[1] == "-version") return true;
        if (prog == "ctest" && words.size() == 2 && words[1] == "-N") return true;
        return words.size() == 2 && (words[1] == "--version" || words[1] == "-V" || words[1] == "--help" || words[1] == "-h" ||
                                     (words[1] == "-v" && (prog == "node" || prog == "npm" || prog == "bun" || prog == "deno")));
    }
    if (!programs.count(prog)) return false;
    size_t operands = 0;
    for (size_t i = 1; i < words.size(); ++i) {
        const auto& w = words[i];
        if (prog == "find" && (w == "-delete" || w.rfind("-exec", 0) == 0 || w.rfind("-ok", 0) == 0 || w.rfind("-fprint", 0) == 0 || w == "-fls")) return false;
        if (prog == "sed" && (w == "-i" || w.rfind("-i", 0) == 0 || w == "--in-place")) return false;
        if ((prog == "sort" || prog == "tree") && (w == "-o" || w.rfind("-o", 0) == 0 || w.rfind("--output", 0) == 0)) return false;
        if (prog == "date" && (w == "-s" || w.rfind("--set", 0) == 0)) return false;
        if (w.empty() || w[0] != '-') ++operands;
    }
    // `uniq IN OUT` writes OUT; `env` with anything but flags runs a program or sets a variable; `command X` runs X.
    if (prog == "uniq" && operands > 1) return false;
    if (prog == "env" && words.size() > 1) return false;
    if (prog == "command" && (words.size() != 3 || (words[1] != "-v" && words[1] != "-V"))) return false;
    if (prog == "hostname" && operands > 0) return false;
    if (prog == "git") {
        if (words.size() < 2 || !git_read.count(words[1])) return false;
        // Subcommands that also write: only their listing forms.
        if ((words[1] == "branch" || words[1] == "tag" || words[1] == "remote") && words.size() > 2) {
            for (size_t i = 2; i < words.size(); ++i) {
                if (words[i] != "-a" && words[i] != "-v" && words[i] != "-vv" && words[i] != "-r" && words[i] != "--list" && words[i] != "-l") return false;
            }
        }
        if (words[1] == "stash" && (words.size() < 3 || (words[2] != "list" && words[2] != "show"))) return false;
        if (words[1] == "worktree" && (words.size() < 3 || words[2] != "list")) return false;
        if (words[1] == "submodule" && (words.size() < 3 || words[2] != "status")) return false;
        if (words[1] == "reflog" && words.size() > 2 && words[2] != "show" && words[2][0] != '-') return false;
        if (words[1] == "config") {
            if (words.size() < 3) return false;
            for (size_t i = 2; i < words.size(); ++i) {
                if (words[i] == "--list" || words[i] == "-l" || words[i].rfind("--get", 0) == 0) return true;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

bool is_simple_command(const std::string& command) {
    if (command.find_first_of(";&|<>`\n\r") != std::string::npos) return false;
    return command.find("$(") == std::string::npos;
}

bool is_read_only_command(const std::string& command) {
    // No redirection, substitution, backgrounding or multi-line scripts.
    for (const char* bad : {">", "<", "`", "$(", "\n", "\r"}) {
        if (command.find(bad) != std::string::npos) return false;
    }
    std::string rest = command;
    for (const char* sep : {"&&", "||"}) {
        for (size_t p; (p = rest.find(sep)) != std::string::npos;) rest.replace(p, 2, ";");
    }
    if (rest.find('&') != std::string::npos) return false;
    for (char& c : rest) {
        if (c == '|') c = ';';
    }
    std::istringstream in(rest);
    bool any = false;
    for (std::string segment; std::getline(in, segment, ';');) {
        if (segment.find_first_not_of(" \t") == std::string::npos) continue;
        if (!read_only_segment(segment)) return false;
        any = true;
    }
    return any;
}

std::string_view mode_name(Mode mode) {
    switch (mode) {
        case Mode::Manual: return "manual";
        case Mode::AutoRead: return "auto-read";
        case Mode::Edit: return "edit";
        case Mode::Auto: return "auto";
        case Mode::Plan: return "plan";
    }
    return "manual";
}

std::optional<Mode> parse_mode(std::string_view name) {
    for (Mode m : {Mode::Manual, Mode::AutoRead, Mode::Edit, Mode::Auto, Mode::Plan}) {
        if (mode_name(m) == name) {
            return m;
        }
    }
    return std::nullopt;
}

Mode next_mode(Mode mode) {
    switch (mode) {
        case Mode::Manual: return Mode::AutoRead;
        case Mode::AutoRead: return Mode::Edit;
        case Mode::Edit: return Mode::Auto;
        case Mode::Auto: return Mode::Plan;
        case Mode::Plan: return Mode::Manual;
    }
    return Mode::Manual;
}

namespace {

int mode_rank(Mode mode) {
    switch (mode) {
        case Mode::Plan: return 0;
        case Mode::Manual: return 1;
        case Mode::AutoRead: return 2;
        case Mode::Edit: return 3;
        case Mode::Auto: return 4;
    }
    return 1;
}

}  // namespace

Mode narrower_mode(Mode a, Mode b) {
    return mode_rank(a) <= mode_rank(b) ? a : b;
}

Harness::~Harness() {
    for (auto& re : forbid_res_) regfree(&re.second);
}

Harness::Harness(fs::path workspace) : workspace_(fs::weakly_canonical(workspace)) {
    fs::path home = std::getenv("HOME");
    for (const char* p : {".ssh", ".gnupg", ".aws", ".kube", ".docker", ".password-store", ".local/share/keyrings", ".ollama"}) {
        secret_paths_.push_back(home / p);
    }
    secret_paths_.push_back("/var/lib/maic");
    for (const char* p : {"/etc", "/boot", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt", "/var", "/root", "/sys", "/proc", "/dev"}) {
        system_paths_.push_back(p);
    }
    for (const char* p : {"bin", ".local/bin", ".zshrc", ".zprofile", ".zshenv", ".bashrc", ".bash_profile", ".profile", ".config/autostart", ".config/systemd"}) {
        sensitive_paths_.push_back(home / p);
    }
    fs::path maic = root_dir();
    sensitive_paths_.push_back(maic / "harness");
    sensitive_paths_.push_back(maic / "core" / "src" / "harness.cpp");
    sensitive_paths_.push_back(maic / "core" / "src" / "tripwire.cpp");
    sensitive_paths_.push_back(maic / "core" / "src" / "sandbox.cpp");
}

fs::path Harness::resolve(const std::string& path) const {
    fs::path p = path;
    if (!path.empty() && path[0] == '~') {
        p = fs::path(std::getenv("HOME")) / path.substr(path.size() > 1 && path[1] == '/' ? 2 : 1);
    } else if (p.is_relative()) {
        p = workspace_ / p;
    }
    return fs::weakly_canonical(p);
}

bool Harness::is_secret(const fs::path& p) const {
    return under_any(p, secret_paths_);
}

bool Harness::in_workspace(const fs::path& p) const {
    return under(p, workspace_);
}

std::string Harness::approval_key(const Action& action) {
    switch (action.kind) {
        case Action::Kind::Read: return "read:" + action.path.string();
        case Action::Kind::Write: return "write:" + action.path.string();
        case Action::Kind::Shell: {
            std::istringstream in(action.command);
            std::string program;
            in >> program;
            return "shell:" + program;
        }
    }
    return "";
}

Decision Harness::check(const Action& action, Mode mode, Origin origin) const {
    // Forbidden terms come first: before the tripwire's patterns, the mode, the allow list and the reviewer.
    if (auto term = forbidden(action.kind == Action::Kind::Shell ? action.command + " " + action.workdir.string() : action.path.string())) {
        return {Verdict::Deny, "contains the forbidden term \"" + *term + "\""};
    }
    Decision d{Verdict::Deny, "unknown action"};
    switch (action.kind) {
        case Action::Kind::Shell:
            d = check_shell(action.command, mode);
            if (d.verdict == Verdict::Allow && !action.workdir.empty() && !in_workspace(action.workdir)) {
                d = confined_ ? Decision{Verdict::Deny, "isolated session: commands run only inside the workspace"}
                              : Decision{Verdict::Ask, "runs outside the workspace (" + action.workdir.string() + ")", d.read_only_sandbox};
            }
            break;
        case Action::Kind::Write: d = check_write(action.path, mode); break;
        case Action::Kind::Read: d = check_read(action.path, mode); break;
    }
    if (origin == Origin::Remote && confined_) return {Verdict::Deny, "isolated session: no remote requests"};
    if (agent_def_) d = check_agent_def(action, d);
    // The permission block comes after the fixed rules: a trip or a denial above is not its to lift, and an
    // allow entry never speaks for a remote origin.
    if (d.verdict == Verdict::Allow || d.verdict == Verdict::Ask) {
        if (permitted(permission_.deny, action)) d = {Verdict::Deny, "denied by the permission block"};
        else if (permitted(permission_.ask, action)) d = {Verdict::Ask, "the permission block asks", d.read_only_sandbox};
        else if (origin == Origin::Local && (action.kind != Action::Kind::Shell || is_simple_command(action.command)) && permitted(permission_.allow, action)) d = {Verdict::Allow, "on the allow list", d.read_only_sandbox, true};
    }
    if (d.verdict == Verdict::Allow && origin == Origin::Remote) {
        d = {Verdict::Ask, "request did not come from this terminal"};
    }
    return d;
}

bool Harness::permitted(const std::vector<std::string>& entries, const Action& action) const {
    std::string kind = action.kind == Action::Kind::Shell ? "run_shell" : action.kind == Action::Kind::Write ? "write" : "read";
    std::string given = action.kind == Action::Kind::Shell ? action.command : action.path.string();
    std::string relative = action.kind == Action::Kind::Shell || !in_workspace(action.path) ? "" : action.path.lexically_relative(workspace_).generic_string();
    for (const auto& entry : entries) {
        size_t colon = entry.find(':');
        if (colon == std::string::npos) continue;
        std::string tool = entry.substr(0, colon), pattern = entry.substr(colon + 1);
        if (tool != kind && tool != action.tool && !(tool == "write" && is_write_tool(action.tool))) continue;
        if (!pattern.empty() && pattern[0] == '~') pattern = std::string(std::getenv("HOME")) + pattern.substr(1);
        if (fnmatch(pattern.c_str(), given.c_str(), 0) == 0) return true;
        if (!relative.empty() && fnmatch(pattern.c_str(), relative.c_str(), 0) == 0) return true;
    }
    return false;
}

void Harness::set_agent_def(const AgentDef& agent) {
    agent_def_ = std::make_shared<const AgentDef>(agent);
}

bool Harness::tool_allowed(const std::string& name) const {
    return !agent_def_ || agent_def_->allows_tool(name);
}

Decision Harness::check_agent_def(const Action& action, Decision d) const {
    if (d.verdict == Verdict::Deny || d.verdict == Verdict::Trip) return d;
    const AgentDef& p = *agent_def_;
    std::string who = "the " + p.name + " agent";
    switch (action.kind) {
        case Action::Kind::Write:
            if (p.read_only()) return {Verdict::Deny, who + " is read-only"};
            if (!in_workspace(action.path)) return {Verdict::Deny, who + " writes only inside the workspace"};
            if (!p.allows_write(action.path.lexically_relative(workspace_))) {
                std::string globs;
                for (const auto& g : p.write_paths) globs += (globs.empty() ? "" : ", ") + g;
                return {Verdict::Deny, who + " writes only under " + globs};
            }
            break;
        case Action::Kind::Read:
            if (!p.read_outside && !in_workspace(action.path)) return {Verdict::Deny, who + " reads only inside the workspace"};
            break;
        case Action::Kind::Shell:
            if (p.read_only() && !(is_read_only_command(action.command) || helper_read_only(action.command))) return {Verdict::Deny, who + " runs only read-only commands"};
            if (!p.read_outside && !action.workdir.empty() && !in_workspace(action.workdir)) return {Verdict::Deny, who + " runs commands only inside the workspace"};
            break;
    }
    return d;
}

namespace {

// cai (docs/cai.md) under any of its spellings: `cai`, `maic-cai`, `maic cai`, and `maic trans-fairy`. Read-only are
// the dispatcher's listing, every tool's help, `read` without `--out` (argparse takes any prefix of it), `time`, and
// trans-fairy's plain `state` report and its `--audit`.
bool cai_read_only(const std::vector<std::string>& w) {
    if (w.empty()) return true;
    const std::string& tool = w[0];
    std::string verb = w.size() > 1 ? w[1] : "";
    if (tool == "--help" || tool == "-h") return w.size() == 1;
    if (verb == "--help" || verb == "-h" || verb == "--man-help" || (tool == "trans-fairy" && verb == "--mahd")) return true;
    if (tool == "read") return std::none_of(w.begin() + 1, w.end(), [](const std::string& x) { return x.rfind("--o", 0) == 0; });
    if (tool == "time") return true;
    if (tool == "trans-fairy" && verb == "state") return w.size() == 2 || (w.size() == 3 && w[2] == "--audit");
    return false;
}

// MAIC's own helpers: their looking-only invocations count as read-only commands, each as one simple command.
bool helper_read_only(const std::string& command) {
    if (!is_simple_command(command)) return false;
    std::istringstream in(command);
    std::vector<std::string> words;
    for (std::string x; in >> x;) words.push_back(x);
    std::string prog = words.empty() ? "" : words[0];
    std::string sub = words.size() > 1 ? words[1] : "";
    if (prog == "maic-storyboard") return sub.empty() || sub == "status" || sub == "plan" || sub == "check" || sub == "--help" || sub == "-h";
    if (prog == "maic-workflow-edit") return sub == "inspect" || sub == "--help" || sub == "-h";
    if (prog == "maic-danbooru-tags") return sub == "check" || sub == "search" || sub == "show" || sub == "--help" || sub == "-h" || sub.empty();
    if (prog == "maic-panel-check") return true;  // it only reads the workflow and the local tag file
    bool cai = prog == "cai" || prog == "maic-cai" || (prog == "maic" && (sub == "cai" || sub == "trans-fairy" || sub == "trans-fairy-write"));
    if (cai) {
        size_t skip = prog == "maic" && sub == "cai" ? 2 : 1;
        return cai_read_only(std::vector<std::string>(words.begin() + std::min(skip, words.size()), words.end()));
    }
    if (prog == "maic") return sub == "path" || sub == "status" || sub == "artifacts" || sub == "sessions" || sub == "help" || sub == "vendor" || sub == "doctor" || sub == "tools";
    return false;
}

}  // namespace

void Harness::set_forbid(std::vector<std::string> terms) {
    for (auto& re : forbid_res_) regfree(&re.second);
    forbid_res_.clear();
    forbid_ = std::move(terms);
    for (const auto& term : forbid_) {
        if (term.size() > 2 && term.front() == '/' && term.back() == '/') {
            regex_t re;
            if (regcomp(&re, term.substr(1, term.size() - 2).c_str(), REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0) forbid_res_.emplace_back(term, re);
        }
    }
}

std::optional<std::string> Harness::forbidden(const std::string& text) const {
    if (forbid_.empty()) return std::nullopt;
    std::string low = text;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& term : forbid_) {
        if (term.size() > 2 && term.front() == '/' && term.back() == '/') continue;
        std::string t = term;
        for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!t.empty() && low.find(t) != std::string::npos) return term;
    }
    for (const auto& [term, re] : forbid_res_) {
        if (regexec(&re, text.c_str(), 0, nullptr, 0) == 0) return term;
    }
    return std::nullopt;
}

void Harness::set_allow(const std::vector<std::string>& patterns) {
    std::vector<std::string> kept;
    for (const auto& e : permission_.allow) {
        if (e.rfind("run_shell:", 0) != 0) kept.push_back(e);
    }
    for (const auto& p : patterns) kept.push_back("run_shell:" + p);
    permission_.allow = std::move(kept);
}

std::vector<std::string> Harness::allow() const {
    std::vector<std::string> out;
    for (const auto& e : permission_.allow) {
        if (e.rfind("run_shell:", 0) == 0) out.push_back(e.substr(10));
    }
    return out;
}

bool Harness::allowed_by_list(const std::string& command) const {
    if (!is_simple_command(command)) return false;
    for (const auto& p : allow()) {
        if (fnmatch(p.c_str(), command.c_str(), 0) == 0) return true;
    }
    return false;
}

bool Harness::harmless(const Action& action) const {
    if (action.kind == Action::Kind::Read) return true;
    if (action.kind != Action::Kind::Shell) return false;
    return is_read_only_command(action.command) || helper_read_only(action.command) || allowed_by_list(action.command);
}

Decision Harness::check_shell(const std::string& command, Mode mode) const {
    // std::regex recurses per character; a huge command could overflow the stack while being vetted.
    if (command.size() > 8 * 1024) {
        return {Verdict::Deny, "command too long to vet (over 8 KB); write it to a script file instead"};
    }
    for (const auto& p : trip_patterns()) {
        if (std::regex_search(command, p.re)) {
            return {Verdict::Trip, p.why};
        }
    }
    bool read_only = is_read_only_command(command) || helper_read_only(command);
    if (allowed_by_list(command)) {
        if (mode == Mode::Plan && !read_only) return {Verdict::Deny, "plan mode only runs read-only commands"};
        return {Verdict::Allow, "on the allow list", read_only, true};
    }
    switch (mode) {
        case Mode::Plan:
            if (read_only) return {Verdict::Allow, "read-only command", true};
            return {Verdict::Deny, "plan mode only runs read-only commands"};
        case Mode::AutoRead:
            if (read_only) return {Verdict::Allow, "read-only command", true};
            return {Verdict::Ask, "may change files or the system"};
        case Mode::Auto: return {Verdict::Allow, "auto mode (sandboxed)"};
        default: return {Verdict::Ask, "runs a command"};
    }
}

Decision Harness::check_write(const fs::path& p, Mode mode) const {
    if (is_secret(p) || under_any(p, system_paths_)) {
        return {Verdict::Trip, "write to a protected path: " + p.string()};
    }
    if (mode == Mode::Plan) {
        return {Verdict::Deny, "plan mode is read-only"};
    }
    if (under_any(p, sensitive_paths_)) {
        return {Verdict::Ask, "sensitive path (startup files, ~/bin, or MAIC's harness)"};
    }
    if (!in_workspace(p)) {
        return {Verdict::Ask, "outside the workspace"};
    }
    if (mode == Mode::Manual || mode == Mode::AutoRead) {
        return {Verdict::Ask, "edits a file"};
    }
    return {Verdict::Allow, std::string(mode_name(mode)) + " mode"};
}

Decision Harness::check_read(const fs::path& p, Mode mode) const {
    if (confined_ && !in_workspace(p)) return {Verdict::Deny, "isolated session: reads stay inside the workspace"};
    if (is_secret(p)) {
        return {Verdict::Deny, "credentials and keys are never read"};
    }
    if (in_workspace(p) || mode == Mode::Auto || mode == Mode::AutoRead) {
        return {Verdict::Allow, "read"};
    }
    return {Verdict::Ask, "reads outside the workspace"};
}

}  // namespace maic
