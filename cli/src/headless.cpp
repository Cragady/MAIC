#include "commands.hpp"
#include "headless.hpp"

#include "maic/agent.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>

namespace maic {

namespace {

std::atomic<bool> g_cancel{false};

void on_sigint(int) {
    g_cancel = true;
}

class Printer : public AgentEvents {
public:
    explicit Printer(bool json) : json_(json) {}

    void on_text(std::string_view delta, bool thinking) override {
        if (json_) emit({{"type", thinking ? "thinking" : "text"}, {"text", std::string(delta)}});
        else if (thinking) fprintf(stderr, "%.*s", static_cast<int>(delta.size()), delta.data());
        else fwrite(delta.data(), 1, delta.size(), stdout), fflush(stdout);
    }
    void on_tool_call(const std::string& summary) override {
        if (json_) emit({{"type", "tool_call"}, {"summary", summary}});
        else fprintf(stderr, "▸ %s\n", summary.c_str());
    }
    void on_tool_result(const std::string& text, bool ok) override {
        if (json_) emit({{"type", "tool_result"}, {"ok", ok}, {"text", text}});
        else fprintf(stderr, "  ⎿ %s\n", text.substr(0, text.find('\n')).c_str());
    }
    void on_notice(const std::string& text) override {
        if (json_) emit({{"type", "notice"}, {"text", text}});
        else fprintf(stderr, "※ %s\n", text.c_str());
    }
    std::string question(const std::string& text, const std::vector<std::string>& options) override {
        if (json_) emit({{"type", "question"}, {"text", text}, {"options", options}});
        if (!isatty(STDIN_FILENO)) {
            if (!json_) fprintf(stderr, "? %s\n  (no terminal to answer on)\n", text.c_str());
            return "";
        }
        fprintf(stderr, "\n? %s\n", text.c_str());
        for (size_t i = 0; i < options.size(); ++i) fprintf(stderr, "  [%zu] %s\n", i + 1, options[i].c_str());
        fprintf(stderr, options.empty() ? "answer (empty = none): " : "answer (a number, or your own words; empty = none): ");
        fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line)) return "";
        if (!options.empty() && !line.empty() && line.find_first_not_of("0123456789") == std::string::npos) {
            size_t n = std::stoul(line);
            if (n >= 1 && n <= options.size()) return options[n - 1];
        }
        return line;
    }
    void on_todo(const std::vector<TodoItem>& items) override {
        if (json_) {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& t : items) list.push_back({{"text", t.text}, {"done", t.done}});
            emit({{"type", "todo"}, {"items", list}});
            return;
        }
        size_t done = 0;
        for (const auto& t : items) done += t.done;
        fprintf(stderr, "※ todo %zu/%zu done\n", done, items.size());
        for (const auto& t : items) fprintf(stderr, "  %s %s\n", t.done ? "[x]" : "[ ]", t.text.c_str());
    }
    ApprovalAnswer ask(const ApprovalRequest& r) override {
        if (!isatty(STDIN_FILENO)) {
            if (json_) emit({{"type", "denied"}, {"summary", r.summary}, {"reason", "no terminal to ask on"}});
            else fprintf(stderr, "✗ denied (no terminal to ask on): %s\n", r.summary.c_str());
            return {Approval::No, ""};
        }
        if (!r.preview.empty()) fprintf(stderr, "\n%s", r.preview.c_str());
        fprintf(stderr, "\napprove? %s\n  why asking: %s\n  [y] yes  [n] no  [n: reason] no, with a reason for the model  [a] always: %s (this session)  [t] trip the harness: ",
                r.summary.c_str(), r.reason.c_str(), r.always_covers.c_str());
        fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line) || line.empty()) return {Approval::No, ""};
        switch (line[0]) {
            case 'y': case 'Y': return {Approval::Yes, ""};
            case 'a': case 'A': return {Approval::Always, ""};
            case 't': case 'T': return {Approval::Trip, ""};
            default: {
                std::string why;
                if (auto c = line.find(':'); c != std::string::npos) why = line.substr(c + 1);
                while (!why.empty() && why.front() == ' ') why.erase(0, 1);
                return {Approval::No, why};
            }
        }
    }

private:
    void emit(nlohmann::json j) {
        fprintf(stdout, "%s\n", j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
        fflush(stdout);
    }
    bool json_;
};

}  // namespace

int run_headless(const HeadlessOptions& options) {
    Settings settings = load_settings();
    if (options.model) settings.model = *options.model;
    settings.model = resolve_model_alias(settings.model);
    if (options.mode) settings.mode = *options.mode;
    if (options.system) settings.system_prompt = *options.system;
    if (options.prefill) settings.prefill = *options.prefill;
    if (options.ctx) settings.context = *options.ctx;
    settings.rules.insert(settings.rules.end(), options.rules.begin(), options.rules.end());
    if (options.load_instructions) settings.load_instructions = *options.load_instructions;
    settings.bans.strings.insert(settings.bans.strings.end(), options.bans.begin(), options.bans.end());
    if (options.harness) settings.harness = *options.harness;
    if (settings.harness != "smart" && settings.harness != "dumb") {
        fprintf(stderr, "maic: --harness must be smart or dumb\n");
        return 2;
    }
    auto mode = parse_mode(settings.mode);
    if (!mode) {
        fprintf(stderr, "maic: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    bool stdin_for_context = false;
    for (const auto& c : options.context) stdin_for_context = stdin_for_context || c == "-";
    std::string prompt = options.prompt;
    if (prompt.empty() || prompt == "-") {
        if (stdin_for_context) {
            fprintf(stderr, "maic: stdin can be the prompt or a --context file, not both\n");
            return 2;
        }
        prompt.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    if (prompt.empty()) {
        fprintf(stderr, "maic: nothing to send (give a prompt, or pipe one in with -p -)\n");
        return 2;
    }

    // Unrecorded runs still get a transcript, in the runtime directory (gone at logout).
    std::filesystem::path where = options.record ? resolve_sessions_home(settings, std::filesystem::current_path()) : runtime_sessions_dir();
    std::unique_ptr<SessionLog> log;
    if (options.append && options.resume) log = std::make_unique<SessionLog>(SessionLog::Reopen{}, *options.resume);
    else if (options.resume) log = std::make_unique<SessionLog>(SessionLog::Fork{}, *options.resume, options.fork_at.value_or(count_records(*options.resume)), "headless", where);
    else log = std::make_unique<SessionLog>("headless", where);
    Agent agent(std::filesystem::current_path(), settings.model);
    agent.providers = settings.providers;
    set_context(agent.providers, settings.context);
    if (options.ctx) {
        std::string r = restart_llamacpp_if_changed();
        if (!r.empty()) fprintf(stderr, "※ %s\n", r.c_str());
    }
    agent.mode = *mode;
    agent.review_with_model = settings.harness != "dumb";
    agent.reviewer_model = settings.reviewer_model;
    if (*mode == Mode::Auto && !agent.review_with_model && !settings.dumb_auto_ok && !options.accept_dumb_auto) {
        fprintf(stderr, "dumb harness + auto mode: no model reads the conversation before the agent acts; only the rule list stands between it and your shell.\n");
        if (isatty(STDIN_FILENO) && options.prompt != "-") {
            fprintf(stderr, "continue into auto mode? [y/N] ");
            fflush(stderr);
            std::string line;
            if (!std::getline(std::cin, line) || (line != "y" && line != "Y")) {
                fprintf(stderr, "stopped. Pass --accept-dumb-auto, set dumb_auto_ok = true in settings, or use --mode edit.\n");
                return 2;
            }
        } else {
            fprintf(stderr, "stopped: pass --accept-dumb-auto (or dumb_auto_ok = true in settings) to run auto mode without the reviewer.\n");
            return 2;
        }
    }
    agent.think = options.think || settings.think;
    agent.compaction.at = settings.compact_at;
    agent.compaction.keep_results = settings.compact_keep_results;
    agent.budget_tokens = settings.budget_tokens;
    agent.set_instruction_names(settings.instruction_files);
    agent.load_instruction_files = settings.load_instructions;
    agent.system_prefix = resolve_system_prompt(settings.system_prompt);
    agent.prefill = resolve_system_prompt(settings.prefill);
    agent.rules = settings.rules;
    agent.set_allow(settings.allow);
    agent.set_forbid(settings.forbid);
    if (settings.tripwire == "isolated" && !settings.allow_isolated) {
        fprintf(stderr, "maic: tripwire = \"isolated\" is not allowed: set allow_isolated = true in settings to permit it\n");
        return 2;
    }
    set_tripwire_scope(settings.tripwire, log->path().string() + ".tripped");
    if (settings.tripwire == "isolated") agent.set_confined(true);
    agent.reload_instructions();
    agent.bans = settings.bans;
    {
        auto [provider, name] = resolve_model(agent.providers, agent.model);
        nlohmann::json s = settings.sampling.is_object() ? settings.sampling : nlohmann::json::object();
        nlohmann::json per_provider = provider.options.value("sampling", nlohmann::json::object());
        for (const auto& [k, v] : per_provider.items()) s[k] = v;
        for (const auto& [k, v] : options.sampling.items()) s[k] = v;  // the command line wins
        agent.sampling = s;
        agent.operator_note_in_turn = provider.options.value("operator_note", provider.kind != "anthropic");
    }
    settings.bans.patterns.insert(settings.bans.patterns.end(), options.ban_patterns.begin(), options.ban_patterns.end());
    agent.bans = settings.bans;
    agent.set_log(log.get());
    if (options.resume) {
        LoadedSession old = load_session(*options.resume, options.fork_at.value_or(~size_t(0)));
        agent.restore(old.messages);
        std::string at = options.fork_at ? ", forked at record " + std::to_string(*options.fork_at) : "";
        fprintf(stderr, "※ resumed %s (%zu messages%s)%s\n", options.resume->stem().string().c_str(), old.messages.size(), at.c_str(),
                options.append ? ", appending to it" : options.record ? ", writing to a new file that points at it" : ", temporary transcript");
    }
    if (agent.remote()) fprintf(stderr, "※ REMOTE model %s: prompts and tool output leave this machine\n", agent.model.c_str());
    for (const auto& n : agent.tool_notices()) fprintf(stderr, "※ %s\n", n.c_str());
    for (const auto& c : options.context) {
        try {
            fprintf(stderr, "※ %s\n", agent.add_context_file(c).c_str());
        } catch (const std::exception& e) {
            fprintf(stderr, "maic: %s\n", e.what());
            return 2;
        }
    }

    std::signal(SIGINT, on_sigint);
    Printer printer(options.json);
    try {
        agent.submit(prompt, Origin::Local, printer, g_cancel);
    } catch (const std::exception& e) {
        fprintf(stderr, "maic: %s\n", failure_text(agent, e).c_str());
        return 1;
    }
    if (!options.json) fprintf(stdout, "\n");
    fflush(stdout);
    auto u = agent.usage();
    if (u.calls) {
        if (options.json) fprintf(stdout, "%s\n", nlohmann::json{{"type", "usage"}, {"input", u.total_input}, {"output", u.total_output}, {"calls", u.calls}, {"context", u.last.context}}.dump().c_str());
        else fprintf(stderr, "※ tokens: %ld in, %ld out over %d call%s%s\n", u.total_input, u.total_output, u.calls, u.calls == 1 ? "" : "s",
                     u.last.context ? (" (context " + std::to_string(u.last.input) + "/" + std::to_string(u.last.context) + ")").c_str() : "");
    }
    fprintf(stderr, "※ transcript%s: %s\n", options.record ? "" : " (temporary; --record keeps one)", log->path().string().c_str());
    return g_cancel ? 130 : 0;
}

}  // namespace maic
