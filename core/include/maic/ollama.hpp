#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

struct ToolCall {
    std::string name;
    nlohmann::json arguments;
};

struct Message {
    std::string role;  // system, user, assistant, tool
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_name;  // for role == "tool"
};

struct ChatOptions {
    std::string model;
    bool think = false;
    int num_ctx = 16384;
};

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

// Receives streamed text as it arrives; `thinking` marks the model's reasoning rather than its answer.
using TextSink = std::function<void(std::string_view delta, bool thinking)>;

// One streamed assistant turn from the local Ollama server. Throws Cancelled if `cancel` is set mid-stream.
Message ollama_chat(const ChatOptions& options, const std::vector<Message>& messages, const nlohmann::json& tools,
                    const TextSink& on_text, const std::atomic<bool>& cancel);

}  // namespace maic
