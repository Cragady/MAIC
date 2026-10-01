#include "maic/settings.hpp"

#include "maic/instructions.hpp"
#include "maic/lua.hpp"
#include "maic/paths.hpp"
#include "maic/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace maic {

std::vector<ModelPreset> default_presets() {
    // Context sizes are the figures Micaiah gave or the provider's documented ones; a settings `models` entry
    // overrides any of them. Anthropic's reviewer is Sonnet 5 rather than the model itself: a cheaper second
    // reader is what Anthropic recommends for a review step, and the harness never needs the biggest model.
    return {
        {"opus-5.5", "anthropic/claude-opus-5-5", 1000000, "anthropic/claude-sonnet-5", 1},
        {"fable-5.1", "anthropic/claude-fable-5-1", 1000000, "anthropic/claude-sonnet-5", 1},
        {"sonnet-5", "anthropic/claude-sonnet-5", 1000000, "same", 1},
        {"haiku-4.5", "anthropic/claude-haiku-4-5-20251001", 200000, "same", 0},
        {"qwen-4b", "llamacpp/Qwen3.5-4B-Q4_K_M", 16384, "same", 0},
        // The 9B twice: text-only (a folder with a link to the weights and no projector) at 16k, and with its
        // vision projector at 8k, the most an 8 GB card holds for it.
        {"qwen-9b", "llamacpp/Qwen3.5-9B-Q4_K_M-text", 16384, "same", 0},
        {"qwen-9b-vision", "llamacpp/Qwen3.5-9B-Q4_K_M", 8192, "same", 0},
    };
}

namespace {
std::string preset_key(std::string q) {
    for (auto& c : q) c = (c == ' ' || c == '_' || c == '.') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (q.rfind("claude-", 0) == 0) q = q.substr(7);
    return q;
}
}  // namespace

std::optional<ModelPreset> find_preset(const std::vector<ModelPreset>& presets, const std::string& query) {
    std::string k = preset_key(query);
    for (const auto& p : presets) {
        if (preset_key(p.name) == k) return p;
    }
    // "opus-5-5" written with a hyphen for the dot, or "opus 5.5" with a space, both land above; also accept a
    // version written without its dot ("opus55").
    std::string compact;
    for (char c : k) if (c != '-') compact += c;
    for (const auto& p : presets) {
        std::string pc;
        for (char c : preset_key(p.name)) if (c != '-') pc += c;
        if (pc == compact) return p;
    }
    return std::nullopt;
}

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
        {"visual", {std::nullopt, "#3a3a5c", false, false, false, false, true}},
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
        s.context = std::max(1024, j.value("context", s.context));
        s.context_2 = std::max(1024, j.value("context_2", s.context_2));
        s.title_model = j.value("title_model", s.title_model);
        s.budget_tokens = j.value("budget_tokens", s.budget_tokens);
        s.timestamps = j.value("timestamps", s.timestamps);
        s.compact_at = j.value("compact_at", s.compact_at);
        s.compact_keep_results = j.value("compact_keep_results", s.compact_keep_results);
        if (s.leader == "space" || s.leader == "<space>") s.leader = " ";
        if (j.contains("instruction_files")) s.instruction_files = j["instruction_files"].get<std::vector<std::string>>();
        s.load_instructions = j.value("load_instructions", s.load_instructions);
        s.system_prompt = j.value("system_prompt", s.system_prompt);
        s.prefill = j.value("prefill", s.prefill);
        if (j.contains("forbid") && j["forbid"].is_array()) {
            for (const auto& r : j["forbid"]) if (r.is_string() && !r.get<std::string>().empty()) s.forbid.push_back(r.get<std::string>());
        }
        if (j.contains("allow") && j["allow"].is_array()) {
            for (const auto& r : j["allow"]) if (r.is_string() && !r.get<std::string>().empty()) s.allow.push_back(r.get<std::string>());
        }
        if (j.contains("rules") && j["rules"].is_array()) {
            for (const auto& r : j["rules"]) if (r.is_string() && !r.get<std::string>().empty()) s.rules.push_back(r.get<std::string>());
        }
        s.harness = j.value("harness", s.harness);
        s.tripwire = j.value("tripwire", s.tripwire);
        if (s.tripwire != "machine" && s.tripwire != "session" && s.tripwire != "isolated") throw std::runtime_error(path.string() + ": tripwire must be \"machine\", \"session\" or \"isolated\", not \"" + s.tripwire + "\"");
        s.allow_isolated = j.value("allow_isolated", s.allow_isolated);
        s.browser = j.value("browser", s.browser);
        if (s.browser != "default" && s.browser != "firefox" && s.browser != "chrome") throw std::runtime_error(path.string() + ": browser must be default, firefox or chrome");
        s.remote = j.value("remote", s.remote);
        if (j.contains("sampling") && j["sampling"].is_object()) {
            for (const auto& [k, v] : j["sampling"].items()) s.sampling[k] = v;
        }
        if (s.harness != "smart" && s.harness != "dumb") throw std::runtime_error(path.string() + ": harness must be \"smart\" or \"dumb\", not \"" + s.harness + "\"");
        s.reviewer_model = j.value("reviewer_model", s.reviewer_model);
        s.dumb_auto_ok = j.value("dumb_auto_ok", s.dumb_auto_ok);
        if (j.contains("bans")) {
            Bans b = Bans::from_json(j["bans"]);
            // Layers add strings and tokens; the scalar knobs take the nearest value.
            s.bans.strings.insert(s.bans.strings.end(), b.strings.begin(), b.strings.end());
            s.bans.tokens.insert(s.bans.tokens.end(), b.tokens.begin(), b.tokens.end());
            s.bans.patterns.insert(s.bans.patterns.end(), b.patterns.begin(), b.patterns.end());
            if (j["bans"].contains("window")) s.bans.window = b.window;
            if (j["bans"].contains("retries")) s.bans.retries = b.retries;
            if (j["bans"].contains("replacement")) s.bans.replacement = b.replacement;
            if (j["bans"].contains("ignore_case")) s.bans.ignore_case = b.ignore_case;
        }
        json server = j.value("server", json::object());
        s.server.listen = server.value("listen", s.server.listen);
        if (server.contains("workspaces")) {
            s.server.workspaces.clear();
            for (const auto& w : server["workspaces"]) s.server.workspaces.push_back(expand_vars(w.get<std::string>()));
        }
        if (server.contains("cert")) s.server.cert = expand_vars(server["cert"].get<std::string>());
        if (server.contains("key")) s.server.key = expand_vars(server["key"].get<std::string>());
        json preset_table = j.value("models", json::object());  // a named copy: iterating a temporary dangles
        for (const auto& [name, pj] : preset_table.items()) {
            if (!pj.is_object()) continue;
            ModelPreset mp{name, pj.value("model", ""), pj.value("context", 0), pj.value("reviewer", "same"), pj.contains("think") ? (pj["think"].get<bool>() ? 1 : 0) : -1};
            if (mp.model.empty()) throw std::runtime_error(path.string() + ": models." + name + " needs a model");
            bool replaced = false;
            for (auto& existing : s.presets) {
                if (existing.name == name) existing = mp, replaced = true;
            }
            if (!replaced) s.presets.push_back(mp);
        }
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

std::string resolve_system_prompt(const std::string& value) {
    if (value.empty() || value[0] != '@') return value;
    std::string p = value.substr(1);
    if (!p.empty() && p[0] == '~') p = std::string(std::getenv("HOME")) + p.substr(1);
    std::ifstream in(p);
    if (!in) throw std::runtime_error("system prompt file not found: " + p);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
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

void write_default_settings(bool as_json, const std::string& models_dir) {
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
        {"models_dir", models_dir.empty() ? d.models_dir : models_dir},
        {"context", d.context},
        {"//context", "context window in tokens for the local llama.cpp server (${MAIC_CONTEXT} in service files) and the usage readout; --ctx N and :ctx N override"},
        {"context_2", d.context_2},
        {"//context_2", "the same for the side server llamacpp-2 on port 8082 (${MAIC_CONTEXT_2}); --ctx2 N and :ctx2 N override"},
        {"title_model", d.title_model},
        {"budget_tokens", d.budget_tokens},
        {"timestamps", d.timestamps},
        {"compact_at", d.compact_at},
        {"compact_keep_results", d.compact_keep_results},
        {"//sessions_home", "auto: a project's transcripts (it has a MAIC.md) go under sessions/projects/, others under sessions/general/. Or: general, project, a name."},
        {"instruction_files", d.instruction_files},
        {"load_instructions", d.load_instructions},
        {"system_prompt", d.system_prompt},
        {"prefill", d.prefill},
        {"rules", nlohmann::json::array()},
        {"allow", nlohmann::json::array()},
        {"forbid", nlohmann::json::array()},
        {"//forbid", "terms no tool call may contain, in any letter case; write /.../ for a POSIX extended regex. A search, a command, a path or any argument with one is halted before it runs, under the dumb harness too. Added to the built-in list. :forbid in a session"},
        {"//allow", "command patterns (glob over the whole command line) allowed in every mode but plan, without asking or review; MAIC's own helpers (maic-storyboard*, maic-workflow-edit*, maic path* ...) are always on it. Trip patterns still win. Layers add up. :allow in a session"},
        {"//rules", "standing one-line instructions (\"always answer in French\"); they ride with system_prompt at both ends of the system prompt and in the per-turn note. :rule in a session, --rule on the command line"},
        {"//prefill", "text every reply starts with, sent as the opening of the assistant turn; a guarantee where a system prompt is a request"},
        {"harness", d.harness},
        {"tripwire", d.tripwire},
        {"allow_isolated", d.allow_isolated},
        {"//allow_isolated", "true lets a session set tripwire = \"isolated\" (ignore the machine lock); such a session is confined: no reads outside its directory, no remote requests, no server work"},
        {"browser", d.browser},
        {"//browser", "what maic open SERVICE uses: default (the system's), firefox, chrome"},
        {"remote", d.remote},
        {"//remote", "a maic-server you subscribe to, e.g. https://workstation:7373; maic open prefers the remote's services when it answers"},
        {"//tripwire", "machine: a trip sets the root-owned lock every MAIC process respects, unlock asks for sudo; session: a trip locks this session only (a file beside its transcript), :unlock removes it without sudo. A project's .maic/settings.lua can choose per project"},
        {"//harness", "smart: a model reads the conversation and reviews every command or write the rules would allow without asking (auto, edit); dumb: the rule list alone"},
        {"reviewer_model", d.reviewer_model},
        {"dumb_auto_ok", d.dumb_auto_ok},
        {"bans", {{"strings", nlohmann::json::array()}, {"patterns", nlohmann::json::array()}, {"tokens", nlohmann::json::array()}, {"retries", 3}, {"replacement", "[banned]"}, {"ignore_case", false}, {"window", 64}}},
        {"//bans", "strings and POSIX regex patterns the model must not say (cut and re-asked, then replaced); tokens (ids, or text) become logit_bias on OpenAI-compatible providers. docs/bans.md"},
        {"sampling", nlohmann::json::object()},
        {"//sampling", "sampler keys sent with every request: temperature, top_k, top_p, min_p, seed, repeat_penalty; xtc_probability / xtc_threshold on llama.cpp-style servers only. :sampling changes them live"},
        {"//system_prompt", "text placed first in every system prompt, or \"@~/path/to/file.md\"; independent of instruction files"},
        {"//server", "maic server: listen ADDR:PORT (TLS is required off loopback), workspaces remote sessions may open, cert/key (empty: self-signed)."},
        {"server", {{"listen", d.server.listen}, {"workspaces", json::array()}, {"cert", ""}, {"key", ""}}},
        {"providers", providers},
        {"models", json::object()},
        {"//models", "presets by short name, adding to or overriding the built-in ones (opus-5.5, sonnet-5, haiku-4.5, fable-5.1, qwen-4b, qwen-9b, qwen-9b-vision): models = { [\"opus-5.5\"] = { model = \"anthropic/claude-opus-5-5\", context = 1000000, reviewer = \"anthropic/claude-sonnet-5\", think = true } }. reviewer \"same\" means the model reviews itself; context sizes are your plan's figures. A model on the side server: [\"qwen-4b-side\"] = { model = \"llamacpp-2/Qwen3.5-4B-Q4_K_M\", context = 8192 }"},
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
