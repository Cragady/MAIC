#pragma once

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
//   1. $XDG_CONFIG_HOME/maic/settings.json (default ~/.config/maic/settings.json)
//   2. <dir>/.maic/settings.json and then <dir>/.maic/settings.local.json for every directory from just under
//      $HOME down to the workspace; the nearest file wins. settings.json is meant to be committed with a
//      project, settings.local.json is personal.
struct Settings {
    std::string model = "qwen3.5:4b";
    std::string mode = "manual";
    bool think = false;
    bool markdown = true;   // render markdown in the conversation window
    bool mouse = true;      // scroll wheel (terminal text selection then needs Shift+drag)
    bool sound = false;
    // Where new transcripts go: "auto" (project when the workspace has a MAIC.md, else general), "general",
    // "project", or a name under sessions/.
    std::string sessions_home = "auto";
    std::string leader = " ";  // the vim leader key in normal and visual modes (Space, as in her nvim)
    std::vector<std::filesystem::path> sources;  // the files that were read, in order
    std::vector<Provider> providers = default_providers();
    std::map<std::string, Style> styles;  // by role, see docs/settings.md; defaults are filled in
    std::vector<std::string> instruction_files = {"MAIC.md", "AGENTS.md"};

    const Style& style(const std::string& name) const;
};

std::filesystem::path settings_path();

// Loads the layers for `workspace` over the defaults. Missing files are fine; a broken one throws with the line.
Settings load_settings(const std::filesystem::path& workspace);
Settings load_settings();  // for the current directory

// Resolves "auto" for a workspace: the project home when a MAIC.md is in effect there, else general.
std::filesystem::path resolve_sessions_home(const Settings& settings, const std::filesystem::path& workspace);

// Writes the settings file with a documented default for every key. Never overwrites an existing file.
void write_default_settings();

}  // namespace maic
