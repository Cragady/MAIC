#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace maic {

struct TuiOptions {
    std::optional<std::string> model;  // overrides settings
    std::optional<std::string> mode;
    std::optional<std::filesystem::path> resume;  // an earlier session file to continue
};

// The interactive agent: full-screen, vim-style input and navigation.
int run_tui(const TuiOptions& options);

}  // namespace maic
