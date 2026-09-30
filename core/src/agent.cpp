#include "maic/agent.hpp"

#include "maic/tools.hpp"
#include "maic/tripwire.hpp"

namespace maic {

namespace {

constexpr int kMaxSteps = 40;

}  // namespace

Agent::Agent(std::filesystem::path workspace, std::string model) : model(std::move(model)), harness_(std::move(workspace)) {}

std::string Agent::system_prompt() const {
    std::string mode_rule;
    switch (mode) {
        case Mode::Plan:
            mode_rule = "You are in PLAN mode: only read and investigate, then propose a plan. Do not write files or run commands.";
            break;
        case Mode::Manual:
            mode_rule = "The user approves every edit and command before it runs.";
            break;
        case Mode::Edit:
            mode_rule = "File edits inside the workspace apply automatically; commands need the user's approval.";
            break;
        case Mode::Auto:
            mode_rule = "Edits and sandboxed commands inside the workspace run automatically.";
            break;
    }
    return "You are MAIC, a coding agent working in the terminal on the user's machine.\n"
           "Workspace: " + harness_.workspace().string() + " (relative paths resolve here).\n" +
           mode_rule + "\n"
           "Use the tools to inspect before changing anything. Prefer edit_file over rewriting whole files. "
           "Shell commands run sandboxed: only the workspace is writable, there is no network and no sudo. "
           "If an action is denied, do not try to work around it; tell the user what you needed. "
           "Be concise.";
}

void Agent::clear() {
    messages_.clear();
    always_allowed_.clear();
}

void Agent::submit(const std::string& text, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    if (messages_.empty()) {
        messages_.push_back({"system", "", {}, ""});
    }
    messages_.front().content = system_prompt();  // the mode may have changed since the last turn
    messages_.push_back({"user", text, {}, ""});

    ChatOptions options{model, think};
    for (int step = 0; step < kMaxSteps; ++step) {
        Message reply;
        try {
            reply = ollama_chat(options, messages_, tool_schemas(), [&](std::string_view d, bool t) { events.on_text(d, t); }, cancel);
        } catch (const Cancelled&) {
            messages_.push_back({"user", "[interrupted by the user]", {}, ""});
            events.on_notice("interrupted");
            return;
        }
        messages_.push_back(reply);
        if (reply.tool_calls.empty()) {
            return;
        }
        for (const auto& call : reply.tool_calls) {
            messages_.push_back(run_tool_call(call, origin, events, cancel));
            if (cancel.load()) {
                events.on_notice("interrupted");
                return;
            }
        }
    }
    events.on_notice("stopped after " + std::to_string(kMaxSteps) + " steps; send a message to continue");
}

Message Agent::run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    auto result = [&](const std::string& text, bool ok) {
        events.on_tool_result(text, ok);
        return Message{"tool", text, {}, call.name};
    };

    std::string summary = tool_summary(call.name, call.arguments);
    events.on_tool_call(summary);

    if (tripwire_state()) {
        return result("BLOCKED: the harness tripwire is tripped. Nothing can run until the user unlocks it.", false);
    }

    Action action;
    try {
        action = tool_action(harness_, call.name, call.arguments);
    } catch (const std::exception& e) {
        return result(std::string("error: ") + e.what(), false);
    }

    Decision d = harness_.check(action, mode, origin);
    if (d.verdict == Verdict::Ask && always_allowed_.count(Harness::approval_key(action))) {
        d = {Verdict::Allow, "allowed earlier this session"};
    }
    if (d.verdict == Verdict::Ask) {
        switch (events.ask({call.name, summary, d.reason, origin})) {
            case Approval::Yes:
                d.verdict = Verdict::Allow;
                break;
            case Approval::Always:
                always_allowed_.insert(Harness::approval_key(action));
                d.verdict = Verdict::Allow;
                break;
            case Approval::No:
                return result("DENIED by the user. Ask what they want instead of retrying.", false);
            case Approval::Trip:
                d = {Verdict::Trip, "the user tripped the harness at the approval prompt"};
                break;
        }
    }
    if (d.verdict == Verdict::Trip) {
        try {
            trip_tripwire(summary + " -- " + d.reason);
            events.on_notice("HARNESS TRIPPED: " + d.reason + ". Run `maic unlock` to continue.");
        } catch (const std::exception& e) {
            events.on_notice(std::string("tripwire could not be set: ") + e.what());
        }
        return result("BLOCKED and the harness was tripped (" + d.reason + "). Stop and explain to the user.", false);
    }
    if (d.verdict == Verdict::Deny) {
        return result("DENIED: " + d.reason, false);
    }

    ToolResult r = run_tool(harness_, call.name, call.arguments, cancel);
    return result(r.text, r.ok);
}

}  // namespace maic
