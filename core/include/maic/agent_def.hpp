#pragma once

#include "maic/harness.hpp"

#include <string>
#include <vector>

namespace maic {

// Who may run an agent: the session itself (primary), the task tool (subagent), or both (all). opencode
// calls this `mode`; MAIC says `role` because `mode` on the same definition is already the harness mode.
enum class Role { Primary, Subagent, All };
const char* role_name(Role r);
std::optional<Role> parse_role(const std::string& s);

// An agent: a named narrowing of what an agent may do (opencode's agents, by name and meaning). A session
// runs with everything its mode allows; the task tool runs a subagent under one of these, which can only take
// away: its mode is capped by the session's, its writes stay under write_paths, its tools are a subset, and
// its budget is its own. Built in, as in opencode: build, plan, general, explore. `agents` in settings adds
// agents or narrows the built-in ones (docs/settings.md).
struct AgentDef {
    std::string name;
    Mode mode = Mode::Auto;                // the most the agent allows; the session's own mode caps it
    Role role = Role::All;
    std::string description;               // what it is for, shown to the model with the task tool
    std::vector<std::string> write_paths;  // globs over the path relative to the workspace; empty = the whole workspace
    bool read_outside = true;              // may read outside the workspace (as the mode allows)
    bool network = false;                  // always false for now: no agent has the network
    long budget_tokens = 0;                // input + output over the subagent's run; 0 = the session's
    int max_steps = 40;                    // model calls per task
    std::vector<std::string> tools;        // allow-list of tool names; empty = all
    bool reviewer = true;                  // the smart harness reads its commands and writes (when the session's is on)
    std::string model;                     // "" = chosen by the session's preset (docs/settings.md)

    // An agent with no write tool on its list changes nothing: writes and commands that could write are denied.
    bool read_only() const;
    bool allows_tool(const std::string& name) const;
    bool allows_write(const std::filesystem::path& relative) const;  // against write_paths; true when the list is empty
    bool runs_as_subagent() const { return role != Role::Primary; }
};

std::vector<AgentDef> default_agent_defs();
// By name; the names before opencode's (orchestrator, builder, scout, reviewer) find build, general, explore, plan.
const AgentDef* find_agent_def(const std::vector<AgentDef>& agents, const std::string& name);
// The current name for an older one ("scout" -> "explore"); any other name as given.
std::string agent_def_name(const std::string& name);

// The agent as a session in `session_mode` may run it: the mode clamped to the session's. Throws when the
// agent asks for something no session has (the network).
AgentDef narrow_agent_def(const AgentDef& agent, Mode session_mode);

// The tools that change files; an agent listing none of them is read-only.
bool is_write_tool(const std::string& name);

}  // namespace maic
