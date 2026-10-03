#include "session_commands.hpp"

#include "maid/engine.hpp"
#include "maid/lazy_lock.hpp"
#include "maid/lua_tools.hpp"
#include "maid/paths.hpp"
#include "maid/places.hpp"
#include "maid/script_tools.hpp"
#include "maid/service.hpp"
#include "maid/models.hpp"
#include "maid/status.hpp"
#include "maid/tools.hpp"
#include "maid/tripwire.hpp"
#include "maid/vendor.hpp"

#include <regex.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace maid {

namespace fs = std::filesystem;
using nlohmann::json;

json CommandOutput::json() const {
    nlohmann::json out = {{"ok", ok}, {"lines", lines}};
    if (ask) out["ask"] = {{"id", ask_id}, {"title", ask->title}, {"lines", ask->lines}, {"keys", ask->keys}};
    if (!send.empty()) out["send"] = send;
    return out;
}

namespace {

// The spellings each command answers to, as the TUI has always accepted them; the first is its name.
const std::vector<std::vector<std::string>>& spellings() {
    static const std::vector<std::vector<std::string>> all = {
        {"mode"},          {"harness"},        {"model"},       {"models"},  {"think"},          {"undo"},    {"export"},
        {"rename", "title"}, {"budget"},       {"compact"},     {"clear"},   {"trip"},           {"status"},  {"todo"},
        {"tools"},         {"init"},           {"cd"},          {"ban"},     {"sampling", "sampler"}, {"image", "img"},
        {"forbid"},        {"allow"},          {"rule", "rules"}, {"ctx", "context-size", "ctx2"}, {"prefill", "prefix"},
        {"system"},        {"instructions"},   {"session"},     {"lua", "luafile"}, {"trust"},   {"steering"}, {"usage"},
    };
    return all;
}

// A command's name from how it was typed, "" when it is not one of these.
std::string command_name(const std::string& typed) {
    for (const auto& names : spellings()) {
        if (std::find(names.begin(), names.end(), typed) != names.end()) return names.front();
    }
    return "";
}

// Scaffolds a project: a MAID.md placeholder and .maid/settings.lua. Returns what was created.
std::string init_project(const fs::path& ws) {
    std::string made;
    fs::create_directories(ws / ".maid");
    if (!fs::exists(ws / ".maid" / "settings.lua") && !fs::exists(ws / ".maid" / "settings.json")) {
        std::ofstream(ws / ".maid" / "settings.lua") << "-- Project settings for MAID, committed with the code. Personal overrides go in settings.local.lua\n"
                                                         "-- (add it to .gitignore). Keys: docs/settings.md\n"
                                                         "return {\n}\n";
        made += "created .maid/settings.lua\n";
    }
    if (!fs::exists(ws / "MAID.md")) {
        std::ofstream(ws / "MAID.md") << "# " << ws.filename().string() << "\n\nStanding instructions for agents working in this project.\n";
        made += "created MAID.md (transcripts for this project now go under sessions/projects/)\n";
    }
    return made;
}

// The cost estimate for :status: this session's, from the catalog's prices, and the average per session kept in
// <state>/costs.json. "" when neither has anything to say.
std::string cost_line(const Agent::UsageReport& u) {
    std::string out;
    if (u.cost > 0) out = "~" + format_cost(u.cost, u.currency) + " this session";
    for (const auto& a : session_cost_averages()) {
        out += (out.empty() ? "" : "; ") + std::string("average ~") + format_cost(a.average, a.currency) + " per session over " + std::to_string(a.sessions);
    }
    return out.empty() ? "" : "cost (estimate from the catalog's prices): " + out + "\n";
}

// "12.3k" for a count over a thousand, as the status strip writes it.
std::string kilo(long n) {
    char buf[32];
    if (n >= 1000) std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%ld", n);
    return buf;
}

std::string seconds_text(long ms) {
    return std::to_string((ms + 999) / 1000) + " s";
}

// What the last call sent as the conversation, of the model's window, as the status strip shows it; then the
// turns and model calls so far.
std::string context_line(const Agent::UsageReport& u, const Provider& provider, int turns) {
    long window = u.last.context > 0 ? u.last.context : provider.options.value("context_window", 0);
    std::string out = "context: ";
    if (u.last.input <= 0) out += "not measured yet (no model call since the session opened or was cleared)";
    else {
        out += kilo(u.last.input) + " tokens";
        if (window > 0) out += " of " + kilo(window) + " (" + std::to_string(static_cast<int>(100.0 * u.last.input / window)) + "%)";
    }
    return out + "; " + std::to_string(turns) + (turns == 1 ? " turn, " : " turns, ") + std::to_string(u.calls) + (u.calls == 1 ? " model call" : " model calls") + "\n";
}

// The reasoning effort MAID sends to `provider`, "" when it sends none and the provider's default applies: Anthropic's
// from its options, an OpenAI-compatible server's from the sampling or the provider's extra_body.
std::string effort_text(const Provider& provider, const Agent& agent) {
    auto text = [](const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); };
    if (provider.kind == "anthropic") return text(provider.options, agent.think ? "think_effort" : "effort");
    std::string effort = text(agent.sampling, "reasoning_effort");
    return effort.empty() ? text(provider.options.value("extra_body", json::object()), "reasoning_effort") : effort;
}

// The workspace and whether the project files in it (and above it, up to the project root) are trusted.
std::string workspace_line(const fs::path& ws) {
    size_t dirs = 0;
    std::string untrusted;
    for (const auto& d : project_dirs(ws)) {
        ++dirs;
        if (!trusted(d.dir)) untrusted += (untrusted.empty() ? "" : ", ") + d.dir.string();
    }
    return "workspace: " + ws.string() + "  (" +
           (dirs == 0 ? "no project files here, nothing to trust"
            : untrusted.empty() ? "trusted"
                                : "NOT trusted: " + untrusted + "; its MAID.md, settings and tools are not used until :trust") +
           ")\n";
}

// The key a provider takes, by the variable that supplies it and whether that is set; never the value.
std::string key_text(const Provider& provider) {
    std::string out;
    if (!provider.api_key_env.empty()) {
        const char* v = std::getenv(provider.api_key_env.c_str());
        out = provider.api_key_env + ": " + (v && *v ? "set" : "not set");
    }
    if (!provider.api_key_command.empty()) out += (out.empty() ? "" : "; ") + std::string("api_key_command configured");
    return out.empty() ? "no key" : out;
}

// What a 429 left on the account behind `provider`, shared by every session of this process.
std::string hold_text(const AccountState& a) {
    if (a.breaker) return "CIRCUIT BREAKER open: nothing is sent to it for " + seconds_text(a.hold_ms) + " more (five rate limits in a short time)";
    if (a.hold_ms > 0) return "rate limit: every request holds " + seconds_text(a.hold_ms) + " more";
    return "no rate-limit hold";
}

// :usage: per model what this session sent and the catalog-priced estimate of it, the account's concurrency and holds,
// the session's total, and what is kept across sessions. Estimates throughout, never a bill.
std::string usage_text(const SessionCommands::Session& s) {
    const Agent& agent = s.agent;
    const Agent::UsageReport u = agent.usage();
    auto [current, current_name] = resolve_model(agent.providers, agent.model);
    auto provider_of = [&](const std::string& name) -> const Provider* {
        for (const auto& p : agent.providers) {
            if (p.name == name) return &p;
        }
        return nullptr;
    };
    std::string out = "usage this session (estimates: tokens as the providers reported them, spend from the catalog's prices, not a bill)\n";
    if (u.by_model.empty()) out += "no model call yet\n";
    long hit = 0, calls = 0;
    std::set<std::string> providers = {current.name};
    for (const auto& [key, m] : u.by_model) {
        providers.insert(m.provider);
        hit += m.cached;
        calls += m.calls;
        out += key + "\n  requests " + std::to_string(m.calls) + "; tokens in " + std::to_string(m.input) + " (cache hit " + std::to_string(m.cached) + ", cache miss " +
               std::to_string(m.input - m.cached) + "), out " + std::to_string(m.output) + "\n";
        const Provider* p = provider_of(m.provider);
        if (const ApiModel* priced = find_api_model(api_models(), m.provider, m.model)) {
            std::string period = price_period(priced->pricing, std::time(nullptr)).value("name", "");
            out += "  spend ~" + format_cost(m.cost, priced->pricing.value("currency", "")) + " est." + (period.empty() ? "" : " (prices in force now: " + period + ")") + "\n";
        } else if (p && !p->remote()) {
            out += "  no spend: a local model\n";
        } else {
            out += std::string("  no spend counted: the catalog does not price it") + (p && p->metered() ? " (it is metered: its bill or plan has the real figure)" : "") + "\n";
        }
        if (p) {
            AccountState a = account_state(*p, m.model);
            out += a.cap > 0 ? "  concurrency: " + std::to_string(a.open) + " open of " + std::to_string(a.cap) + " (max_concurrent), " + std::to_string(a.waiting) + " waiting\n"
                             : "  concurrency: no cap (max_concurrent is not set for it)\n";
        }
    }
    if (!u.by_model.empty()) {
        out += "session total: " + std::to_string(calls) + " requests; tokens in " + std::to_string(u.total_input) + " (cache hit " + std::to_string(hit) + ", cache miss " +
               std::to_string(u.total_input - hit) + "), out " + std::to_string(u.total_output) + "; " +
               (u.cost > 0 ? "spend ~" + format_cost(u.cost, u.currency) + " est." : "no spend counted") + "\n";
    }
    out += context_line(u, current, s.turns);
    if (agent.budget_tokens > 0) out += "budget: " + std::to_string(u.total_input + u.total_output) + " of " + std::to_string(agent.budget_tokens) + " tokens (:budget)\n";
    out += "providers (the account's holds and open requests are shared by every session in this process):\n";
    for (const auto& name : providers) {
        const Provider* p = provider_of(name);
        if (p) out += "  " + name + "  " + key_text(*p) + "; " + hold_text(account_state(*p, "")) + "\n";
    }
    std::string kept;
    for (const auto& a : session_cost_averages()) {
        kept += (kept.empty() ? "" : "; ") + std::to_string(a.sessions) + " sessions, ~" + format_cost(a.average * a.sessions, a.currency) + " in all, average ~" + format_cost(a.average, a.currency);
    }
    out += "all sessions: tokens per provider account are not kept across sessions; each session's cost estimate is, locally in <state>/costs.json" +
           (kept.empty() ? std::string(", and none is there yet") : ": " + kept) + "\n";
    out += "Output counts reasoning tokens where the provider includes them in it; MAID does not break them out. Spend of a subagent and of the reviewer is in the totals.";
    return out;
}

std::string todo_text(const std::vector<TodoItem>& todo) {
    if (todo.empty()) return "";
    size_t done = 0;
    std::string out;
    for (const auto& t : todo) {
        done += t.done;
        out += std::string("\n") + (t.done ? "[x] " : "[ ] ") + t.text;
    }
    return "todo: " + std::to_string(done) + "/" + std::to_string(todo.size()) + " done" + out;
}

std::string log_path(const SessionCommands::Session& s) {
    return s.log.path().string() + (s.settings.record ? "" : "  (temporary: --no-record)");
}

fs::path home_path(const std::string& f) {
    return f.rfind("~/", 0) == 0 ? fs::path(std::getenv("HOME")) / f.substr(2) : fs::path(f);
}

}  // namespace

bool SessionCommands::owns(const std::string& name) {
    return !command_name(name).empty();
}

// Section 7: :mode (tightening freely, loosening up to edit), :model, :think, :compact, :rename, :todo, :tools,
// :status, :usage, adding a :forbid term, lowering :budget and :trip. Everything else is local only.
bool SessionCommands::remote_allowed(const std::string& line, Mode current, std::string& why) {
    std::istringstream in(line);
    std::string typed, arg;
    in >> typed;
    std::getline(in >> std::ws, arg, '\0');
    std::string cmd = command_name(typed);
    why = "`:" + typed + "` is not available to a remote client";
    static const std::set<std::string> open = {"model", "think", "compact", "rename", "todo", "tools", "status", "usage", "trip", "steering"};
    if (open.count(cmd)) return true;
    if (cmd == "mode") {
        auto m = parse_mode(arg);
        if (m && *m == Mode::Auto && current != Mode::Auto) {
            why = "a remote client loosens a session to auto only after a step-up check, which needs accounts";
            return false;
        }
        return true;
    }
    if (cmd == "forbid") return !arg.empty() && arg != "list" ? arg.rfind("remove", 0) != 0 : true;
    if (cmd == "budget") return arg.empty() || (arg != "off" && arg != "0" && std::atol(arg.c_str()) > 0);
    return false;
}

void SessionCommands::ask(CommandOutput& out, CommandOutput::Ask question, Then then) {
    out.ask_id = "k" + std::to_string(++asks_made_);
    out.ask = std::move(question);
    asks_[out.ask_id] = std::move(then);
}

CommandOutput SessionCommands::answer(Session& s, const std::string& id, const std::string& key) {
    CommandOutput out;
    auto it = asks_.find(id);
    if (it == asks_.end()) {
        out.error("no question " + id + " is waiting");
        return out;
    }
    Then then = std::move(it->second);
    asks_.erase(it);
    try {
        then(s, out, key);
    } catch (const std::exception& e) {
        out.error(e.what());
    }
    return out;
}

void SessionCommands::apply_sampling(Session& s) {
    maid::apply_sampling(s.agent, s.settings, live_sampling_);
}

// Changes the mode; auto under a dumb harness is confirmed once per session (or dumb_auto_ok in settings).
void SessionCommands::request_mode(Session& s, CommandOutput& out, Mode m) {
    if (m == Mode::Auto && !s.agent.review_with_model && !dumb_auto_ok) {
        ask(out,
            {" dumb harness + auto mode ",
             {"No model reads the conversation before the agent acts. In auto mode only the rule list stands between",
              "the agent and your shell: the trip patterns, the read-only classifier, the workspace fence and the",
              "sandbox. A wrong but well-formed command runs. Nothing asks you first.",
              "",
              "Continue into auto mode?  [y] yes, for this session   [n] stay in " + std::string(mode_name(s.agent.mode.load())) +
                  "   (dumb_auto_ok = true in settings skips this)"},
             "yn"},
            [this](Session& s, CommandOutput& out, const std::string& key) {
                if (key == "y") {
                    dumb_auto_ok = true;
                    s.agent.mode = Mode::Auto;
                    s.changed({{"mode", "auto"}});
                    out.info("auto mode under a dumb harness: the rule list alone decides what runs. `:harness smart` brings the reviewer back.");
                } else {
                    out.info("staying in " + std::string(mode_name(s.agent.mode.load())));
                }
            });
        return;
    }
    s.agent.mode = m;
    s.changed({{"mode", mode_name(m)}});
}

void SessionCommands::set_model(Session& s, CommandOutput& out, const std::string& model_in) {
    Agent& agent = s.agent;
    std::string preset = apply_preset(s.settings, model_in);
    std::string model = resolve_model_alias(preset.empty() ? model_in : s.settings.model);
    auto [provider, name] = resolve_model(agent.providers, model);
    agent.model = model;
    if (!preset.empty()) {
        agent.providers = s.settings.providers;
        agent.think = s.settings.think;
        set_context(agent.providers, s.settings.context);
        set_context(agent.providers, s.settings.context_2, "llamacpp-2");
        if (is_llama_server(provider.name)) {
            try {
                std::string r = restart_llamacpp_if_changed(provider.name);
                if (!r.empty()) out.info(r);
            } catch (const std::exception& e) {
                out.warn(e.what());
            }
        }
    }
    apply_sampling(s);
    ModelPick reviewer = agent.reviewer().pick;
    std::string note = "model: " + model + " (" + provider.name + ", " + provider.kind + ")" +
                       (preset.empty() ? "" : "  preset " + preset + ": context " + std::to_string(s.settings.providers.empty() ? 0 : provider.options.value("context_window", 0)) +
                                                  ", reviewer " + (reviewer.model.empty() ? "off" : reviewer.preset.empty() ? reviewer.model : reviewer.preset) + ", thinking " + (agent.think ? "on" : "off"));
    if (provider.metered() && provider.kind == "cli") note += "\nMETERED: spends your plan's usage through " + provider.name;
    else if (provider.metered()) note += "\nMETERED: billed per token to the account of " + (provider.api_key_env.empty() ? provider.name + "'s key" : "$" + provider.api_key_env);
    if (provider.remote()) {
        out.warn(note + "\nREMOTE: prompts, files the agent reads and command output will be sent to " + provider.base_url);
    } else {
        out.info(note);
    }
    s.changed({{"model", model}, {"remote_model", agent.remote()}, {"think", agent.think}});
}

// Moves the workspace to `to` once its project directories are settled: the settings re-read there use the trust
// and Lua level of the new chain.
void SessionCommands::cd_to(Session& s, CommandOutput& out, const fs::path& ws, const fs::path& to) {
    Agent& agent = s.agent;
    for (const auto& n : trust_notices(to)) out.info(n);
    for (const auto& n : settle_trust(to)) out.info(n);
    Settings next = s.settings_at(to);
    agent.set_workspace(to, Origin::Local);
    previous_ws_ = ws;
    // What a start there would read, but the session's safety stays its own: a directory never changes these.
    const std::set<std::string> keep = {"tripwire", "allow_isolated", "forbid", "record", "harness", "dumb_auto_ok", "bare"};
    next.tripwire = s.settings.tripwire;
    next.allow_isolated = s.settings.allow_isolated;
    next.forbid = s.settings.forbid;
    next.record = s.settings.record;
    next.harness = s.settings.harness;
    next.dumb_auto_ok = s.settings.dumb_auto_ok;
    next.bare = s.settings.bare;
    std::vector<std::string> changed, kept;
    std::set<std::string> keys;
    for (const auto& [k, v] : s.settings.layered.items()) keys.insert(k);
    for (const auto& [k, v] : next.layered.items()) keys.insert(k);
    for (const auto& k : keys) {
        if (k.rfind("//", 0) == 0 || s.settings.layered.value(k, nlohmann::json()) == next.layered.value(k, nlohmann::json())) continue;
        (keep.count(k) ? kept : changed).push_back(k);
    }
    s.settings = std::move(next);
    const Settings& st = s.settings;
    auto has = [&](const char* k) { return std::find(changed.begin(), changed.end(), k) != changed.end(); };
    if (has("providers") || has("context") || has("context_2")) {
        agent.providers = st.providers;
        set_context(agent.providers, st.context);
        set_context(agent.providers, st.context_2, "llamacpp-2");
    }
    if (has("models")) agent.presets = st.presets;
    if (has("think")) agent.think = st.think;
    if (has("reviewer_model")) agent.reviewer_model = st.reviewer_model;
    if (has("checkers")) agent.checkers = st.checkers;
    if (has("small_model") || has("title_model")) agent.small_model = st.small_model;
    if (has("reviewer_budget_tokens")) agent.reviewer_budget_tokens = st.reviewer_budget_tokens;
    if (has("budget_tokens")) agent.budget_tokens = st.budget_tokens;
    if (has("full_output")) agent.full_output = st.full_output;
    if (has("full_output_max_mb")) agent.full_output_max_mb = static_cast<size_t>(st.full_output_max_mb);
    if (has("compact_at")) agent.compaction.at = st.compact_at;
    if (has("compact_keep_results")) agent.compaction.keep_results = st.compact_keep_results;
    if (has("compact_model")) agent.compaction.model = st.compact_model;
    if (has("system_prompt")) agent.set_system_prefix(resolve_system_prompt(st.system_prompt));
    if (has("prefill")) agent.prefill = resolve_system_prompt(st.prefill);
    if (has("rules")) agent.set_rules(st.rules);
    if (has("permission")) agent.set_permission(st.permission);
    if (has("agents")) agent.agents = st.agents;
    if (has("bans")) agent.bans = st.bans;
    if (has("instructions") || has("load_instructions")) {
        agent.set_instruction_options(st.instructions);
        agent.load_instruction_files = st.load_instructions;
        agent.reload_instructions();
    }
    apply_sampling(s);
    s.changed({{"workspace", to.string()}});
    auto list = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& x : v) out += (out.empty() ? "" : ", ") + x;
        return out;
    };
    std::vector<std::string> files;
    for (const auto& f : agent.instructions()) files.push_back(f.path.string());
    out.info("workspace: " + to.string() + "  (was " + ws.string() + "; :cd - returns)" +
             (files.empty() ? "\ninstructions: none there" : "\ninstructions: " + list(files)) +
             (changed.empty() ? "\nsettings: unchanged" : "\nsettings changed: " + list(changed)) +
             (kept.empty() ? "" : "\nkept as they were (a directory never changes them mid-session): " + list(kept)));
    // Last: these can ask or fail on their own, and the move itself is done.
    if (has("mode")) {
        if (auto md = parse_mode(st.mode)) request_mode(s, out, *md);
    }
    if (has("model")) set_model(s, out, st.model);
}

// :cd's trust prompt: each untrusted project directory of the new chain in turn (t, s, n or v), then the move.
void SessionCommands::ask_cd_trust(Session& s, CommandOutput& out, std::vector<ProjectDir> dirs, const fs::path& ws, const fs::path& to) {
    if (dirs.empty()) return cd_to(s, out, ws, to);
    ProjectDir p = dirs.front();
    dirs.erase(dirs.begin());
    std::vector<std::string> lines = trust_prompt(p);
    lines.push_back("[t] trust fully: its Lua runs as you   [s] trust sandboxed: its Lua runs in a child process that cannot reach the system");
    lines.push_back("[n] not now (untrusted this session)   [v] never (remember)");
    ask(out, {" trust this directory? ", lines, "tsnv"}, [this, p, dirs, ws, to](Session& s, CommandOutput& out, const std::string& key) {
        out.info(answer_trust(p, key));
        ask_cd_trust(s, out, dirs, ws, to);
    });
}

// Imports of the user's own instruction files waiting for approval: each asked once per session (`again`: :trust
// imports --approve asks about declined ones too); yes records it for every later session.
void SessionCommands::ask_imports(CommandOutput& out, std::vector<PendingImport> pending, bool again) {
    while (!pending.empty() && !again && asked_imports_.count(pending.front().importer.string() + ">" + pending.front().target.string())) pending.erase(pending.begin());
    if (pending.empty()) return;
    PendingImport p = pending.front();
    pending.erase(pending.begin());
    asked_imports_.insert(p.importer.string() + ">" + p.target.string());
    std::vector<std::string> lines = {"your instruction file imports a file from outside your trusted directories:"};
    for (const auto& l : import_prompt(p.importer, p.target, import_exception_status(p.importer, p.target))) lines.push_back("  " + l);
    lines.push_back("[y] approve: remembered for every session (maid trust imports lists, --remove forgets)   [n] not now");
    ask(out, {" import from outside? ", lines, "yn"}, [this, p, pending, again](Session&, CommandOutput& out, const std::string& key) {
        if (key == "y") {
            try {
                approve_import(p.importer, p.target, Origin::Local);
                out.info("approved import: " + p.target.string() + " is read from your next message");
            } catch (const std::exception& e) {
                out.error(e.what());
            }
        } else {
            out.info("not imported: " + p.target.string() + " (:trust imports --approve asks again)");
        }
        ask_imports(out, pending, again);
    });
}

void SessionCommands::lua(Session& s, CommandOutput& out, const std::string& code, bool from_file) {
    lua_notice_ = s.notice;
    if (!lua_) lua_ = std::make_unique<Lua>(s.agent.harness().workspace(), [this](const std::string& t) {
        if (lua_notice_) lua_notice_(t);
    });
    Lua::Result r;
    if (from_file) r = lua_->run_file(s.agent.harness().resolve(code));
    else if (!code.empty() && code[0] == '=') r = lua_->run("return " + code.substr(1));
    else {
        // An expression shows its value; a statement runs as is.
        r = lua_->compiles("return " + code) ? lua_->run("return " + code) : lua_->run(code);
    }
    lua_notice_ = nullptr;
    std::string shown = r.output;
    while (!shown.empty() && shown.back() == '\n') shown.pop_back();
    out.line(shown.empty() ? "(no output)" : shown, r.ok ? "info" : "error");
    out.ok = r.ok;
    if (!r.output.empty()) {
        std::string context = "[The user ran Lua in MAID: `" + code + "`]\n" + (r.output.size() > 32 * 1024 ? r.output.substr(0, 32 * 1024) + "\n[truncated]" : r.output);
        if (s.running) s.agent.post_message(context);
        else s.agent.add_context(context);
    }
}

CommandOutput SessionCommands::run(Session& s, const std::string& line) {
    CommandOutput out;
    std::istringstream in(line);
    std::string typed, arg;
    in >> typed;
    std::getline(in >> std::ws, arg, '\0');  // the rest, newlines and all: a :lua chunk can be several lines
    std::string cmd = command_name(typed);
    Agent& agent = s.agent;
    auto idle = [&] {
        if (s.running) out.error(":" + typed + " has to wait until the agent is idle");
        return !s.running;
    };
    // Service files that don't load are skipped and said once per command; the command goes on without them.
    bool told = false;
    auto services = [&] {
        std::vector<std::string> problems;
        auto defs = load_services(root_dir() / "services", &problems);
        for (const auto& p : problems) if (!told) out.warn(p);
        told = true;
        return defs;
    };
    try {
        if (cmd.empty()) {
            out.error("unknown command :" + typed + " (try :help)");
        } else if (cmd == "mode") {
            if (auto m = parse_mode(arg)) request_mode(s, out, *m);
            else out.error("modes: manual, auto-read, edit, auto, plan");
        } else if (cmd == "harness") {
            if (arg.empty()) {
                Agent::ReviewerInfo r = agent.reviewer();
                std::string spent = ", " + std::to_string(r.tokens) + " tokens so far" + (s.settings.reviewer_budget_tokens > 0 ? " of " + std::to_string(s.settings.reviewer_budget_tokens) : "");
                std::string panel;
                for (const auto& k : agent.checkers.judges) {
                    panel += (panel.empty() ? "" : ", ") + k.model + (k.think == 1 ? " thinking" : k.think == 0 ? " not thinking" : "") + ", " + std::to_string(k.timeout) + " s";
                }
                out.info(!agent.review_with_model ? "harness: dumb. The rule list alone decides; nothing reads the conversation. `:harness smart` brings the reviewer back."
                         : !panel.empty()
                             ? "harness: smart, judged by the checkers" + (agent.checkers.setup.empty() ? "" : " " + agent.checkers.setup) + " in order (" + panel + "), combine " +
                                   agent.checkers.combine + spent + ". Each verdict and who decided is noted on every reviewed call."
                         : r.pick.model.empty()
                             ? "harness: smart, but the reviewer is off for this session (" + r.pick.reason + spent + "): every action it would review is asked."
                             : "harness: smart. A model (" + r.pick.model + ", " + r.pick.reason + spent +
                                   ") reads the conversation and reviews every command or write the rules would allow without asking. `:harness dumb` turns that off.");
                if (!s.tier.empty()) out.info(s.tier);
            } else if (arg == "smart") {
                agent.review_with_model = true;
                s.changed({{"harness", "smart"}});
                out.info("harness: smart (reviewer on)");
            } else if (arg == "dumb") {
                agent.review_with_model = false;
                s.changed({{"harness", "dumb"}});
                if (agent.mode.load() == Mode::Auto && !dumb_auto_ok) {
                    agent.mode = Mode::Edit;
                    s.changed({{"mode", "edit"}});
                    out.info("harness: dumb. Dropped to edit mode until you confirm auto.");
                    request_mode(s, out, Mode::Auto);
                } else {
                    out.info("harness: dumb (rules only)");
                }
            } else {
                out.error(":harness [smart|dumb]");
            }
        } else if (cmd == "model") {
            if (arg.empty()) {
                std::string list = "model: " + agent.model + "\npresets (:model NAME):" + preset_lines(s.settings);
                list += "\nproviders:";
                for (const auto& p : agent.providers) list += "\n  " + p.name + "/<model>  (" + p.kind + ", " + (p.kind == "cli" ? p.options.value("command", "") + ", MAID's tools over MCP" : p.base_url) + (p.remote() ? ", REMOTE)" : ")");
                out.info(list);
            } else if (idle()) {
                set_model(s, out, arg);
            }
        } else if (cmd == "models") {
            auto [provider, name] = resolve_model(agent.providers, agent.model);
            std::string text = "models on " + provider.name + " (" + provider.base_url + "):";
            try {
                std::vector<std::string> names = list_openai_models(provider);
                for (const auto& m : names) text += "\n  " + provider.name + "/" + m + (provider.name + "/" + m == agent.model ? "   (in use)" : "");
                if (is_llama_server(provider.name)) {
                    text += "\nfiles under " + llamacpp_models_root().string() + " (a subdirectory holds a GGUF plus its mmproj); one model is resident per server; :model " + provider.name + "/NAME switches";
                }
            } catch (const std::exception& e) {
                text += "\n  " + std::string(e.what());
            }
            out.info(text);
        } else if (cmd == "think") {
            if (idle()) {
                agent.think = arg != "off";
                s.changed({{"think", agent.think}});
                out.info(agent.think ? "thinking on (slower, better on hard problems)" : "thinking off");
            }
        } else if (cmd == "undo") {
            if (idle()) out.info(agent.undo(arg.empty() ? 1 : static_cast<size_t>(std::max(1, std::atoi(arg.c_str())))));
        } else if (cmd == "export") {
            auto info = find_session(s.log.path().stem().string());
            fs::path to = arg.empty() ? agent.harness().workspace() / (s.log.path().stem().string() + ".md") : agent.harness().resolve(arg);
            if (!info) out.error("this session is not listed (temporary transcripts cannot be exported by id yet)");
            else {
                std::ofstream f(to);
                f << export_markdown(*info, load_session(s.log.path()));
                out.info("exported to " + to.string());
            }
        } else if (cmd == "rename") {
            if (arg.empty()) out.error(":rename TITLE");
            else s.rename(arg);
        } else if (cmd == "budget") {
            if (arg == "off" || arg == "0") agent.budget_tokens = 0, out.info("no token budget");
            else if (!arg.empty()) agent.budget_tokens = std::atol(arg.c_str()), out.info("token budget: " + std::to_string(agent.budget_tokens));
            else {
                auto u = agent.usage();
                out.info("used " + std::to_string(u.total_input + u.total_output) + " tokens this session" +
                         (agent.budget_tokens ? " of " + std::to_string(agent.budget_tokens) : " (no budget; :budget N sets one)"));
            }
        } else if (cmd == "compact") {
            if (idle()) {
                std::atomic<bool> no{false};
                if (arg == "all") out.info(agent.compact(Agent::Compaction::All, no));
                else if (arg == "head") out.info(agent.compact(Agent::Compaction::Head, no));
                else if (arg == "prune") out.info(agent.compact(Agent::Compaction::Prune, no));
                else out.info(agent.compact_auto(no));
            }
        } else if (cmd == "clear") {
            if (idle()) agent.clear();
        } else if (cmd == "trip") {
            trip_tripwire("manual trip: " + (arg.empty() ? std::string("from the agent session") : arg));
            out.warn(s.settings.tripwire == "session" ? "SESSION TRIPPED. Nothing runs in this session until :unlock (no sudo: the lock is this session's own)"
                                                      : "HARNESS TRIPPED. Nothing will run until :unlock");
        } else if (cmd == "status") {
            auto [provider, name] = resolve_model(agent.providers, agent.model);
            // The services and nvim lines are consulted on the side: if they fail, the session lines still show.
            std::string text;
            try {
                StatusReport report = status_report(services());
                report.lazy_lock = lazy_lock_summary(lazy_lock_state(lazy_lock_path(s.settings.lazy_lock)));
                text = format_status(report);
            } catch (const std::exception& e) {
                text = "services: not shown (" + std::string(e.what()) + ")\n";
            }
            const Agent::UsageReport u = agent.usage();
            text += "session: " + s.id + (s.title.empty() ? "  (untitled)" : "  \"" + s.title + "\"") + "\n";
            text += "transcript: " + log_path(s) + "\n";
            text += "model: " + name + " via " + provider.name + " at " + provider.base_url + (provider.remote() ? "  [REMOTE: data leaves this machine]" : "  [local]") + "\n";
            std::string answered = u.last_served.empty() ? u.last_model : u.last_served + " (asked for " + u.last_model + ")";
            if (!answered.empty() && answered != provider.name + "/" + name) text += "last answered by: " + answered + "\n";
            std::string effort = effort_text(provider, agent);
            text += std::string("thinking: ") + (agent.think ? "on" : "off") + (effort.empty() ? (agent.think ? ", the provider's default reasoning effort" : "") : ", reasoning effort " + effort) + "\n";
            text += context_line(u, provider, s.turns);
            text += cost_line(u);
            text += "mode: " + std::string(mode_name(agent.mode.load())) + (s.running ? "  (working)" : "  (idle)");
            if (size_t q = agent.queued()) text += "  " + std::to_string(q) + " queued  -> :w now";
            text += std::string("\nharness: ") + (agent.review_with_model ? "smart (a model reviews what the rules allow)" : "dumb (the rule list alone)");
            if (!s.tier.empty()) text += "\n" + s.tier;
            text += "\n" + workspace_line(agent.harness().workspace());
            text += s.daemon + "\n";
            text += "background tasks: " + (s.settings.max_tasks <= 0 ? std::string("off (max_tasks = 0)")
                                            : std::to_string(s.tasks_running) + " running of " + std::to_string(s.tasks_started) + " started (max_tasks " + std::to_string(s.settings.max_tasks) + ")");
            if (!agent.tools().empty() || !agent.script_tools().empty()) {
                text += "\ntools:";
                for (const auto& t : agent.tools()) text += " " + t.name;
                for (const auto& t : agent.script_tools()) text += " " + t.name;
            }
            if (std::string todo = todo_text(agent.todo()); !todo.empty()) text += "\n" + todo;
            text += "\n:usage has the tokens, estimated spend, concurrency and rate-limit state per model";
            out.info(text);
        } else if (cmd == "usage") {
            out.info(usage_text(s));
        } else if (cmd == "steering") {
            const SteeringSettings& st = s.settings.steering;
            auto list = [](const std::vector<std::string>& v) {
                std::string out;
                for (const auto& a : v) out += (out.empty() ? "" : ", ") + a;
                return out.empty() ? std::string("none") : out;
            };
            auto from = [&](const char* key) {
                auto it = st.from.find(key);
                return "  (" + (it == st.from.end() ? std::string("default") : it->second) + ")";
            };
            std::string text = "steering in force:\n";
            text += "  actions: " + list(st.actions) + from("actions") + "\n";
            text += "  clients: local " + list(st.clients_local) + "; remote " + list(st.clients_remote) + from("clients") + "\n";
            text += "  drop_trim: " + st.drop_trim + from("drop_trim") + "\n";
            text += "  on_running_tool: " + st.on_running_tool + from("on_running_tool") + "\n";
            text += "  ban_actions: " + list(st.ban_actions) + from("ban_actions") + "\n";
            text += "  halt_message: \"" + st.halt_message + "\"" + from("halt_message");
            for (const auto& a : agent.agents) {
                if (!a.steering.is_null() && !a.steering.empty()) text += "\n  agents." + a.name + ".steering: " + a.steering.dump();
            }
            text += "\nCtrl-S pauses a running turn (interrupt), Ctrl-Q resumes it; :steer ACTION [NOTE] sends any of them";
            out.info(text);
        } else if (cmd == "todo") {
            std::string todo = todo_text(agent.todo());
            out.info(todo.empty() ? "no plan yet: the agent keeps one with the todo tool during multi-step work" : todo);
        } else if (cmd == "tools") {
            std::string text = "built-in tools (all through the harness):";
            for (const auto& t : tool_schemas()) {
                std::string desc = t["function"].value("description", "");
                if (auto nl = desc.find('\n'); nl != std::string::npos) desc = desc.substr(0, nl);
                if (desc.size() > 90) desc = desc.substr(0, 87) + "...";
                text += "\n  " + t["function"].value("name", "") + "  " + desc;
            }
            text += "\nhelpers: maid-workflow-edit, maid-storyboard, maid-danbooru-tags, maid-panel-check (run_shell; allow-listed)";
            if (agent.tools().empty() && agent.script_tools().empty()) text += "\nno user-defined tools. Put a <name>.lua or a <name>/tool.json in .maid/tools/ or " + global_tools_dir().string() + " (see :h tools)";
            for (const auto& t : agent.tools()) text += "\n  " + t.name + "  (lua)  " + t.file.string() + "\n    " + t.description;
            auto globs = [](const std::vector<std::string>& g) {
                std::string x;
                for (const auto& p : g) x += (x.empty() ? "" : ", ") + p;
                return x.empty() ? "nothing" : x;
            };
            for (const auto& t : agent.script_tools()) {
                text += "\n  " + t.name + "  (" + script_tool_language(t) + ")  " + (t.dir / "tool.json").string() + "\n    " + t.description + "\n    reads " + globs(t.reads) + "; writes " + globs(t.writes);
            }
            for (const auto& n : agent.tool_notices()) text += "\n  " + n;
            out.info(text);
        } else if (cmd == "trust") {
            // The machine's trust store is the front end's; what is the session's is its pending imports.
            std::istringstream words(arg);
            std::vector<std::string> args;
            for (std::string w; words >> w;) args.push_back(w);
            if (args == std::vector<std::string>{"imports", "--approve"}) {
                if (s.running) {
                    out.info(":trust imports --approve waits until the turn is over");
                } else {
                    agent.reload_instructions();
                    if (agent.pending_imports().empty()) out.info("no import of your own instruction files waits for approval");
                    ask_imports(out, agent.pending_imports(), true);
                }
            } else if (args == std::vector<std::string>{"imports", "--pending"}) {
                // What the TUI asks after every turn: the imports not asked about yet, as of the last reload.
                if (!s.running) ask_imports(out, agent.pending_imports(), false);
            } else {
                out.error(":trust " + arg + " is the front end's (the trust store is the machine's)");
            }
        } else if (cmd == "init") {
            fs::path ws = agent.harness().workspace();
            std::string made = init_project(ws);
            out.info(made.empty() ? "already initialised: MAID.md and .maid/settings.lua exist" : made);
            if (!trusted(ws)) {
                out.info(ws.string() + " is not trusted, so its MAID.md and .maid/settings.lua are not read until it is: "
                         ":trust (fully: its Lua runs as you) or :trust --lua sandbox (its Lua in a child process that cannot reach the system)");
            }
            // The session joins the project's transcripts when it worked here throughout (docs/sessions.md, Homes).
            InitMove m = init_move_check(s.log.path(), ws, s.settings.record, static_cast<size_t>(std::max(0, s.settings.init_move_outside_reads)));
            fs::path dest = sessions_home("project:" + ws.string());
            std::string id = s.log.path().stem().string(), here = s.log.path().parent_path().lexically_relative(sessions_dir()).string();
            std::string there = dest.lexically_relative(sessions_dir()).string() + "/";
            auto move = [dest, id, here, there](Session& s, CommandOutput& out, const std::string& why) {
                try {
                    fs::path old_lock = s.log.path().string() + ".tripped";
                    fs::path to = s.log.relocate(dest, "init");
                    set_tripwire_scope(s.settings.tripwire, to.string() + ".tripped");
                    std::error_code ec;
                    if (fs::exists(old_lock, ec)) fs::rename(old_lock, to.string() + ".tripped", ec);  // a trip while it moved
                    out.info("this session moved to " + there + why + "; `maid sessions rehome " + id + " " + here + "` moves it back");
                } catch (const std::exception& e) {
                    out.error("this session stays in " + here + "/: " + e.what());
                }
            };
            if (m.verdict == InitMove::Move) {
                move(s, out, " (" + m.reason + ")");
            } else if (m.verdict == InitMove::Ask) {
                ask(out,
                    {" move this session? ",
                     {"this session read " + std::to_string(m.outside_reads) + " and wrote " + std::to_string(m.outside_writes) +
                          " files outside the project; move it into the project's home anyway?",
                      "[y] move it to " + there + "   [n] leave it in " + here + "/   (later: maid sessions rehome " + id + " project)"},
                     "yn"},
                    [move, id, here](Session& s, CommandOutput& out, const std::string& key) {
                        if (key == "y") move(s, out, "");
                        else out.info("this session stays in " + here + "/; `maid sessions rehome " + id + " project` moves it later");
                    });
            } else if (!s.settings.record) {
                out.info("this session stays out of the project's home: " + m.reason);
            }
            if (!fs::exists(ws / "MAID.md") || fs::file_size(ws / "MAID.md") < 200) {
                out.send = "Look over this project (list the top level, read the README and build files) and write a MAID.md at the workspace root: "
                           "what the project is, how it is built and tested, the conventions to follow, and anything an agent should know before editing. "
                           "Keep it under 60 lines. Use write_file for MAID.md only.";
            }
        } else if (cmd == "cd") {
            fs::path ws = agent.harness().workspace();
            if (arg.empty()) {
                out.info("workspace: " + ws.string() + (previous_ws_.empty() ? "" : "\n:cd - returns to " + previous_ws_.string()));
            } else if (!idle()) {
                out.error(":cd waits until the turn ends (Ctrl-C stops it); the workspace is still " + ws.string());
            } else {
                fs::path to = cd_target(arg, ws, previous_ws_, known_places(s.settings, ws, services(), s.log.path()));
                if (to == ws) out.info("already in " + ws.string());
                // A start there would ask about its project directories first: so does :cd.
                else ask_cd_trust(s, out, trust_to_ask(to), ws, to);
            }
        } else if (cmd == "ban") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto& b = agent.bans;
            if (sub.empty() || sub == "list") {
                std::string text = "banned strings (" + std::to_string(b.strings.size()) + "):";
                for (size_t i = 0; i < b.strings.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". \"" + b.strings[i] + "\"";
                text += "\nbanned patterns (" + std::to_string(b.patterns.size()) + ", POSIX extended regex, window " + std::to_string(b.window) + "):";
                for (size_t i = 0; i < b.patterns.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". /" + b.patterns[i] + "/";
                text += "\nbanned tokens (" + std::to_string(b.tokens.size()) + "):";
                for (size_t i = 0; i < b.tokens.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". " + b.tokens[i].dump();
                text += "\nretries " + std::to_string(b.retries) + ", then replaced by \"" + b.replacement + "\"" + (b.ignore_case ? ", case-insensitive" : "") +
                        "\n:ban add TEXT|@FILE · :ban pattern REGEX|@FILE · :ban token ID|TEXT|@FILE · :ban remove N · :ban patterns remove N · :ban tokens remove N · :ban clear · :ban retries N · :ban case on|off · :ban window N";
                out.info(text);
            } else if (sub == "pattern" && !rest.empty()) {
                std::vector<std::string> entries = expand_ban_entry(rest);
                int added = 0;
                for (const auto& pat : entries) {
                    regex_t re;
                    int rc = regcomp(&re, pat.c_str(), REG_EXTENDED | (b.ignore_case ? REG_ICASE : 0));
                    if (rc != 0) {
                        char err[200];
                        regerror(rc, &re, err, sizeof(err));
                        out.error("not a valid POSIX extended regex: /" + pat + "/: " + err);
                        continue;
                    }
                    regfree(&re);
                    b.patterns.push_back(pat);
                    ++added;
                }
                if (added) out.info(added == 1 && entries.size() == 1 ? "banned /" + entries[0] + "/ (from the next model call)" : "banned " + std::to_string(added) + " patterns from " + rest.substr(1));
            } else if (sub == "patterns" && rest.rfind("remove ", 0) == 0) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str() + 7));
                if (n >= 1 && n <= b.patterns.size()) b.patterns.erase(b.patterns.begin() + static_cast<long>(n - 1)), out.info("removed");
                else out.error("no banned pattern " + rest.substr(7));
            } else if (sub == "window" && !rest.empty()) {
                b.window = std::max(8, std::atoi(rest.c_str()));
                out.info("regex hold-back window: " + std::to_string(b.window) + " characters");
            } else if ((sub == "add" || sub == "token") && !rest.empty()) {
                std::vector<std::string> entries = expand_ban_entry(rest);
                for (const auto& e : entries) {
                    if (sub == "add") {
                        b.strings.push_back(e);
                        continue;
                    }
                    bool numeric = std::all_of(e.begin(), e.end(), [](unsigned char c) { return std::isdigit(c); });
                    b.tokens.push_back(numeric ? nlohmann::json(std::stoll(e)) : nlohmann::json(e));
                }
                if (entries.size() == 1 && rest[0] != '@') {
                    out.info(sub == "add" ? "banned \"" + rest + "\" (from the next model call)"
                                          : "banned token " + rest + (b.tokens.back().is_number() ? " (logit_bias on OpenAI-compatible providers only)" : ""));
                } else {
                    out.info("banned " + std::to_string(entries.size()) + (sub == "add" ? " phrases" : " tokens") + " from " + rest.substr(1));
                }
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= b.strings.size()) b.strings.erase(b.strings.begin() + static_cast<long>(n - 1)), out.info("removed");
                else out.error("no banned string " + rest);
            } else if (sub == "tokens" && rest.rfind("remove ", 0) == 0) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str() + 7));
                if (n >= 1 && n <= b.tokens.size()) b.tokens.erase(b.tokens.begin() + static_cast<long>(n - 1)), out.info("removed");
                else out.error("no banned token " + rest.substr(7));
            } else if (sub == "clear") {
                b.strings.clear();
                b.patterns.clear();
                b.tokens.clear();
                out.info("bans cleared");
            } else if (sub == "retries" && !rest.empty()) {
                b.retries = std::max(0, std::atoi(rest.c_str()));
                out.info("ban retries: " + std::to_string(b.retries));
            } else if (sub == "case") {
                b.ignore_case = rest == "off" || rest == "ignore";
                out.info(b.ignore_case ? "bans ignore case" : "bans match case");
            } else {
                out.error(":ban [list] · add TEXT · pattern REGEX · token ID|TEXT · remove N · patterns remove N · tokens remove N · clear · retries N · case on|off · window N");
            }
        } else if (cmd == "sampling") {
            std::istringstream a(arg);
            std::string key, v1, v2;
            a >> key >> v1 >> v2;
            auto [provider, mname] = resolve_model(agent.providers, agent.model);
            auto number = [](const std::string& x) {
                nlohmann::json j = nlohmann::json::parse(x, nullptr, false);
                return j.is_number() ? j : nlohmann::json(x);
            };
            if (key.empty()) {
                std::string text = "sampling for " + agent.model + " (" + provider.kind + "):";
                if (agent.sampling.empty()) text += " defaults";
                for (const auto& [k, v] : agent.sampling.items()) text += "\n  " + k + " = " + v.dump();
                text += "\n:sampling KEY VALUE · :sampling xtc P [T] · :sampling unset KEY · :sampling reset";
                if (provider.kind == "anthropic") text += "\nAnthropic's current models reject sampling parameters; nothing is sent there.";
                out.info(text);
            } else if (key == "xtc") {
                if (v1.empty()) out.error(":sampling xtc PROBABILITY [THRESHOLD]  (0.5 0.1 is a common start)");
                else {
                    live_sampling_["xtc_probability"] = number(v1);
                    live_sampling_["xtc_threshold"] = v2.empty() ? nlohmann::json(0.1) : number(v2);
                    apply_sampling(s);
                    out.info("XTC: probability " + live_sampling_["xtc_probability"].dump() + ", threshold " + live_sampling_["xtc_threshold"].dump() +
                             (provider.kind == "openai" ? " (sent as xtc_probability / xtc_threshold; llama.cpp server, koboldcpp and the like honour it)"
                                                        : " (this provider has no XTC; the keys are kept for when you switch to a llama.cpp-style server)"));
                }
            } else if (key == "unset" && !v1.empty()) {
                live_sampling_.erase(v1);
                live_sampling_[v1] = nullptr;  // masks a settings value
                apply_sampling(s);
                agent.sampling.erase(v1);
                out.info("unset " + v1);
            } else if (key == "reset") {
                live_sampling_ = nlohmann::json::object();
                apply_sampling(s);
                out.info("sampling back to the settings");
            } else if (!v1.empty()) {
                live_sampling_[key] = number(v1);
                apply_sampling(s);
                out.info(key + " = " + live_sampling_[key].dump() + (provider.kind == "anthropic" ? " (not sent to Anthropic)" : ""));
            } else {
                out.error(":sampling [KEY VALUE | xtc P [T] | unset KEY | reset]");
            }
        } else if (cmd == "image") {
            if (arg.empty()) {
                auto pics = agent.pending_images();
                std::string text = pics.empty() ? "no image attached. :image FILE attaches one to the next message; a file dropped onto the terminal is attached on send" : "attached to the next message:";
                for (const auto& p : pics) text += "\n  " + p;
                out.info(text);
            } else if (arg == "clear") {
                agent.clear_pending_images();
                out.info("attachments dropped");
            } else {
                fs::path f = home_path(arg);
                if (f.is_relative()) f = agent.harness().workspace() / f;  // the session's directory, not the process's (the daemon's is $HOME)
                agent.attach_image(f);
                out.info("image attached to the next message: " + f.filename().string() + "  (:image lists, :image clear drops)");
            }
        } else if (cmd == "forbid") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto fb = agent.harness().forbid();
            if (arg.empty() || arg == "list") {
                std::string text = "forbidden terms (" + std::to_string(fb.size()) + "; any tool call containing one is halted, in every mode, under every harness):";
                for (size_t i = 0; i < fb.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". " + fb[i];
                text += "\n:forbid TERM adds one · :forbid remove N   (forbid = { ... } in settings keeps them)";
                out.info(text);
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= fb.size()) fb.erase(fb.begin() + static_cast<long>(n - 1)), agent.set_forbid(fb), out.info("removed");
                else out.error("no term " + rest);
            } else {
                fb.push_back(arg);
                agent.set_forbid(fb);
                out.info("forbidden: " + arg);
            }
        } else if (cmd == "allow") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto al = agent.harness().allow();
            if (arg.empty() || arg == "list") {
                std::string text = "allowed command patterns (" + std::to_string(al.size()) + "; no asking, no review, trip patterns still win):";
                for (size_t i = 0; i < al.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". " + al[i];
                text += "\n:allow PATTERN adds one (glob over the whole command, e.g. `pytest *`) · :allow remove N";
                out.info(text);
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= al.size()) al.erase(al.begin() + static_cast<long>(n - 1)), agent.set_allow(al), out.info("removed");
                else out.error("no pattern " + rest);
            } else {
                al.push_back(arg);
                agent.set_allow(al);
                out.info("allowed: " + arg + " (this session; put it under `allow` in settings to keep it)");
            }
        } else if (cmd == "rule") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto rs = agent.rules;
            if (arg.empty() || arg == "list") {
                std::string text = "standing rules (" + std::to_string(rs.size()) + "):";
                for (size_t i = 0; i < rs.size(); ++i) text += "\n  " + std::to_string(i + 1) + ". " + rs[i];
                text += "\n:rule TEXT adds one · :rule remove N · :rule clear   (a rule is a request the model is reminded of every turn; :prefix gives the literal first words)";
                out.info(text);
            } else if (!idle()) {
                out.error("wait for the turn to finish");
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= rs.size()) {
                    rs.erase(rs.begin() + static_cast<long>(n - 1));
                    agent.set_rules(rs);
                    out.info("rule removed");
                } else out.error("no rule " + rest);
            } else if (sub == "clear") {
                agent.set_rules({});
                out.info("rules cleared");
            } else {
                rs.push_back(arg);
                agent.set_rules(rs);
                out.info("rule " + std::to_string(rs.size()) + " added; it is carried with the operator instructions from the next turn");
            }
        } else if (cmd == "ctx") {
            bool side = typed == "ctx2";
            int& current = side ? s.settings.context_2 : s.settings.context;
            std::string service = side ? "llamacpp-2" : "llamacpp";
            if (arg.empty()) {
                out.info((side ? "side server context window: " : "context window: ") + std::to_string(current) + " tokens (:" + typed + " N sets it and restarts " + service + "; --" + typed + " N on the command line; " +
                         (side ? "context_2" : "context") + " in settings)");
            } else if (!idle()) {
                out.error("wait for the turn to finish");
            } else {
                int n = std::atoi(arg.c_str());
                if (n < 1024) {
                    out.error(":" + typed + " N takes tokens (8192, 16384, 32768, ...)");
                } else {
                    current = n;
                    set_context(agent.providers, n, service);
                    std::string r;
                    try {
                        require_armed("restart services");
                        r = restart_llamacpp_if_changed(service);
                    } catch (const std::exception& e) {
                        r = e.what();
                    }
                    out.info((side ? "side server context window: " : "context window: ") + std::to_string(n) + " tokens" + (r.empty() ? " (" + service + " was not running with another size)" : "; " + r));
                }
            }
        } else if (cmd == "prefill") {
            if (arg == "off" || arg == "none") agent.prefill.clear(), out.info("no prefill");
            else if (!arg.empty()) {
                agent.prefill = resolve_system_prompt(arg);
                out.info("every reply now begins with these literal words: \"" + agent.prefill + "\"  (a rule such as \"always start with X\" belongs in :system; here you give X itself)");
            } else {
                out.info(agent.prefill.empty() ? "no prefill (:prefill TEXT makes every reply start with TEXT; :prefill off clears)" : "replies start with: " + agent.prefill);
            }
        } else if (cmd == "system") {
            if (arg == "off" && idle()) {
                agent.set_system_prefix("");
                out.info("operator instructions withdrawn");
            } else if (!arg.empty() && arg != "off" && idle()) {
                agent.set_system_prefix(resolve_system_prompt(arg));
                out.info("operator instructions set: they now lead the system prompt and close each of your messages as the model sees them");
            } else if (arg.empty()) {
                out.info(agent.system_prefix.empty() ? "no operator instructions (:system TEXT or :system @file sets them; --system on the command line)"
                                                      : "operator instructions (placed first in the system prompt):\n" + agent.system_prefix);
            }
        } else if (cmd == "instructions") {
            agent.reload_instructions();
            if (!agent.load_instruction_files) {
                out.info("instruction files are disabled for this session (--no-instructions or load_instructions = false); :instructions on enables them");
                if (arg == "on") agent.load_instruction_files = true, agent.reload_instructions(), out.info("instruction files enabled");
            } else if (arg == "off") {
                agent.load_instruction_files = false;
                agent.reload_instructions();
                out.info("instruction files disabled for the next turns");
            } else {
                std::string text = "instruction files in effect (re-read every turn):";
                for (const auto& f : agent.instructions()) text += "\n  " + f.path.string() + "  (" + std::to_string(f.text.size()) + " bytes" + (f.imported_by.empty() ? "" : ", imported by " + f.imported_by.string()) + ")";
                if (agent.instructions().empty()) text += "\n  none. Create " + global_instructions_path().string() + " or a MAID.md / AGENTS.md / CLAUDE.md in the workspace.";
                out.info(text);
            }
        } else if (cmd == "session") {
            out.info("this session: " + log_path(s) + (s.settings.record ? "\nhome: " + s.log.path().parent_path().lexically_relative(sessions_dir()).string() +
                                                          "  (maid sessions rehome " + s.log.path().stem().string() + " project|general|NAME moves it)"
                                                    : "\nnot kept: it lives in the runtime directory and is gone at logout") +
                     "\n"
                     "all sessions: " + sessions_dir().string() + "\n`maid sessions` lists them, `maid artifacts` cleans");
        } else if (cmd == "lua") {
            lua(s, out, arg, typed == "luafile");
        }
    } catch (const std::exception& e) {
        out.error(e.what());
    }
    return out;
}

}  // namespace maid
