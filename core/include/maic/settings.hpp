#pragma once

#include "maic/bans.hpp"
#include "maic/llm.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace maic {

// One visual style. Colors are FTXUI names ("red", "gray_dark"), "#rrggbb", or 0-255 palette indexes.
struct Style {
    std::optional<std::string> fg;
    std::optional<std::string> bg;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool inverted = false;

    Style merged_over(const Style& base) const;
};

// Layered settings, every key optional:
//   1. $XDG_CONFIG_HOME/maic/settings.lua (default ~/.config/maic/settings.lua)
//   2. <dir>/.maic/settings.lua and then <dir>/.maic/settings.local.lua for every directory from just under
//      $HOME down to the workspace; the nearest file wins. settings.lua is meant to be committed with a
//      project, settings.local.lua is personal.
// At each location a .json file with the same stem is the fallback when no .lua exists.
// `maic server`: where it listens, which directories remote sessions may open, and the TLS pair.
struct ServerSettings {
    std::string listen = "127.0.0.1:7373";           // loopback needs no TLS; any other address gets it
    std::vector<std::filesystem::path> workspaces;  // allowed roots for remote sessions; empty = ~/dev2 if it exists, else the current directory
    std::filesystem::path cert;                     // PEM pair; empty = a self-signed one generated under state/server on first use
    std::filesystem::path key;
};

struct Settings {
    std::string model = "llamacpp/current";  // the vendored llama-server serves the linked GGUF as `current`
    std::string mode = "manual";
    bool think = false;
    bool markdown = true;   // render markdown in the conversation window
    bool mouse = true;      // scroll wheel (terminal text selection then needs Shift+drag)
    bool sound = false;
    // Where new transcripts go: "auto" (project when the workspace has a MAIC.md, else general), "general",
    // "project", or a name under sessions/.
    std::string sessions_home = "auto";
    std::string leader = " ";
    std::string models_dir;
    std::string title_model;   // names a session after its first turn ("" = off; e.g. "qwen3.5:4b")
    long budget_tokens = 0;    // per-session token budget, 0 = unlimited
    bool timestamps = false;   // a time beside each conversation entry
    bool record = true;
    double compact_at = 0.75;      // auto-compact at this share of the context window; 0 turns it off
    int compact_keep_results = 4;  // tool results that never get pruned (the most recent)
    std::vector<std::filesystem::path> sources;  // the files that were read, in order
    std::vector<Provider> providers = default_providers();
    std::map<std::string, Style> styles;  // by role, see docs/settings.md; defaults are filled in
    std::vector<std::string> instruction_files = {"MAIC.md", "AGENTS.md"};
    bool load_instructions = true;  // false: no MAIC.md / AGENTS.md anywhere
    std::string system_prompt;      // text placed first in the system prompt; "@path" reads a file (~ expands)
    std::string prefill;            // text every reply starts with (the model continues it); "@path" reads a file
    std::vector<std::string> rules; // standing one-line instructions, carried with system_prompt; layers add up
    Bans bans;                      // strings, patterns and tokens the model must not produce (docs/bans.md)
    nlohmann::json sampling = nlohmann::json::object();  // sampler keys for every provider; a provider's options.sampling overrides
    std::string harness = "smart";  // "smart": a model reviews commands and writes the rules would allow; "dumb": rules only
    std::string reviewer_model;     // the reviewer ("" = the session's model)
    bool dumb_auto_ok = false;      // true: no warning when entering auto mode under a dumb harness
    ServerSettings server;

    const Style& style(const std::string& name) const;
};

std::filesystem::path settings_path();

// Loads the layers for `workspace` over the defaults. Missing files are fine; a broken one throws with the line.
Settings load_settings(const std::filesystem::path& workspace);
Settings load_settings();  // for the current directory

// The text of a system_prompt setting or --system argument: as given, or the file's contents for "@path".
std::string resolve_system_prompt(const std::string& value);

// Resolves "auto" for a workspace: the project home when a MAIC.md is in effect there, else general.
std::filesystem::path resolve_sessions_home(const Settings& settings, const std::filesystem::path& workspace);

// Writes the settings file with a documented default for every key. Never overwrites an existing file.
// Lua by default (settings.lua); `json` writes settings.json instead.
void write_default_settings(bool json = false);

}  // namespace maic
