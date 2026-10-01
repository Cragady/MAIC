#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace maic {

struct SandboxResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;  // stdout and stderr interleaved (run_sandboxed) or stdout alone (run_sandboxed_argv), trimmed to a size the model can take
    std::string error;   // run_sandboxed_argv only: stderr, trimmed the same way
};

// Runs `command` with bash inside bubblewrap: the workspace is the only writable path (none at all when
// `read_only`), the rest of the filesystem is read-only, secrets are hidden, there is no network, and
// nothing can gain privileges.
SandboxResult run_sandboxed(const std::string& command, const std::filesystem::path& workspace, bool read_only,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel,
                            const std::filesystem::path& workdir = {});

// The same sandbox around `argv` run directly, no shell, with `input` written to its stdin; stdout and stderr
// come back apart. How a script tool runs (docs/tools.md).
SandboxResult run_sandboxed_argv(const std::vector<std::string>& argv, const std::string& input, const std::filesystem::path& workspace,
                                 bool read_only, std::chrono::seconds timeout, const std::atomic<bool>& cancel,
                                 const std::filesystem::path& workdir = {});

}  // namespace maic
