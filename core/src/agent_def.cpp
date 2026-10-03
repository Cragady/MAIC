#include "maid/agent_def.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <stdexcept>

namespace maid {

const char* role_name(Role r) {
    switch (r) {
        case Role::Primary: return "primary";
        case Role::Subagent: return "subagent";
        case Role::All: return "all";
    }
    return "?";
}

std::optional<Role> parse_role(const std::string& s) {
    if (s == "primary") return Role::Primary;
    if (s == "subagent") return Role::Subagent;
    if (s == "all") return Role::All;
    return std::nullopt;
}

bool is_write_tool(const std::string& name) {
    static const char* const writers[] = {"write_file", "edit_file", "multi_edit", "apply_patch", "move_file", "copy_file", "delete_file", "make_dir"};
    return std::find(std::begin(writers), std::end(writers), name) != std::end(writers);
}

bool AgentDef::read_only() const {
    if (mode == Mode::Plan) return true;
    if (tools.empty()) return false;
    return std::none_of(tools.begin(), tools.end(), is_write_tool);
}

bool AgentDef::allows_tool(const std::string& name) const {
    return tools.empty() || std::find(tools.begin(), tools.end(), name) != tools.end();
}

bool AgentDef::allows_write(const std::filesystem::path& relative) const {
    if (write_paths.empty()) return true;
    std::string rel = relative.generic_string();
    for (const auto& pattern : write_paths) {
        std::string p = pattern;
        while (!p.empty() && p.back() == '/') p.pop_back();
        if (p.empty()) continue;
        if (fnmatch(p.c_str(), rel.c_str(), 0) == 0) return true;
        if (rel.size() > p.size() && rel.compare(0, p.size(), p) == 0 && rel[p.size()] == '/') return true;  // a directory name covers what is under it
    }
    return false;
}

std::vector<AgentDef> default_agent_defs() {
    // opencode's four, with its descriptions adapted.
    std::vector<AgentDef> out;
    out.push_back({"build", Mode::Auto, Role::Primary, "The default agent. Executes tools based on configured permissions."});
    AgentDef plan{"plan", Mode::Plan, Role::All, "Plan mode. Disallows all edit tools: reads inside the workspace only, for a review of a change against what was asked."};
    plan.budget_tokens = 50000;
    plan.read_outside = false;
    plan.tools = {"read_file", "list_dir", "glob", "search_files", "diagnostics"};
    plan.reviewer = false;
    out.push_back(plan);
    out.push_back({"general", Mode::Edit, Role::Subagent, "General-purpose agent for researching complex questions and executing multi-step tasks; edits inside the workspace."});
    AgentDef explore{"explore", Mode::AutoRead, Role::Subagent,
                     "Fast agent specialized for exploring codebases: finds files by pattern, searches code for keywords and answers questions about "
                     "the codebase with reads and read-only commands. Ask for a short report with paths and line numbers."};
    explore.budget_tokens = 50000;
    explore.tools = {"read_file", "list_dir", "glob", "search_files", "run_shell", "diagnostics"};
    explore.reviewer = false;
    out.push_back(explore);
    return out;
}

std::string agent_def_name(const std::string& name) {
    if (name == "orchestrator") return "build";
    if (name == "builder") return "general";
    if (name == "scout") return "explore";
    if (name == "reviewer") return "plan";
    return name;
}

const AgentDef* find_agent_def(const std::vector<AgentDef>& agents, const std::string& name) {
    std::string n = agent_def_name(name);
    for (const auto& a : agents) {
        if (a.name == n) return &a;
    }
    return nullptr;
}

AgentDef narrow_agent_def(const AgentDef& agent, Mode session_mode) {
    if (agent.network) throw std::runtime_error("agent " + agent.name + " asks for the network, which no session has");
    AgentDef a = agent;
    a.mode = narrower_mode(agent.mode, session_mode);
    return a;
}

}  // namespace maid
