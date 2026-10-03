#pragma once

// The `:` commands the engine owns (docs/design/engine-protocol.md, sections 7 and 8): everything that acts on one
// session's agent, settings and transcript. maid.session.command runs them under the session's lock. A front end
// keeps its own: the view, the editor, themes, registers, and the machine's services and trust store.

#include "maid/agent.hpp"
#include "maid/lua.hpp"
#include "maid/session.hpp"
#include "maid/settings.hpp"
#include "maid/trust.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace maid {

// What a command answers: lines for the person who typed it ({text, level}: info, warn, error), a question to
// put to them first, or text to send as their next message (:init's request for a MAID.md).
struct CommandOutput {
    bool ok = true;
    nlohmann::json lines = nlohmann::json::array();
    void info(std::string text) { line(std::move(text), "info"); }
    void warn(std::string text) { line(std::move(text), "warn"); }  // shown as an error; the command still did what was asked
    void error(std::string text) {
        ok = false;
        line(std::move(text), "error");
    }
    void line(std::string text, const char* level) { lines.push_back({{"text", std::move(text)}, {"level", level}}); }

    struct Ask {
        std::string title;
        std::vector<std::string> lines;
        std::string keys;  // the answers it takes, one character each; Esc means "n"
    };
    std::optional<Ask> ask;
    std::string ask_id;  // set by SessionCommands
    std::string send;

    nlohmann::json json() const;  // the result of maid.session.command
};

class SessionCommands {
public:
    // What a command acts on, lent by the engine for one call under the session's lock.
    struct Session {
        Agent& agent;
        SessionLog& log;
        Settings& settings;  // the session's own: :ctx, :cd and a preset change it
        bool running;        // a turn is running
        std::function<void(nlohmann::json)> changed;              // maid.session.settings with these fields
        std::function<void(const std::string&)> rename;           // the title, as updateConversation sets it
        std::function<void(const std::string&)> notice;           // a maid.notice now (what a :lua chunk prints)
        std::function<Settings(const std::filesystem::path&)> settings_at;  // what a start in that directory reads
        std::string tier;  // "protocol tier: guarded (global default)", for :status and :harness
        // For :status, from the engine's own record of the session.
        std::string id, title;
        int turns = 0;
        int tasks_running = 0, tasks_started = 0;  // background tasks of this load
        std::string daemon;  // "daemon: attached, via socket" or why not
    };

    // One command line, without its ':' ("ban add foo"), by its full name or an alias the TUI accepts.
    CommandOutput run(Session& s, const std::string& line);
    // The answer to a question a command asked (CommandOutput::ask), by its id and the key chosen.
    CommandOutput answer(Session& s, const std::string& ask, const std::string& key);

    // Whether a remote client may run this command line: the allow-list of section 7.
    static bool remote_allowed(const std::string& line, Mode current, std::string& why);
    // Whether `name` is one of these commands (the TUI runs the rest itself).
    static bool owns(const std::string& name);

    // Auto under a dumb harness was confirmed for this session (or dumb_auto_ok in settings).
    bool dumb_auto_ok = false;

private:
    using Then = std::function<void(Session&, CommandOutput&, const std::string&)>;
    void ask(CommandOutput& out, CommandOutput::Ask question, Then then);
    void request_mode(Session& s, CommandOutput& out, Mode m);
    void set_model(Session& s, CommandOutput& out, const std::string& model);
    void apply_sampling(Session& s);
    void cd_to(Session& s, CommandOutput& out, const std::filesystem::path& ws, const std::filesystem::path& to);
    void ask_cd_trust(Session& s, CommandOutput& out, std::vector<ProjectDir> dirs, const std::filesystem::path& ws, const std::filesystem::path& to);
    void ask_imports(CommandOutput& out, std::vector<PendingImport> pending, bool again);
    void lua(Session& s, CommandOutput& out, const std::string& code, bool from_file);

    std::map<std::string, Then> asks_;
    long asks_made_ = 0;
    std::filesystem::path previous_ws_;                         // the workspace before the last :cd
    nlohmann::json live_sampling_ = nlohmann::json::object();  // :sampling changes, over the settings
    std::set<std::string> asked_imports_;                      // importer > target pairs asked about this session
    std::unique_ptr<Lua> lua_;                                 // created on first :lua; keeps globals between calls
    std::function<void(const std::string&)> lua_notice_;
};

}  // namespace maid
