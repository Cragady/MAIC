#pragma once

#include "maic/settings.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace maic {

// A theme: a named set of styles by role. A theme file is Lua returning
//   { name = "gruvbox-dark", background = "dark", styles = { error = { fg = "#fb4934", bold = true }, ... } }
// with role names from default_styles(); a theme may set any subset, the rest keep the built-in default.
struct Theme {
    std::string name;
    std::string background = "dark";      // "dark" or "light": the terminal background the colours are meant for
    std::map<std::string, Style> styles;  // the roles it sets; each replaces the built-in role whole
    std::filesystem::path path;           // where it was read from; empty for the built-in default
};

// ~/.config/maic/themes ($XDG_CONFIG_HOME respected), then the themes/ shipped with MAIC.
std::filesystem::path user_themes_dir();
std::filesystem::path builtin_themes_dir();

// A theme table (a theme file's result, or one built elsewhere) checked and converted. `where` prefixes errors;
// `source` is the file's text when there is one, so an error can name the line of the key it is about.
Theme parse_theme(const nlohmann::json& table, const std::string& where, const std::string& source = "");
// Reads a theme file. Errors name the file and line.
Theme load_theme_file(const std::filesystem::path& path);
// Finds NAME in the user's themes, then the built-ins, and loads it. "default" with no file is the built-in table.
Theme load_theme(const std::string& name);

struct ThemeInfo {
    std::string name;
    std::filesystem::path path;  // the file in effect (a user theme shadows a built-in of the same name)
};
std::vector<ThemeInfo> list_themes();

// The styles in effect: the built-in defaults, the theme's roles over them, the settings' `style` entries merged
// over those. Sets settings.theme to the theme's name.
void apply_theme(Settings& settings, const Theme& theme);

// The theme as a readable Lua file, `comment` (lines, without the "-- ") at the top.
std::string theme_lua(const Theme& theme, const std::string& comment = "");

// Colour depth: "truecolor" when COLORTERM is truecolor or 24bit, else "256" when TERM or COLORTERM says 256,
// else "16"; `setting` ("auto", "truecolor", "256", "16") overrides the detection.
enum class ColorDepth { Ansi16, Xterm256, Truecolor };
ColorDepth color_depth(const std::string& setting);
// The nearest xterm-256 colour (the 6x6x6 cube and the grey ramp, 16-255) by squared distance in sRGB.
int nearest_xterm256(uint8_t r, uint8_t g, uint8_t b);
// The nearest of the 16 ANSI colours, at xterm's default values.
int nearest_ansi16(uint8_t r, uint8_t g, uint8_t b);
// An xterm palette index (0-255) as #rrggbb, at xterm's default values.
std::string xterm_hex(int index);

// Importing a colorscheme from neovim. The highlight groups read, each as nvim_get_hl(0, {name, link = false})
// returns it with fg / bg / sp as "#rrggbb".
const std::vector<std::string>& nvim_theme_groups();
// The groups of one colorscheme ({ "Normal": {"fg": "#c7c7c7", "bg": ...}, ... }) mapped onto MAIC's roles
// (the table in docs/settings.md). Kept apart from running nvim so a host nvim's palette can be mapped the same way.
Theme theme_from_nvim(const nlohmann::json& groups, const std::string& name, const std::string& background);
// Runs `nvim --headless` with the user's configuration, applies `colorscheme`, maps its groups and writes the
// result to the user's themes directory as <as>.lua (default "nvim-<colorscheme>"), with a header saying where it
// came from and when. Returns the theme as written. Throws when nvim is missing, times out, or has no such scheme.
Theme import_nvim_theme(const std::string& colorscheme, const std::string& as = "", const std::string& nvim = "nvim",
                        std::chrono::seconds timeout = std::chrono::seconds(15));
// The colorschemes that nvim knows (getcompletion('', 'color')), from the same kind of run.
std::vector<std::string> nvim_colorschemes(const std::string& nvim = "nvim", std::chrono::seconds timeout = std::chrono::seconds(15));

}  // namespace maic
