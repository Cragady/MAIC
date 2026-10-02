#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

enum class OutputStream { Stdout, Stderr };

// A program's output while it runs, for display only: `bytes` start at byte `offset` of that stream's whole
// output, so a gap between two calls is output that was dropped (SandboxResult::dropped_chunks).
using OnOutput = std::function<void(OutputStream stream, std::string_view bytes, size_t offset)>;

// Copies of a program's output beside its result; both optional, neither changes the result.
struct OutputTaps {
    // For display: batched, called from a thread of its own, dropped past a backlog (run_sandboxed says how).
    OnOutput on_output;
    // Every byte as it is read, on the reading thread, unbatched and never dropped: for a writer that keeps the
    // whole output (a session's full-output file). The pipes wait while it runs, so it must be quick.
    OnOutput on_read;
};

// What the model is given of a long output: this much of its head and of its tail, the middle counted.
constexpr size_t kOutputHeadBytes = 24 * 1024;
constexpr size_t kOutputTailBytes = 8 * 1024;

struct SandboxResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;  // stdout and stderr interleaved (run_sandboxed) or stdout alone (run_sandboxed_argv), trimmed to a size the model can take
    std::string error;   // run_sandboxed_argv only: stderr, trimmed the same way
    size_t output_bytes = 0;  // everything `output` was trimmed from
    // Chunks, and their bytes, that on_output never got because it fell behind; output and error are the same either way.
    size_t dropped_chunks = 0;
    size_t dropped_bytes = 0;
};

// Runs `command` with bash inside bubblewrap: the workspace is the only writable path (none at all when
// `read_only`), the rest of the filesystem is read-only, secrets are hidden, there is no network, and
// nothing can gain privileges.
//
// `taps.on_output`, when set, also gets the output as it arrives (all of it as Stdout here, stdout and stderr
// interleaved as in `output`): batched per stream, a chunk every 100 ms or 16 KiB, whichever comes first, and a
// last flush when the program ends, every call made before this returns. It is a tee: the pipes are read at
// full speed and the result is the same with or without it. The calls come from a thread of their own, in
// order; while more than 2 MiB waits for a slow `on_output`, newer chunks are dropped and counted, and once
// the program has ended what is still waiting after 200 ms is dropped the same way.
SandboxResult run_sandboxed(const std::string& command, const std::filesystem::path& workspace, bool read_only,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel,
                            const std::filesystem::path& workdir = {}, const OutputTaps& taps = {});

// The same sandbox around `argv` run directly, no shell, with `input` written to its stdin; stdout and stderr
// come back apart, and the taps get them apart too. How a script tool runs (docs/tools.md).
SandboxResult run_sandboxed_argv(const std::vector<std::string>& argv, const std::string& input, const std::filesystem::path& workspace,
                                 bool read_only, std::chrono::seconds timeout, const std::atomic<bool>& cancel,
                                 const std::filesystem::path& workdir = {}, const OutputTaps& taps = {});

}  // namespace maic
