#include "maic/settings.hpp"

#include "maic/instructions.hpp"
#include "maic/lua.hpp"
#include "maic/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::map<std::string, Style>& default_styles() {
    static const std::map<std::string, Style> styles = {
        // transcript
        {"user", {std::nullopt, std::nullopt, true}},
        {"assistant", {}},
        {"thinking", {std::nullopt, std::nullopt, false, true}},
        {"tool", {"cyan"}},
        {"tool_ok", {"gray_dark"}},
        {"tool_err", {"red"}},
        {"notice", {"yellow"}},
        {"error", {"red_light"}},
        {"shell", {"green_light"}},
        // markdown, applied on top of the entry's style
        {"md_heading", {"magenta", std::nullopt, true}},
        {"md_bold", {std::nullopt, std::nullopt, true}},
        {"md_italic", {std::nullopt, std::nullopt, false, false, true}},
        {"md_code", {"yellow_light"}},
        {"md_code_block", {"white", "#1c1c1c"}},
        {"md_link", {"blue_light", std::nullopt, false, false, false, true}},
        {"md_url", {"gray_dark"}},
        {"md_quote", {"gray", std::nullopt, false, false, true}},
        {"md_bullet", {"cyan"}},
        {"md_rule", {"gray_dark"}},
        // chrome
        {"input", {}},
        {"input_prompt_insert", {"green"}},
        {"input_prompt_normal", {"blue"}},
        {"separator", {"gray_dark"}},
        {"focus", {"cyan"}},
        {"visual", {std::nullopt, "#3a3a5c"}},
        {"search", {"black", "yellow"}},
        {"cursor_line", {std::nullopt, "#262626"}},
        {"status", {}},
        {"status_insert", {"green", std::nullopt, true, false, false, false, true}},
        {"status_normal", {"blue", std::nullopt, true, false, false, false, true}},
        {"status_visual", {"magenta", std::nullopt, true, false, false, false, true}},
        {"status_dim", {std::nullopt, std::nullopt, false, true}},
        {"mode_manual", {"blue", std::nullopt, true}},
        {"mode_auto-read", {"cyan", std::nullopt, true}},
        {"mode_edit", {"yellow", std::nullopt, true}},
        {"mode_auto", {"red", std::nullopt, true}},
        {"mode_plan", {"green", std::nullopt, true}},
        {"harness_armed", {"green"}},
        {"harness_tripped", {"red", std::nullopt, true}},
        {"remote", {"red_light", std::nullopt, true}},
        {"approval", {"yellow"}},
    };
    return styles;
}

Style parse_style(const json& j) {
    Style s;
    if (!j.is_object()) throw std::runtime_error("a style must be an object");
    if (j.contains("fg") && j["fg"].is_string()) s.fg = j["fg"].get<std::string>();
    if (j.contains("bg") && j["bg"].is_string()) s.bg = j["bg"].get<std::string>();
    s.bold = j.value("bold", false);
    s.dim = j.value("dim", false);
    s.italic = j.value("italic", false);
    s.underline = j.value("underline", false);
    s.inverted = j.value("inverted", false);
    return s;
}

json style_json(const Style& s) {
    json j = json::object();
    if (s.fg) j["fg"] = *s.fg;
    if (s.bg) j["bg"] = *s.bg;
    if (s.bold) j["bold"] = true;
    if (s.dim) j["dim"] = true;
    if (s.italic) j["italic"] = true;
    if (s.underline) j["underline"] = true;
    if (s.inverted) j["inverted"] = true;
    return j;
}

}  // namespace

Style Style::merged_over(const Style& base) const {
    Style out = base;
    if (fg) out.fg = fg;
    if (bg) out.bg = bg;
    out.bold |= bold;
    out.dim |= dim;
    out.italic |= italic;
    out.underline |= underline;
    out.inverted |= inverted;
    return out;
}

const Style& Settings::style(const std::string& name) const {
    if (auto it = styles.find(name); it != styles.end()) return it->second;
    static const Style none;
    return none;
}

fs::path settings_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "maic" / "settings.json";
    return fs::path(std::getenv("HOME")) / ".config" / "maic" / "settings.json";
}

namespace {

// Applies one settings location over `s`: `<stem>.lua` when it exists (a chunk returning a table), else
// `<stem>.json`. Scalars replace, providers merge by name, styles merge by role.
void apply_file(Settings& s, const fs::path& json_path, const fs::path& workspace) {
    fs::path lua_path = json_path;
    lua_path.replace_extension(".lua");
    json j;
    fs::path path;
    std::error_code ec;
    if (fs::is_regular_file(lua_path, ec)) {
        path = lua_path;
        try {
            Lua lua(workspace);
            j = lua.eval_table_file(lua_path);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("settings: ") + e.what());
        }
    } else {
        std::ifstream in(json_path);
        if (!in) return;
        path = json_path;
        try {
            j = json::parse(in, nullptr, true, true);  // comments allowed
        } catch (const json::exception& e) {
            throw std::runtime_error(path.string() + ": " + e.what());
        }
    }
    s.sources.push_back(path);
    try {
        s.model = j.value("model", s.model);
        s.mode = j.value("mode", s.mode);
        s.think = j.value("think", s.think);
        s.markdown = j.value("markdown", s.markdown);
        s.mouse = j.value("mouse", s.mouse);
        s.sound = j.value("sound", s.sound);
        s.sessions_home = j.value("sessions_home", s.sessions_home);
        s.leader = j.value("leader", s.leader);
        s.record = j.value("record", s.record);
        s.models_dir = j.value("models_dir", s.models_dir);
        s.compact_at = j.value("compact_at", s.compact_at);
        s.compact_keep_results = j.value("compact_keep_results", s.compact_keep_results);
        if (s.leader == "space" || s.leader == "<space>") s.leader = " ";
        if (j.contains("instruction_files")) s.instruction_files = j["instruction_files"].get<std::vector<std::string>>();
        json providers = j.value("providers", json::object());
        for (const auto& [name, pj] : providers.items()) {
            Provider* p = nullptr;
            for (auto& existing : s.providers) {
                if (existing.name == name) p = &existing;
            }
            if (!p) {
                s.providers.push_back({name, "openai", "", "", "", json::object()});
                p = &s.providers.back();
            }
            p->kind = pj.value("kind", p->kind);
            p->base_url = pj.value("base_url", p->base_url);
            p->api_key_env = pj.value("api_key_env", p->api_key_env);
            p->api_key_command = pj.value("api_key_command", p->api_key_command);
            if (pj.contains("api_key")) throw std::runtime_error("providers." + name + ": keys don't go in settings; use api_key_env or api_key_command");
            json opts = pj.value("options", json::object());
            for (const auto& [k, v] : opts.items()) p->options[k] = v;
            if (p->kind == "ollama" && p->base_url.empty()) p->base_url = "http://127.0.0.1:11434";
            if (p->base_url.empty()) throw std::runtime_error("providers." + name + ": base_url is required");
        }
        json styles = j.value("style", json::object());
        for (const auto& [name, sj] : styles.items()) {
            s.styles[name] = parse_style(sj).merged_over(s.style(name));
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}

}  // namespace

Settings load_settings(const fs::path& workspace) {
    Settings s;
    s.styles = default_styles();
    apply_file(s, settings_path(), workspace);
    // Project layers: from just under $HOME down to the workspace, like instruction files.
    std::error_code ec;
    fs::path home = fs::weakly_canonical(std::getenv("HOME"), ec);
    fs::path ws = fs::weakly_canonical(workspace, ec);
    std::vector<fs::path> dirs;
    for (fs::path d = ws; !d.empty(); d = d.parent_path()) {
        if (d == home) break;
        dirs.push_back(d);
        auto rel = d.lexically_relative(home);
        if (rel.empty() || *rel.begin() == "..") break;  // not under $HOME
        if (d == d.root_path()) break;
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        apply_file(s, *it / ".maic" / "settings.json", workspace);
        apply_file(s, *it / ".maic" / "settings.local.json", workspace);
    }
    return s;
}

Settings load_settings() {
    return load_settings(fs::current_path());
}

fs::path resolve_sessions_home(const Settings& settings, const fs::path& workspace) {
    std::string home = settings.sessions_home;
    if (home == "auto") {
        bool project = false;
        for (const auto& f : load_instructions(workspace, settings.instruction_files)) {
            if (f.path != global_instructions_path()) project = true;
        }
        home = project ? "project" : "general";
    }
    if (home == "project") return sessions_home("project:" + fs::weakly_canonical(workspace).string());
    return sessions_home(home);
}

namespace {

// JSON -> Lua table literal, with the key order kept readable.
std::string lua_literal(const json& j, int indent) {
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    if (j.is_object()) {
        std::string out = "{\n";
        for (const auto& [k, v] : j.items()) {
            if (k.rfind("//", 0) == 0) continue;  // "//key" comments are emitted next to their key below
            if (auto c = j.find("//" + k); c != j.end() && c->is_string()) out += pad + "  -- " + c->get<std::string>() + "\n";
            bool plain = !k.empty() && (std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_') &&
                         std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
            out += pad + "  " + (plain ? k : "[" + json(k).dump() + "]") + " = " + lua_literal(v, indent + 1) + ",\n";
        }
        return out + pad + "}";
    }
    if (j.is_array()) {
        std::string out = "{ ";
        for (const auto& v : j) out += lua_literal(v, indent + 1) + ", ";
        return out + "}";
    }
    return j.dump();  // strings, numbers, booleans, null
}

}  // namespace

void write_default_settings(bool as_json) {
    fs::path p = settings_path();
    if (!as_json) p.replace_extension(".lua");
    if (fs::exists(p) || fs::exists(settings_path()) || fs::exists(fs::path(settings_path()).replace_extension(".lua"))) {
        throw std::runtime_error("a settings file already exists in " + p.parent_path().string());
    }
    fs::create_directories(p.parent_path());
    Settings d;
    json providers = json::object();
    for (const auto& pr : d.providers) {
        json pj = {{"kind", pr.kind}, {"base_url", pr.base_url}};
        if (!pr.api_key_env.empty()) pj["api_key_env"] = pr.api_key_env;
        if (!pr.options.empty()) pj["options"] = pr.options;
        providers[pr.name] = pj;
    }
    json styles = json::object();
    for (const auto& [name, st] : default_styles()) styles[name] = style_json(st);
    json j = {
        {"//", "MAIC settings. Every key is optional; delete what you don't change. Comments are allowed."},
        {"model", d.model},
        {"mode", d.mode},
        {"think", d.think},
        {"markdown", d.markdown},
        {"mouse", d.mouse},
        {"sessions_home", d.sessions_home},
        {"leader", "space"},
        {"record", d.record},
        {"models_dir", d.models_dir},
        {"compact_at", d.compact_at},
        {"compact_keep_results", d.compact_keep_results},
        {"//sessions_home", "auto: a project's transcripts (it has a MAIC.md) go under sessions/projects/, others under sessions/general/. Or: general, project, a name."},
        {"instruction_files", d.instruction_files},
        {"providers", providers},
        {"style", styles},
    };
    std::ofstream out(p);
    if (as_json) {
        out << j.dump(2) << "\n";
        return;
    }
    j.erase("//");
    out << "-- MAIC settings (Lua). Every key is optional; delete what you don't change. This file is code: use\n"
           "-- os.getenv, maic.hostname, maic.home or maic.workspace for per-machine choices. A settings.json in the same\n"
           "-- place is used only when no settings.lua exists. Reference: docs/settings.md\n"
           "return "
        << lua_literal(j, 0) << "\n";
}

}  // namespace maic
