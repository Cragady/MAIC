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

// $XDG_CONFIG_HOME/maic/settings.json (default ~/.config/maic/settings.json). Every key is optional.
struct Settings {
    std::string model = "qwen3.5:4b";
    std::string mode = "manual";
    bool think = false;
    bool markdown = true;   // render markdown in the conversation window
    bool mouse = true;      // scroll wheel (terminal text selection then needs Shift+drag)
    bool sound = false;
    std::string sessions_home = "general";  // where new transcripts go: "general", "project", or a name under sessions/
    std::vector<Provider> providers = default_providers();
    std::map<std::string, Style> styles;  // by role, see docs/settings.md; defaults are filled in
    std::vector<std::string> instruction_files = {"MAIC.md", "AGENTS.md"};

    const Style& style(const std::string& name) const;
};

std::filesystem::path settings_path();

// Loads settings over the defaults. A missing file is fine; a broken one throws with the line.
Settings load_settings();

// Writes the settings file with a documented default for every key. Never overwrites an existing file.
void write_default_settings();

}  // namespace maic
