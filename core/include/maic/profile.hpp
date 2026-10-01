#pragma once

#include "maic/harness.hpp"

#include <string>
#include <vector>

namespace maic {

// A named narrowing of what an agent may do. A session runs with everything its mode allows; a subagent runs
// under a profile, which can only take away: its mode is capped by the session's, its writes stay under
// write_paths, its tools are a subset, and its budget is its own. Built in: orchestrator, builder, scout,
// reviewer. `profiles` in settings adds profiles or narrows the built-in ones (docs/settings.md).
struct Profile {
    std::string name;
    Mode mode = Mode::Auto;                // the most the profile allows; the session's own mode caps it
    std::vector<std::string> write_paths;  // globs over the path relative to the workspace; empty = the whole workspace
    bool read_outside = true;              // may read outside the workspace (as the mode allows)
    bool network = false;                  // always false for now: no profile has the network
    long budget_tokens = 0;                // input + output over the subagent's run; 0 = the session's
    int max_steps = 40;                    // model calls per task
    std::vector<std::string> tools;        // allow-list of tool names; empty = all
    bool reviewer = true;                  // the smart harness reads its commands and writes (when the session's is on)
    std::string model;                     // "" = the session's model

    // A profile with no write tool on its list changes nothing: writes and commands that could write are denied.
    bool read_only() const;
    bool allows_tool(const std::string& name) const;
    bool allows_write(const std::filesystem::path& relative) const;  // against write_paths; true when the list is empty
};

std::vector<Profile> default_profiles();
const Profile* find_profile(const std::vector<Profile>& profiles, const std::string& name);

// The profile as a session in `session_mode` may run it: the mode clamped to the session's. Throws when the
// profile asks for something no session has (the network).
Profile narrow_profile(const Profile& profile, Mode session_mode);

// The tools that change files; a profile listing none of them is read-only.
bool is_write_tool(const std::string& name);

}  // namespace maic
