#include "audit_trail.hpp"
#include "commands.hpp"
#include "headless.hpp"

#include "maic/agent.hpp"
#include "maic/engine.hpp"
#include "maic/protocol.hpp"
#include "maic/status.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"
#include "maic/vendor.hpp"

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace maic {

namespace {

using nlohmann::json;

std::atomic<bool> g_cancel{false};

void on_sigint(int) {
    g_cancel = true;
}

// A context file that cannot be attached ends the run before the turn, with exit status 2.
struct ContextError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One turn through an in-process engine connection: the reply on stdout, tool activity on stderr, or every event as
// a JSON line (--json). Approvals and the question tool are asked on the terminal when stdin is one, else denied
// and left unanswered.
class Printer {
public:
    Printer(Engine& engine, std::string client, bool json) : engine_(engine), client_(std::move(client)), json_(json) {
        // MAIC_PROTOCOL_RECORD=DIR keeps the exchange for `maic protocol check`, as the TUI does.
        if (const char* dir = std::getenv("MAIC_PROTOCOL_RECORD"); dir && *dir) {
            recorder_ = std::make_unique<protocol::Recorder>(std::filesystem::path(dir) / ("headless-" + std::to_string(getpid()) + ".jsonl"));
        }
    }

    json call(const std::string& method, json params) {
        json msg = {{"jsonrpc", "2.0"}, {"id", next_++}, {"method", method}, {"params", std::move(params)}};
        json reply = engine_.call(client_, msg);
        record("in", msg);
        record("out", reply);
        return reply;
    }

    // Follows the session until its turn is over (idle after the response that ends it): "" when it ended, the
    // failure's text when it failed. Ctrl-C cancels the running response.
    std::string follow() {
        bool cancel_sent = false, final = false;
        for (;;) {
            if (g_cancel && !cancel_sent && !response.empty()) {
                cancel_sent = true;
                call("cancelResponse", {{"response_id", response}});
            }
            for (const auto& m : engine_.take(client_, std::chrono::milliseconds(100))) {
                record("out", m);
                if (m.value("method", "") != "maic.event") continue;
                const json& e = m["params"];
                std::string type = e.value("type", "");
                if (type == "response.completed" || type == "response.failed" || type == "maic.response.cancelled") {
                    final = e["response"].contains("maic") && e["response"]["maic"].value("final", false);
                    if (type == "response.failed") failure_ = e["response"]["error"].value("message", "the turn failed");
                }
                if (type == "maic.session.state" && e.value("activity", "") == "idle" && final) return failure_;
                on_event(e);
            }
            if (!engine_.closed(client_).empty()) return failure_;
        }
    }

    std::string session;   // the one this connection follows
    std::string response;  // the running response, for Ctrl-C
    json usage;            // the last maic.usage.updated

private:
    void record(const char* dir, const json& msg) {
        if (!recorder_) return;
        if (auto v = recorder_->add(dir, client_, msg)) fprintf(stderr, "maic: protocol: %s\n", protocol::describe(*v).c_str());
    }

    void on_event(const json& e) {
        std::string type = e.value("type", "");
        if (type == "response.created") {
            response = e["response"].value("id", "");
        } else if (type == "response.output_text.delta" || type == "response.reasoning_text.delta") {
            text(e.value("delta", ""), type == "response.reasoning_text.delta");
        } else if (type == "response.output_item.added") {
            std::string kind = e["item"].value("type", "");
            if (kind == "function_call" || kind == "shell_call") tool_call(e["item"].contains("maic") ? e["item"]["maic"].value("summary", "") : "");
        } else if (type == "response.output_item.done") {
            const json& item = e["item"];
            std::string kind = item.value("type", "");
            if ((kind != "function_call_output" && kind != "shell_call_output") || !item.contains("maic")) return;
            tool_result(kind == "function_call_output" ? item.value("output", "") : item["output"].empty() ? "" : item["output"][0].value("stdout", ""),
                        item["maic"].value("ok", false));
        } else if (type == "maic.notice") {
            std::string kind = e.value("kind", "");
            if (kind == "tool_call") tool_call(e.value("text", ""));
            else if (kind == "tool_result") tool_result(e.value("text", ""), e.value("ok", false));
            else notice(e.value("text", ""));
        } else if (type == "maic.todo.updated") {
            todo(e.value("items", json::array()));
        } else if (type == "maic.approval.requested") {
            ask(e);
        } else if (type == "maic.question.asked") {
            question(e);
        } else if (type == "maic.usage.updated") {
            usage = e;
        }
    }

    void text(const std::string& delta, bool thinking) {
        if (json_) emit({{"type", thinking ? "thinking" : "text"}, {"text", delta}});
        else if (thinking) fprintf(stderr, "%.*s", static_cast<int>(delta.size()), delta.data());
        else fwrite(delta.data(), 1, delta.size(), stdout), fflush(stdout);
    }
    void tool_call(const std::string& summary) {
        if (json_) emit({{"type", "tool_call"}, {"summary", summary}});
        else fprintf(stderr, "▸ %s\n", summary.c_str());
    }
    void tool_result(const std::string& text, bool ok) {
        if (json_) emit({{"type", "tool_result"}, {"ok", ok}, {"text", text}});
        else fprintf(stderr, "  ⎿ %s\n", text.substr(0, text.find('\n')).c_str());
    }
    void notice(const std::string& text) {
        if (json_) emit({{"type", "notice"}, {"text", text}});
        else fprintf(stderr, "※ %s\n", text.c_str());
    }
    void todo(const json& items) {
        if (json_) {
            emit({{"type", "todo"}, {"items", items}});
            return;
        }
        size_t done = 0;
        for (const auto& t : items) done += t.value("done", false);
        fprintf(stderr, "※ todo %zu/%zu done\n", done, items.size());
        for (const auto& t : items) fprintf(stderr, "  %s %s\n", t.value("done", false) ? "[x]" : "[ ]", t.value("text", "").c_str());
    }

    void question(const json& e) {
        std::string text = e.value("text", "");
        std::vector<std::string> options = e.value("options", std::vector<std::string>{});
        if (json_) emit({{"type", "question"}, {"text", text}, {"options", options}});
        std::string answer;
        if (!isatty(STDIN_FILENO)) {
            if (!json_) fprintf(stderr, "? %s\n  (no terminal to answer on)\n", text.c_str());
        } else {
            fprintf(stderr, "\n? %s\n", text.c_str());
            for (size_t i = 0; i < options.size(); ++i) fprintf(stderr, "  [%zu] %s\n", i + 1, options[i].c_str());
            fprintf(stderr, options.empty() ? "answer (empty = none): " : "answer (a number, or your own words; empty = none): ");
            fflush(stderr);
            if (std::getline(std::cin, answer) && !options.empty() && !answer.empty() && answer.find_first_not_of("0123456789") == std::string::npos) {
                size_t n = std::stoul(answer);
                if (n >= 1 && n <= options.size()) answer = options[n - 1];
            }
        }
        call("maic.question.reply", {{"session", session}, {"question", e.value("id", "")}, {"text", answer}});
    }

    void ask(const json& r) {
        std::string summary = r.value("summary", ""), choice = "no", why;
        if (!isatty(STDIN_FILENO)) {
            if (json_) emit({{"type", "denied"}, {"summary", summary}, {"reason", "no terminal to ask on"}});
            else fprintf(stderr, "✗ denied (no terminal to ask on): %s\n", summary.c_str());
        } else {
            if (std::string preview = r.value("preview", ""); !preview.empty()) fprintf(stderr, "\n%s", preview.c_str());
            fprintf(stderr, "\napprove? %s\n  why asking: %s\n  [y] yes  [n] no  [n: reason] no, with a reason for the model  [a] always: %s (this session)  [t] trip the harness: ",
                    summary.c_str(), r.value("reason", "").c_str(), r.value("always_covers", "").c_str());
            fflush(stderr);
            std::string line;
            if (std::getline(std::cin, line) && !line.empty()) {
                switch (line[0]) {
                    case 'y': case 'Y': choice = "yes"; break;
                    case 'a': case 'A': choice = "always"; break;
                    case 't': case 'T': choice = "trip"; break;
                    default:
                        if (auto c = line.find(':'); c != std::string::npos) why = line.substr(c + 1);
                        while (!why.empty() && why.front() == ' ') why.erase(0, 1);
                }
            }
        }
        call("maic.approval.answer", {{"session", session}, {"approval", r.value("id", "")}, {"choice", choice}, {"feedback", why}});
    }

    void emit(const json& j) {
        fprintf(stdout, "%s\n", j.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        fflush(stdout);
    }

    Engine& engine_;
    std::string client_;
    bool json_;
    long next_ = 1;
    std::string failure_;
    std::unique_ptr<protocol::Recorder> recorder_;
};

}  // namespace

int run_headless(const HeadlessOptions& options) {
    // Nothing is asked here: an untrusted project directory stays untrusted unless --trust was given.
    for (const auto& n : trust_notices(std::filesystem::current_path())) fprintf(stderr, "※ %s\n", n.c_str());
    for (const auto& n : settle_trust(std::filesystem::current_path())) fprintf(stderr, "※ %s\n", n.c_str());
    Settings settings = load_settings();
    for (const auto& w : settings.warnings) fprintf(stderr, "※ %s\n", w.c_str());
    audit_gate(settings);  // a due audit holds here, before the session opens (docs/audit-trail.md)
    if (options.model) settings.model = *options.model;
    apply_preset(settings, settings.model);
    settings.model = resolve_model_alias(settings.model);
    if (options.mode) settings.mode = *options.mode;
    if (options.system) settings.system_prompt = *options.system;
    if (options.prefill) settings.prefill = *options.prefill;
    if (options.ctx) settings.context = *options.ctx;
    if (options.ctx2) settings.context_2 = *options.ctx2;
    settings.rules.insert(settings.rules.end(), options.rules.begin(), options.rules.end());
    if (options.load_instructions) settings.load_instructions = *options.load_instructions;
    settings.bans.strings.insert(settings.bans.strings.end(), options.bans.begin(), options.bans.end());
    settings.bans.patterns.insert(settings.bans.patterns.end(), options.ban_patterns.begin(), options.ban_patterns.end());
    if (options.harness) settings.harness = *options.harness;
    if (options.think) settings.think = true;
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
    std::string transcript = log->path().string();
    if (options.ctx) {
        set_context(settings.providers, settings.context);
        std::string r = restart_llamacpp_if_changed();
        if (!r.empty()) fprintf(stderr, "※ %s\n", r.c_str());
    }
    if (options.ctx2) {
        set_context(settings.providers, settings.context_2, "llamacpp-2");
        std::string r = restart_llamacpp_if_changed("llamacpp-2");
        if (!r.empty()) fprintf(stderr, "※ %s\n", r.c_str());
    }
    if (*mode == Mode::Auto && settings.harness == "dumb" && !settings.dumb_auto_ok && !options.accept_dumb_auto) {
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
    if (settings.tripwire == "isolated" && !settings.allow_isolated) {
        fprintf(stderr, "maic: tripwire = \"isolated\" is not allowed: set allow_isolated = true in settings to permit it\n");
        return 2;
    }
    set_tripwire_scope(settings.tripwire, transcript + ".tripped");

    // The turn runs in an in-process engine, as the TUI's do (docs/design/engine-protocol.md, build step 6).
    EngineOptions eo;
    eo.settings = settings;
    eo.kind = "headless";
    Engine engine(eo);
    std::string client = engine.connect(Origin::Local, "maic -p", "in-process");
    Printer printer(engine, client, options.json);
    printer.call("maic.hello", {{"protocol", 1}, {"client", {{"name", "maic -p"}, {"version", MAIC_VERSION}}}});
    LocalSession ls;
    ls.workspace = std::filesystem::current_path();
    ls.settings = settings;
    ls.log = std::move(log);
    ls.titles = false;  // a one-shot is not titled (maic sessions name does it on request)
    ls.setup = [&](Agent& agent, SessionLog& l) {
        configure_agent(agent, settings);
        apply_sampling(agent, settings, options.sampling);  // the command line wins
        agent.mode = *mode;
        if (settings.tripwire == "isolated") agent.set_confined(true);
        agent.reload_instructions();
        for (const auto& p : agent.pending_imports()) {
            fprintf(stderr, "※ %s imports %s from outside your trusted directories: not read until you approve it (%s); maic trust imports --approve asks at a terminal\n",
                    p.importer.c_str(), p.target.c_str(), p.changed ? "it changed since you did" : "not approved yet");
        }
        agent.set_log(&l);
        if (options.resume) {
            LoadedSession old = load_session(*options.resume, options.fork_at.value_or(~size_t(0)));
            agent.restore(old.messages);
            std::string at = options.fork_at ? ", forked at record " + std::to_string(*options.fork_at) : "";
            fprintf(stderr, "※ resumed %s (%zu messages%s)%s\n", options.resume->stem().string().c_str(), old.messages.size(), at.c_str(),
                    options.append ? ", appending to it" : options.record ? ", writing to a new file that points at it" : ", temporary transcript");
        }
        if (agent.remote()) fprintf(stderr, "※ REMOTE model %s: prompts and tool output leave this machine\n", agent.model.c_str());
        for (const auto& n : agent.tool_notices()) fprintf(stderr, "※ %s\n", n.c_str());
        for (const auto& im : options.images) agent.attach_image(im);
        for (const auto& c : options.context) {
            try {
                fprintf(stderr, "※ %s\n", agent.add_context_file(c).c_str());
            } catch (const std::exception& e) {
                throw ContextError(e.what());
            }
        }
    };
    try {
        printer.session = engine.open_local(client, std::move(ls));
    } catch (const ContextError& e) {
        fprintf(stderr, "maic: %s\n", e.what());
        return 2;
    }
    printer.call("maic.session.attach", {{"session", printer.session}});

    std::signal(SIGINT, on_sigint);
    json reply = printer.call("response.create", {{"conversation", printer.session}, {"input", prompt}});
    if (reply.contains("error")) {
        fprintf(stderr, "maic: %s\n", reply["error"].value("message", "the engine refused the prompt").c_str());
        return 1;
    }
    printer.response = reply["result"].value("id", "");
    if (std::string failure = printer.follow(); !failure.empty()) {
        fprintf(stderr, "maic: %s\n", failure.c_str());
        return 1;
    }
    if (!options.json) fprintf(stdout, "\n");
    fflush(stdout);
    const json& u = printer.usage;
    if (u.value("calls", 0) > 0) {
        json total = u.value("total", json::object());
        long in = total.value("input", 0L), out = total.value("output", 0L), last_input = u.value("last_input", 0L), context = u.value("context", 0L);
        int calls = u.value("calls", 0);
        json usage = {{"type", "usage"}, {"input", in}, {"output", out}, {"calls", calls}, {"context", context}};
        if (u.contains("normalized")) usage["normalized"] = u["normalized"];
        if (options.json) fprintf(stdout, "%s\n", usage.dump().c_str());
        else fprintf(stderr, "※ tokens: %ld in, %ld out over %d call%s%s\n", in, out, calls, calls == 1 ? "" : "s",
                     context ? (" (context " + std::to_string(last_input) + "/" + std::to_string(context) + ")").c_str() : "");
        if (!options.json && u.contains("normalized")) fprintf(stderr, "※ adapter normalizations: %s\n", usage["normalized"].dump().c_str());
    }
    fprintf(stderr, "※ transcript%s: %s\n", options.record ? "" : " (temporary; --record keeps one)", transcript.c_str());
    return g_cancel ? 130 : 0;
}

}  // namespace maic
