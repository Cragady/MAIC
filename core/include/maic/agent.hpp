#pragma once

#include "maic/harness.hpp"
#include "maic/instructions.hpp"
#include "maic/llm.hpp"
#include "maic/session.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
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
};

// What a front end (the CLI now, the server later) implements to follow and steer a turn.
// Every method is called from the agent's worker thread.
class AgentEvents {
public:
    virtual ~AgentEvents() = default;
    virtual void on_text(std::string_view delta, bool thinking) = 0;
    virtual void on_tool_call(const std::string& summary) = 0;
    virtual void on_tool_result(const std::string& text, bool ok) = 0;
    virtual void on_notice(const std::string& text) = 0;
    // Blocks until the user answers.
    virtual Approval ask(const ApprovalRequest& request) = 0;
};

class Agent {
public:
    Agent(std::filesystem::path workspace, std::string model);

    // Runs the user's message to completion: model replies, tool calls, approvals. Throws on transport errors.
    void submit(const std::string& text, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel);
    void clear();

    // Something the user did outside the agent (e.g. a `!command` and its output) that the model should know
    // about on its next turn. Call only while idle.
    void add_context(const std::string& text);

    // Messages typed while a turn is running. They reach the model at its next call in the current turn;
    // deliver_now() also aborts the model call in progress so the next one starts at once. Thread-safe.
    void post_message(const std::string& text);
    void deliver_now();
    size_t queued() const;
    std::vector<std::string> take_queued();

    // Every turn is also written here when set.
    void set_log(SessionLog* log);

    // Continues an earlier session: its messages become the history, and the model is told it resumed.
    void restore(std::vector<Message> messages);

    std::atomic<Mode> mode{Mode::Manual};
    std::string model;  // "<provider>/<model>", or a bare name for the first provider (Ollama)
    bool think = false;
    std::vector<Provider> providers = default_providers();

    // True when the current model runs off this machine: prompts and tool output leave it.
    bool remote() const { return resolve_model(providers, model).first.remote(); }

    // Token accounting: the last model call and this session's running totals. Thread-safe.
    struct UsageReport {
        Usage last;
        long total_input = 0;
        long total_output = 0;
        int calls = 0;
    };
    UsageReport usage() const;

    const Harness& harness() const { return harness_; }

    // MAIC.md / AGENTS.md files in effect. Re-read from disk at the start of every turn.
    const std::vector<InstructionFile>& instructions() const { return instructions_; }
    void reload_instructions() { instructions_ = load_instructions(harness_.workspace()); }

private:
    Message run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel);
    std::string system_prompt() const;
    std::string instructions_text() const;
    // History is append-only (newer Anthropic models reject edited history): mode and instruction changes after
    // the first turn are appended as system messages instead of rewriting the system prompt.
    void start_or_update_conversation();
    void push(Message m);  // appends to the history and the session log

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
    std::atomic<bool> deliver_now_{false};
    bool drain_mailbox();  // appends queued messages as user turns; true if any
};

}  // namespace maic
