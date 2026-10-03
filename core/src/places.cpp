#include "maid/places.hpp"

#include "maid/artifacts.hpp"
#include "maid/instructions.hpp"
#include "maid/paths.hpp"
#include "maid/session.hpp"
#include "maid/vendor.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace maid {

namespace fs = std::filesystem;

std::vector<Place> known_places(const Settings& settings, const fs::path& workspace, const std::vector<ServiceDef>& services,
                                const std::optional<fs::path>& session) {
    std::vector<Place> out;
    auto add = [&](std::string name, fs::path path, std::string description, bool file = false) {
        for (const auto& p : out) {
            if (p.name == name) return;
        }
        out.push_back({std::move(name), std::move(path), std::move(description), file});
    };
    add("workspace", workspace, "the directory this session works in");
    if (session) add("session", *session, "this session's transcript (JSONL)", true);
    add("sessions", sessions_dir(), "all transcripts, by home");
    add("state", state_dir(), "MAID's state: sessions, logs, vendor links, workflows, templates");
    add("config", settings_path().parent_path(), "MAID's configuration directory");
    fs::path lua = settings_path();
    lua.replace_extension(".lua");
    std::error_code ec;
    add("settings", fs::exists(lua, ec) ? lua : settings_path(), "the global settings file", true);
    add("instructions", global_instructions_path(), "the global MAID.md", true);
    add("logs", state_dir() / "logs", "service logs (maid logs NAME shows one)");
    add("root", root_dir(), "the MAID repository (services/, vendor/, tools/)");
    add("tools", root_dir() / "tools", "MAID's helper scripts");
    fs::path models = settings.models_dir.empty() ? state_dir() / "models" : fs::path(settings.models_dir);
    add("models", models, "model files (models_dir)");
    add("models/llamacpp", models / "llamacpp", "GGUFs the llama.cpp router serves");
    add("vendor", vendor_dir(), "links to the vendored services");
    // The vendor links and the artifacts are listed on the side: if either fails, the other places still are.
    try {
        for (const auto& e : load_vendor_manifest()) {
            fs::path link = vendor_link(e);
            if (fs::exists(link, ec)) add("vendor/" + e.name, fs::weakly_canonical(link, ec), e.description.substr(0, 60));
        }
    } catch (const std::exception& e) {
        std::cerr << "maid: the vendor places are skipped: " << e.what() << "\n";
    }
    add("workflows", state_dir() / "workflows" / "comfyui", "your saved ComfyUI workflows");
    add("templates", state_dir() / "templates" / "comfyui", "your ComfyUI templates (originals, opened as copies)");
    try {
        for (const auto& a : list_artifacts(services)) {
            add(a.owner + "/" + a.name, a.resolved.empty() ? a.path : fs::path(a.resolved), a.description);
        }
    } catch (const std::exception& e) {
        std::cerr << "maid: the artifact places are skipped: " << e.what() << "\n";
    }
    return out;
}

const Place& find_place(const std::vector<Place>& places, const std::string& query) {
    for (const auto& p : places) {
        if (p.name == query) return p;
    }
    // A prefix of a top-level name wins over one of an owner/name entry (`sess` is `sessions`, not `maid/sessions`).
    std::vector<const Place*> top, hits;
    for (const auto& p : places) {
        bool head = p.name.rfind(query, 0) == 0;
        auto slash = p.name.find('/');
        bool tail = slash != std::string::npos && p.name.compare(slash + 1, query.size(), query) == 0;
        if (head && slash == std::string::npos) top.push_back(&p);
        if (head || tail) hits.push_back(&p);
    }
    if (top.size() == 1) return *top.front();
    if (hits.size() == 1) return *hits.front();
    std::string names;
    for (const auto* p : hits.empty() ? std::vector<const Place*>{} : hits) names += (names.empty() ? "" : ", ") + p->name;
    if (hits.empty()) {
        for (const auto& p : places) names += (names.empty() ? "" : ", ") + p.name;
        throw std::runtime_error("no place named '" + query + "'. Places: " + names);
    }
    throw std::runtime_error("'" + query + "' matches several places: " + names);
}

fs::path cd_target(const std::string& arg, const fs::path& workspace, const fs::path& previous, const std::vector<Place>& places) {
    std::error_code ec;
    if (arg == "-") {
        if (previous.empty()) throw std::runtime_error("no previous workspace in this session");
        if (!fs::is_directory(previous, ec)) throw std::runtime_error("the previous workspace " + previous.string() + " is gone");
        return previous;
    }
    fs::path p = arg;
    const char* home = std::getenv("HOME");
    if ((arg == "~" || arg.rfind("~/", 0) == 0) && (!home || !*home)) throw std::runtime_error("HOME is not set, so " + arg + " names nothing; give the full path");
    if (arg == "~") p = home;
    else if (arg.rfind("~/", 0) == 0) p = fs::path(home) / arg.substr(2);
    else if (p.is_relative()) p = workspace / p;
    if (fs::is_directory(p, ec)) return fs::weakly_canonical(p, ec);
    const Place* place = nullptr;
    try {
        place = &find_place(places, arg);
    } catch (const std::runtime_error&) {
    }
    if (!place) throw std::runtime_error("no directory " + p.lexically_normal().string() + ", and no place named '" + arg + "'");
    if (!fs::is_directory(place->path, ec)) throw std::runtime_error("the place " + place->name + " is " + place->path.string() + ", not a directory");
    return fs::weakly_canonical(place->path, ec);
}

std::string browser_command(const std::string& browser, const std::string& url) {
    std::string q = "'" + url + "'";
    if (browser == "firefox") return "firefox " + q + " >/dev/null 2>&1 &";
    if (browser == "chrome") return "(google-chrome " + q + " || chromium " + q + " || chrome " + q + ") >/dev/null 2>&1 &";
    return "xdg-open " + q + " >/dev/null 2>&1 &";
}

std::string shell_init(const std::string& shell) {
    if (shell == "fish") {
        return "# maid shell integration (fish): eval (maid shell-init fish | psub)? Put this in ~/.config/fish/config.fish instead:\n"
               "function mcd; set -l p (maid cd $argv[1]); and cd $p; end\n"
               "function mpath; maid path $argv; end\n"
               "function mcp; maid path $argv[1] --copy; end\n"
               "complete -c mcd -f -a '(maid path --names)'\n"
               "complete -c mpath -f -a '(maid path --names)'\n"
               "complete -c mcp -f -a '(maid path --names)'\n";
    }
    std::string s =
        "# maid shell integration: eval \"$(maid shell-init)\" in your rc file\n"
        "mcd() { local p; p=\"$(maid cd \"$1\")\" || return 1; cd \"$p\" || return 1; }\n"
        "mpath() { maid path \"$@\"; }\n"
        "mcp() { maid path \"$1\" --copy; }\n";
    if (shell == "bash") {
        s += "_maid_places() { COMPREPLY=($(compgen -W \"$(maid path --names)\" -- \"${COMP_WORDS[COMP_CWORD]}\")); }\n"
             "complete -F _maid_places mcd mpath mcp\n";
    } else {
        s += "_maid_places() { local -a names; names=(${(f)\"$(maid path --names)\"}); _describe 'place' names; }\n"
             "compdef _maid_places mcd mpath mcp 2>/dev/null\n";
    }
    return s;
}

}  // namespace maid
