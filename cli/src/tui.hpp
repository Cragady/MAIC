#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace maid {

struct Settings;

struct TuiOptions {
    std::optional<std::string> model;  // overrides settings
    std::optional<std::string> mode;
    std::optional<std::filesystem::path> resume;  // an earlier session file to continue
    bool append = true;  // continue in the same file (false: a new file that points at the old one)
    std::optional<size_t> fork_at;  // with resume: fork from its first N records (append is then false)
    std::vector<std::filesystem::path> context;  // files attached to the conversation before the first turn
    std::vector<std::filesystem::path> images;   // --image FILE: pictures for the first turn
    std::string initial_prompt;
    std::optional<bool> record;  // overrides settings.record
    std::optional<std::string> system;        // --system TEXT|@FILE, overrides settings.system_prompt
    std::optional<std::string> prefill;       // --prefill TEXT|@FILE
    std::optional<int> ctx;
    std::optional<int> ctx2;  // --ctx2 N, the side server                   // --ctx N
    std::vector<std::string> rules;           // --rule TEXT, repeatable
    std::optional<bool> load_instructions;    // --no-instructions
    std::vector<std::string> bans;            // --ban STRING, repeatable
    std::vector<std::string> ban_patterns;    // --ban-pattern REGEX, repeatable
    nlohmann::json sampling = nlohmann::json::object();  // --sampling KEY=VALUE and --xtc, over the settings
    std::optional<std::string> harness;       // --harness smart|dumb
    bool bare = false;                        // --bare: nothing from nvim (also MAID_BARE=1, bare = true)
    bool trust = false;                       // --trust: project directories trusted for this process only
    bool accept_dumb_auto = false;            // --accept-dumb-auto                  // sent as the first turn (maid -p "..." --interactive); "-" reads stdin
    std::optional<std::string> ui;            // --ui tui|nvim, over settings.ui
    std::vector<std::string> engine_args;     // the agent's flags as given, for `maid --rpc` under nvim
};

// The interactive agent: full-screen, vim-style input and navigation.
int run_tui(const TuiOptions& options);

// The settings files for `workspace` with the command line's flags over them.
Settings tui_settings(const TuiOptions& options, const std::filesystem::path& workspace);

// `maid --rpc`: the engine on stdio, JSON-RPC 2.0 one message per line, for an interface that is not this terminal
// (maid.nvim's interface mode). Its sessions are set up as the TUI's, from the same flags.
int run_rpc(const TuiOptions& options);

}  // namespace maid
