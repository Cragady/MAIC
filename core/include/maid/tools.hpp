#pragma once

#include "maid/harness.hpp"
#include "maid/sandbox.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace maid {

// Tool schemas in OpenAI's function-calling format.
const nlohmann::json& tool_schemas();

// What a call would do, for the harness to judge: one action for most tools, one per path for move_file
// (both ends are writes), copy_file (a read, then a write) and apply_patch (a write per file). Every one
// must be allowed before the tool runs. Throws on malformed arguments, including a patch that does not parse.
std::vector<Action> tool_actions(const Harness& harness, const std::string& name, const nlohmann::json& args);

// One-line description for the transcript and approval prompts.
std::string tool_summary(const std::string& name, const nlohmann::json& args);

// For the approval prompt: what a write would change, as - / + lines (capped); for a delete, what goes; for a
// patch, the patch. "" otherwise.
std::string tool_preview(const Harness& harness, const std::string& name, const nlohmann::json& args);

// For the approval prompt's diff in a host nvim: what `path` would hold after a write_file, edit_file,
// multi_edit or apply_patch call. nullopt for any other tool, a deletion, or a call that would fail.
std::optional<std::string> tool_proposed(const Harness& harness, const std::string& name, const nlohmann::json& args, const std::filesystem::path& path);

// Removed and added lines between two texts (- / + lines), the common head and tail left out, at most `cap` lines.
std::string change_lines(const std::string& before, const std::string& after, size_t cap);

// Rename, or copy and remove when the two paths are on different filesystems. Also how undo reverses a move.
std::error_code move_path(const std::filesystem::path& from, const std::filesystem::path& to);

// "Read_File", "readFile" or "read-file" -> "read_file"; "" when it is not a built-in.
std::string canonical_tool_name(const std::string& name);
std::string snake_tool_name(const std::string& name);  // the same spelling fix, for any name
std::string tool_names();  // "read_file, list_dir, ..." for error messages

struct ToolResult {
    bool ok;
    std::string text;
};

// Runs an already-approved call. `read_only_sandbox` mounts the workspace read-only for run_shell, and `taps`
// get copies of its output (run_sandboxed); no other tool streams.
ToolResult run_tool(const Harness& harness, const std::string& name, const nlohmann::json& args,
                    bool read_only_sandbox, const std::atomic<bool>& cancel, const OutputTaps& taps = {});

}  // namespace maid
