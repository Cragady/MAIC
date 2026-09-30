#include "style.hpp"

#include <cstdlib>
#include <map>

namespace maic {

using namespace ftxui;

Color parse_color(const std::string& name) {
    static const std::map<std::string, Color> named = {
        {"black", Color::Black}, {"red", Color::Red}, {"green", Color::Green}, {"yellow", Color::Yellow},
        {"blue", Color::Blue}, {"magenta", Color::Magenta}, {"cyan", Color::Cyan}, {"white", Color::White},
        {"gray", Color::GrayLight}, {"gray_light", Color::GrayLight}, {"gray_dark", Color::GrayDark},
        {"red_light", Color::RedLight}, {"green_light", Color::GreenLight}, {"yellow_light", Color::YellowLight},
        {"blue_light", Color::BlueLight}, {"magenta_light", Color::MagentaLight}, {"cyan_light", Color::CyanLight},
        {"default", Color::Default},
    };
    if (auto it = named.find(name); it != named.end()) return it->second;
    if (name.size() == 7 && name[0] == '#') {
        auto hex = [&](size_t i) { return static_cast<uint8_t>(std::strtol(name.substr(i, 2).c_str(), nullptr, 16)); };
        return Color::RGB(hex(1), hex(3), hex(5));
    }
    if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
        return Color::Palette256(static_cast<uint8_t>(std::atoi(name.c_str()) & 255));
    }
    return Color::Default;
}

Decorator decorate(const Style& s) {
    return [s](Element e) {
        if (s.fg) e = e | color(parse_color(*s.fg));
        if (s.bg) e = e | bgcolor(parse_color(*s.bg));
        if (s.bold) e = e | bold;
        if (s.dim) e = e | dim;
        if (s.italic) e = e | dim;  // FTXUI 5 has no italic
        if (s.underline) e = e | underlined;
        if (s.inverted) e = e | inverted;
        return e;
    };
}

Style span_style(const Settings& settings, const Style& base, unsigned f) {
    Style s = base;
    if (f & MdCodeBlock) s = settings.style("md_code_block").merged_over(s);
    if (f & MdQuote) s = settings.style("md_quote").merged_over(s);
    if (f & MdHeading) s = settings.style("md_heading").merged_over(s);
    if (f & MdBullet) s = settings.style("md_bullet").merged_over(s);
    if (f & MdRule) s = settings.style("md_rule").merged_over(s);
    if (f & MdBold) s = settings.style("md_bold").merged_over(s);
    if (f & MdItalic) s = settings.style("md_italic").merged_over(s);
    if (f & MdLink) s = settings.style("md_link").merged_over(s);
    if (f & MdCode) s = settings.style("md_code").merged_over(s);
    if (f & MdUrl) s = settings.style("md_url").merged_over(s);
    return s;
}

namespace {

size_t next_cp(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

}  // namespace

Element render_line(const Settings& settings, const StyledLine& line, const Style& base, const std::vector<Overlay>& overlays) {
    // Expand to one cell per code point so overlays can cut across spans, then coalesce equal runs.
    struct Cell {
        std::string text;
        unsigned flags;
        unsigned overlay_mask;
    };
    std::vector<Cell> cells;
    for (const auto& span : line) {
        for (size_t i = 0; i < span.text.size();) {
            size_t n = next_cp(span.text, i);
            cells.push_back({span.text.substr(i, n - i), span.flags, 0});
            i = n;
        }
    }
    for (size_t o = 0; o < overlays.size() && o < 32; ++o) {
        for (size_t c = overlays[o].begin; c < overlays[o].end && c < cells.size(); ++c) cells[c].overlay_mask |= 1u << o;
    }
    Elements out;
    size_t i = 0;
    while (i < cells.size()) {
        size_t j = i;
        std::string run;
        while (j < cells.size() && cells[j].flags == cells[i].flags && cells[j].overlay_mask == cells[i].overlay_mask) run += cells[j++].text;
        Style s = span_style(settings, base, cells[i].flags);
        for (size_t o = 0; o < overlays.size() && o < 32; ++o) {
            if (cells[i].overlay_mask & (1u << o)) s = overlays[o].style->merged_over(s);
        }
        out.push_back(text(run) | decorate(s));
        i = j;
    }
    if (out.empty()) out.push_back(text(""));
    return hbox(out);
}

}  // namespace maic
