#pragma once

#include "maic/harness.hpp"
#include "maic/tools.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace maic {

// A tool the user wrote in any language: <workspace>/.maic/tools/<dir>/tool.json or $XDG_CONFIG_HOME/maic/tools/<dir>/tool.json
// beside its script. The manifest holds name, description, parameters (a JSON schema), run (argv: run[0] on PATH or a
// file in the directory), timeout_s (default 60), network (false; true is refused for now) and reads / writes (globs the
// harness judges before the script starts). See docs/tools.md.
struct ScriptTool {
    std::string name;
    std::string description;
    nlohmann::json parameters;
    std::vector<std::string> run;  // as written, except that an argument naming a file in `dir` is made absolute
    int timeout_s = 60;
    std::vector<std::string> reads, writes;
    std::filesystem::path dir;  // holds tool.json and the script
};

struct ScriptToolSet {
    std::vector<ScriptTool> tools;
    std::vector<std::string> notices;  // manifests that were skipped, and why
};

// Workspace tools first, then the global ones. A manifest that does not parse, names a built-in, a name in `taken`
// (the Lua tools) or an earlier tool, asks for the network, or whose interpreter is not on PATH is skipped with a notice.
ScriptToolSet load_script_tools(const std::filesystem::path& workspace, const std::vector<std::string>& taken = {});

// One manifest; throws with the reason it cannot be used. What `maic tools check` reports per tool.
ScriptTool read_script_tool(const std::filesystem::path& manifest);

// "python", "sh", "perl", "node", "deno", "wasm", or the program's own name (a Go binary): what the listings show.
std::string script_tool_language(const ScriptTool& tool);

// The manifest's schema itself, for `maic tools check`: "" when well-formed, else the problem.
std::string check_schema(const nlohmann::json& schema);
// The model's arguments against the schema: type, required, properties, items, enum and additionalProperties. "" when
// they fit, else what is wrong, worded for the model.
std::string check_arguments(const nlohmann::json& schema, const nlohmann::json& args);

// What the harness judges before the script starts: a Read for every `reads` glob and a Write for every `writes` glob,
// each at the glob's fixed prefix resolved against the workspace (`docs/**` is docs, `*` is the workspace itself).
// Throws when a write glob reaches outside the workspace: that is refused outright, never asked.
std::vector<Action> script_tool_actions(const Harness& harness, const ScriptTool& tool);

// Runs the script in the command sandbox, the arguments as JSON on its stdin, the workspace as its working directory
// and writable only when `read_only` is false. stdout is the result, capped like command output; a non-zero exit fails
// the call with stderr appended; the process group is killed at timeout_s.
ToolResult run_script_tool(const ScriptTool& tool, const nlohmann::json& args, const Harness& harness, bool read_only, const std::atomic<bool>& cancel);

// `maic tools new`: writes <dir>/tool.json and a stub that echoes its arguments; `lang` is python, sh, perl or node.
// Returns the files written. Throws when the directory exists, the name is illegal or the language unknown.
std::vector<std::filesystem::path> scaffold_script_tool(const std::filesystem::path& dir, const std::string& name, const std::string& lang);

}  // namespace maic
