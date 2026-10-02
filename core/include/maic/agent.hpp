#pragma once

#include "maic/audit_trail.hpp"
#include "maic/bans.hpp"
#include "maic/harness.hpp"
#include "maic/instructions.hpp"
#include "maic/llm.hpp"
#include "maic/lua_tools.hpp"
#include "maic/agent_def.hpp"
#include "maic/nvim_host.hpp"
#include "maic/sandbox.hpp"
#include "maic/script_tools.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

enum class Approval { Yes, No, Always, Trip };

struct ApprovalRequest {
    std::string tool;
    std::string summary;
    std::string reason;
    Origin origin;
    std::string always_covers;  // what "always allow" would cover: "git", "this file", ...
    std::string preview;        // for writes: the lines that would change
    std::filesystem::path path;  // the file or directory the action is about; empty for a command
    std::optional<std::string> proposed;  // for a write_file, edit_file, multi_edit or apply_patch: the file's content after it
};

// The user's answer; `feedback` is a sentence for the model when the answer is No ("use the test config").
struct ApprovalAnswer {
    Approval choice = Approval::No;
    std::string feedback;
};

// One line of the model's plan (the todo tool).
struct TodoItem {
    std::string text;
    bool done = false;
};

// What a front end (the CLI now, the server later) implements to follow and steer a turn.
// Every method is called from the agent's worker thread, except on_tool_output (below).
class AgentEvents {
public:
    virtual ~AgentEvents() = default;
    virtual void on_text(std::string_view delta, bool thinking) = 0;
    virtual void on_tool_call(const std::string& summary) = 0;
    virtual void on_tool_result(const std::string& text, bool ok) = 0;
    virtual void on_notice(const std::string& text) = 0;
    // Blocks until the user answers.
    virtual ApprovalAnswer ask(const ApprovalRequest& request) = 0;
    // The question tool: blocks until the user answers; "" when they gave no answer. Options may be empty.
    virtual std::string question(const std::string& text, const std::vector<std::string>& options) {
        (void)text;
        (void)options;
        return "";
    }
    // The model replaced its plan.
    virtual void on_todo(const std::vector<TodoItem>& items) { (void)items; }
    // Right after on_tool_call: the tool by name and the path it names as the model gave it ("" for none).
    virtual void on_tool_started(const std::string& tool, const std::string& path, const std::string& summary) {
        (void)tool, (void)path, (void)summary;
    }
    // A tool call that changed this file finished without error (a write, an edit, a patch, a move's two ends,
    // a delete; a Lua tool's maic.write).
    virtual void on_file_written(const std::filesystem::path& path, const std::string& tool) { (void)path, (void)tool; }
    // A running command's output, for display only (the model gets the result): run_shell's output, a script
    // tool's stderr, what a Lua tool's maic.shell prints. `chunk` starts at byte `offset` of the call's stream; a
    // gap between two chunks is output dropped because the front end fell behind. Called between on_tool_call
    // and on_tool_result, in order, from the sandbox's delivery thread while the worker waits for the tool. A
    // subagent's calls arrive as "<agent>:<call id>".
    virtual void on_tool_output(const std::string& call_id, OutputStream stream, std::string_view chunk, size_t offset) {
        (void)call_id, (void)stream, (void)chunk, (void)offset;
    }
    // Right before on_tool_result, when the call's whole output outgrew the model's cap and was kept beside the
    // session (`file`, its .out; Agent::full_output). Display only: the result is what the model got.
    virtual void on_tool_full_output(const std::filesystem::path& file) { (void)file; }
};

// An action that would change which directories are trusted or at what tier (a write to <state>/trust*, a
// `maic trust` / `maic untrust` / `maic ... --trust` command). The agent's authorise step never lets one run:
// the smart harness trips on it, the dumb one refuses it.
bool touches_trust(const Action& action);
// Whether the action writes a file the user's own instructions import with their approval (maic trust imports).
bool changes_approved_import(const Action& action);

class Agent {
public:
    Agent(std::filesystem::path workspace, std::string model);

    // Runs the user's message to completion: model replies, tool calls, approvals. Throws on transport errors.
    void submit(const std::string& text, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel);
    // Pictures for the next user turn (the user's own attachments: --image, :image, a dropped file).
    void attach_image(const std::filesystem::path& file);  // throws when it cannot be read
    std::vector<std::string> pending_images() const;       // their names
    void clear_pending_images();
    void clear();

    // Something the user did outside the agent (e.g. a `!command` and its output) that the model should know
    // about on its next turn. Call only while idle.
    void add_context(const std::string& text);

    // A file's contents as context, labelled with its path ("-" reads stdin). Refuses binary files. Returns a
    // one-line description for the transcript.
    std::string add_context_file(const std::filesystem::path& path);

    // Messages typed while a turn is running. They reach the model at its next call in the current turn;
    // deliver_now() also aborts the model call in progress so the next one starts at once. Thread-safe.
    void post_message(const std::string& text);
    void deliver_now();
    size_t queued() const;
    std::vector<std::string> take_queued();

    // Every turn is also written here when set.
    void set_log(SessionLog* log);

    // A command's whole output (run_shell, a script tool), kept beside the session file when it outgrows what the
    // model is given, at most full_output_max_mb of it (FullOutputWriter); settings `full_output` and
    // `full_output_max_mb`. Only with a log.
    bool full_output = true;
    size_t full_output_max_mb = 64;

    // Continues an earlier session: its messages become the history, and the model is told it resumed.
    void restore(std::vector<Message> messages);

    std::atomic<Mode> mode{Mode::Manual};
    std::string model;  // "<provider>/<model>", or a bare name for the first provider (llamacpp)
    bool think = false;
    std::vector<Provider> providers = default_providers();

    // True when the current model runs off this machine: prompts and tool output leave it.
    bool remote() const { return resolve_model(providers, model).first.remote(); }

    // Compaction. Micaiah's flow: tool results are what fill a context, dialog is cheap, so old tool results go
    // first (Prune) and the dialog stays intact; only when that is not enough are the oldest turns summarised
    // (Head), which moves the compaction point forward. All is the traditional whole-history summary.
    enum class Compaction { Prune, Head, All };
    struct CompactionSettings {
        double at = 0.75;         // auto-compact when the last call used this share of the context window
        int keep_results = 4;     // the most recent tool results always stay intact
        double head_fraction = 0.5;  // Head summarises this share of the turns, oldest first
    };
    CompactionSettings compaction;

    // Runs one stage now (Prune returns how many results it stubbed; Head/All call the model with no tools).
    // Returns a one-line report. Call only while idle, or from within submit.
    std::string compact(Compaction stage, const std::atomic<bool>& cancel);
    // Her default: Prune, then Head only if the estimate says pruning was not enough.
    // `window` is the context size in tokens when known (0: use the last call's report); `force_head` skips
    // the estimate and summarises after pruning, for when the server already refused the request.
    std::string compact_auto(const std::atomic<bool>& cancel, size_t window = 0, bool force_head = false);
    // Rough token count of the history from its bytes (3.5 bytes per token, on the safe side).
    size_t estimated_tokens() const { return history_bytes() * 2 / 7; }

    const std::vector<Message>& messages() const { return messages_; }

    // Undo points: the previous content of each file a tool changed this session, newest last. A move is one
    // point that moves the file back; a deleted directory has no point (only files are kept).
    struct UndoPoint {
        std::filesystem::path path;
        std::optional<std::string> before;  // nullopt: the file did not exist
        std::string summary;
        std::filesystem::path moved_to;  // set: undo moves it back from here to `path`
    };
    const std::vector<UndoPoint>& undo_points() const { return undo_; }
    std::string undo(size_t count = 1);  // restores the newest `count` points; returns what was restored

    // Session token budget (input + output over all calls); 0 = unlimited. When reached, the turn stops and
    // later turns refuse until it is raised.
    long budget_tokens = 0;

    // The same call this many times in a row is refused (the model is stuck); at trip_repeats the lock trips.
    int repeat_limit = 3;
    int repeat_trip = 5;
    // This many denials by the user in one turn end the turn.
    int denials_limit = 3;
    // Model calls per turn before the agent stops and waits for the user (an agent definition sets a subagent's).
    int max_steps = 40;
    int steps() const { return steps_; }  // model calls so far

    // The permission block and the allow list (its run_shell allow entries); from settings `permission` / `allow`.
    void set_permission(Permission p) { harness_.set_permission(std::move(p)); }
    void set_allow(const std::vector<std::string>& patterns) { harness_.set_allow(patterns); }
    void set_confined(bool on) { harness_.set_confined(on); }
    // :cd, a user command only (a remote origin is refused): the harness, relative paths and the instruction files
    // follow `dir`, the transcript gets a `workspace` record and the conversation a system note saying so.
    // Throws when the harness refuses the move (a confined session leaving its start directory).
    void set_workspace(const std::filesystem::path& dir, Origin origin);
    void set_forbid(std::vector<std::string> terms) { harness_.set_forbid(std::move(terms)); }

    // Subagents. The `task` tool runs a child Agent in this workspace as one of these agents (role subagent or all), with
    // its own transcript (kind sub) and the parent's provider, permission, forbidden terms and operator text.
    // A child's mode is its agent's capped by the parent's; it has no task, question or todo tool.
    std::vector<AgentDef> agents = default_agent_defs();
    // Model presets (settings `models`). A subagent's model is, first match wins: its agent's model, the
    // task call's `model` (one on this model's subagents list), this preset's subagent_pick, this model.
    // A subagent whose model hits its usage limit continues once on that preset's on_limit_pick.
    std::vector<ModelPreset> presets = default_presets();
    // Makes this agent a subagent running as `def` (already narrowed to the session's mode, see narrow_agent_def).
    void set_agent_def(const AgentDef& def);
    const std::string& agent_name() const { return agent_name_; }  // "" for a session

    // Which instruction files are read and how (the global settings' `instructions`; docs/instructions.md).
    void set_instruction_options(InstructionOptions options);

    // The harness's second reader. When on ("smart" harness), a model reads the recent conversation and the
    // action before any command or write that the rules would let through without asking, and answers
    // ALLOW, ASK or DENY; ASK becomes an approval prompt, and a reviewer that cannot answer means ASK. Reads
    // are never reviewed. When off ("dumb" harness) the rule list alone decides.
    // The reviewer's model is reviewer_pick's: reviewer_model (the user's pin), the preset's reviewer,
    // small_model, the default small model. One that hits its usage limit is replaced for the session by a
    // cheaper one, or the reviewer goes off; off, every action it would review is asked (fail closed). Its
    // tokens count toward budget_tokens, and past reviewer_budget_tokens it goes off the same way.
    bool review_with_model = true;
    // audit.lua (docs/audit-trail.md): when enabled, every tool call also goes to the audit trail, recorded
    // session or not. Subagents inherit it.
    AuditSettings audit;
    std::string reviewer_model;
    std::string small_model;
    long reviewer_budget_tokens = 0;  // 0 = no cap of its own
    struct ReviewerInfo {
        ModelPick pick;  // pick.model "" when the reviewer is off for the session, pick.reason says why
        long tokens = 0;
    };
    ReviewerInfo reviewer() const;

    // Things the model must not say; see bans.hpp. Changes apply from the next model call.
    Bans bans;
    // Sampler settings merged into every request (temperature, top_k, ...), from the provider's settings.
    nlohmann::json sampling;

    // Text every reply starts with, put in the model's mouth: sent as the opening of the assistant turn, so
    // the model continues it rather than being asked to comply. A prefilled turn rarely calls a tool.
    std::string prefill;

    // Whether the operator instructions also close each user turn as the model sees it (they always lead and
    // close the system prompt). Set per provider: `options.operator_note`, default on except for Anthropic.
    bool operator_note_in_turn = true;

    // Operator text placed at the very top of the system prompt, before MAIC's own briefing. Independent of
    // instruction files: use both, either, or neither.
    std::string system_prefix;
    // Standing rules, one line each; they ride with the operator text wherever it goes.
    std::vector<std::string> rules;
    // The operator text as the model sees it: system_prefix, then the rules as a list. "" when both are empty.
    std::string operator_text() const;
    // Set either; in a running conversation the new text is also appended as a system note, since the system
    // prompt itself is never rewritten (append-only history).
    void set_system_prefix(const std::string& text);
    void set_rules(std::vector<std::string> new_rules);
    // When false, no instruction file is loaded or attached, anywhere.
    bool load_instruction_files = true;

    // Token accounting: the last model call and this session's running totals. Thread-safe.
    struct UsageReport {
        Usage last;
        long total_input = 0;
        long total_output = 0;
        int calls = 0;
        std::map<std::string, int> normalized;  // adapter rules applied to what the providers sent (normalize_openai), by rule
    };
    UsageReport usage() const;

    const Harness& harness() const { return harness_; }

    // The nvim MAIC runs inside (maic.nvim). While it is connected the model has the `diagnostics` tool (a read
    // of the path or the workspace) and the Lua tools have maic.nvim.diagnostics / buffers. Set while idle;
    // subagents get the same host.
    void set_nvim_host(std::shared_ptr<NvimHost> host) { nvim_ = std::move(host); }

    // Instruction files in effect. Re-read from disk at the start of every turn, with the nested files on-demand
    // loading may attach.
    const std::vector<InstructionFile>& instructions() const { return instructions_; }
    void reload_instructions();
    // Imports of your own files waiting for your approval, as of the last reload (read it while idle).
    const std::vector<PendingImport>& pending_imports() const { return pending_imports_; }

    // The model's current plan, replaced whole by every todo call; cleared with the conversation.
    const std::vector<TodoItem>& todo() const { return todo_; }

    // User-defined tools, loaded once at construction (docs/tools.md): Lua files, then script tools from manifests,
    // and the files that were skipped.
    const std::vector<LuaTool>& tools() const { return tools_; }
    const std::vector<ScriptTool>& script_tools() const { return script_tools_; }
    const std::vector<std::string>& tool_notices() const { return tool_notices_; }

private:
    Message run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel);
    void audit_tool_call(const nlohmann::json& record, bool ran, bool ok, const std::string& text, AgentEvents& events);
    ToolResult run_task(const nlohmann::json& args, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel, nlohmann::json& record);
    // Policy, then this session's "always" answers (local requests only), then the user. Never returns Ask: a No becomes Deny with the
    // user's words, a Trip has already tripped the lock. For Deny and Trip the reason is the text the model
    // sees. Every tool action, built-in or from a Lua tool, goes through here; `record` gets the log fields.
    Decision authorise(const Action& action, const std::string& tool, const std::string& summary, const std::string& preview,
                       Origin origin, AgentEvents& events, nlohmann::json& record, std::optional<std::string> proposed = std::nullopt);
    bool host_tool() const;            // the diagnostics tool is on offer: a connected host, no user tool of that name, the agent allows it
    nlohmann::json offered_schemas() const;  // schemas_, plus diagnostics while host_tool()
    ToolResult run_diagnostics(const nlohmann::json& args, const Action& action);
    const LuaTool* find_tool(const std::string& name) const;
    const ScriptTool* find_script_tool(const std::string& name) const;
    std::string system_prompt() const;
    std::string instructions_text() const;
    std::string user_tools_text() const;
    // History is append-only (newer Anthropic models reject edited history): mode and instruction changes after
    // the first turn are appended as system messages instead of rewriting the system prompt.
    void start_or_update_conversation();
    void push(Message m);  // appends to the history and the session log
    // ChatOptions::normalized for a call to `provider`: counts the rule in usage_ and writes a `normalized` record
    // {rule, provider, upstream, count}, so a server's departure from OpenAI's shapes is never silent.
    std::function<void(const std::string&, int)> count_normalized(const Provider& provider);

    Harness harness_;
    std::vector<Message> messages_;
    std::set<std::string> always_allowed_;
    SessionLog* log_ = nullptr;
    std::vector<InstructionFile> instructions_;
    Mode prompted_mode_ = Mode::Manual;  // what the conversation was last told
    std::string prompted_instructions_;

    mutable std::mutex usage_mu_;
    UsageReport usage_;
    mutable std::mutex mailbox_mu_;
    std::deque<std::string> mailbox_;
    std::vector<ImageData> pending_images_;
    std::atomic<bool> deliver_now_{false};
    bool drain_mailbox();  // appends queued messages as user turns; true if any
    void rewrite_log();    // after compaction: a reset record and the new history, so resume sees the same thing
    size_t history_bytes() const;
    std::string summarise(size_t from, size_t to, const std::atomic<bool>& cancel);  // messages [from, to) -> summary text
    bool touches_harness(const Action& action) const;
    void save_undo_point(const std::filesystem::path& path, const std::string& summary);  // a file's content before a write; nothing for a directory
    void push_undo(UndoPoint u);
    Decision review(const Action& action, const std::string& summary, const std::string& preview, AgentEvents& events, nlohmann::json& record);
    void review_budget_check(AgentEvents& events);  // turns the reviewer off, with one notice, once its budget is spent
    void use_preset(const ModelPreset& preset);  // its model, thinking and context window, as apply_preset sets them
    std::string task_agents_text() const;    // the agents task can run, with what each is for
    std::string task_models_text() const;    // the presets task may run a subagent on, for the parent model
    std::string with_operator_note(const std::string& text) const;
    std::string nested_instructions(const std::filesystem::path& file);  // instruction files between the workspace and `file`, each once

    std::vector<UndoPoint> undo_;
    bool warned_token_bans_ = false;
    bool warned_bad_patterns_ = false;
    std::vector<TodoItem> todo_;
    std::vector<LuaTool> tools_;
    std::vector<ScriptTool> script_tools_;
    std::vector<std::string> tool_notices_;
    nlohmann::json schemas_;  // the built-ins, then the Lua tools, then the script tools
    InstructionOptions instruction_options_;
    std::vector<PendingImport> pending_imports_;
    std::set<std::filesystem::path> nested_allowed_;        // nested files trusted chain directories hash (nested_allowed)
    std::set<std::filesystem::path> attached_instructions_;  // nested files attached in this conversation
    std::string last_call_;
    int repeats_ = 0;
    int denials_ = 0;
    int steps_ = 0;
    bool stuck_ = false;  // the same harmless call kept repeating: end the turn, do not trip
    std::shared_ptr<NvimHost> nvim_;
    std::string agent_name_;  // set: this agent is a subagent
    std::string parent_id_;     // the parent's session id, for the start record
    std::string model_reason_;  // a subagent: why it runs on its model, for the start record
    std::string limit_from_;    // a subagent: the preset it left after a usage limit ("" = none yet)
    // The reviewer's state, under usage_mu_: models that hit their usage limit while reviewing, why it is off
    // ("" while on; the budget, or a parent's), and what it has spent.
    std::set<std::string> reviewer_failed_;
    std::string reviewer_off_;
    long reviewer_tokens_ = 0;
};

}  // namespace maic
