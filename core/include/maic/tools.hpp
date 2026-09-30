#pragma once

#include "maic/harness.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

namespace maic {

// Tool schemas in Ollama's function-calling format.
const nlohmann::json& tool_schemas();

// What a call would do, for the harness to judge. Throws on malformed arguments.
Action tool_action(const Harness& harness, const std::string& name, const nlohmann::json& args);

// One-line description for the transcript and approval prompts.
std::string tool_summary(const std::string& name, const nlohmann::json& args);

struct ToolResult {
    bool ok;
    std::string text;
};

// Runs an already-approved call. `read_only_sandbox` mounts the workspace read-only for run_shell.
ToolResult run_tool(const Harness& harness, const std::string& name, const nlohmann::json& args,
                    bool read_only_sandbox, const std::atomic<bool>& cancel);

}  // namespace maic
