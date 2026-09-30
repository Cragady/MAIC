#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace maic {

struct SandboxResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;  // stdout and stderr interleaved, trimmed to a size the model can take
};

// Runs `command` with bash inside bubblewrap: the workspace is the only writable path, the rest of the
// filesystem is read-only, secrets are hidden, there is no network, and nothing can gain privileges.
SandboxResult run_sandboxed(const std::string& command, const std::filesystem::path& workspace,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel);

}  // namespace maic
