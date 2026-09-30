#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maic {

struct TuiOptions {
    std::optional<std::string> model;  // overrides settings
    std::optional<std::string> mode;
    std::optional<std::filesystem::path> resume;  // an earlier session file to continue
    bool append = true;  // continue in the same file (false: a new file that points at the old one)
    std::optional<size_t> fork_at;  // with resume: fork from its first N records (append is then false)
    std::vector<std::filesystem::path> context;  // files attached to the conversation before the first turn
    std::string initial_prompt;
    std::optional<bool> record;  // overrides settings.record                  // sent as the first turn (maic -p "..." --interactive); "-" reads stdin
};

// The interactive agent: full-screen, vim-style input and navigation.
int run_tui(const TuiOptions& options);

}  // namespace maic
