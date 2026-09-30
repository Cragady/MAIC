#include "maic/harness.hpp"

#include <fnmatch.h>

#include "maic/paths.hpp"

#include <cstdlib>
#include <regex>
#include <set>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;

namespace {

struct Pattern {
    std::regex re;
    const char* why;
};

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
        "bash", "zsh", "sh", "docker", "ollama", "clang-format", "clang-tidy", "ctest",
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
    if (d.verdict == Verdict::Allow && origin == Origin::Remote) {
        d = {Verdict::Ask, "request did not come from this terminal"};
    }
    return d;
}

namespace {

// MAIC's own helpers: their looking-only invocations count as read-only commands.
bool helper_read_only(const std::string& command) {
    std::istringstream in(command);
    std::string prog, sub;
    in >> prog >> sub;
    if (prog == "maic-storyboard") return sub.empty() || sub == "status" || sub == "plan" || sub == "check" || sub == "--help" || sub == "-h";
    if (prog == "maic-workflow-edit") return sub == "inspect" || sub == "--help" || sub == "-h";
    if (prog == "maic") return sub == "path" || sub == "status" || sub == "artifacts" || sub == "sessions" || sub == "help" || sub == "vendor" || sub == "doctor" || sub == "tools";
    return false;
}

}  // namespace

bool Harness::allowed_by_list(const std::string& command) const {
    for (const auto& p : allow_) {
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
