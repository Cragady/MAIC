#pragma once

#include "maic/harness.hpp"
#include "maic/ollama.hpp"

#include <atomic>
#include <filesystem>
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

    std::atomic<Mode> mode{Mode::Manual};
    std::string model;
    bool think = false;

    const Harness& harness() const { return harness_; }

private:
    Message run_tool_call(const ToolCall& call, Origin origin, AgentEvents& events, const std::atomic<bool>& cancel);
    std::string system_prompt() const;

    Harness harness_;
    std::vector<Message> messages_;
    std::set<std::string> always_allowed_;
};

}  // namespace maic
