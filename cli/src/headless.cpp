#include "headless.hpp"

#include "maic/agent.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>

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
    Approval ask(const ApprovalRequest& r) override {
        if (!isatty(STDIN_FILENO)) {
            if (json_) emit({{"type", "denied"}, {"summary", r.summary}, {"reason", "no terminal to ask on"}});
            else fprintf(stderr, "✗ denied (no terminal to ask on): %s\n", r.summary.c_str());
            return Approval::No;
        }
        fprintf(stderr, "\napprove? %s\n  why asking: %s\n  [y] yes  [n] no  [a] always this session  [t] trip the harness: ", r.summary.c_str(), r.reason.c_str());
        fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line) || line.empty()) return Approval::No;
        switch (line[0]) {
            case 'y': case 'Y': return Approval::Yes;
            case 'a': case 'A': return Approval::Always;
            case 't': case 'T': return Approval::Trip;
            default: return Approval::No;
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
    if (options.mode) settings.mode = *options.mode;
    auto mode = parse_mode(settings.mode);
    if (!mode) {
        fprintf(stderr, "maic: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    std::string prompt = options.prompt;
    if (prompt.empty() || prompt == "-") {
        prompt.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    if (prompt.empty()) {
        fprintf(stderr, "maic: nothing to send (give a prompt, or pipe one in)\n");
        return 2;
    }

    SessionLog log = !options.resume        ? SessionLog("headless")
                     : options.append         ? SessionLog::reopen(*options.resume)
                                              : SessionLog::fork(*options.resume, count_records(*options.resume), "headless");
    Agent agent(std::filesystem::current_path(), settings.model);
    agent.providers = settings.providers;
    agent.mode = *mode;
    agent.think = options.think || settings.think;
    agent.set_log(&log);
    if (options.resume) {
        LoadedSession old = load_session(*options.resume);
        agent.restore(old.messages);
        fprintf(stderr, "※ resumed %s (%zu messages)%s\n", options.resume->stem().string().c_str(), old.messages.size(),
                options.append ? ", appending to it" : ", writing to a new file that points at it");
    }
    if (agent.remote()) fprintf(stderr, "※ REMOTE model %s: prompts and tool output leave this machine\n", agent.model.c_str());

    std::signal(SIGINT, on_sigint);
    Printer printer(options.json);
    try {
        agent.submit(prompt, Origin::Local, printer, g_cancel);
    } catch (const std::exception& e) {
        fprintf(stderr, "maic: %s\n", e.what());
        return 1;
    }
    if (!options.json) fprintf(stdout, "\n");
    fprintf(stderr, "※ transcript: %s\n", log.path().string().c_str());
    return g_cancel ? 130 : 0;
}

}  // namespace maic
