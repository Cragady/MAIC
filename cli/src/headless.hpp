#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace maic {

struct HeadlessOptions {
    std::string prompt;  // "-" or empty reads stdin
    std::optional<std::string> model;
    std::optional<std::string> mode;
    bool json = false;   // JSONL events on stdout instead of text
    bool think = false;
    std::optional<std::filesystem::path> resume;
};

// `maic -p "..."`: one turn, no UI. The reply streams to stdout, tool activity goes to stderr.
// Approvals are asked on the terminal when stdin is one, otherwise denied.
int run_headless(const HeadlessOptions& options);

}  // namespace maic
