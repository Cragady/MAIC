#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

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

// Where a request came from. Anything not typed by the local user is always asked, in every mode.
enum class Origin { Local, Remote };

struct Action {
    enum class Kind { Read, Write, Shell } kind;
    std::filesystem::path path;  // Read / Write
    std::string command;         // Shell
    std::filesystem::path workdir;  // Shell only: where the command runs (resolved); empty = the workspace
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
};

// Commands that only look at things (ls, grep, git log, ...), with no redirection or substitution.
// Allowed ones still run in a fully read-only sandbox, so a wrong guess can't change anything.
bool is_read_only_command(const std::string& command);

class Harness {
public:
    explicit Harness(std::filesystem::path workspace);

    const std::filesystem::path& workspace() const { return workspace_; }

    // Resolves a tool-supplied path against the workspace, following symlinks and `..`.
    std::filesystem::path resolve(const std::string& path) const;

    Decision check(const Action& action, Mode mode, Origin origin) const;

    // Credentials and keys: never read, never written, skipped by recursive search.
    bool is_secret(const std::filesystem::path& p) const;

    // What "always allow" remembers for the session: the file for writes, the program for commands.
    static std::string approval_key(const Action& action);

private:
    bool in_workspace(const std::filesystem::path& p) const;
    Decision check_shell(const std::string& command, Mode mode) const;
    Decision check_write(const std::filesystem::path& p, Mode mode) const;
    Decision check_read(const std::filesystem::path& p, Mode mode) const;

    std::filesystem::path workspace_;
    std::vector<std::filesystem::path> secret_paths_;     // never read, never written
    std::vector<std::filesystem::path> system_paths_;     // never written
    std::vector<std::filesystem::path> sensitive_paths_;  // always asked before writing, whatever the mode
};

}  // namespace maic
