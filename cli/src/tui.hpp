#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

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
    std::optional<bool> record;  // overrides settings.record
    std::optional<std::string> system;        // --system TEXT|@FILE, overrides settings.system_prompt
    std::optional<std::string> prefill;       // --prefill TEXT|@FILE
    std::optional<bool> load_instructions;    // --no-instructions
    std::vector<std::string> bans;            // --ban STRING, repeatable
    std::vector<std::string> ban_patterns;    // --ban-pattern REGEX, repeatable
    nlohmann::json sampling = nlohmann::json::object();  // --sampling KEY=VALUE and --xtc, over the settings
    std::optional<std::string> harness;       // --harness smart|dumb
    bool accept_dumb_auto = false;            // --accept-dumb-auto                  // sent as the first turn (maic -p "..." --interactive); "-" reads stdin
};

// The interactive agent: full-screen, vim-style input and navigation.
int run_tui(const TuiOptions& options);

}  // namespace maic
