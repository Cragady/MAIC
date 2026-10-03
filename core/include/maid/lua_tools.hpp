#pragma once

#include "maid/harness.hpp"
#include "maid/tools.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace maid {

class NvimHost;

// A tool the user wrote in Lua: <workspace>/.maid/tools/<name>.lua or $XDG_CONFIG_HOME/maid/tools/<name>.lua,
// a chunk returning { name, description, parameters (a JSON schema as a table), run = function(args) ... end }.
// See docs/tools.md.
struct LuaTool {
    std::string name;
    std::string description;
    nlohmann::json parameters;
    std::filesystem::path file;
    std::string source;  // read once, when loaded; every call runs it again in a fresh state
};

struct LuaToolSet {
    std::vector<LuaTool> tools;
    std::vector<std::string> notices;  // files that were skipped, and why
};

std::filesystem::path global_tools_dir();  // $XDG_CONFIG_HOME/maid/tools, default ~/.config/maid/tools

// Workspace tools first, then the global ones. A file that fails to load, or names a built-in or an earlier
// tool, is skipped with a notice.
LuaToolSet load_lua_tools(const std::filesystem::path& workspace);

// The gate every maid.* call inside a tool goes through: the agent hands in the same step the built-in tools
// use. The verdict is never Ask; for Deny and Trip the reason is the message the model sees.
using Authorise = std::function<Decision(const Action& action, const std::string& summary, const std::string& preview)>;

// Runs one call in its own state: base, string, table, math and bit only; no load, loadstring, dofile,
// loadfile or require, and io, os, package, debug, ffi and jit are never opened. A `maid` table offers read,
// write, list, search, shell, json_encode and json_decode; the first five build the Action a built-in would
// and go through `authorise`, and a denial is raised as a Lua error carrying the denial text. With a connected
// `nvim` host it also offers maid.nvim.diagnostics(path?) and maid.nvim.buffers(), read-only, each authorised as
// a read (of the path, or of the workspace) and limited to the workspace. The call is aborted on cancel or
// after `timeout`; the result is capped at 64 KB. `on_output` gets what maid.shell commands print while they
// run, as one stream across the call (run_sandboxed).
ToolResult run_lua_tool(const LuaTool& tool, const nlohmann::json& args, const Harness& harness, const Authorise& authorise,
                        const std::atomic<bool>& cancel, std::chrono::seconds timeout = std::chrono::seconds(60), NvimHost* nvim = nullptr,
                        const OnOutput& on_output = {});

}  // namespace maid
