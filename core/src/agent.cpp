#include "maic/agent.hpp"

#include "maic/tools.hpp"
#include "maic/tripwire.hpp"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
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
    LuaToolSet set = load_lua_tools(harness_.workspace());
    tools_ = std::move(set.tools);
    tool_notices_ = std::move(set.notices);
    schemas_ = tool_schemas();
    for (const auto& t : tools_) {
        schemas_.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    }
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

std::string Agent::user_tools_text() const {
    if (tools_.empty()) return "";
    std::string out = "The user added tools of their own, described in the tool list like the others: ";
    for (size_t i = 0; i < tools_.size(); ++i) out += (i ? ", " : "") + tools_[i].name;
    return out + ". Call them like any other tool.\n";
}

std::string Agent::instructions_text() const {
    std::string out;
    if (instructions_.empty()) return out;
    out += "\n# Standing instructions\n"
           "The user wrote the files below about themselves and about how they want you to work. Follow them. "
           "In them, \"I\", \"me\" and \"my\" mean the user, never you: they describe the person you are talking to. "
           "You are MAIC's agent, a separate thing from the user. Their contents are included right here; do not "
           "read these files with a tool.\n";
    for (const auto& f : instructions_) {
        out += "\n## " + f.path.string() + "\n" + f.text + "\n";
    }
    return out;
}

std::string Agent::system_prompt() const {
    std::string prompt;
    if (!system_prefix.empty()) {
        prompt += "# Operator instructions\nThese come from the operator running MAIC and take precedence over everything below.\n" + system_prefix + "\n\n";
    }
    prompt +=
        "You are the agent inside MAIC, a terminal coding tool on the user's own machine. The user is a person talking "
        "to you through a vim-style interface; you work through tools. You are not the user.\n"
        "\n"
        "# Where you are\n"
        "Workspace: " + harness_.workspace().string() + " (relative paths resolve here; everything you do is scoped to it).\n"
        "Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) + " The user can change modes at any time "
        "(manual, auto-read, edit, auto, plan); you will be told when that happens.\n"
        "\n"
        "# Tools\n"
        "read_file, list_dir, glob (find files by name pattern), search_files (grep -E syntax), write_file, edit_file (one "
        "exact replacement), run_shell.\n"
        "run_shell is bash inside a sandbox: only the workspace is writable, there is no network, no sudo, and a "
        "timeout (default 120 s). In auto-read and plan modes only read-only commands run, with the workspace "
        "read-only too. Tool output is capped; read files in ranges when they are long.\n"
        "question asks the user one thing and waits for the answer; offer options when the choice is fixed. Use it "
        "for decisions that are theirs, not for things the other tools can tell you.\n"
        "todo is your plan for work with several steps: the user sees it. Send the whole list each time, mark items "
        "done as you finish them, and keep it current until the work is done.\n" +
        user_tools_text() +
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
    push({"system", (system_prefix.empty() ? "" : "# Operator instructions (take precedence)\n" + system_prefix + "\n\n") +
                        "This session was resumed. Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) +
                        (prompted_instructions_.empty() ? "" : " Current standing instructions:" + prompted_instructions_)});
}

void Agent::clear() {
    messages_.clear();
    always_allowed_.clear();
    todo_.clear();
    if (log_) log_->write("clear", {});
}

void Agent::add_context(const std::string& text) {
    start_or_update_conversation();
    if (log_) log_->write("context", {{"text", text}});
    push({"user", text});
}

size_t Agent::history_bytes() const {
    size_t n = 0;
    for (const auto& m : messages_) n += m.content.size() + (m.raw.is_null() ? 0 : m.raw.dump().size()) + 40;
    return n;
}

void Agent::rewrite_log() {
    if (!log_) return;
    log_->write("reset", {{"messages", messages_.size()}});
    for (const auto& m : messages_) log_->write("msg", message_to_json(m));
}

std::string Agent::summarise(size_t from, size_t to, const std::atomic<bool>& cancel) {
    std::string transcript;
    for (size_t i = from; i < to; ++i) {
        const auto& m = messages_[i];
        if (m.role == "system") continue;
        if (m.role == "user") transcript += "[User]: " + m.content + "\n\n";
        else if (m.role == "assistant") {
            if (!m.content.empty()) transcript += "[Assistant]: " + m.content + "\n\n";
            for (const auto& c : m.tool_calls) transcript += "[Assistant tool call]: " + c.name + "(" + c.arguments.dump() + ")\n\n";
        } else if (m.role == "tool") {
            transcript += "[Tool result]: " + (m.content.size() > 2000 ? m.content.substr(0, 2000) + " ..." : m.content) + "\n\n";
        }
    }
    std::vector<Message> req = {
        {"system", "You write a handover note for a coding agent that is about to lose the conversation below from its context. "
                   "Write only the note, in markdown with exactly these sections: ## Objective, ## Important details, "
                   "## Work state (Completed / Active / Blocked), ## Next move, ## Relevant files. Terse bullets. Keep exact "
                   "paths, commands, names and numbers. Include decisions the user made and anything they asked to remember. "
                   "If the conversation contains an earlier handover note, merge it in; it will be discarded. Do not mention "
                   "that context was compacted."},
        {"user", "The conversation:\n\n" + transcript + "\nWrite the handover note."},
    };
    auto [provider, model_name] = resolve_model(providers, model);
    ChatOptions options{model_name, false};
    Message reply = chat(provider, options, req, nlohmann::json::array(), [](std::string_view, bool) {}, cancel);
    return reply.content;
}

std::string Agent::compact(Compaction stage, const std::atomic<bool>& cancel) {
    if (messages_.size() < 3) return "nothing to compact";
    size_t before = history_bytes();
    std::string report;
    if (stage == Compaction::Prune) {
        // Stub every tool result except the most recent keep_results.
        int seen = 0;
        size_t stubbed = 0;
        for (size_t i = messages_.size(); i-- > 0;) {
            auto& m = messages_[i];
            if (m.role != "tool") continue;
            if (++seen <= compaction.keep_results) continue;
            if (m.content.rfind("[pruned", 0) == 0) continue;
            size_t lines = std::count(m.content.begin(), m.content.end(), '\n') + 1;
            m.content = "[pruned " + std::to_string(lines) + " lines / " + std::to_string(m.content.size()) + " bytes of " + m.tool_name +
                        " output to save context; call the tool again if you need it]";
            ++stubbed;
        }
        if (!stubbed) return "nothing to prune";
        report = "pruned " + std::to_string(stubbed) + " old tool result" + (stubbed == 1 ? "" : "s");
    } else {
        // Summarise messages [1, cut) into one user message; the system prompt at 0 stays.
        size_t turns = 0;
        for (const auto& m : messages_) turns += m.role == "user";
        size_t target = stage == Compaction::All ? turns : std::max<size_t>(1, static_cast<size_t>(turns * compaction.head_fraction));
        size_t cut = 1, seen = 0;
        for (size_t i = 1; i < messages_.size(); ++i) {
            if (messages_[i].role == "user" && ++seen > target) {
                cut = i;
                break;
            }
            cut = i + 1;
        }
        // Never cut inside a tool round: a tool_call must keep its result.
        while (cut < messages_.size() && messages_[cut].role == "tool") ++cut;
        if (cut <= 1) return "nothing to summarise";
        std::string summary = summarise(1, cut, cancel);
        std::vector<Message> rest(messages_.begin() + static_cast<long>(cut), messages_.end());
        messages_.resize(1);
        messages_.push_back({"user", "[Handover note for the earlier part of this conversation]\n" + summary});
        messages_.insert(messages_.end(), rest.begin(), rest.end());
        report = (stage == Compaction::All ? "summarised the whole conversation" : "summarised the oldest " + std::to_string(cut - 1) + " messages");
        if (log_) log_->write("compact", {{"stage", stage == Compaction::All ? "all" : "head"}, {"summary", summary}, {"messages", cut - 1}});
    }
    // Edited history can't carry provider thinking blocks; replay assistant turns as plain text from here on.
    for (auto& m : messages_) {
        if (m.role == "assistant") m.raw = nullptr, m.raw_kind.clear();
    }
    if (stage == Compaction::Prune && log_) log_->write("compact", {{"stage", "prune"}, {"bytes_before", before}, {"bytes_after", history_bytes()}});
    rewrite_log();
    size_t after = history_bytes();
    return report + " (" + std::to_string(before / 1024) + " KB -> " + std::to_string(after / 1024) + " KB of history)";
}

std::string Agent::compact_auto(const std::atomic<bool>& cancel) {
    UsageReport u = usage();
    size_t before = history_bytes();
    std::string report = compact(Compaction::Prune, cancel);
    // Was pruning enough? Estimate the next call's size from the byte ratio.
    if (u.last.context > 0 && u.last.input > 0 && before > 0) {
        double est = static_cast<double>(u.last.input) * history_bytes() / before;
        if (est / u.last.context >= compaction.at) report += "; " + compact(Compaction::Head, cancel);
    }
    return report;
}

Agent::UsageReport Agent::usage() const {
    std::lock_guard lock(usage_mu_);
    return usage_;
}

std::string Agent::add_context_file(const std::filesystem::path& path) {
    std::string text;
    std::string label = path.string();
    if (label == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        label = "stdin";
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("can't read context file " + label);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.find('\0') != std::string::npos) throw std::runtime_error(label + " looks binary; context files must be text");
    add_context("[Context the user attached from " + label + " (" + std::to_string(text.size()) + " bytes)]\n" + text);
    return "attached " + label + " (" + std::to_string(text.size()) + " bytes) as context";
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
    options.notice = [&](const std::string& t) { events.on_notice(t); };
    denials_ = 0;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (drain_mailbox()) events.on_notice("delivered your queued message");
        if (budget_tokens > 0) {
            UsageReport u = usage();
            if (u.total_input + u.total_output >= budget_tokens) {
                events.on_notice("token budget reached (" + std::to_string(u.total_input + u.total_output) + " of " + std::to_string(budget_tokens) +
                                 "); stopping. :budget N raises it, :budget off removes it.");
                push({"user", "[stopped: the session's token budget is used up]"});
                return;
            }
        }
        if (denials_ >= denials_limit) {
            events.on_notice(std::to_string(denials_) + " denials this turn; stopping so you can say what you want instead");
            push({"user", "[stopped: the user denied " + std::to_string(denials_) + " actions this turn; wait for new instructions]"});
            return;
        }
        {
            UsageReport u = usage();
            if (u.last.context > 0 && compaction.at > 0 && static_cast<double>(u.last.input) / u.last.context >= compaction.at) {
                events.on_notice("context at " + std::to_string(100 * u.last.input / u.last.context) + "%: compacting (" + compact_auto(cancel) + ")");
                std::lock_guard lock(usage_mu_);
                usage_.last.input = 0;  // until the next call reports the real size
            }
        }
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
                reply = chat(provider, options, messages_, schemas_, [&](std::string_view d, bool t) { events.on_text(d, t); }, abort);
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

    std::string name = canonical_tool_name(call.name);
    const LuaTool* lua = name.empty() ? find_tool(call.name) : nullptr;
    if (lua) name = lua->name;
    if (name.empty()) {
        std::string names = tool_names();
        for (const auto& t : tools_) names += ", " + t.name;
        return result("unknown tool '" + call.name + "'. The tools are: " + names + ". Call one of those.", false);
    }
    bool harness_action = !lua && name != "question" && name != "todo";
    Action action;
    if (harness_action) {
        try {
            action = tool_action(harness_, name, call.arguments);
        } catch (const std::exception& e) {
            return result(std::string("error: ") + e.what(), false);
        }
    }

    // The same call over and over means the model is stuck, not working.
    std::string sig = name + "\x1f" + call.arguments.dump();
    repeats_ = sig == last_call_ ? repeats_ + 1 : 1;
    last_call_ = sig;
    if (repeats_ >= repeat_trip) {
        try {
            trip_tripwire("repeated call: " + summary + " x" + std::to_string(repeats_));
            events.on_notice("HARNESS TRIPPED: the same call was repeated " + std::to_string(repeats_) + " times. Run `maic unlock` to continue.");
        } catch (const std::exception& e) {
            events.on_notice(std::string("tripwire could not be set: ") + e.what());
        }
        return result("BLOCKED and the harness was tripped: this exact call was repeated " + std::to_string(repeats_) + " times. Stop.", false);
    }
    if (repeats_ >= repeat_limit) {
        record["decision"] = "deny";
        record["reason"] = "repeated call";
        return result("REFUSED: this exact call has been made " + std::to_string(repeats_) + " times in a row. Do something different, or tell the user what is blocking you.", false);
    }

    if (name == "question") {
        // Changes nothing on the machine, so no policy; the user answers or not.
        if (!call.arguments.contains("question") || !call.arguments["question"].is_string()) return result("error: missing string argument 'question'", false);
        std::vector<std::string> options;
        if (call.arguments.contains("options") && call.arguments["options"].is_array()) {
            for (const auto& o : call.arguments["options"]) {
                if (o.is_string()) options.push_back(o.get<std::string>());
            }
        }
        std::string answer = events.question(call.arguments["question"].get<std::string>(), options);
        record["answer"] = answer;
        return result(answer.empty() ? "(the user gave no answer)" : answer, true);
    }
    if (name == "todo") {
        if (!call.arguments.contains("items") || !call.arguments["items"].is_array()) return result("error: missing array argument 'items'", false);
        todo_.clear();
        for (const auto& it : call.arguments["items"]) {
            if (it.is_string()) todo_.push_back({it.get<std::string>(), false});
            else if (it.is_object() && it.contains("text") && it["text"].is_string()) {
                todo_.push_back({it["text"].get<std::string>(), it.contains("done") && it["done"].is_boolean() && it["done"].get<bool>()});
            }
        }
        events.on_todo(todo_);
        size_t done = std::count_if(todo_.begin(), todo_.end(), [](const TodoItem& t) { return t.done; });
        std::string text = "todo: " + std::to_string(done) + "/" + std::to_string(todo_.size()) + " done";
        for (const auto& t : todo_) text += std::string("\n") + (t.done ? "[x] " : "[ ] ") + t.text;
        return result(text, true);
    }
    if (lua) {
        // Every maic.* call inside the tool is one action, authorised exactly like a built-in and logged with it.
        record["file"] = lua->file.string();
        record["actions"] = nlohmann::json::array();
        Authorise gate = [&](const Action& a, const std::string& s, const std::string& preview) {
            nlohmann::json sub = {{"action", s}};
            Decision d = authorise(a, name, s, preview, origin, events, sub);
            if (d.verdict == Verdict::Allow && a.kind == Action::Kind::Write) save_undo_point(a.path, s + " (" + name + ")");
            record["actions"].push_back(sub);
            return d;
        };
        ToolResult r = run_lua_tool(*lua, call.arguments, harness_, gate, cancel);
        return result(r.text, r.ok);
    }

    Decision d = authorise(action, name, summary, tool_preview(harness_, name, call.arguments), origin, events, record);
    if (d.verdict != Verdict::Allow) return result(d.reason, false);
    if (action.kind == Action::Kind::Write) save_undo_point(action.path, summary);
    ToolResult r = run_tool(harness_, name, call.arguments, d.read_only_sandbox, cancel);
    if (r.ok && name == "read_file") {
        std::string extra = nested_instructions(action.path);
        if (!extra.empty()) r.text += "\n" + extra;
    }
    return result(r.text, r.ok);
}

Decision Agent::authorise(const Action& action, const std::string& tool, const std::string& summary, const std::string& preview,
                          Origin origin, AgentEvents& events, nlohmann::json& record) {
    Decision d = harness_.check(action, mode, origin);
    record["decision"] = verdict_name(d.verdict);
    record["reason"] = d.reason;
    if (d.verdict == Verdict::Ask && always_allowed_.count(Harness::approval_key(action))) {
        d = {Verdict::Allow, "allowed earlier this session"};
    }
    if (d.verdict == Verdict::Ask) {
        std::string key = Harness::approval_key(action);
        std::string covers = key.rfind("shell:", 0) == 0 ? "the program `" + key.substr(6) + "`" : key.rfind("write:", 0) == 0 ? "writes to this file" : "reads of this file";
        ApprovalAnswer answer = events.ask({tool, summary, d.reason, origin, covers, preview});
        record["approval"] = approval_name(answer.choice);
        if (!answer.feedback.empty()) record["feedback"] = answer.feedback;
        switch (answer.choice) {
            case Approval::Yes:
                d.verdict = Verdict::Allow;
                break;
            case Approval::Always:
                always_allowed_.insert(key);
                d.verdict = Verdict::Allow;
                break;
            case Approval::No:
                ++denials_;
                if (!answer.feedback.empty()) return {Verdict::Deny, "DENIED by the user, who says: " + answer.feedback};
                return {Verdict::Deny, "DENIED by the user. Ask what they want instead of retrying."};
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
        return {Verdict::Trip, "BLOCKED and the harness was tripped (" + d.reason + "). Stop and explain to the user."};
    }
    if (d.verdict == Verdict::Deny) return {Verdict::Deny, "DENIED: " + d.reason};
    return d;
}

const LuaTool* Agent::find_tool(const std::string& name) const {
    std::string snake = snake_tool_name(name);
    for (const auto& t : tools_) {
        if (t.name == name || t.name == snake) return &t;
    }
    return nullptr;
}

void Agent::set_instruction_names(std::vector<std::string> names) {
    instruction_names_ = std::move(names);
}

void Agent::save_undo_point(const std::filesystem::path& path, const std::string& summary) {
    UndoPoint u{path, std::nullopt, summary};
    std::ifstream in(path, std::ios::binary);
    if (in) u.before = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    undo_.push_back(std::move(u));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

std::string Agent::undo(size_t count) {
    std::string report;
    for (size_t i = 0; i < count && !undo_.empty(); ++i) {
        UndoPoint u = std::move(undo_.back());
        undo_.pop_back();
        std::error_code ec;
        if (u.before) {
            std::ofstream out(u.path, std::ios::binary | std::ios::trunc);
            out << *u.before;
            report += "restored " + u.path.string() + " (undid: " + u.summary + ")\n";
        } else {
            std::filesystem::remove(u.path, ec);
            report += "removed " + u.path.string() + " (it did not exist before: " + u.summary + ")\n";
        }
        if (log_) log_->write("undo", {{"path", u.path.string()}, {"summary", u.summary}});
    }
    if (report.empty()) return "nothing to undo";
    add_context("[The user undid the last file change(s) with :undo]\n" + report);
    return report;
}

std::string Agent::nested_instructions(const std::filesystem::path& file) {
    if (!load_instruction_files) return "";
    std::error_code ec;
    std::filesystem::path ws = std::filesystem::weakly_canonical(harness_.workspace(), ec);
    std::filesystem::path dir = std::filesystem::weakly_canonical(file, ec).parent_path();
    auto rel = dir.lexically_relative(ws);
    if (rel.empty() || *rel.begin() == "..") return "";  // outside the workspace: nothing extra
    std::string out;
    for (std::filesystem::path d = dir; d != ws && d != d.parent_path(); d = d.parent_path()) {
        for (const auto& fname : instruction_names_) {
            std::filesystem::path f = d / fname;
            if (!std::filesystem::is_regular_file(f, ec) || attached_instructions_.count(f.string())) continue;
            bool at_top = false;
            for (const auto& top : instructions_) at_top = at_top || top.path == f;
            if (at_top) continue;
            std::ifstream in(f);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (text.size() > 32 * 1024) text = text.substr(0, 32 * 1024) + "\n[truncated]";
            attached_instructions_.insert(f.string());
            out += "[Instructions from " + f.string() + " apply to files under " + d.string() + "; follow them]\n" + text + "\n";
        }
    }
    return out;
}

}  // namespace maic
