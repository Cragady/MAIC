#include "maic/settings.hpp"

#include "maic/lua.hpp"
#include "maic/paths.hpp"
#include "maic/session.hpp"
#include "maic/theme.hpp"
#include "maic/trust.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>

namespace maic {

std::vector<ModelPreset> default_presets() {
    // Context sizes are the figures Micaiah gave or the provider's documented ones; a settings `models` entry
    // changes any of them. The Anthropic presets leave `reviewer` empty, so the harness reviews on the small
    // model (Haiku): a one-line verdict needs no more. The local ones review with themselves, already loaded.
    // Fable is marked limited because Fable plans commonly carry a usage cap (Micaiah's does): its subagents
    // and its reviewer step aside to a model without one. A local preset hands subagents only to local presets,
    // so a local session's data stays on the machine unless the user adds a cloud preset to its list.
    const std::vector<std::string> cloud = {"fable-5.1", "opus-5.5", "sonnet-5", "haiku-4.5"};
    const std::vector<std::string> local = {"qwen-9b", "qwen-9b-vision", "qwen-4b"};
    return {
        {"opus-5.5", "anthropic/claude-opus-5-5", 1000000, "", 1, 40, false, cloud},
        {"fable-5.1", "anthropic/claude-fable-5-1", 1000000, "", 1, 50, true, cloud},
        {"sonnet-5", "anthropic/claude-sonnet-5", 1000000, "", 1, 30, false, cloud},
        {"haiku-4.5", "anthropic/claude-haiku-4-5-20251001", 200000, "", 0, 20, false, cloud},
        {"qwen-4b", "llamacpp/Qwen3.5-4B-Q4_K_M", 16384, "same", 0, 10, false, local},
        // The 9B twice: text-only (a folder with a link to the weights and no projector) at 16k, and with its
        // vision projector at 8k, the most an 8 GB card holds for it.
        {"qwen-9b", "llamacpp/Qwen3.5-9B-Q4_K_M-text", 16384, "same", 0, 12, false, local},
        {"qwen-9b-vision", "llamacpp/Qwen3.5-9B-Q4_K_M", 8192, "same", 0, 12, false, local},
        // Claude Code on the user's own login and plan: the helpers' model, or the agent on MAIC's tools over MCP.
        {"claude-haiku-cli", "claude-cli/haiku", 200000, "", -1, 20, false, {}},
        {"claude-sonnet-cli", "claude-cli/sonnet", 1000000, "", -1, 30, false, {}},
    };
}

namespace {
std::string preset_key(std::string q) {
    for (auto& c : q) c = (c == ' ' || c == '_' || c == '.') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (q.rfind("claude-", 0) == 0) q = q.substr(7);
    return q;
}

// The strongest non-limited preset other than `p` on its list, below its tier if one is; nullopt when none.
std::optional<ModelPreset> step_aside(const std::vector<ModelPreset>& presets, const ModelPreset& p) {
    std::optional<ModelPreset> below, any;
    for (const auto& q : subagent_presets(presets, p)) {
        if (q.limited || q.name == p.name) continue;
        if (!any) any = q;
        if (!below && q.tier < p.tier) below = q;
    }
    return below ? below : any;
}

ModelPick pick_of(const ModelPreset& p, std::string reason) {
    return {p.name, p.model, std::move(reason)};
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

std::optional<ModelPreset> preset_for_model(const std::vector<ModelPreset>& presets, const std::string& model) {
    for (const auto& p : presets) {
        if (p.model == model) return p;
    }
    return std::nullopt;
}

void set_preset_window(std::vector<Provider>& providers, const ModelPreset& preset) {
    if (preset.context <= 0) return;
    std::string name = resolve_model(providers, preset.model).first.name;
    for (auto& pr : providers) {
        if (pr.name == name) pr.options["context_window"] = preset.context;
    }
}

std::vector<ModelPreset> subagent_presets(const std::vector<ModelPreset>& presets, const ModelPreset& p) {
    std::vector<ModelPreset> out = {p};
    for (const auto& n : p.subagents) {
        auto q = find_preset(presets, n);
        if (q && std::none_of(out.begin(), out.end(), [&](const ModelPreset& o) { return o.name == q->name; })) out.push_back(*q);
    }
    std::stable_sort(out.begin(), out.end(), [](const ModelPreset& a, const ModelPreset& b) { return a.tier > b.tier; });
    return out;
}

ModelPick subagent_pick(const std::vector<ModelPreset>& presets, const ModelPreset& p) {
    if (!p.subagent.empty()) {
        if (preset_key(p.subagent) == "same") return pick_of(p, p.name + "'s subagent setting");
        if (auto q = find_preset(presets, p.subagent)) return pick_of(*q, p.name + "'s subagent setting");
    }
    if (!p.limited) return pick_of(p, "the same model");
    if (auto q = step_aside(presets, p)) return pick_of(*q, p.name + " is limited");
    return pick_of(p, p.name + " is limited, but every model on its list is too");
}

std::optional<ModelPreset> on_limit_pick(const std::vector<ModelPreset>& presets, const ModelPreset& p) {
    if (!p.on_limit.empty()) {
        if (auto q = find_preset(presets, p.on_limit); q && q->name != p.name) return q;
    }
    return step_aside(presets, p);
}

ModelPick default_small_model(const std::vector<ModelPreset>& presets, const std::vector<Provider>& providers, const ModelPreset& p) {
    if (!resolve_model(providers, p.model).first.remote()) return pick_of(p, "a local model reviews itself");
    auto list = subagent_presets(presets, p);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (!it->limited) return pick_of(*it, "the small model (the lowest non-limited tier on " + p.name + "'s list)");
    }
    return pick_of(p, "the model itself (nothing on its list is unlimited)");
}

ModelPick reviewer_pick(const std::vector<ModelPreset>& presets, const std::vector<Provider>& providers, const std::string& model,
                        const std::string& pin, const std::string& small_model, const std::set<std::string>& failed) {
    auto self = preset_for_model(presets, model);
    auto named = [&](const std::string& m, std::string reason) {
        auto q = find_preset(presets, m);
        if (!q) q = preset_for_model(presets, m);
        return q ? pick_of(*q, std::move(reason)) : ModelPick{"", m, std::move(reason)};
    };
    ModelPick pick;
    if (!pin.empty()) pick = named(pin, "reviewer_model");
    else if (self && self->reviewer == "same") pick = pick_of(*self, self->name + "'s reviewer setting (itself)");
    else if (self && !self->reviewer.empty()) pick = named(self->reviewer, self->name + "'s reviewer setting");
    else if (!small_model.empty()) pick = named(small_model, "small_model");
    else if (self) pick = default_small_model(presets, providers, *self);
    else pick = {"", model, "the session's model"};
    if (!failed.count(pick.model)) return pick;

    std::string who = pick.preset.empty() ? pick.model : pick.preset;
    auto failed_preset = preset_for_model(presets, pick.model);
    if (!failed_preset) return {"", "", who + " hit its usage limit and is not a preset with a fallback"};
    int cap = failed_preset->tier;
    for (const auto& m : failed) {
        if (auto q = preset_for_model(presets, m)) cap = std::min(cap, q->tier);
    }
    auto usable = [&](const ModelPreset& q) { return !q.limited && !failed.count(q.model) && q.tier <= cap; };
    std::string reason = who + " hit its usage limit";
    if (!failed_preset->on_limit.empty()) {
        if (auto q = find_preset(presets, failed_preset->on_limit); q && usable(*q)) return pick_of(*q, reason + " (its on_limit)");
    }
    auto list = subagent_presets(presets, self ? *self : *failed_preset);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (usable(*it)) return pick_of(*it, reason);
    }
    return {"", "", reason + " and nothing cheaper is left"};
}

namespace fs = std::filesystem;
using nlohmann::json;

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
        // the nvim highlighter's captures (highlight = "nvim"), applied like the markdown styles
        {"hl_keyword", {"blue_light", std::nullopt, true}},
        {"hl_string", {"green"}},
        {"hl_comment", {"gray", std::nullopt, false, false, true}},
        {"hl_heading", {"magenta", std::nullopt, true}},
        {"hl_code", {"yellow_light"}},
        // diffs: the approval preview and tool output that is a diff
        {"diff_added", {"green"}},
        {"diff_removed", {"red"}},
        {"diff_hunk", {std::nullopt, std::nullopt, false, true}},
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

namespace {

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

// The global file's own tier and memory cap, read from its text: they have to be known before the file runs, so
// they count only when written literally (global_lua = "sandbox", lua_memory_mb = 512).
std::pair<std::string, size_t> literal_lua_choice(const fs::path& lua_path) {
    std::ifstream in(lua_path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::regex tier(R"((^|[\s,{;])global_lua\s*=\s*["'](full|sandbox|restricted)["'])");
    static const std::regex memory(R"((^|[\s,{;])lua_memory_mb\s*=\s*([0-9]+))");
    std::smatch m;
    std::pair<std::string, size_t> out{"", 0};
    if (std::regex_search(text, m, tier)) out.first = m[2].str();
    if (std::regex_search(text, m, memory)) out.second = std::stoul(m[2].str());
    return out;
}

}  // namespace

bool SteeringSettings::allows(const std::string& action, bool remote) const {
    const auto& side = remote ? clients_remote : clients_local;
    return std::find(actions.begin(), actions.end(), action) != actions.end() && std::find(side.begin(), side.end(), action) != side.end();
}

void read_steering(SteeringSettings& into, const json& t, const std::string& where, bool global, bool narrow_only, std::vector<std::string>& warnings) {
    if (!t.is_object()) throw std::runtime_error(where + " must be a table");
    const auto all = SteeringSettings::steer_actions();
    auto list = [&](const json& v, const std::string& key) {
        if (v.is_string() && (v == "all" || v == "none")) return v == "all" ? all : std::vector<std::string>{};
        if (!v.is_array()) throw std::runtime_error(where + "." + key + " must be a list of actions, \"all\" or \"none\"");
        std::vector<std::string> out;
        for (const auto& a : v) {
            std::string name = a.is_string() ? a.get<std::string>() : "";
            if (std::find(all.begin(), all.end(), name) == all.end()) throw std::runtime_error(where + "." + key + ": no steering action " + a.dump() + " (steer, drop, further, interrupt, keep, halt)");
            out.push_back(name);
        }
        return out;
    };
    auto narrowed = [&](std::vector<std::string>& current, const std::vector<std::string>& given, const std::string& key) {
        if (narrow_only) {
            for (const auto& a : given) {
                if (std::find(current.begin(), current.end(), a) == current.end()) throw std::runtime_error(where + "." + key + ": " + a + " is not allowed above; this layer can only remove actions");
            }
        }
        current = given;
        into.from[key] = where;
    };
    for (const auto& [key, v] : t.items()) {
        if (key == "actions") {
            narrowed(into.actions, list(v, key), key);
        } else if (key == "ban_actions") {
            auto given = list(v, key);
            if (std::find(given.begin(), given.end(), "further") != given.end()) throw std::runtime_error(where + ".ban_actions: further is never a ban's (it would go deeper into the banned topic)");
            narrowed(into.ban_actions, given, key);
        } else if (key == "halt_message") {
            if (!v.is_string() || v.get<std::string>().empty()) throw std::runtime_error(where + ".halt_message must be text");
            into.halt_message = v;
            into.from[key] = where;
        } else if (key == "drop_trim") {
            std::string d = v.is_string() ? v.get<std::string>() : "";
            if (d != "none" && d != "sentence" && d != "paragraph" && d != "all") throw std::runtime_error(where + ".drop_trim must be none, sentence, paragraph or all");
            into.drop_trim = d;
            into.from[key] = where;
        } else if (key == "on_running_tool") {
            std::string d = v.is_string() ? v.get<std::string>() : "";
            if (d != "cancel" && d != "wait") throw std::runtime_error(where + ".on_running_tool must be cancel or wait");
            into.on_running_tool = d;
            into.from[key] = where;
        } else if (key == "clients") {
            if (!global) {
                warnings.push_back(where + ".clients is ignored: only your global settings file sets it");
                continue;
            }
            if (!v.is_object()) throw std::runtime_error(where + ".clients must be a table: { [\"local\"] = \"all\", remote = \"all\" }");
            if (v.contains("local")) into.clients_local = list(v["local"], "clients.local");
            if (v.contains("remote")) into.clients_remote = list(v["remote"], "clients.remote");
            into.from["clients"] = where;
        } else {
            throw std::runtime_error(where + ": steering has no key " + key + " (actions, halt_message, drop_trim, on_running_tool, clients, ban_actions)");
        }
    }
}

namespace {

// Applies one settings location over `s`: `<stem>.lua` when it exists (a chunk returning a table), else
// `<stem>.json`. Scalars replace, providers merge by name, styles merge by role. `global` is the user's own
// file: its Lua runs at the tier it names literally (full by default), and only it sets global_lua,
// lua_memory_mb and the trust_* keys. A project's file runs at its directory's trust level (`tier`).
void apply_file(Settings& s, const fs::path& json_path, const fs::path& workspace, bool global, LuaTier tier, size_t memory_mb) {
    fs::path lua_path = json_path;
    lua_path.replace_extension(".lua");
    json j;
    fs::path path;
    std::error_code ec;
    if (fs::is_regular_file(lua_path, ec)) {
        path = lua_path;
        std::pair<std::string, size_t> literal;
        if (global) {
            literal = literal_lua_choice(lua_path);
            tier = parse_lua_tier(literal.first).value_or(LuaTier::Full);
            if (literal.second) memory_mb = literal.second;
        }
        try {
            j = eval_lua_data_file(lua_path, workspace, tier, memory_mb, &s.warnings);
        } catch (const std::exception& e) {
            std::string what = e.what();
            throw std::runtime_error("settings: " + (what.find(lua_path.string()) == std::string::npos ? lua_path.string() + ": " : "") + what);
        }
        if (global && j.is_object() && j.contains("global_lua") && j["global_lua"] != (literal.first.empty() ? json("full") : json(literal.first))) {
            throw std::runtime_error(lua_path.string() + ": global_lua counts only written literally (global_lua = \"sandbox\"), since it decides how the file runs");
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
    s.layered.merge_patch(j);
    try {
        if (global) {
            s.global_lua = j.value("global_lua", s.global_lua);
            if (!parse_lua_tier(s.global_lua)) throw std::runtime_error(path.string() + ": global_lua must be \"full\", \"sandbox\" or \"restricted\"");
            s.lua_memory_mb = j.value("lua_memory_mb", s.lua_memory_mb);
            if (s.lua_memory_mb < 16) throw std::runtime_error(path.string() + ": lua_memory_mb must be at least 16");
            s.trust_strictness = j.value("trust_strictness", s.trust_strictness);
            if (!valid_trust_level(s.trust_strictness)) throw std::runtime_error(path.string() + ": trust_strictness must be \"strict\", \"standard\" or \"relaxed\"");
            if (j.contains("trust_identities") && j["trust_identities"].is_array()) s.trust_identities = j["trust_identities"].get<std::vector<std::string>>();
            json levels = j.value("trust_levels", json::object());
            for (const auto& [dir, level] : levels.items()) {
                if (!level.is_string() || !valid_trust_level(level.get<std::string>())) throw std::runtime_error(path.string() + ": trust_levels." + dir + " must be \"strict\", \"standard\" or \"relaxed\"");
                s.trust_levels[dir] = level.get<std::string>();
            }
            s.protocol_tier = j.value("protocol_tier", s.protocol_tier);
            if (!valid_protocol_tier(s.protocol_tier)) throw std::runtime_error(path.string() + ": protocol_tier must be \"open\", \"guarded\" or \"airtight\"");
            json tiers = j.value("protocol_tiers", json::object());
            for (const auto& [dir, tier] : tiers.items()) {
                if (!tier.is_string() || !valid_protocol_tier(tier.get<std::string>())) throw std::runtime_error(path.string() + ": protocol_tiers." + dir + " must be \"open\", \"guarded\" or \"airtight\"");
                s.protocol_tiers[dir] = tier.get<std::string>();
            }
            json chain = j.value("instructions", json::object());
            if (chain.contains("project_markers") && chain["project_markers"].is_array()) s.project_markers = chain["project_markers"].get<std::vector<std::string>>();
            for (auto& m : s.project_markers) {
                while (m.size() > 1 && m.back() == '/') m.pop_back();
            }
            s.instructions_bound = chain.value("bound", s.instructions_bound);
            if (s.instructions_bound != "project" && s.instructions_bound != "home") throw std::runtime_error(path.string() + ": instructions.bound must be \"project\" or \"home\"");
            InstructionOptions& o = s.instructions;
            if (chain.contains("files")) {
                o.files = chain["files"].get<std::vector<std::string>>();
                for (const auto& f : o.files) {
                    if (f.empty() || f.find('/') != std::string::npos || f == "." || f == "..") throw std::runtime_error(path.string() + ": instructions.files holds file names, not \"" + f + "\"");
                }
            }
            std::string read = chain.value("read", o.highest ? "highest" : "all");
            if (read != "all" && read != "highest") throw std::runtime_error(path.string() + ": instructions.read must be \"all\" or \"highest\"");
            o.highest = read == "highest";
            o.local_files = chain.value("local_files", o.local_files);
            o.import_depth = chain.value("imports", json::object()).value("depth", o.import_depth);
            if (o.import_depth < 0) throw std::runtime_error(path.string() + ": instructions.imports.depth must be 0 or more");
            o.extra_dirs = chain.value("extra_dirs", o.extra_dirs);
        } else if (j.is_object()) {
            for (const auto& [key, v] : j.items()) {
                if (key == "global_lua" || key == "lua_memory_mb" || key.rfind("trust_", 0) == 0 || key.rfind("protocol_tier", 0) == 0) s.warnings.push_back(path.string() + ": " + key + " is ignored: only your global settings file sets it");
            }
            json chain = j.value("instructions", json::object());
            if (chain.is_object()) {
                for (const auto& [key, v] : chain.items()) s.warnings.push_back(path.string() + ": instructions." + key + " is ignored: only your global settings file sets it");
            }
        }
        if (j.is_object() && j.contains("instruction_files")) s.warnings.push_back(path.string() + ": instruction_files is ignored: instructions.files in your global settings file replaces it (docs/instructions.md)");
        s.model = j.value("model", s.model);
        s.mode = j.value("mode", s.mode);
        s.think = j.value("think", s.think);
        s.markdown = j.value("markdown", s.markdown);
        s.mouse = j.value("mouse", s.mouse);
        s.sound = j.value("sound", s.sound);
        s.sessions_home = j.value("sessions_home", s.sessions_home);
        s.init_move_outside_reads = j.value("init_move_outside_reads", s.init_move_outside_reads);
        s.full_output = j.value("full_output", s.full_output);
        s.full_output_max_mb = j.value("full_output_max_mb", s.full_output_max_mb);
        if (s.full_output_max_mb < 1) throw std::runtime_error(path.string() + ": full_output_max_mb must be at least 1");
        s.leader = j.value("leader", s.leader);
        s.highlight = j.value("highlight", s.highlight);
        s.theme = j.value("theme", s.theme);
        s.follow_nvim_theme = j.value("follow_nvim_theme", s.follow_nvim_theme);
        s.bare = j.value("bare", s.bare);
        s.ui = j.value("ui", s.ui);
        if (s.ui != "tui" && s.ui != "nvim") throw std::runtime_error(path.string() + ": ui must be \"tui\" or \"nvim\", not \"" + s.ui + "\"");
        s.daemon = j.value("daemon", s.daemon);
        if (s.daemon != "attach" && s.daemon != "off") throw std::runtime_error(path.string() + ": daemon must be \"attach\" or \"off\", not \"" + s.daemon + "\"");
        s.colors = j.value("colors", s.colors);
        if (s.colors != "auto" && s.colors != "truecolor" && s.colors != "256" && s.colors != "16") throw std::runtime_error(path.string() + ": colors must be \"auto\", \"truecolor\", \"256\" or \"16\", not \"" + s.colors + "\"");
        s.enter_sends = j.value("enter_sends", s.enter_sends);
        s.session_leave = j.value("session_leave", s.session_leave);
        if (s.session_leave != "default" && s.session_leave != "ask" && s.session_leave != "bg" && s.session_leave != "park" && s.session_leave != "stop") {
            throw std::runtime_error(path.string() + ": session_leave must be \"default\", \"ask\", \"bg\", \"park\" or \"stop\", not \"" + s.session_leave + "\"");
        }
        s.record = j.value("record", s.record);
        s.models_dir = j.value("models_dir", s.models_dir);
        s.context = std::max(1024, j.value("context", s.context));
        s.context_2 = std::max(1024, j.value("context_2", s.context_2));
        s.small_model = j.value("small_model", j.value("title_model", s.small_model));  // title_model: the older name
        s.budget_tokens = j.value("budget_tokens", s.budget_tokens);
        s.timestamps = j.value("timestamps", s.timestamps);
        s.compact_at = j.value("compact_at", s.compact_at);
        s.compact_keep_results = j.value("compact_keep_results", s.compact_keep_results);
        s.compact_model = j.value("compact_model", s.compact_model);
        if (s.leader == "space" || s.leader == "<space>") s.leader = " ";
        s.load_instructions = j.value("load_instructions", s.load_instructions);
        s.system_prompt = j.value("system_prompt", s.system_prompt);
        s.prefill = j.value("prefill", s.prefill);
        if (j.contains("forbid") && j["forbid"].is_array()) {
            for (const auto& r : j["forbid"]) if (r.is_string() && !r.get<std::string>().empty()) s.forbid.push_back(r.get<std::string>());
        }
        if (j.contains("allow") && j["allow"].is_array()) {
            for (const auto& r : j["allow"]) if (r.is_string() && !r.get<std::string>().empty()) s.permission.allow.push_back("run_shell:" + r.get<std::string>());
        }
        if (j.contains("permission")) {
            if (!j["permission"].is_object()) throw std::runtime_error(path.string() + ": permission must be a table with allow, ask and deny lists");
            auto entries = [&](const char* key, std::vector<std::string>& into) {
                if (!j["permission"].contains(key)) return;
                for (const auto& e : j["permission"][key]) {
                    std::string entry = e.is_string() ? e.get<std::string>() : "";
                    size_t colon = entry.find(':');
                    if (colon == std::string::npos || colon == 0 || colon + 1 == entry.size()) {
                        throw std::runtime_error(path.string() + ": permission." + key + " entries are \"tool:pattern\" (run_shell:pytest *, write_file:src/**), not \"" + entry + "\"");
                    }
                    into.push_back(entry);
                }
            };
            entries("allow", s.permission.allow);
            entries("ask", s.permission.ask);
            entries("deny", s.permission.deny);
        }
        std::vector<AgentDef> builtins = default_agent_defs();
        for (const char* key : {"profiles", "agents"}) {  // `profiles` is the older spelling
            json agent_table = j.value(key, json::object());
            if (!agent_table.is_object()) throw std::runtime_error(path.string() + ": " + key + " must be a table of agents by name");
            for (const auto& [given, pj] : agent_table.items()) {
                std::string name = agent_def_name(given);
                std::string where = path.string() + ": " + key + "." + given;
                if (!pj.is_object()) throw std::runtime_error(where + " must be a table");
                const AgentDef* builtin = find_agent_def(builtins, name);
                AgentDef p = builtin ? *builtin : AgentDef{name};
                if (pj.contains("mode")) {
                    auto m = parse_mode(pj["mode"].get<std::string>());
                    if (!m) throw std::runtime_error(where + ".mode must be manual, auto-read, edit, auto or plan");
                    if (builtin && narrower_mode(*m, builtin->mode) != *m) throw std::runtime_error(where + ": mode " + std::string(mode_name(*m)) + " is wider than the built-in " + name + " (" + std::string(mode_name(builtin->mode)) + "); an agent can only narrow");
                    p.mode = *m;
                }
                if (pj.contains("role")) {
                    auto r = parse_role(pj["role"].get<std::string>());
                    if (!r) throw std::runtime_error(where + ".role must be primary, subagent or all");
                    p.role = *r;
                }
                p.description = pj.value("description", p.description);
                if (pj.contains("write_paths")) p.write_paths = pj["write_paths"].get<std::vector<std::string>>();
                if (pj.contains("read_outside")) {
                    p.read_outside = pj["read_outside"].get<bool>();
                    if (builtin && p.read_outside && !builtin->read_outside) throw std::runtime_error(where + ": the built-in " + name + " does not read outside the workspace; an agent can only narrow");
                }
                if (pj.value("network", false)) throw std::runtime_error(where + ": no agent has the network yet");
                if (pj.contains("budget_tokens")) {
                    p.budget_tokens = pj["budget_tokens"].get<long>();
                    if (builtin && builtin->budget_tokens > 0 && (p.budget_tokens <= 0 || p.budget_tokens > builtin->budget_tokens)) throw std::runtime_error(where + ": budget_tokens above the built-in " + name + "'s " + std::to_string(builtin->budget_tokens) + "; an agent can only narrow");
                }
                if (pj.contains("max_steps")) {
                    p.max_steps = pj["max_steps"].get<int>();
                    if (p.max_steps < 1 || (builtin && p.max_steps > builtin->max_steps)) throw std::runtime_error(where + ": max_steps must be between 1 and " + std::to_string(builtin ? builtin->max_steps : AgentDef{}.max_steps));
                }
                if (pj.contains("tools")) {
                    p.tools = pj["tools"].get<std::vector<std::string>>();
                    if (builtin && !builtin->tools.empty()) {
                        for (const auto& t : p.tools) {
                            if (!builtin->allows_tool(t)) throw std::runtime_error(where + ": the built-in " + name + " has no " + t + " tool; an agent can only narrow");
                        }
                    }
                }
                if (pj.contains("reviewer")) p.reviewer = pj["reviewer"].get<bool>();
                p.model = pj.value("model", p.model);
                if (pj.contains("steering")) {
                    // Checked now against the full set; the session's own narrows it again when it runs as the agent.
                    SteeringSettings check;
                    read_steering(check, pj["steering"], where + ".steering", global, true, s.warnings);
                    p.steering = pj["steering"];
                    if (!global) p.steering.erase("clients");
                }
                if (pj.contains("protocol_tier")) {
                    std::string tier = pj["protocol_tier"].is_string() ? pj["protocol_tier"].get<std::string>() : "";
                    if (!valid_protocol_tier(tier)) throw std::runtime_error(where + ".protocol_tier must be open, guarded or airtight");
                    if (global) p.protocol_tier = tier;
                    else s.warnings.push_back(where + ".protocol_tier is ignored: only your global settings file sets it");
                }
                bool replaced = false;
                for (auto& existing : s.agents) {
                    if (existing.name == name) existing = p, replaced = true;
                }
                if (!replaced) s.agents.push_back(p);
            }
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
        s.lazy_lock = j.value("lazy_lock", s.lazy_lock);
        s.lazy_lock_notice = j.value("lazy_lock_notice", s.lazy_lock_notice);
        if (j.contains("sampling") && j["sampling"].is_object()) {
            for (const auto& [k, v] : j["sampling"].items()) s.sampling[k] = v;
        }
        if (s.harness != "smart" && s.harness != "dumb") throw std::runtime_error(path.string() + ": harness must be \"smart\" or \"dumb\", not \"" + s.harness + "\"");
        s.reviewer_model = j.value("reviewer_model", s.reviewer_model);
        s.reviewer_budget_tokens = j.value("reviewer_budget_tokens", s.reviewer_budget_tokens);
        s.dumb_auto_ok = j.value("dumb_auto_ok", s.dumb_auto_ok);
        if (j.contains("steering")) read_steering(s.steering, j["steering"], path.string() + ": steering", global, !global, s.warnings);
        if (j.contains("bans")) {
            Bans b = Bans::from_json(j["bans"]);
            // Layers add strings, patterns and tokens; the scalar knobs take the nearest value.
            s.bans.add(b);
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
        s.server.relay = server.value("relay", s.server.relay);
        if (server.contains("relay_cert")) s.server.relay_cert = expand_vars(server["relay_cert"].get<std::string>());
        json preset_table = j.value("models", json::object());  // a named copy: iterating a temporary dangles
        for (const auto& [name, pj] : preset_table.items()) {
            if (!pj.is_object()) continue;
            // An existing preset changes field by field; a new one needs a model.
            ModelPreset* mp = nullptr;
            for (auto& existing : s.presets) {
                if (existing.name == name) mp = &existing;
            }
            if (!mp) {
                if (pj.value("model", "").empty()) throw std::runtime_error(path.string() + ": models." + name + " needs a model");
                s.presets.push_back({name});
                mp = &s.presets.back();
            }
            mp->model = pj.value("model", mp->model);
            mp->context = pj.value("context", mp->context);
            mp->reviewer = pj.value("reviewer", mp->reviewer);
            if (pj.contains("think")) mp->think = pj["think"].get<bool>() ? 1 : 0;
            mp->tier = pj.value("tier", mp->tier);
            mp->limited = pj.value("limited", mp->limited);
            if (pj.contains("subagents")) {
                mp->subagents.clear();
                for (const auto& n : pj["subagents"]) mp->subagents.push_back(n.get<std::string>());  // an empty Lua table arrives as {}
            }
            mp->subagent = pj.value("subagent", mp->subagent);
            mp->on_limit = pj.value("on_limit", mp->on_limit);
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
            p->upstream = pj.value("upstream", p->upstream);
            if (pj.contains("api_key")) throw std::runtime_error("providers." + name + ": keys don't go in settings; use api_key_env or api_key_command");
            json opts = pj.value("options", json::object());
            for (const auto& [k, v] : opts.items()) p->options[k] = v;
            if (p->base_url.empty() && p->kind != "cli") throw std::runtime_error("providers." + name + ": base_url is required");
        }
        json styles = j.value("style", json::object());
        for (const auto& [name, sj] : styles.items()) {
            s.style_overrides[name] = parse_style(sj).merged_over(s.style_overrides[name]);
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}

}  // namespace

Settings load_settings(const fs::path& workspace) {
    Settings s;
    apply_file(s, settings_path(), workspace, true, LuaTier::Full, s.lua_memory_mb);
    set_trust_config({s.trust_strictness, s.trust_identities, s.trust_levels, s.project_markers, s.instructions_bound, s.instructions});
    set_lua_data_limits({*parse_lua_tier(s.global_lua), size_t(s.lua_memory_mb)});
    // audit.lua is the user's own file, at their Lua level; no project layer below can touch it.
    s.audit = load_audit_settings();
    // Project layers: the config chain (the project root, or just under $HOME, down to the workspace), like
    // instruction files, each only once its directory is trusted (docs/harness.md, Trust).
    for (const auto& d : config_chain(workspace)) {
        if (!trusted(d)) continue;
        LuaTier tier = trust_lua_tier(d);
        apply_file(s, d / ".maic" / "settings.json", workspace, false, tier, s.lua_memory_mb);
        apply_file(s, d / ".maic" / "settings.local.json", workspace, false, tier, s.lua_memory_mb);
    }
    // An agent's model, small_model and the names inside presets may be preset names; presets from every
    // layer are known only now.
    for (auto& p : s.agents) {
        if (p.model.empty()) continue;
        if (auto preset = find_preset(s.presets, p.model)) p.model = preset->model;
    }
    if (auto preset = find_preset(s.presets, s.small_model)) s.small_model = preset->model;
    if (auto preset = find_preset(s.presets, s.compact_model)) s.compact_model = preset->model;
    for (const auto& p : s.presets) {
        auto known = [&](const std::string& n, const char* field) {
            if (!find_preset(s.presets, n)) throw std::runtime_error("models." + p.name + "." + field + ": no preset named " + n);
        };
        for (const auto& n : p.subagents) known(n, "subagents");
        if (!p.subagent.empty() && p.subagent != "same") known(p.subagent, "subagent");
        if (!p.on_limit.empty()) known(p.on_limit, "on_limit");
    }
    // A ban's steer is checked once every layer has said which actions bans may name.
    for (const auto& steers : {s.bans.string_steers, s.bans.pattern_steers}) {
        for (const auto& b : steers) {
            if (b.action.empty()) continue;
            if (std::find(s.steering.ban_actions.begin(), s.steering.ban_actions.end(), b.action) == s.steering.ban_actions.end()) {
                throw std::runtime_error("bans: an entry names steer = \"" + b.action + "\", which is not in steering.ban_actions" +
                                         (b.action == "further" ? " (further never is: it would go deeper into the banned topic)" : ""));
            }
        }
    }
    if (const char* bare = std::getenv("MAIC_BARE"); bare && std::string(bare) == "1") s.bare = true;
    // The theme is read once every layer has had its say; a broken one leaves the built-in default and the reason.
    try {
        apply_theme(s, load_theme(s.theme));
    } catch (const std::exception& e) {
        s.theme_error = e.what();
        apply_theme(s, Theme{"default"});
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
        // Where transcripts go is not a matter of trust: a MAIC.md on the chain counts whether or not it is trusted.
        bool project = false;
        std::error_code ec;
        for (const auto& d : config_chain(workspace)) {
            for (const auto& name : settings.instructions.files) project = project || fs::is_regular_file(d / name, ec);
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
        if (!pr.upstream.empty()) pj["upstream"] = pr.upstream;
        if (!pr.options.empty()) pj["options"] = pr.options;
        providers[pr.name] = pj;
    }
    json j = {
        {"//", "MAIC settings. Every key is optional; delete what you don't change. Comments are allowed."},
        {"model", d.model},
        {"mode", d.mode},
        {"think", d.think},
        {"markdown", d.markdown},
        {"mouse", d.mouse},
        {"sessions_home", d.sessions_home},
        {"init_move_outside_reads", d.init_move_outside_reads},
        {"//init_move_outside_reads", ":init moves this session into the project's home without asking when it wrote nothing outside the project and read at most this many files there"},
        {"full_output", d.full_output},
        {"//full_output", "keep a command's whole output beside the session when the model gets it capped: <session>.d/<call>.out, display only (maic sessions output). docs/sessions.md"},
        {"full_output_max_mb", d.full_output_max_mb},
        {"//full_output_max_mb", "at most this many MiB of it per call; past that the file keeps the head and the tail and says how much was dropped"},
        {"leader", "space"},
        {"highlight", d.highlight},
        {"theme", d.theme},
        {"//theme", "a theme by name: default, gruvbox-dark, gruvbox-light, mono, or a file of yours in ~/.config/maic/themes/NAME.lua; `style` entries below override single roles on top of it. :theme lists and switches, :theme nvim:NAME imports a neovim colorscheme. docs/themes.md"},
        {"follow_nvim_theme", d.follow_nvim_theme},
        {"//follow_nvim_theme", "inside nvim with maic.nvim (a connected host): follow its colorscheme live as the session theme nvim:NAME; false keeps `theme`"},
        {"ui", d.ui},
        {"//ui", "tui: MAIC's own interface; nvim: nvim with maic.nvim as the whole interface, your config and mappings included, the engine its job (maic --ui nvim; never inside nvim, never with bare). maic help ui"},
        {"daemon", d.daemon},
        {"//daemon", "attach: when a daemon runs (maic daemon start), the TUI and maic --rpc (maic.nvim) open their sessions in it, so a session outlives the window it started in; off: each runs its own engine. maic help daemon"},
        {"bare", d.bare},
        {"//bare", "true: nothing from nvim (no $NVIM host, the built-in highlighter, no theme from nvim, no lazy-lock notice, no keymap check); MAIC's own settings, themes, Lua and tools still load. Also maic --bare and MAIC_BARE=1. :h bare"},
        {"colors", d.colors},
        {"//colors", "colour depth: auto (truecolor when COLORTERM says so, 256 when TERM does, else 16), truecolor, 256 or 16"},
        {"//highlight", "builtin, or nvim: an embedded nvim --embed highlights the input (markdown with treesitter); falls back to builtin when nvim is missing"},
        {"enter_sends", d.enter_sends},
        {"//enter_sends", "true: Enter sends a one-line input in insert mode, Shift+Enter or Alt+Enter insert a newline; false (vim-like): Enter is always a newline, Alt+Enter or :w sends"},
        {"session_leave", d.session_leave},
        {"//session_leave", "what :new, :switch and :fork do with the session you leave: default (a working one goes to the background, an idle one is parked), ask, bg, park or stop; --bg, --park or --stop on the command decides once"},
        {"record", d.record},
        {"models_dir", models_dir.empty() ? d.models_dir : models_dir},
        {"context", d.context},
        {"//context", "context window in tokens for the local llama.cpp server (${MAIC_CONTEXT} in service files) and the usage readout; --ctx N and :ctx N override"},
        {"context_2", d.context_2},
        {"//context_2", "the same for the side server llamacpp-2 on port 8082 (${MAIC_CONTEXT_2}); --ctx2 N and :ctx2 N override"},
        {"small_model", d.small_model},
        {"//small_model", "opencode's small_model: a cheap model (a preset or provider/model) that titles each session after its first turn and is the reviewer's default; empty: no titles, and the reviewer uses the session preset's lowest non-limited tier (haiku-4.5 for the Anthropic presets, the model itself for a local one). title_model is its older name"},
        {"budget_tokens", d.budget_tokens},
        {"timestamps", d.timestamps},
        {"compact_at", d.compact_at},
        {"compact_keep_results", d.compact_keep_results},
        {"compact_model", d.compact_model},
        {"//compact_model", "the model that writes compaction summaries (a preset or provider/model, e.g. claude-sonnet-cli); empty: the session's model. A remote one is used only when the session's model is remote too"},
        {"//sessions_home", "auto: a project's transcripts (it has a MAIC.md) go under sessions/projects/, others under sessions/general/. Or: general, project, a name."},
        {"instructions", {{"files", d.instructions.files}, {"read", "all"}, {"local_files", d.instructions.local_files}, {"imports", {{"depth", d.instructions.import_depth}}}, {"extra_dirs", d.instructions.extra_dirs}}},
        {"//instructions", "which instruction files the model sees, global settings only: files are the classes, lowest priority first; read = \"highest\" takes only the top class in each directory; local_files reads MAIC.local.md and the like; imports.depth is how far @path imports go (0: none); extra_dirs lets extra directories add theirs. Also project_markers and bound. docs/instructions.md"},
        {"load_instructions", d.load_instructions},
        {"system_prompt", d.system_prompt},
        {"prefill", d.prefill},
        {"rules", nlohmann::json::array()},
        {"permission", {{"allow", nlohmann::json::array()}, {"ask", nlohmann::json::array()}, {"deny", nlohmann::json::array()}}},
        {"agents", nlohmann::json::object()},
        {"forbid", nlohmann::json::array()},
        {"//forbid", "terms no tool call may contain, in any letter case; write /.../ for a POSIX extended regex. A search, a command, a path or any argument with one is halted before it runs, under the dumb harness too. Added to the built-in list. :forbid in a session"},
        {"//permission", "allow / ask / deny lists of \"tool:pattern\" (run_shell:pytest *, write_file:src/**, read_file:/etc/**; write: and read: for any such tool). deny wins over ask over allow; allow runs without asking or review in every mode but plan. Trip patterns, secrets and system paths are checked first and are not touched. MAIC's own helpers (maic-storyboard*, maic-workflow-edit*, maic-panel-check*, maic path* ...) are always allowed. Layers add up. :allow in a session"},
        {"//agents", "agents by name (opencode's term), adding to or narrowing the built-in build, plan, general and explore: agents = { explore = { budget_tokens = 20000 }, docs = { mode = \"edit\", role = \"subagent\", write_paths = { \"docs/**\" }, tools = { \"read_file\", \"edit_file\", \"write_file\" }, model = \"qwen-4b\" } }. Fields: mode, role (primary, subagent, all: who may run it; the task tool runs subagent and all), description, write_paths, read_outside, budget_tokens, max_steps, tools, reviewer, model. A built-in can only be narrowed. profiles is the older name for this key. docs/settings.md"},
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
        {"lazy_lock", d.lazy_lock},
        {"//lazy_lock", "nvim's lazy-lock.json, watched for plugin and lazy.nvim updates; empty: $XDG_CONFIG_HOME/$NVIM_APPNAME/lazy-lock.json (~/.config/nvim/lazy-lock.json). maic lazy-lock, docs/lazy-lock.md"},
        {"lazy_lock_notice", d.lazy_lock_notice},
        {"//lazy_lock_notice", "false: no start notice and no lock≠ in the status strip when lazy-lock.json is out of sync; maic status, maic doctor and maic lazy-lock still report"},
        {"//tripwire", "machine: a trip sets the root-owned lock every MAIC process respects, unlock asks for sudo; session: a trip locks this session only (a file beside its transcript), :unlock removes it without sudo. A project's .maic/settings.lua can choose per project"},
        {"//harness", "smart: a model reads the conversation and reviews every command or write the rules would allow without asking (auto, edit); dumb: the rule list alone"},
        {"reviewer_model", d.reviewer_model},
        {"//reviewer_model", "pins the reviewer; empty: the preset's reviewer, else small_model (docs/settings.md)"},
        {"reviewer_budget_tokens", d.reviewer_budget_tokens},
        {"//reviewer_budget_tokens", "the reviewer's own token cap (it also counts toward budget_tokens); past it every action it would review is asked. 0: none"},
        {"dumb_auto_ok", d.dumb_auto_ok},
        {"protocol_tier", d.protocol_tier},
        {"//protocol_tier", "this file only: how closely the engine checks its protocol. open: no checks (an unchecked session shows OPEN); guarded: every check runs and logs what it finds (<state>/engine/protocol.log); airtight: refuses what fails (needs a build that passed conformance). protocol_tiers = { [\"~/scratch\"] = \"open\" } sets one per directory, as does maic trust DIR --protocol TIER; agents.NAME.protocol_tier one per agent; :tier tightens a session. docs/design/protocol-security.md"},
        {"protocol_tiers", json::object()},
        {"bans", {{"strings", nlohmann::json::array()}, {"patterns", nlohmann::json::array()}, {"tokens", nlohmann::json::array()}, {"retries", 3}, {"replacement", "[banned]"}, {"ignore_case", false}, {"window", 64}}},
        {"//steering", "the six steering actions (steer, drop, further, interrupt, keep, halt): which a session accepts (actions), from which clients (clients, this file only), drop's trim, what steer and drop do to a running tool, the halt message, and which actions a ban entry may name. :steering shows them. docs/design/engine-protocol.md section 11"},
        {"//bans", "strings and POSIX regex patterns the model must not say (cut and re-asked, then replaced); tokens (ids, or text) become logit_bias on OpenAI-compatible providers. docs/bans.md"},
        {"sampling", nlohmann::json::object()},
        {"//sampling", "sampler keys sent with every request: temperature, top_k, top_p, min_p, seed, repeat_penalty; xtc_probability / xtc_threshold on llama.cpp-style servers only. :sampling changes them live"},
        {"//system_prompt", "text placed first in every system prompt, or \"@~/path/to/file.md\"; independent of instruction files"},
        {"//server", "maic server: listen ADDR:PORT (TLS is required off loopback), workspaces remote sessions may open, cert/key (empty: self-signed), relay (https://host:port of a maic-relay the server dials out to for the phone away from home; pair with maic server pair), relay_cert (PEM pinning a self-signed relay certificate)."},
        {"server", {{"listen", d.server.listen}, {"workspaces", json::array()}, {"cert", ""}, {"key", ""}, {"relay", ""}, {"relay_cert", ""}}},
        {"providers", providers},
        {"models", json::object()},
        {"//models", "presets by short name, adding to the built-in ones (opus-5.5, sonnet-5, haiku-4.5, fable-5.1, qwen-4b, qwen-9b, qwen-9b-vision) or changing them field by field: models = { [\"opus-5.5\"] = { limited = true } }. Fields: model (needed for a new name), context, reviewer (\"same\" = itself, empty = small_model), think, tier (higher is stronger), limited (your plan caps it: subagents and the reviewer step aside), subagents (presets a subagent may run on), subagent (\"same\" or a preset; empty = the rule), on_limit (where a subagent continues after a usage limit). A model on the side server: [\"qwen-4b-side\"] = { model = \"llamacpp-2/Qwen3.5-4B-Q4_K_M\", context = 8192 }. docs/settings.md"},
        {"style", json::object()},
        {"//style", "single roles over the theme, merged into it: style = { user = { fg = \"#ff8800\" } } keeps the theme's bold. Every role and its default: themes/default.lua"},
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
