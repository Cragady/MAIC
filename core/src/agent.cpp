#include "maic/agent.hpp"

#include "maic/tools.hpp"
#include "maic/tripwire.hpp"

#include <unistd.h>

#include <thread>

namespace maic {

namespace {

constexpr int kMaxSteps = 40;
constexpr size_t kMaxLoggedResult = 64 * 1024;

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Allow: return "allow";
        case Verdict::Ask: return "ask";
        case Verdict::Deny: return "deny";
        case Verdict::Trip: return "trip";
    }
    return "?";
}

const char* approval_name(Approval a) {
    switch (a) {
        case Approval::Yes: return "yes";
        case Approval::No: return "no";
        case Approval::Always: return "always";
        case Approval::Trip: return "trip";
    }
    return "?";
}

}  // namespace

Agent::Agent(std::filesystem::path workspace, std::string model) : model(std::move(model)), harness_(std::move(workspace)) {
    reload_instructions();
}

std::string mode_rule(Mode mode) {
    std::string mode_rule;
    switch (mode) {
        case Mode::Plan:
            mode_rule = "You are in PLAN mode: only read and investigate (read-only commands like ls, grep and git log work), "
                        "then propose a plan. Do not write files.";
            break;
        case Mode::AutoRead:
            mode_rule = "Reads and read-only commands run automatically; edits and other commands need the user's approval.";
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
    return mode_rule;
}

std::string Agent::instructions_text() const {
    std::string out;
    if (instructions_.empty()) return out;
    out += "\n# Standing instructions\n"
           "The user wrote the files below about themselves and about how they want you to work. Follow them. "
           "In them, \"I\", \"me\" and \"my\" mean the user, never you: they describe the person you are talking to. "
           "You are MAIC's agent, a separate thing from the user.\n";
    for (const auto& f : instructions_) {
        out += "\n## " + f.path.string() + "\n" + f.text + "\n";
    }
    return out;
}

std::string Agent::system_prompt() const {
    std::string prompt =
        "You are the agent inside MAIC, a terminal coding tool on the user's own machine. The user is a person talking "
        "to you through a vim-style interface; you work through tools. You are not the user.\n"
        "\n"
        "# Where you are\n"
        "Workspace: " + harness_.workspace().string() + " (relative paths resolve here; everything you do is scoped to it).\n"
        "Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) + " The user can change modes at any time "
        "(manual, auto-read, edit, auto, plan); you will be told when that happens.\n"
        "\n"
        "# Tools\n"
        "read_file, list_dir, search_files (grep -E syntax), write_file, edit_file (one exact replacement), run_shell.\n"
        "run_shell is bash inside a sandbox: only the workspace is writable, there is no network, no sudo, and a "
        "timeout (default 120 s). In auto-read and plan modes only read-only commands run, with the workspace "
        "read-only too. Tool output is capped; read files in ranges when they are long.\n"
        "\n"
        "# The harness\n"
        "Every tool call is checked before it runs. Results starting with DENIED or BLOCKED are final for that "
        "call: do not retry it, do not look for another route to the same effect, and do not ask the user to "
        "disable anything. Explain what you needed and let them decide. Some actions trip a lock that stops all "
        "tools until the user resets it with their password; if you see that, stop and summarise where things "
        "stand. Credentials and keys are never readable. You cannot escalate privileges.\n"
        "\n"
        "# Working style\n"
        "Inspect before changing. Prefer edit_file over rewriting whole files. Keep changes small and in the "
        "style of the surrounding code. Say what you changed and what you did not verify. Lines beginning "
        "\"[The user ran this in their shell:\" are commands the user ran themselves, with the real output; treat "
        "them as facts about the machine. Messages can arrive mid-turn; the newest one is the current instruction.\n"
        "Never infer the user's name or pronouns from file paths, usernames, email addresses or commit authors; "
        "use only what the user or their instructions state, and otherwise don't address them by name. "
        "Be concise.\n";
    if (log_) {
        prompt += "This session is being saved to " + log_->path().string() +
                  ". The user can list and clean MAIC's transcripts and logs with `maic artifacts`.\n";
    }
    return prompt + instructions_text();
}

void Agent::start_or_update_conversation() {
    reload_instructions();
    std::string instructions = instructions_text();
    if (messages_.empty()) {
        push({"system", system_prompt()});
        prompted_mode_ = mode;
        prompted_instructions_ = instructions;
        return;
    }
    if (mode != prompted_mode_) {
        push({"system", "The mode is now " + std::string(mode_name(mode)) + ". " + mode_rule(mode)});
        prompted_mode_ = mode;
    }
    if (instructions != prompted_instructions_) {
        push({"system", "The user's standing instructions changed. Follow these from now on:" + instructions});
        prompted_instructions_ = instructions;
    }
}

void Agent::set_log(SessionLog* log) {
    log_ = log;
    if (log_) {
        char host[256] = "";
        gethostname(host, sizeof(host) - 1);
        log_->write("start", {{"workspace", harness_.workspace().string()}, {"model", model}, {"mode", mode_name(mode)},
                              {"host", host}, {"pid", getpid()}});
    }
}

void Agent::push(Message m) {
    if (log_) log_->write("msg", message_to_json(m));
    messages_.push_back(std::move(m));
}

void Agent::restore(std::vector<Message> messages) {
    messages_ = std::move(messages);
    always_allowed_.clear();
    if (messages_.empty()) return;
    // A turn cut off mid-tool-round can't be continued; drop the dangling assistant call.
    while (!messages_.empty() && messages_.back().role == "assistant" && !messages_.back().tool_calls.empty()) messages_.pop_back();
    reload_instructions();
    prompted_instructions_ = instructions_text();
    prompted_mode_ = mode;
    push({"system", "This session was resumed. Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) +
                        (prompted_instructions_.empty() ? "" : " Current standing instructions:" + prompted_instructions_)});
}

void Agent::clear() {
    messages_.clear();
    always_allowed_.clear();
    if (log_) log_->write("clear", {});
}

void Agent::add_context(const std::string& text) {
    start_or_update_conversation();
    if (log_) log_->write("context", {{"text", text}});
    push({"user", text});
}

Agent::UsageReport Agent::usage() const {
    std::lock_guard lock(usage_mu_);
    return usage_;
}

void Agent::post_message(const std::string& text) {
    std::lock_guard lock(mailbox_mu_);
    mailbox_.push_back(text);
}

void Agent::deliver_now() {
    deliver_now_ = true;
}

size_t Agent::queued() const {
    std::lock_guard lock(mailbox_mu_);
    return mailbox_.size();
}

std::vector<std::string> Agent::take_queued() {
    std::lock_guard lock(mailbox_mu_);
    std::vector<std::string> out(mailbox_.begin(), mailbox_.end());
    mailbox_.clear();
    return out;
}

bool Agent::drain_mailbox() {
    std::deque<std::string> pending;
    {
        std::lock_guard lock(mailbox_mu_);
        pending.swap(mailbox_);
    }
    for (const auto& text : pending) {
        if (log_) log_->write("user", {{"text", text}, {"queued", true}});
        push({"user", text});
    }
    return !pending.empty();
}

void Agent::submit(const std::string& text, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    start_or_update_conversation();
    auto [provider, model_name] = resolve_model(providers, model);
    if (log_) {
        log_->write("user", {{"text", text}, {"provider", provider.name}, {"model", model}, {"remote", provider.remote()},
                             {"mode", mode_name(mode)}, {"origin", origin == Origin::Local ? "local" : "remote"}});
    }
    push({"user", text});

    ChatOptions options{model_name, think};
    for (int step = 0; step < kMaxSteps; ++step) {
        if (drain_mailbox()) events.on_notice("delivered your queued message");
        Message reply;
        try {
            // A queued message with deliver_now aborts this call; the retry below starts with the message included.
            std::atomic<bool> abort{false};
            std::thread watcher([&] {
                while (!abort.load()) {
                    if (cancel.load() || (deliver_now_.load() && queued() > 0)) {
                        abort = true;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            try {
                reply = chat(provider, options, messages_, tool_schemas(), [&](std::string_view d, bool t) { events.on_text(d, t); }, abort);
            } catch (...) {
                abort = true;
                watcher.join();
                throw;
            }
            abort = true;
            watcher.join();
        } catch (const Cancelled&) {
            if (!cancel.load() && deliver_now_.exchange(false)) {
                events.on_notice("delivering your message now");
                continue;  // nothing from the aborted call was kept; the loop re-asks with the mailbox drained
            }
            push({"user", "[interrupted by the user]"});
            events.on_notice("interrupted");
            return;
        }
        deliver_now_ = false;
        if (log_ && !reply.content.empty()) log_->write("assistant", {{"text", reply.content}});
        if (reply.usage.input || reply.usage.output) {
            std::lock_guard lock(usage_mu_);
            usage_.last = reply.usage;
            usage_.total_input += reply.usage.input;
            usage_.total_output += reply.usage.output;
            ++usage_.calls;
            if (log_) log_->write("usage", {{"input", reply.usage.input}, {"output", reply.usage.output}, {"context", reply.usage.context}});
        }
        push(reply);
        if (reply.tool_calls.empty()) {
            return;
        }
        for (size_t i = 0; i < reply.tool_calls.size(); ++i) {
            push(run_tool_call(reply.tool_calls[i], origin, events, cancel));
            if (cancel.load()) {
                // Every call in the turn needs a result, or the next request is rejected.
                for (size_t j = i + 1; j < reply.tool_calls.size(); ++j) {
                    const auto& c = reply.tool_calls[j];
                    push({"tool", "cancelled by the user", {}, c.name, c.id, true});
                }
                events.on_notice("interrupted");
                return;
            }
        }
    }
    events.on_notice("stopped after " + std::to_string(kMaxSteps) + " steps; send a message to continue");
}

Message Agent::run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    nlohmann::json record = {{"tool", call.name}, {"arguments", call.arguments}};
    auto result = [&](const std::string& text, bool ok) {
        events.on_tool_result(text, ok);
        if (log_) {
            record["ok"] = ok;
            record["result"] = text.size() > kMaxLoggedResult ? text.substr(0, kMaxLoggedResult) + "\n[truncated]" : text;
            log_->write("tool", record);
        }
        return Message{"tool", text, {}, call.name, call.id, !ok};
    };

    std::string summary = tool_summary(call.name, call.arguments);
    events.on_tool_call(summary);

    if (tripwire_state()) {
        return result("BLOCKED: the harness tripwire is tripped. Nothing can run until the user unlocks it.", false);
    }
    if (call.arguments.contains("_maic_invalid_input")) {
        return result("INVALID_JSON: the tool input was not valid JSON, so nothing ran. Send the call again with valid arguments.", false);
    }

    Action action;
    try {
        action = tool_action(harness_, call.name, call.arguments);
    } catch (const std::exception& e) {
        return result(std::string("error: ") + e.what(), false);
    }

    Decision d = harness_.check(action, mode, origin);
    record["decision"] = verdict_name(d.verdict);
    record["reason"] = d.reason;
    if (d.verdict == Verdict::Ask && always_allowed_.count(Harness::approval_key(action))) {
        d = {Verdict::Allow, "allowed earlier this session"};
    }
    if (d.verdict == Verdict::Ask) {
        Approval answer = events.ask({call.name, summary, d.reason, origin});
        record["approval"] = approval_name(answer);
        switch (answer) {
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

    ToolResult r = run_tool(harness_, call.name, call.arguments, d.read_only_sandbox, cancel);
    return result(r.text, r.ok);
}

}  // namespace maic
