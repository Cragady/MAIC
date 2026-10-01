#pragma once

#include <filesystem>
#include <regex.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

struct AgentDef;

// How much the agent may do without asking. Cycled with Shift-Tab in the CLI.
enum class Mode {
    Manual,    // ask before any write or command
    AutoRead,  // reads anywhere and read-only commands are automatic; writes and other commands ask
    Edit,      // file edits inside the workspace are automatic; commands ask
    Auto,      // edits and sandboxed commands inside the workspace are automatic
    Plan,      // read-only: reads and read-only commands only
};

std::string_view mode_name(Mode mode);
std::optional<Mode> parse_mode(std::string_view name);
Mode next_mode(Mode mode);
// The one that allows less: plan, then manual, auto-read, edit, auto.
Mode narrower_mode(Mode a, Mode b);

// Where a request came from. Anything not typed by the local user is always asked, in every mode.
enum class Origin { Local, Remote };

struct Action {
    enum class Kind { Read, Write, Shell } kind;
    std::filesystem::path path;  // Read / Write
    std::string command;         // Shell
    std::filesystem::path workdir;  // Shell only: where the command runs (resolved); empty = the workspace
    std::string tool;            // the tool making it ("write_file", "run_shell"), matched by `permission` entries
};

// `permission` in settings: patterns over `tool:argument` (`run_shell:pytest *`, `write_file:src/**`,
// `read_file:/etc/**`; `write:` and `read:` stand for any write or read tool). deny wins over ask wins over
// allow. It runs after the fixed rules, so it never reaches a trip pattern, a secret or a system path, and
// allow entries are ignored for a remote origin.
struct Permission {
    std::vector<std::string> allow, ask, deny;
};

enum class Verdict {
    Allow,
    Ask,
    Deny,
    Trip,  // deny and trip the tripwire
};

struct Decision {
    Verdict verdict;
    std::string reason;
    bool read_only_sandbox = false;  // run with the workspace mounted read-only too
    bool trusted = false;            // on the allow list: no second reader, and never a reason to trip on repeats
};

// Commands that only look at things (ls, grep, git log, ...), with no redirection or substitution.
// Allowed ones still run in a fully read-only sandbox, so a wrong guess can't change anything.
bool is_read_only_command(const std::string& command);

class Harness {
public:
    explicit Harness(std::filesystem::path workspace);
    ~Harness();
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    const std::filesystem::path& workspace() const { return workspace_; }

    // Resolves a tool-supplied path against the workspace, following symlinks and `..`.
    std::filesystem::path resolve(const std::string& path) const;

    Decision check(const Action& action, Mode mode, Origin origin) const;

    // Credentials and keys: never read, never written, skipped by recursive search.
    bool is_secret(const std::filesystem::path& p) const;

    // What "always allow" remembers for the session: the file for writes, the program for commands.
    static std::string approval_key(const Action& action);

    // The additive permission block. Its run_shell allow entries are the allow list (`:allow`).
    void set_permission(Permission p) { permission_ = std::move(p); }
    const Permission& permission() const { return permission_; }
    // Commands the user pre-approved (glob patterns over the whole command line, `*` and `?`): allowed in
    // every mode but plan without asking or review. Trip patterns are checked first and still win.
    void set_allow(const std::vector<std::string>& patterns);  // replaces the run_shell entries of permission.allow
    std::vector<std::string> allow() const;
    bool allowed_by_list(const std::string& command) const;
    // A subagent's agent: a write outside its write_paths, a read outside the workspace when it may not, a
    // tool off its list and, for a read-only agent, anything that could write are denied, the agent named.
    void set_agent_def(const AgentDef& agent);
    const AgentDef* agent_def() const { return agent_def_.get(); }
    bool tool_allowed(const std::string& name) const;
    // A repeat of this action is harmless (a read, a read-only or allow-listed command): refuse, never trip.
    bool harmless(const Action& action) const;
    // Forbidden terms: a tool call whose name, arguments, command or path contains one (any letter case) is
    // halted before anything runs, in every mode and under every harness. From `forbid` in settings.
    void set_forbid(std::vector<std::string> terms);  // a term written /like this/ is a POSIX extended regex
    const std::vector<std::string>& forbid() const { return forbid_; }
    std::optional<std::string> forbidden(const std::string& text) const;  // the term found, if any
    // An isolated session (tripwire = "isolated"): reads stay inside the workspace, commands run only there,
    // and requests from a remote origin are refused.
    void set_confined(bool on) { confined_ = on; }
    bool confined() const { return confined_; }

private:
    bool in_workspace(const std::filesystem::path& p) const;

    Decision check_shell(const std::string& command, Mode mode) const;
    Decision check_write(const std::filesystem::path& p, Mode mode) const;
    Decision check_read(const std::filesystem::path& p, Mode mode) const;
    Decision check_agent_def(const Action& action, Decision d) const;
    bool permitted(const std::vector<std::string>& entries, const Action& action) const;

    std::filesystem::path workspace_;
    Permission permission_;
    std::shared_ptr<const AgentDef> agent_def_;
    std::vector<std::string> forbid_;
    std::vector<std::pair<std::string, regex_t>> forbid_res_;  // compiled /regex/ entries, by their text
    bool confined_ = false;
    std::vector<std::filesystem::path> secret_paths_;     // never read, never written
    std::vector<std::filesystem::path> system_paths_;     // never written
    std::vector<std::filesystem::path> sensitive_paths_;  // always asked before writing, whatever the mode
};

}  // namespace maic
