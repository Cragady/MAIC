#include "maic/harness.hpp"

#include "maic/paths.hpp"

#include <cstdlib>
#include <regex>
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

}  // namespace

std::string_view mode_name(Mode mode) {
    switch (mode) {
        case Mode::Manual: return "manual";
        case Mode::Edit: return "edit";
        case Mode::Auto: return "auto";
        case Mode::Plan: return "plan";
    }
    return "manual";
}

std::optional<Mode> parse_mode(std::string_view name) {
    for (Mode m : {Mode::Manual, Mode::Edit, Mode::Auto, Mode::Plan}) {
        if (mode_name(m) == name) {
            return m;
        }
    }
    return std::nullopt;
}

Mode next_mode(Mode mode) {
    switch (mode) {
        case Mode::Manual: return Mode::Edit;
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
        case Action::Kind::Shell: d = check_shell(action.command, mode); break;
        case Action::Kind::Write: d = check_write(action.path, mode); break;
        case Action::Kind::Read: d = check_read(action.path, mode); break;
    }
    if (d.verdict == Verdict::Allow && origin == Origin::Remote) {
        d = {Verdict::Ask, "request did not come from this terminal"};
    }
    return d;
}

Decision Harness::check_shell(const std::string& command, Mode mode) const {
    for (const auto& p : trip_patterns()) {
        if (std::regex_search(command, p.re)) {
            return {Verdict::Trip, p.why};
        }
    }
    switch (mode) {
        case Mode::Plan: return {Verdict::Deny, "plan mode is read-only"};
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
    if (mode == Mode::Manual) {
        return {Verdict::Ask, "edits a file"};
    }
    return {Verdict::Allow, std::string(mode_name(mode)) + " mode"};
}

Decision Harness::check_read(const fs::path& p, Mode mode) const {
    if (is_secret(p)) {
        return {Verdict::Deny, "credentials and keys are never read"};
    }
    if (in_workspace(p) || mode == Mode::Auto) {
        return {Verdict::Allow, "read"};
    }
    return {Verdict::Ask, "reads outside the workspace"};
}

}  // namespace maic
