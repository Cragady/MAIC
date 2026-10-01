#include "maic/profile.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <stdexcept>

namespace maic {

bool is_write_tool(const std::string& name) {
    static const char* const writers[] = {"write_file", "edit_file", "multi_edit", "apply_patch", "move_file", "copy_file", "delete_file", "make_dir"};
    return std::find(std::begin(writers), std::end(writers), name) != std::end(writers);
}

bool Profile::read_only() const {
    if (mode == Mode::Plan) return true;
    if (tools.empty()) return false;
    return std::none_of(tools.begin(), tools.end(), is_write_tool);
}

bool Profile::allows_tool(const std::string& name) const {
    return tools.empty() || std::find(tools.begin(), tools.end(), name) != tools.end();
}

bool Profile::allows_write(const std::filesystem::path& relative) const {
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

std::vector<Profile> default_profiles() {
    std::vector<Profile> out;
    out.push_back({"orchestrator"});
    out.push_back({"builder", Mode::Edit});
    Profile scout{"scout", Mode::AutoRead};
    scout.budget_tokens = 50000;
    scout.tools = {"read_file", "list_dir", "glob", "search_files", "run_shell"};
    scout.reviewer = false;
    out.push_back(scout);
    Profile reviewer{"reviewer", Mode::Plan};
    reviewer.budget_tokens = 50000;
    reviewer.read_outside = false;
    reviewer.tools = {"read_file", "list_dir", "glob", "search_files"};
    reviewer.reviewer = false;
    out.push_back(reviewer);
    return out;
}

const Profile* find_profile(const std::vector<Profile>& profiles, const std::string& name) {
    for (const auto& p : profiles) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

Profile narrow_profile(const Profile& profile, Mode session_mode) {
    if (profile.network) throw std::runtime_error("profile " + profile.name + " asks for the network, which no session has");
    Profile p = profile;
    p.mode = narrower_mode(profile.mode, session_mode);
    return p;
}

}  // namespace maic
