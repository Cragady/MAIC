#include "maic/agent.hpp"

#include "maic/full_output.hpp"

#include "maic/paths.hpp"
#include "maic/settings.hpp"
#include "maic/tools.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <iostream>
#include <iterator>
#include <thread>
#include <tuple>

namespace maic {

namespace {

constexpr size_t kMaxLoggedResult = 64 * 1024;
constexpr size_t kMaxDelegateResult = 16 * 1024;  // a subagent's report, as the parent's tool result

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

// A subagent's events, as the parent's front end sees them: its tool calls and notices carry the agent's
// name, its approvals are asked of the user with the agent named, and its prose is not streamed (the final
// answer comes back as the task result).
struct ChildEvents : AgentEvents {
    AgentEvents& parent;
    std::string agent;
    ChildEvents(AgentEvents& parent, std::string agent) : parent(parent), agent(std::move(agent)) {}
    void on_text(std::string_view, bool) override {}
    void on_tool_call(const std::string& summary) override { parent.on_tool_call("↳ " + agent + ": " + summary); }
    void on_tool_result(const std::string& text, bool ok) override { parent.on_tool_result(text, ok); }
    void on_notice(const std::string& text) override { parent.on_notice(agent + ": " + text); }
    void on_tool_started(const std::string& tool, const std::string& path, const std::string& summary) override {
        parent.on_tool_started(tool, path, agent + ": " + summary);
    }
    void on_file_written(const std::filesystem::path& path, const std::string& tool) override { parent.on_file_written(path, tool); }
    void on_tool_output(const std::string& call_id, OutputStream stream, std::string_view chunk, size_t offset) override {
        parent.on_tool_output(agent + ":" + call_id, stream, chunk, offset);
    }
    void on_tool_full_output(const std::filesystem::path& file) override { parent.on_tool_full_output(file); }
    ApprovalAnswer ask(const ApprovalRequest& request) override {
        ApprovalRequest r = request;
        r.summary = agent + ": " + request.summary;
        return parent.ask(r);
    }
};

}  // namespace

Agent::Agent(std::filesystem::path workspace, std::string model) : model(std::move(model)), harness_(std::move(workspace)) {
    reload_instructions();
    LuaToolSet set = load_lua_tools(harness_.workspace());
    tools_ = std::move(set.tools);
    tool_notices_ = std::move(set.notices);
    std::vector<std::string> taken;
    for (const auto& t : tools_) taken.push_back(t.name);
    ScriptToolSet scripts = load_script_tools(harness_.workspace(), taken);
    script_tools_ = std::move(scripts.tools);
    tool_notices_.insert(tool_notices_.end(), scripts.notices.begin(), scripts.notices.end());
    schemas_ = tool_schemas();
    for (const auto& t : tools_) {
        schemas_.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    }
    for (const auto& t : script_tools_) {
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
    if (tools_.empty() && script_tools_.empty()) return "";
    std::string out = "The user added tools of their own, described in the tool list like the others; call them like any other tool.\n";
    for (const auto& t : tools_) out += "- " + t.name + ": " + t.description + " (Lua; every file it touches is checked like a built-in call)\n";
    for (const auto& t : script_tools_) {
        auto list = [](const std::vector<std::string>& globs) {
            std::string s;
            for (const auto& g : globs) s += (s.empty() ? "" : ", ") + g;
            return s.empty() ? "nothing" : s;
        };
        out += "- " + t.name + ": " + t.description + " (a " + script_tool_language(t) + " script; may read " + list(t.reads) + ", may write " + list(t.writes) + ")\n";
    }
    return out;
}

std::string Agent::instructions_text() const {
    std::string out;
    if (instructions_.empty()) return out;
    out += "\n# Standing instructions\n"
           "The user wrote the files below about themselves and about how they want you to work. Follow them. "
           "In them, \"I\", \"me\" and \"my\" mean the user, never you: they describe the person you are talking to. "
           "You are MAIC's agent, a separate thing from the user. Their contents are included right here; do not "
           "read these files with a tool. They run from the most general (system-wide, then the user's own) to the "
           "most specific (the directory closest to the workspace); where two conflict, the later one takes precedence.\n";
    for (const auto& f : instructions_) {
        out += "\n## " + f.path.string() + (f.imported_by.empty() ? "" : " (imported by " + f.imported_by.string() + ")") + "\n" + f.text + "\n";
    }
    return out;
}

void Agent::reload_instructions() {
    pending_imports_.clear();
    instructions_ = load_instruction_files ? load_instructions(harness_.workspace(), instruction_options_, {}, &pending_imports_) : std::vector<InstructionFile>{};
    nested_allowed_ = load_instruction_files ? nested_allowed(harness_.workspace()) : std::set<std::filesystem::path>{};
}

// The operator instructions again, at the end of the user's turn. Measured with a 4B against this very
// prompt and its tool schemas: every system-side placement (top, end, both, a system reminder before the
// turn, even the rule alone) was ignored once tools were attached; the rule closing the user turn was
// followed every time. The transcript keeps the user's words as typed; this is what the model is sent.
std::string Agent::operator_text() const {
    std::string out = system_prefix;
    for (const auto& r : rules) out += (out.empty() ? "" : "\n") + ("- " + r);
    return out;
}

void Agent::set_system_prefix(const std::string& text) {
    system_prefix = text;
    if (messages_.empty()) return;
    std::string now = operator_text();
    push({"system", now.empty() ? "The operator instructions given earlier are withdrawn."
                                : "# Operator instructions (take precedence over everything before)\n" + now});
}

void Agent::set_workspace(const std::filesystem::path& dir, Origin origin) {
    if (origin == Origin::Remote) throw std::runtime_error("the workspace is changed by the local user only");
    std::filesystem::path from = harness_.workspace();
    harness_.set_workspace(dir);
    reload_instructions();
    if (log_) log_->write("workspace", {{"from", from.string()}, {"to", harness_.workspace().string()}});
    if (messages_.empty()) return;
    prompted_instructions_ = instructions_text();
    push({"system", "The workspace moved from " + from.string() + " to " + harness_.workspace().string() +
                        ": relative paths resolve there now and the harness scopes your work to it." +
                        (prompted_instructions_.empty() ? " No standing instructions apply there." : " Standing instructions in effect there:" + prompted_instructions_)});
}

void Agent::set_rules(std::vector<std::string> new_rules) {
    rules = std::move(new_rules);
    if (messages_.empty()) return;
    std::string now = operator_text();
    push({"system", now.empty() ? "The operator instructions given earlier are withdrawn."
                                : "# Operator instructions (take precedence over everything before)\n" + now});
}

void Agent::attach_image(const std::filesystem::path& file) {
    pending_images_.push_back(load_image(file));
}

std::vector<std::string> Agent::pending_images() const {
    std::vector<std::string> out;
    for (const auto& im : pending_images_) out.push_back(im.name);
    return out;
}

void Agent::clear_pending_images() { pending_images_.clear(); }

std::string Agent::with_operator_note(const std::string& text) const {
    std::string op = operator_text();
    if (op.empty() || !operator_note_in_turn) return text;
    return text + "\n\n(Operator instructions in force, they take precedence: " + op + ")";
}

std::string Agent::system_prompt() const {
    std::string prompt;
    if (!operator_text().empty()) {
        prompt += "# Operator instructions\nThese come from the operator running MAIC and take precedence over everything below.\n" + operator_text() + "\n\n";
    }
    prompt +=
        "You are the agent inside MAIC, a terminal coding tool on the user's own machine. The user is a person talking "
        "to you through a vim-style interface; you work through tools. You are not the user.\n"
        "\n"
        "# Where you are\n"
        "Workspace: " + harness_.workspace().string() + " (relative paths resolve here; everything you do is scoped to it).\n"
        "Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) + " The user can change modes at any time "
        "(manual, auto-read, edit, auto, plan); you will be told when that happens.\n"
        "\n";
    if (!agent_name_.empty()) {
        const AgentDef* p = harness_.agent_def();
        std::string tools;
        for (const auto& t : p->tools) tools += (tools.empty() ? "" : ", ") + t;
        std::string paths;
        for (const auto& g : p->write_paths) paths += (paths.empty() ? "" : ", ") + g;
        prompt += "# You are a subagent\n"
                  "A parent agent gave you one task as the " + agent_name_ + " agent" +
                  (p->read_only() ? ", which is read-only: you change nothing" : paths.empty() ? "" : ", which writes only under " + paths) +
                  (tools.empty() ? "" : "; your tools are " + tools) +
                  ". You have no user to talk to: question, todo and task are not available. Approvals your mode "
                  "requires are asked of the user through the parent. Do the task with your tools, then write your report as your final "
                  "answer: it is handed to the parent as the result of its task call and nothing else of yours is, so make it "
                  "complete and self-contained (paths, line numbers, what you found or changed, what you could not do).\n\n";
    }
    prompt +=
        "# Tools\n"
        "read_file (with grep to get only matching lines of a big file), list_dir (depth for a tree), glob (find files by "
        "name pattern), search_files (grep -E syntax), write_file, edit_file (one exact replacement), run_shell.\n"
        "multi_edit makes several replacements in one file in one step; if any fails nothing is written. apply_patch "
        "applies a unified diff (diff -u / git diff format) to one or more files; context must match exactly.\n"
        "move_file, copy_file, delete_file and make_dir move, copy, delete and mkdir; use them, a shell mv, cp, rm "
        "or mkdir will ask the user. delete_file needs recursive: true for a directory with contents.\n"
        "run_shell is bash inside a sandbox: only the workspace is writable, there is no network, no sudo, and a "
        "timeout (default 120 s). In auto-read and plan modes only read-only commands run, with the workspace "
        "read-only too. Tool output is capped; read files in ranges when they are long.\n" +
        (agent_name_.empty()
             ? "question asks the user one thing and waits for the answer; offer options when the choice is fixed. Use it "
               "for decisions that are theirs, not for things the other tools can tell you.\n"
               "todo is your plan for work with several steps: the user sees it. Send the whole list each time, mark items "
               "done as you finish them, and keep it current until the work is done.\n"
               "task hands one job to a subagent that works in this workspace as one of the agents below and returns its "
               "report as the result. Use explore for a long read or search you do not want in your own context (ask it for "
               "a short report with paths and line numbers), and plan for a review of your own change before you call the "
               "work done; general takes a self-contained piece of editing. The subagent sees none of this conversation: put "
               "everything it needs in prompt and context. It cannot run task, ask the user or keep a plan, its approvals come "
               "to the user through you, and it stops at its agent's budget.\n"
               "Agents for task: " + task_agents_text() + "\n" + (task_models_text().empty() ? "" : "task's model: " + task_models_text() + "\n")
             : std::string()) +
        user_tools_text() +
        "\n"
        "# Helpers on this machine\n"
        "`maic-workflow-edit FILE ...` (through run_shell) edits the tunable fields of a ComfyUI workflow JSON without "
        "touching its wiring: run `maic-workflow-edit inspect FILE --json` first to learn node ids, titles and field "
        "names, then `set FILE NODE.FIELD VALUE`, `append`, `prepend`, `replace-all FILE OLD NEW`, or `apply FILE "
        "edits.json`; `--dry-run` shows the diff. The file must be inside the workspace to be written.\n"
        "`maic-storyboard` merges a story JSON (characters, setting, panels) into a MAIC manga workflow. Run it with no "
        "arguments to be told how to begin: ask the user for the story file and the destination, `maic-storyboard start "
        "STORY TEMPLATE --out DEST`, then `maic-storyboard next` once per panel after running the command it prints. It "
        "does the mechanical part itself and checks your work; you only write each panel's tags.\n"
        "`maic-danbooru-tags check --prompt \"1girl, grey hair, ...\"` says which tags are real Danbooru tags, which are aliases "
        "of a canonical tag, and which are unknown (with near matches); `maic-danbooru-tags search WORD` lists tags containing a "
        "word; `maic-danbooru-tags groups show NAME` prints a whole tag group page (posture, hair, attire, image composition, ...) "
        "and `groups search WORD` finds a word across them. Check a prompt before writing it. All of these answer from local "
        "files; never run `fetch` yourself.\n"
        "`maic-panel-check WORKFLOW N` prints panel N's prompt, negative, sampler settings and captions on one screen and flags "
        "the usual mistakes offline (no or doubled count tag, solo with 2girls, a tag in both prompt and negative, too many tags, "
        "unknown Danbooru tags, a caption too long for its overlay); run it after writing a panel and fix what it flags.\n"
        "For transcripts, a MAIC session or a Claude Code one: `cai read FILE.jsonl` prints what was said and writes nothing, "
        "and `cai trans-fairy` cuts, composes and grafts transcripts into new files, never over the source (`cai trans-fairy "
        "--man-help` is its reference).\n"
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
    prompt += instructions_text();
    // Stated again at the end: a small model drops a short rule buried under the briefing, and keeps one
    // that closes the prompt (measured with a 4B against this prompt: top alone and end alone are ignored,
    // both together are followed).
    if (!operator_text().empty()) prompt += "\n\n# Operator instructions, again\nThey take precedence over everything above:\n" + operator_text() + "\n";
    return prompt;
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
        nlohmann::json start = {{"workspace", harness_.workspace().string()}, {"model", model}, {"mode", mode_name(mode)}, {"host", host}, {"pid", getpid()}};
        if (!agent_name_.empty()) {
            start["agent"] = agent_name_;
            start["parent"] = parent_id_;
            if (!model_reason_.empty()) start["model_reason"] = model_reason_;
        }
        log_->write("start", start);
    }
}

void Agent::set_agent_def(const AgentDef& def) {
    agent_name_ = def.name;
    mode = def.mode;
    harness_.set_agent_def(def);
    budget_tokens = def.budget_tokens;
    max_steps = def.max_steps;
    review_with_model = review_with_model && def.reviewer;
    nlohmann::json kept = nlohmann::json::array();
    for (const auto& s : schemas_) {
        std::string name = s["function"]["name"];
        if (name == "task" || name == "question" || name == "todo" || !def.allows_tool(name)) continue;
        kept.push_back(s);
    }
    schemas_ = std::move(kept);
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
    push({"system", (operator_text().empty() ? "" : "# Operator instructions (take precedence)\n" + operator_text() + "\n\n") +
                        "This session was resumed. Mode: " + std::string(mode_name(mode)) + ". " + mode_rule(mode) +
                        (prompted_instructions_.empty() ? "" : " Current standing instructions:" + prompted_instructions_)});
}

void Agent::clear() {
    messages_.clear();
    always_allowed_.clear();
    todo_.clear();
    attached_instructions_.clear();  // the new conversation has not seen them
    if (log_) log_->write("clear", {});
}

void Agent::add_context(const std::string& text) {
    start_or_update_conversation();
    if (log_) log_->write("context", {{"text", text}});
    push({"user", text});
}

size_t Agent::history_bytes() const {
    size_t n = 0;
    for (const auto& m : messages_) {
        n += m.content.size() + (m.raw.is_null() ? 0 : m.raw.dump().size()) + 40;
        for (const auto& im : m.images) n += im.base64.size();
    }
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
    options.normalized = count_normalized(provider);
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
        // Pictures are the other thing that fills a context: keep the ones on the last two user turns.
        int user_seen = 0;
        size_t pictures = 0;
        for (size_t i = messages_.size(); i-- > 0;) {
            auto& m = messages_[i];
            if (m.role != "user") continue;
            if (++user_seen <= 2 || m.images.empty()) continue;
            for (const auto& im : m.images) m.content += "\n[image " + im.name + " removed to save context]";
            pictures += m.images.size();
            m.images.clear();
        }
        if (!stubbed && !pictures) return "nothing to prune";
        report = "pruned " + std::to_string(stubbed) + " old tool result" + (stubbed == 1 ? "" : "s") + (pictures ? ", " + std::to_string(pictures) + " old image" + (pictures == 1 ? "" : "s") : "");
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

std::string Agent::compact_auto(const std::atomic<bool>& cancel, size_t window, bool force_head) {
    UsageReport u = usage();
    if (!window) window = u.last.context;
    size_t before = history_bytes();
    std::string report = compact(Compaction::Prune, cancel);
    // Was pruning enough? Scale the last reported size by the byte ratio, or estimate from bytes alone.
    double est = u.last.input > 0 && before > 0 ? static_cast<double>(u.last.input) * history_bytes() / before : static_cast<double>(estimated_tokens());
    if (force_head || (window > 0 && est / window >= compaction.at)) {
        report += "; " + compact(Compaction::Head, cancel);
        if (window > 0 && static_cast<double>(estimated_tokens()) > window) report += "; " + compact(Compaction::All, cancel);
    }
    return report;
}

Agent::UsageReport Agent::usage() const {
    std::lock_guard lock(usage_mu_);
    return usage_;
}

std::function<void(const std::string&, int)> Agent::count_normalized(const Provider& provider) {
    return [this, name = provider.name, upstream = provider.upstream_name()](const std::string& rule, int count) {
        {
            std::lock_guard lock(usage_mu_);
            usage_.normalized[rule] += count;
        }
        if (log_) log_->write("normalized", {{"rule", rule}, {"provider", name}, {"upstream", upstream}, {"count", count}});
    };
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
        push({"user", with_operator_note(text)});
    }
    return !pending.empty();
}

void Agent::submit(const std::string& text, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    start_or_update_conversation();
    if (agent_name_.empty()) {
        // The agents and the model can change between turns, and with them what task may use.
        std::string models = "Agents: " + task_agents_text() + ".";
        if (!task_models_text().empty()) models += " " + task_models_text();
        for (auto& s : schemas_) {
            if (s["function"]["name"] != "task") continue;
            for (const auto& base : tool_schemas()) {
                if (base["function"]["name"] == "task") s["function"]["description"] = base["function"]["description"].get<std::string>() + " " + models;
            }
        }
    }
    auto [provider, model_name] = resolve_model(providers, model);
    if (log_) {
        log_->write("user", {{"text", text}, {"provider", provider.name}, {"model", model}, {"remote", provider.remote()},
                             {"mode", mode_name(mode)}, {"origin", origin == Origin::Local ? "local" : "remote"}});
    }
    {
        Message user{"user", with_operator_note(text)};
        user.images = std::move(pending_images_);
        pending_images_.clear();
        push(std::move(user));
    }

    ChatOptions options{model_name, think};
    options.notice = [&](const std::string& t) { events.on_notice(t); };
    options.normalized = count_normalized(provider);
    options.sampling = sampling;
    // Token bans: logit_bias where the provider takes it; elsewhere text tokens become string bans (the filter
    // does that) and numeric ids are reported once.
    if (!bans.tokens.empty()) {
        if (provider.kind == "openai") {
            options.logit_bias = nlohmann::json::object();
            for (const auto& t : bans.tokens) options.logit_bias[t.is_string() ? t.get<std::string>() : std::to_string(t.get<long long>())] = -100;
        } else {
            int numeric = 0;
            for (const auto& t : bans.tokens) numeric += t.is_number_integer();
            if (numeric && !warned_token_bans_) {
                warned_token_bans_ = true;
                events.on_notice(std::to_string(numeric) + " token ban(s) by id need an OpenAI-compatible provider (logit_bias); " + provider.name + " ignores them. Text bans still apply.");
            }
        }
    }
    int ban_attempts = 0;
    int context_retries = 0;
    denials_ = 0;
    for (int step = 0; step < max_steps; ++step) {
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
        if (stuck_) {
            stuck_ = false;
            events.on_notice("the agent kept repeating the same harmless call; stopped so you can say what to do next");
            push({"user", "[stopped: you repeated the same call five times; wait for the user's instructions]"});
            return;
        }
        if (denials_ >= denials_limit) {
            events.on_notice(std::to_string(denials_) + " denials this turn; stopping so you can say what you want instead");
            push({"user", "[stopped: the user denied " + std::to_string(denials_) + " actions this turn; wait for new instructions]"});
            return;
        }
        // The context window: the last call's report, else what the provider says it is.
        size_t window = usage().last.context;
        if (!window) window = static_cast<size_t>(provider.options.value("context_window", 0));
        {
            // Two views of how full the window is: the last call's real count, and a byte estimate that also
            // sees what tool results added since (a single read can outgrow the window in one step).
            UsageReport u = usage();
            double reported = window ? static_cast<double>(u.last.input) / window : 0;
            double estimated = window ? static_cast<double>(estimated_tokens()) / window : 0;
            if (window && compaction.at > 0 && std::max(reported, estimated) >= compaction.at) {
                events.on_notice("context at " + std::to_string(static_cast<int>(100 * std::max(reported, estimated))) + "%: compacting (" + compact_auto(cancel, window) + ")");
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
            // String bans: the reply streams through a filter that cuts before a banned string shows.
            BanFilter filter(bans, ban_attempts >= bans.retries);
            if (!filter.bad_patterns().empty() && !warned_bad_patterns_) {
                warned_bad_patterns_ = true;
                for (const auto& b : filter.bad_patterns()) events.on_notice("ban pattern not in force: " + b);
            }
            std::string thinking_seen;
            auto sink = [&](std::string_view d, bool t) {
                if (t) {
                    events.on_text(d, true);
                    return;
                }
                std::string safe = filter.feed(d);
                if (!safe.empty()) events.on_text(safe, false);
                if (filter.triggered()) abort = true;
            };
            try {
                if (!prefill.empty()) {
                    // The prefill is shown and counted as the reply's start; the model gets it as an open
                    // assistant turn and continues from there. llama-server echoes the prefill at the head
                    // of its output, Anthropic sends only the continuation: a repeat is dropped either way.
                    std::vector<Message> with_prefill = messages_;
                    with_prefill.push_back({"assistant", prefill});
                    sink(prefill, false);
                    std::string head;
                    bool decided = false;
                    auto strip = [&](std::string_view d, bool t) {
                        if (t || decided) {
                            sink(d, t);
                            return;
                        }
                        head.append(d);
                        if (head.size() < prefill.size() && prefill.compare(0, head.size(), head) == 0) return;  // could still be the echo
                        decided = true;
                        if (head.rfind(prefill, 0) == 0) head.erase(0, prefill.size());
                        if (!head.empty()) sink(head, false);
                        head.clear();
                    };
                    reply = chat(provider, options, with_prefill, offered_schemas(), strip, abort);
                    if (!decided && !head.empty()) sink(head, false);  // a reply shorter than the prefill
                } else {
                    reply = chat(provider, options, messages_, offered_schemas(), sink, abort);
                }
            } catch (...) {
                abort = true;
                watcher.join();
                if (filter.triggered()) {
                    // Keep the clean part, tell the model, and ask again.
                    ++ban_attempts;
                    if (!filter.clean().empty()) push({"assistant", filter.clean()});
                    push({"system", "The reply was cut off because it started the banned phrase \"" + filter.hit() + "\". Continue from exactly where it stopped, "
                                    "without that phrase or any of these: " + [&] {
                                        std::string all;
                                        for (const auto& s : bans.strings) all += (all.empty() ? "" : ", ") + ("\"" + s + "\"");
                                        for (const auto& p : bans.patterns) all += (all.empty() ? "" : ", ") + ("anything matching /" + p + "/");
                                        return all;
                                    }() + "."});
                    events.on_notice("cut: banned phrase \"" + filter.hit() + "\"; asking again (" + std::to_string(ban_attempts) + "/" + std::to_string(bans.retries) + ")");
                    --step;  // this attempt does not count toward the step limit
                    continue;
                }
                throw;
            }
            abort = true;
            watcher.join();
            std::string tail = filter.flush();
            if (!tail.empty()) events.on_text(tail, false);
            reply.content = filter.clean();
        } catch (const Cancelled&) {
            if (!cancel.load() && deliver_now_.exchange(false)) {
                events.on_notice("delivering your message now");
                continue;  // nothing from the aborted call was kept; the loop re-asks with the mailbox drained
            }
            push({"user", "[interrupted by the user]"});
            events.on_notice("interrupted");
            return;
        } catch (const ApiError& e) {
            if (!agent_name_.empty() && is_usage_limit(e)) {
                // A subagent continues on its preset's on_limit model, once, with the conversation so far.
                auto current = preset_for_model(presets, model);
                std::string from = current ? current->name : model;
                if (!limit_from_.empty()) throw std::runtime_error(from + " hit its usage limit after " + limit_from_ + " did; the subagent stops here");
                auto next = current ? on_limit_pick(presets, *current) : std::nullopt;
                if (!next) throw std::runtime_error(from + " hit its usage limit and has no on_limit model to continue on");
                bool was_remote = provider.remote();
                limit_from_ = from;
                use_preset(*next);
                std::tie(provider, model_name) = resolve_model(providers, model);
                options.model = model_name;
                options.think = think;
                if (log_) log_->write("model", {{"from", from}, {"to", next->name}, {"model", model}, {"reason", "usage limit"}});
                events.on_notice(from + " hit its usage limit; continuing on " + next->name +
                                 (provider.remote() && !was_remote ? ", a remote model: the task and what it reads leave this machine" : ""));
                --step;
                continue;
            }
            // The server refused the request as too long: compact and try again, twice at most.
            std::string what = e.what();
            bool too_long = e.status == 400 && (what.find("context") != std::string::npos || what.find("too long") != std::string::npos || what.find("too many tokens") != std::string::npos);
            if (!too_long || context_retries >= 2) throw;
            ++context_retries;
            events.on_notice("the request was over the context window: compacting (" + compact_auto(cancel, window, context_retries == 2) + ") and trying again");
            --step;
            continue;
        }
        context_retries = 0;
        ban_attempts = 0;
        deliver_now_ = false;
        ++steps_;
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
        // A session keeps running while tripped, so the user can talk it through; a subagent has no one to talk to.
        if (!agent_name_.empty() && tripwire_state()) {
            events.on_notice("the harness is tripped; the subagent stops here");
            return;
        }
    }
    events.on_notice("stopped after " + std::to_string(max_steps) + " steps" + (agent_name_.empty() ? "; send a message to continue" : " (the agent's limit)"));
}

// One audit trail entry: the call as given and what the harness, the reviewer and the user made of it. Never the
// result text, a feedback the user typed, the answer to a question, or the reviewer's reasoning.
void Agent::audit_tool_call(const nlohmann::json& record, bool ran, bool ok, const std::string& text, AgentEvents& events) {
    auto pick = [](const nlohmann::json& from) {
        nlohmann::json out = nlohmann::json::object();
        for (const char* key : {"tool", "arguments", "decision", "reason", "approval"}) {
            if (from.contains(key)) out[key] = from[key];
        }
        out["judged_by"] = "harness";
        if (from.contains("review") && from["review"].is_object()) {
            out["review"] = {{"verdict", from["review"].value("verdict", "")}, {"model", from["review"].value("model", "")}};
            out["judged_by"] = "reviewer";
        }
        if (from.contains("approval")) out["judged_by"] = "user";
        return out;
    };
    nlohmann::json entry = pick(record);
    if (record.contains("actions")) {
        entry["actions"] = nlohmann::json::array();
        for (const auto& a : record["actions"]) {
            nlohmann::json sub = pick(a);
            sub["action"] = a.value("action", "");
            entry["actions"].push_back(sub);
        }
    }
    entry["session"] = log_ ? log_->path().stem().string() : "";
    entry["recorded"] = log_ && log_->recorded();
    entry["workspace"] = harness_.workspace().string();
    entry["ran"] = ran;
    entry["ok"] = ok;
    if (record.contains("full_output")) entry["full_output"] = true;  // that it was kept, never where or what
    if (int code; ran && std::sscanf(text.c_str(), "exit code %d", &code) == 1) entry["exit"] = code;
    try {
        append_audit_trail(std::move(entry), audit.file_mb);
    } catch (const std::exception& e) {
        events.on_notice(std::string("audit trail: ") + e.what());
    }
}

Message Agent::run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel) {
    nlohmann::json record = {{"tool", call.name}, {"arguments", call.arguments}};
    bool ran = false;
    auto result = [&](const std::string& text, bool ok) {
        events.on_tool_result(text, ok);
        if (log_) {
            record["ok"] = ok;
            record["result"] = text.size() > kMaxLoggedResult ? text.substr(0, kMaxLoggedResult) + "\n[truncated]" : text;
            log_->write("tool", record);
        }
        if (audit.enabled) audit_tool_call(record, ran, ok, text, events);
        return Message{"tool", text, {}, call.name, call.id, !ok};
    };

    std::string summary = tool_summary(call.name, call.arguments);
    events.on_tool_call(summary);
    {
        std::string named;
        for (const char* key : {"path", "from"}) {
            if (named.empty() && call.arguments.is_object() && call.arguments.contains(key) && call.arguments[key].is_string()) named = call.arguments[key];
        }
        std::string canonical = canonical_tool_name(call.name);
        events.on_tool_started(canonical.empty() ? call.name : canonical, named, summary);
    }

    if (tripwire_state()) {
        return result("BLOCKED: the harness tripwire is tripped. Nothing can run until the user unlocks it.", false);
    }
    if (call.arguments.contains("_maic_invalid_input")) {
        return result("INVALID_JSON: the tool input was not valid JSON, so nothing ran. Send the call again with valid arguments.", false);
    }

    std::string name = canonical_tool_name(call.name);
    if (name.empty() && host_tool() && snake_tool_name(call.name) == "diagnostics") name = "diagnostics";
    const LuaTool* lua = name.empty() ? find_tool(call.name) : nullptr;
    if (lua) name = lua->name;
    const ScriptTool* script = name.empty() ? find_script_tool(call.name) : nullptr;
    if (script) name = script->name;
    if (name.empty()) {
        std::string names = tool_names();
        for (const auto& t : tools_) names += ", " + t.name;
        for (const auto& t : script_tools_) names += ", " + t.name;
        return result("unknown tool '" + call.name + "'. The tools are: " + names + ". Call one of those.", false);
    }
    // Forbidden terms: the whole call, name and arguments, before anything else looks at it. This is a rule,
    // so the dumb harness enforces it exactly as the smart one does.
    if (auto term = harness_.forbidden(name + " " + call.arguments.dump())) {
        record["decision"] = "deny";
        record["reason"] = "forbidden term";
        events.on_notice("HALTED: the call contains the forbidden term \"" + *term + "\"; nothing ran");
        return result("DENIED: the call contains the forbidden term \"" + *term + "\". Do not search for, run, or write anything involving it; tell the user it is forbidden if they asked for it.", false);
    }
    if (!agent_name_.empty() && (name == "task" || name == "question" || name == "todo")) {
        record["decision"] = "deny";
        record["reason"] = "not a subagent's tool";
        return result("DENIED: a subagent has no " + name + " tool. Report to the parent in your final answer instead.", false);
    }
    if (!harness_.tool_allowed(name)) {
        record["decision"] = "deny";
        record["reason"] = "tool not in the agent's list";
        std::string allowed;
        for (const auto& t : harness_.agent_def()->tools) allowed += (allowed.empty() ? "" : ", ") + t;
        return result("DENIED: the " + agent_name_ + " agent has no " + name + " tool. Its tools are: " + allowed + ".", false);
    }
    bool harness_action = !lua && !script && name != "question" && name != "todo" && name != "task" && name != "diagnostics";
    std::vector<Action> actions;
    if (name == "diagnostics") {
        // A read of the file, or of the workspace for all of them.
        try {
            std::string path = call.arguments.is_object() ? call.arguments.value("path", "") : "";
            actions = {{Action::Kind::Read, path.empty() ? harness_.workspace() : harness_.resolve(path), "", {}, "diagnostics"}};
        } catch (const std::exception& e) {
            return result(std::string("error: ") + e.what(), false);
        }
    }
    if (harness_action) {
        try {
            actions = tool_actions(harness_, name, call.arguments);
        } catch (const std::exception& e) {
            return result(std::string("error: ") + e.what(), false);
        }
    }
    if (script) {
        // The manifest's declarations are judged before the script starts; a write glob outside the workspace is
        // refused here, not asked about.
        if (std::string bad = check_arguments(script->parameters, call.arguments); !bad.empty()) return result("error: " + bad, false);
        try {
            actions = script_tool_actions(harness_, *script);
        } catch (const std::exception& e) {
            record["decision"] = "deny";
            record["reason"] = e.what();
            return result(std::string("DENIED: ") + e.what(), false);
        }
    }

    // The same call over and over means the model is stuck, not working.
    std::string sig = name + "\x1f" + call.arguments.dump();
    repeats_ = sig == last_call_ ? repeats_ + 1 : 1;
    last_call_ = sig;
    bool harmless = std::all_of(actions.begin(), actions.end(), [&](const Action& a) { return harness_.harmless(a); });
    if (repeats_ >= repeat_trip && harmless) {
        // A confused model re-running a read or a harmless helper is not an attack: stop the turn instead.
        stuck_ = true;
        record["decision"] = "deny";
        record["reason"] = "repeated harmless call";
        return result("REFUSED: this exact call has been made " + std::to_string(repeats_) + " times in a row and it changes nothing. The turn ends here; the user will say what to do next.", false);
    }
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

    // A running command's output reaches the front end as it arrives; the model gets only the result. Its whole
    // output is kept beside the session when it outgrows that result.
    OnOutput stream_output = [&](OutputStream s, std::string_view chunk, size_t offset) { events.on_tool_output(call.id, s, chunk, offset); };
    std::optional<FullOutputWriter> keep;
    if (log_ && full_output && (name == "run_shell" || script)) keep.emplace(full_output_path(log_->path(), call.id), full_output_max_mb << 20);
    auto taps = [&](OnOutput display) {
        OutputTaps t{std::move(display), {}};
        if (keep) t.on_read = [&](OutputStream s, std::string_view bytes, size_t) { keep->add(s, bytes); };
        return t;
    };
    auto kept = [&] {
        if (!keep) return;
        if (auto j = keep->finish(log_->path().parent_path())) {
            record["full_output"] = *j;
            events.on_tool_full_output(log_->path().parent_path() / (*j)["path"].get<std::string>());
        } else if (!keep->error().empty()) {
            events.on_notice("the whole output was not kept: " + keep->error());
        }
    };

    if (name == "task") {
        ran = true;
        ToolResult r = run_task(call.arguments, origin, events, cancel, record);
        return result(r.text, r.ok);
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
        ran = true;
        std::string answer = events.question(call.arguments["question"].get<std::string>(), options);
        record["answer"] = answer;
        return result(answer.empty() ? "(the user gave no answer)" : answer, true);
    }
    if (name == "todo") {
        if (!call.arguments.contains("items") || !call.arguments["items"].is_array()) return result("error: missing array argument 'items'", false);
        ran = true;
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
        std::vector<std::filesystem::path> written;
        Authorise gate = [&](const Action& a, const std::string& s, const std::string& preview) {
            nlohmann::json sub = {{"action", s}};
            Decision d = authorise(a, name, s, preview, origin, events, sub);
            if (d.verdict == Verdict::Allow && a.kind == Action::Kind::Write) {
                save_undo_point(a.path, s + " (" + name + ")");
                written.push_back(a.path);
            }
            record["actions"].push_back(sub);
            return d;
        };
        ran = true;
        ToolResult r = run_lua_tool(*lua, call.arguments, harness_, gate, cancel, std::chrono::seconds(60), nvim_.get(), stream_output);
        for (const auto& p : written) events.on_file_written(p, name);
        return result(r.text, r.ok);
    }
    if (script) {
        record["manifest"] = (script->dir / "tool.json").string();
        record["actions"] = nlohmann::json::array();
        for (const auto& a : actions) {
            std::string s = std::string(a.kind == Action::Kind::Write ? "writes " : "reads ") + a.path.string() + " (declared by " + name + ")";
            nlohmann::json sub = {{"action", s}};
            Decision d = authorise(a, name, s, "", origin, events, sub);
            record["actions"].push_back(sub);
            if (d.verdict != Verdict::Allow) return result(d.reason, false);
        }
        // The workspace is writable only for a tool that declared writes; the mode has already allowed each of them.
        ran = true;
        // Its stdout is the result, so only stderr streams.
        ToolResult r = run_script_tool(*script, call.arguments, harness_, script->writes.empty(), cancel, taps([&](OutputStream s, std::string_view chunk, size_t offset) {
            if (s == OutputStream::Stderr) stream_output(s, chunk, offset);
        }));
        kept();
        return result(r.text, r.ok);
    }

    if (name == "diagnostics") {
        Decision d = authorise(actions[0], name, summary, "", origin, events, record);
        if (d.verdict != Verdict::Allow) return result(d.reason, false);
        ran = true;
        ToolResult r = run_diagnostics(call.arguments, actions[0]);
        return result(r.text, r.ok);
    }

    // Every path the call touches is judged before anything runs: a move out of the workspace asks on its
    // destination, a patch with one file under /etc trips before any file is written.
    std::string preview = tool_preview(harness_, name, call.arguments);
    Decision d{Verdict::Allow, ""};
    for (const auto& a : actions) {
        std::optional<std::string> proposed = a.kind == Action::Kind::Write ? tool_proposed(harness_, name, call.arguments, a.path) : std::nullopt;
        d = authorise(a, name, summary, preview, origin, events, record, std::move(proposed));
        if (d.verdict != Verdict::Allow) return result(d.reason, false);
    }
    if (name != "move_file") {
        for (const auto& a : actions) {
            if (a.kind == Action::Kind::Write) save_undo_point(a.path, summary);
        }
    }
    ran = true;
    ToolResult r = run_tool(harness_, name, call.arguments, d.read_only_sandbox, cancel, taps(stream_output));
    kept();
    if (r.ok && name == "move_file") push_undo({actions[0].path, std::nullopt, summary, actions[1].path});
    if (r.ok) {
        for (const auto& a : actions) {
            if (a.kind == Action::Kind::Write) events.on_file_written(a.path, name);
        }
    }
    if (r.ok && name == "read_file") {
        std::string extra = nested_instructions(actions[0].path);
        if (!extra.empty()) r.text += "\n" + extra;
    }
    return result(r.text, r.ok);
}

bool Agent::host_tool() const {
    return nvim_ && nvim_->connected() && !find_tool("diagnostics") && !find_script_tool("diagnostics") && harness_.tool_allowed("diagnostics");
}

nlohmann::json Agent::offered_schemas() const {
    if (!host_tool()) return schemas_;
    nlohmann::json out = schemas_;
    out.push_back(diagnostics_tool_schema());
    return out;
}

ToolResult Agent::run_diagnostics(const nlohmann::json& args, const Action& action) {
    bool whole = !args.is_object() || args.value("path", "").empty();
    try {
        nlohmann::json found = host_diagnostics(*nvim_, whole ? std::filesystem::path() : action.path);
        std::string text = format_diagnostics(found, harness_.workspace(), whole);
        if (!text.empty()) text.pop_back();
        if (text.empty()) return {true, whole ? "no diagnostics in the files open in nvim" : "no diagnostics for " + action.path.string() + " (nvim reports them only for files it has open)"};
        return {true, text};
    } catch (const std::exception& e) {
        return {false, std::string("error: nvim: ") + e.what()};
    }
}

Decision Agent::authorise(const Action& action, const std::string& tool, const std::string& summary, const std::string& preview,
                          Origin origin, AgentEvents& events, nlohmann::json& record, std::optional<std::string> proposed) {
    // The harness protects itself: its settings, its locks, its server tokens and its lock helper are not the
    // agent's to change. Under the smart harness this trips the machine lock (a request from another agent
    // that tries it is exactly what the global lock is for); the dumb harness asks.
    if (action.kind != Action::Kind::Read && touches_harness(action)) {
        if (review_with_model) {
            Decision t{Verdict::Trip, std::string(origin == Origin::Remote ? "a remote request" : "the agent") + " tried to change the harness itself"};
            record["decision"] = "trip";
            record["reason"] = t.reason;
            try {
                trip_tripwire(summary + " -- " + t.reason);
                events.on_notice("HARNESS TRIPPED: " + t.reason + ". Run `maic unlock` to continue.");
            } catch (const std::exception& e) {
                events.on_notice(std::string("tripwire could not be set: ") + e.what());
            }
            return {Verdict::Trip, "BLOCKED and the harness was tripped (" + t.reason + "). Stop and explain to the user."};
        }
    }
    Decision d = harness_.check(action, mode, origin);
    if (action.kind != Action::Kind::Read && touches_harness(action) && d.verdict == Verdict::Allow) d = {Verdict::Ask, "changes the harness's own files"};
    if (touches_trust(action) && d.verdict != Verdict::Trip) d = {Verdict::Deny, "trust is the user's alone: only they grant it, at the terminal (:trust, maic trust PATH)"};
    // A file the user's own instructions import with their approval would change their standing instructions.
    if (d.verdict != Verdict::Trip && d.verdict != Verdict::Deny && changes_approved_import(action)) {
        if (!review_with_model) d = {Verdict::Deny, "it is a file your own instructions import (an approved import); the dumb harness never lets the agent change it"};
        else if (d.verdict == Verdict::Allow) d = {Verdict::Ask, "changes a file your own instructions import (an approved import)"};
    }
    record["decision"] = verdict_name(d.verdict);
    record["reason"] = d.reason;
    bool user_allowed = false;
    // A session's "always" answers are the local user's and speak only for local requests: a remote one is asked
    // every time (docs/harness.md, rule 5), and an "always" answered for it counts once.
    if (d.verdict == Verdict::Ask && origin == Origin::Local && (action.kind != Action::Kind::Shell || is_simple_command(action.command)) &&
        always_allowed_.count(Harness::approval_key(action))) {
        d = {Verdict::Allow, "allowed earlier this session"};
        user_allowed = true;
    }
    // The second reader: only for what would otherwise run silently, and never for allow-listed commands.
    if (d.verdict == Verdict::Allow && !user_allowed && !d.trusted && action.kind != Action::Kind::Read && review_with_model) {
        nlohmann::json rr = nlohmann::json::object();
        Decision r = review(action, summary, preview, events, rr);
        rr["verdict"] = verdict_name(r.verdict);
        rr["reason"] = r.reason;
        record["review"] = rr;
        if (r.verdict != Verdict::Allow) {
            if (r.verdict == Verdict::Deny) events.on_notice("reviewer refused: " + summary + " (" + r.reason + ")");
            d = {r.verdict, "reviewer: " + r.reason, d.read_only_sandbox};
        }
    }
    if (d.verdict == Verdict::Ask) {
        std::string key = Harness::approval_key(action);
        std::string covers = origin != Origin::Local ? "this call only (a remote request is asked every time)"
                             : key.rfind("shell:", 0) == 0 ? "the program `" + key.substr(6) + "`"
                             : key.rfind("write:", 0) == 0 ? "writes to this file"
                             : "reads of this file";
        ApprovalAnswer answer = events.ask({tool, summary, d.reason, origin, covers, preview, action.path, std::move(proposed)});
        record["approval"] = approval_name(answer.choice);
        if (!answer.feedback.empty()) record["feedback"] = answer.feedback;
        switch (answer.choice) {
            case Approval::Yes:
                d.verdict = Verdict::Allow;
                break;
            case Approval::Always:
                if (origin == Origin::Local) always_allowed_.insert(key);
                else events.on_notice("\"always\" for a remote request counts for this call only: the next one is asked again");
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

ToolResult Agent::run_task(const nlohmann::json& args, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel, nlohmann::json& record) {
    if (!args.contains("agent") || !args["agent"].is_string() || !args.contains("prompt") || !args["prompt"].is_string()) {
        return {false, "error: task needs the string arguments 'agent' and 'prompt'"};
    }
    std::string wanted = args["agent"];
    const AgentDef* found = find_agent_def(agents, wanted);
    if (!found || !found->runs_as_subagent()) {
        std::string names;
        for (const auto& a : agents) {
            if (a.runs_as_subagent()) names += (names.empty() ? "" : ", ") + a.name;
        }
        return {false, "error: " + (found ? "the " + found->name + " agent is a primary agent, which task cannot run" : "no agent named '" + wanted + "'") +
                           ". The agents task can run are: " + names + "."};
    }
    AgentDef def;
    try {
        def = narrow_agent_def(*found, mode);
    } catch (const std::exception& e) {
        return {false, std::string("error: ") + e.what()};
    }
    record["agent"] = def.name;

    // The child's model, first match wins: the agent's (the user's pin), the call's `model` (one on this
    // model's subagents list), this preset's pick, this model.
    auto own = preset_for_model(presets, model);
    std::string asked = args.contains("model") && args["model"].is_string() ? args["model"].get<std::string>() : "";
    ModelPick pick;
    if (!def.model.empty()) {
        auto q = preset_for_model(presets, def.model);
        pick = {q ? q->name : "", def.model, "the " + def.name + " agent's model" + (asked.empty() ? "" : "; the model argument does not override it")};
    } else if (!asked.empty()) {
        auto q = find_preset(presets, asked);
        std::vector<ModelPreset> allowed = own ? subagent_presets(presets, *own) : std::vector<ModelPreset>{};
        if (!q || std::none_of(allowed.begin(), allowed.end(), [&](const ModelPreset& a) { return a.name == q->name; })) {
            if (allowed.empty()) return {false, "error: this session's model (" + model + ") is not a preset, so a subagent runs on it; leave out model."};
            std::string names;
            for (const auto& a : allowed) names += (names.empty() ? "" : ", ") + a.name + " (tier " + std::to_string(a.tier) + (a.limited ? ", limited" : "") + ")";
            return {false, "error: '" + asked + "' is not a model this session may hand a task to. The allowed models are: " + names + "."};
        }
        pick = {q->name, q->model, "asked for by the parent"};
    } else if (own) {
        pick = subagent_pick(presets, *own);
    } else {
        pick = {"", model, "the session's model"};
    }
    record["model"] = pick.model;
    record["model_reason"] = pick.reason;

    Agent child(harness_.workspace(), model);
    child.providers = providers;
    child.think = think;
    child.presets = presets;
    if (pick.model != model) {
        if (auto q = preset_for_model(presets, pick.model)) child.use_preset(*q);
        else child.model = pick.model;
    }
    child.model_reason_ = pick.reason;
    child.sampling = sampling;
    child.bans = bans;
    child.operator_note_in_turn = operator_note_in_turn;
    child.load_instruction_files = load_instruction_files;
    child.instruction_options_ = instruction_options_;
    child.system_prefix = system_prefix;
    child.rules = rules;
    child.compaction = compaction;
    child.review_with_model = review_with_model;
    child.audit = audit;
    child.reviewer_model = reviewer_model;
    child.small_model = small_model;
    if (review_with_model) review_budget_check(events);
    {
        // The reviewer's limits and spend are the session's: what failed stays failed, and the child gets
        // what is left of the reviewer's budget.
        std::lock_guard lock(usage_mu_);
        child.reviewer_failed_ = reviewer_failed_;
        child.reviewer_off_ = reviewer_off_;
        if (reviewer_budget_tokens > 0) child.reviewer_budget_tokens = reviewer_budget_tokens - reviewer_tokens_;
    }
    child.full_output = full_output;
    child.full_output_max_mb = full_output_max_mb;
    child.repeat_limit = repeat_limit;
    child.repeat_trip = repeat_trip;
    child.denials_limit = denials_limit;
    child.harness_.set_permission(harness_.permission());
    child.harness_.set_forbid(harness_.forbid());
    child.harness_.set_confined(harness_.confined());
    child.nvim_ = nvim_;
    child.set_agent_def(def);
    if (budget_tokens > 0) {
        // The child's tokens count against this session, so it never gets more than what is left.
        UsageReport u = usage();
        long remaining = budget_tokens - (u.total_input + u.total_output);
        if (remaining <= 0) return {false, "DENIED: the session's token budget is used up"};
        if (child.budget_tokens <= 0 || child.budget_tokens > remaining) child.budget_tokens = remaining;
    }
    child.reload_instructions();
    std::unique_ptr<SessionLog> child_log;
    if (log_) {
        child_log = std::make_unique<SessionLog>("sub", log_->path().parent_path());
        child.parent_id_ = log_->path().stem().string();
        child.set_log(child_log.get());
        record["child"] = child_log->path().string();
    }

    std::string task = args["prompt"];
    if (args.contains("context") && args["context"].is_string() && !args["context"].get<std::string>().empty()) {
        task += "\n\nContext from the parent agent:\n" + args["context"].get<std::string>();
    }
    ChildEvents child_events(events, def.name);
    events.on_tool_call("↳ " + def.name + " on " + (pick.preset.empty() ? pick.model : pick.preset) + " (" + pick.reason + ")");
    if (child.remote() && !remote()) events.on_notice(def.name + ": runs on " + child.model + ", a remote model; the task and what it reads leave this machine");
    std::string failure;
    try {
        child.submit(task, origin, child_events, cancel);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    UsageReport cu = child.usage();
    long tokens = cu.total_input + cu.total_output;
    {
        std::lock_guard lock(usage_mu_);
        std::lock_guard child_lock(child.usage_mu_);
        usage_.total_input += cu.total_input;
        usage_.total_output += cu.total_output;
        for (const auto& [rule, n] : cu.normalized) usage_.normalized[rule] += n;
        reviewer_tokens_ += child.reviewer_tokens_;
        reviewer_failed_.insert(child.reviewer_failed_.begin(), child.reviewer_failed_.end());
        if (reviewer_off_.empty()) reviewer_off_ = child.reviewer_off_;
    }
    record["steps"] = child.steps();
    record["tokens"] = tokens;
    std::string used = "(the subagent used " + std::to_string(child.steps()) + " step" + (child.steps() == 1 ? "" : "s") + ", " + std::to_string(tokens) + " tokens)";

    std::string answer;
    for (size_t i = child.messages().size(); i-- > 0;) {
        const auto& m = child.messages()[i];
        if (m.role == "assistant" && !m.content.empty()) {
            answer = m.content;
            break;
        }
    }
    if (answer.size() > kMaxDelegateResult) answer = answer.substr(0, kMaxDelegateResult) + "\n[truncated]";
    if (!failure.empty()) return {false, "error: the subagent failed: " + failure + (answer.empty() ? "" : "\nIts last words:\n" + answer) + "\n" + used};
    if (cancel.load()) return {false, "cancelled by the user " + used};
    if (tripwire_state()) return {false, "BLOCKED and the harness was tripped during the subagent's work. Stop and explain to the user. " + used};
    if (answer.empty()) return {false, "the subagent gave no final answer " + used};
    return {true, answer + "\n\n" + used};
}

const LuaTool* Agent::find_tool(const std::string& name) const {
    std::string snake = snake_tool_name(name);
    for (const auto& t : tools_) {
        if (t.name == name || t.name == snake) return &t;
    }
    return nullptr;
}

const ScriptTool* Agent::find_script_tool(const std::string& name) const {
    std::string snake = snake_tool_name(name);
    for (const auto& t : script_tools_) {
        if (t.name == name || t.name == snake) return &t;
    }
    return nullptr;
}

// Which directories are trusted, and at what tier, is the user's alone (docs/harness.md, Trust): no write to the
// record, no `maic trust`, `maic untrust` or `maic ... --trust` from the agent, a tool, or a remote request.
bool touches_trust(const Action& action) {
    if (action.kind == Action::Kind::Write) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(action.path, ec);
        return p.parent_path() == std::filesystem::weakly_canonical(state_dir(), ec) && p.filename().string().rfind("trust", 0) == 0;
    }
    if (action.kind != Action::Kind::Shell) return false;
    static const std::regex re(R"((^|[\s;&|(`])(\S*/)?maic\s+(un)?trust(\s|$)|(^|[\s;&|(`])(\S*/)?maic\s[^;&|\n]*--trust(\s|=|$)|maic/trust[.-])");
    return std::regex_search(action.command, re);
}

// A write to a target `maic trust imports` lists, or a command that is not read-only and names one.
bool changes_approved_import(const Action& action) {
    if (action.kind == Action::Kind::Read) return false;
    if (action.kind == Action::Kind::Shell && is_read_only_command(action.command)) return false;
    std::error_code ec;
    for (const auto& t : import_exception_targets()) {
        if (action.kind == Action::Kind::Shell ? action.command.find(t.string()) != std::string::npos : std::filesystem::weakly_canonical(action.path, ec) == t) return true;
    }
    return false;
}

bool Agent::touches_harness(const Action& action) const {
    if (touches_trust(action)) return true;
    std::error_code ec;
    auto under = [&](const std::filesystem::path& p, const std::filesystem::path& dir) {
        if (dir.empty()) return false;
        auto rel = std::filesystem::weakly_canonical(p, ec).lexically_relative(std::filesystem::weakly_canonical(dir, ec));
        return !rel.empty() && *rel.begin() != "..";
    };
    if (action.kind == Action::Kind::Write) {
        const auto& p = action.path;
        std::string name = p.filename().string();
        return under(p, settings_path().parent_path()) || under(p, harness_.workspace() / ".maic") || under(p, "/var/lib/maic") ||
               under(p, state_dir() / "server") || under(p, state_dir() / "run") || (name.size() > 8 && name.compare(name.size() - 8, 8, ".tripped") == 0);
    }
    if (action.kind == Action::Kind::Shell) {
        const std::string& c = action.command;
        for (const char* needle : {"maic-lock", "maic unlock", "maic trip", "/var/lib/maic", ".maic/settings", "config/maic/", ".tripped", "maic/server/tokens", "maic server token"}) {
            if (c.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

void Agent::set_instruction_options(InstructionOptions options) {
    instruction_options_ = std::move(options);
}

Agent::ReviewerInfo Agent::reviewer() const {
    std::lock_guard lock(usage_mu_);
    if (!reviewer_off_.empty()) return {{"", "", reviewer_off_}, reviewer_tokens_};
    return {reviewer_pick(presets, providers, model, reviewer_model, small_model, reviewer_failed_), reviewer_tokens_};
}

void Agent::use_preset(const ModelPreset& preset) {
    model = preset.model;
    if (preset.think >= 0) think = preset.think == 1;
    set_preset_window(providers, preset);
}

std::string Agent::task_agents_text() const {
    std::string out;
    for (const auto& a : agents) {
        if (a.runs_as_subagent()) out += (out.empty() ? "" : "; ") + a.name + (a.description.empty() ? "" : " (" + a.description + ")");
    }
    return out;
}

std::string Agent::task_models_text() const {
    auto own = preset_for_model(presets, model);
    if (!own) return "";
    ModelPick pick = subagent_pick(presets, *own);
    auto tier = [](const ModelPreset& p) { return "tier " + std::to_string(p.tier) + (p.limited ? ", limited" : ""); };
    std::string list;
    for (const auto& p : subagent_presets(presets, *own)) {
        if (p.name == pick.preset) list = p.name + " (" + tier(p) + "; the default: " + (p.name == own->name ? "your own model" : pick.reason) + ")" + list;
        else list += ", " + p.name + " (" + tier(p) + ")";
    }
    return "Its `model` may name one of these presets, the default first: " + list +
           ". Leave it out for the default. Choose a lower tier for wide reads, searches and mechanical work, and a higher one only for a "
           "hard reasoning subtask.";
}

void Agent::review_budget_check(AgentEvents& events) {
    std::string off;
    {
        std::lock_guard lock(usage_mu_);
        if (!reviewer_off_.empty() || reviewer_budget_tokens <= 0 || reviewer_tokens_ < reviewer_budget_tokens) return;
        off = reviewer_off_ = "its token budget is used up (" + std::to_string(reviewer_tokens_) + " of " + std::to_string(reviewer_budget_tokens) + ")";
    }
    events.on_notice("reviewer: " + off + "; every action it would review is asked from now on");
}

Decision Agent::review(const Action& action, const std::string& summary, const std::string& preview, AgentEvents& events, nlohmann::json& record) {
    review_budget_check(events);
    ModelPick pick = reviewer().pick;
    if (pick.model.empty()) return {Verdict::Ask, "no reviewer: " + pick.reason};
    record["model"] = pick.model;
    record["model_reason"] = pick.reason;
    // The recent conversation, from the model's side: the last few things the user said and the agent's
    // last words, so the reviewer judges the action against what was actually asked.
    std::string recent;
    int users = 0;
    for (size_t i = messages_.size(); i-- > 0 && users < 3;) {
        const auto& m = messages_[i];
        if (m.role == "user" && m.content.rfind("[", 0) != 0) {
            recent = "User: " + (m.content.size() > 600 ? m.content.substr(0, 600) + " ..." : m.content) + "\n" + recent;
            ++users;
        }
    }
    std::string last_words;
    for (size_t i = messages_.size(); i-- > 0;) {
        if (messages_[i].role == "assistant" && !messages_[i].content.empty()) {
            last_words = messages_[i].content.size() > 600 ? messages_[i].content.substr(0, 600) + " ..." : messages_[i].content;
            break;
        }
    }
    std::string what = action.kind == Action::Kind::Shell ? "Command: " + action.command + (action.workdir.empty() ? "" : "\nIn directory: " + action.workdir.string())
                                                          : "Write to: " + action.path.string() + (preview.empty() ? "" : "\nChange:\n" + preview);
    std::vector<Message> req = {
        {"system", "You review one action a coding agent is about to take on the user's machine, before it runs. The user lets the agent act "
                   "without asking in this mode; you are the second reader who checks the action against the conversation. Answer with exactly one "
                   "line: ALLOW, ASK or DENY, then a colon and one short reason. ALLOW when the action plainly serves what the user asked and stays "
                   "within it. ASK when it is surprising, touches something the user did not mention, removes or overwrites broadly, or you are "
                   "unsure. DENY when it is destructive, sends data out, or works against what the user said."},
        {"user", "Workspace: " + harness_.workspace().string() + "\nMode: " + std::string(mode_name(mode)) + "\n\nRecent conversation:\n" + recent +
                     (last_words.empty() ? "" : "Agent's last words: " + last_words + "\n") + "\nThe action: " + summary + "\n" + what + "\n\nYour one line:"},
    };
    try {
        std::string reviewer = pick.model;
        if (reviewer_model.empty() && reviewer == model) {
            // The side llama server, when it is up, reviews with the same model so the main server keeps its
            // model resident; otherwise the session's model reviews itself.
            auto [main_provider, main_name] = resolve_model(providers, model);
            if (main_provider.name == "llamacpp") {
                for (const auto& p : providers) {
                    if (p.name == "llamacpp-2" && server_answers(p)) reviewer = "llamacpp-2/" + main_name;
                }
            }
        }
        auto [provider, name] = resolve_model(providers, reviewer);
        ChatOptions opt{name, false};
        opt.retries = 1;
        opt.normalized = count_normalized(provider);
        std::atomic<bool> no{false};
        Message reply = chat(provider, opt, req, nlohmann::json::array(), [](std::string_view, bool) {}, no);
        {
            std::lock_guard lock(usage_mu_);
            usage_.total_input += reply.usage.input;
            usage_.total_output += reply.usage.output;
            reviewer_tokens_ += reply.usage.input + reply.usage.output;
        }
        std::string t = reply.content;
        if (auto p = t.find("</think>"); p != std::string::npos) t = t.substr(p + 8);
        size_t start = t.find_first_not_of(" \n\t*");
        if (start == std::string::npos) return {Verdict::Ask, "the reviewer gave no verdict"};
        t = t.substr(start);
        std::string word;
        for (char c : t) {
            if (!std::isalpha(static_cast<unsigned char>(c))) break;
            word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        std::string reason = t.substr(word.size());
        while (!reason.empty() && (reason.front() == ':' || reason.front() == ' ' || reason.front() == '*')) reason.erase(0, 1);
        if (auto nl = reason.find('\n'); nl != std::string::npos) reason = reason.substr(0, nl);
        if (reason.size() > 200) reason = reason.substr(0, 200) + " ...";
        if (word == "ALLOW") return {Verdict::Allow, reason};
        if (word == "DENY") return {Verdict::Deny, reason.empty() ? "the reviewer refused it" : reason};
        if (word == "ASK") return {Verdict::Ask, reason.empty() ? "the reviewer wants you to decide" : reason};
        return {Verdict::Ask, "the reviewer gave no clear verdict"};
    } catch (const ApiError& e) {
        if (!is_usage_limit(e)) return {Verdict::Ask, std::string("the reviewer could not answer (") + e.what() + ")"};
        // Never again this session: the next review goes to a cheaper model, or none is left and every action
        // it would review is asked.
        ModelPick next;
        {
            std::lock_guard lock(usage_mu_);
            reviewer_failed_.insert(pick.model);
            next = reviewer_pick(presets, providers, model, reviewer_model, small_model, reviewer_failed_);
        }
        std::string who = pick.preset.empty() ? pick.model : pick.preset;
        events.on_notice(next.model.empty() ? "reviewer: " + next.reason + "; every action it would review is asked for the rest of the session"
                                            : "reviewer: " + who + " hit its usage limit; reviewing on " + (next.preset.empty() ? next.model : next.preset));
        return {Verdict::Ask, "the reviewer hit its usage limit"};
    } catch (const std::exception& e) {
        return {Verdict::Ask, std::string("the reviewer could not answer (") + e.what() + ")"};
    }
}

void Agent::push_undo(UndoPoint u) {
    undo_.push_back(std::move(u));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

void Agent::save_undo_point(const std::filesystem::path& path, const std::string& summary) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return;  // a directory's contents are not kept
    UndoPoint u{path, std::nullopt, summary};
    std::ifstream in(path, std::ios::binary);
    if (in) u.before = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    push_undo(std::move(u));
}

std::string Agent::undo(size_t count) {
    std::string report;
    for (size_t i = 0; i < count && !undo_.empty(); ++i) {
        UndoPoint u = std::move(undo_.back());
        undo_.pop_back();
        std::error_code ec;
        if (!u.moved_to.empty()) {
            ec = move_path(u.moved_to, u.path);
            report += ec ? "could not move " + u.moved_to.string() + " back to " + u.path.string() + ": " + ec.message() + "\n"
                         : "moved " + u.moved_to.string() + " back to " + u.path.string() + " (undid: " + u.summary + ")\n";
        } else if (u.before) {
            std::filesystem::create_directories(u.path.parent_path(), ec);
            std::ofstream out(u.path, std::ios::binary | std::ios::trunc);
            out << *u.before;
            report += "restored " + u.path.string() + " (undid: " + u.summary + ")\n";
        } else {
            std::filesystem::remove_all(u.path, ec);
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
    std::set<std::filesystem::path> seen = attached_instructions_;
    for (const auto& f : instructions_) seen.insert(std::filesystem::weakly_canonical(f.path, ec));
    std::string out;
    for (const auto& f : maic::nested_instructions(harness_.workspace(), file, instruction_options_, nested_allowed_, seen)) {
        attached_instructions_.insert(std::filesystem::weakly_canonical(f.path, ec));
        std::string from = f.imported_by.empty() ? "" : ", imported by " + f.imported_by.string();
        out += "[MAIC system note: standing instructions from " + f.path.string() + from + ". They apply to the files under " +
               (f.imported_by.empty() ? f.path : f.imported_by).parent_path().string() + " and take precedence over the earlier ones there; follow them]\n" + f.text + "\n";
    }
    return out;
}

}  // namespace maic
