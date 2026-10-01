#pragma once

#include "maic/markdown.hpp"
#include "maic/settings.hpp"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace maic {

// Settings colors ("red", "gray_dark", "#rrggbb", 0-255) to FTXUI. Unknown names fall back to the default color.
// A hex colour is sent as is on a truecolor terminal, else as the nearest xterm-256 or ANSI 16 colour.
ftxui::Color parse_color(const std::string& name);
// The terminal's colour depth for parse_color: the `colors` setting, "auto" detecting it (maic/theme.hpp).
void set_color_depth(const std::string& setting);

ftxui::Decorator decorate(const Style& style);

// The style for a span: its entry's base style with the markdown styles it carries layered on top.
Style span_style(const Settings& settings, const Style& base, unsigned md_flags);

// An overlay applied to a column range of a rendered line (selection, search hit, cursor).
struct Overlay {
    size_t begin;  // columns, inclusive
    size_t end;    // exclusive
    const Style* style;
};

// Renders one already-wrapped line as a row of styled text elements.
ftxui::Element render_line(const Settings& settings, const StyledLine& line, const Style& base,
                           const std::vector<Overlay>& overlays = {});

}  // namespace maic
