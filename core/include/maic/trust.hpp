#pragma once

#include <filesystem>
#include "maic/instructions.hpp"
#include "maic/lua.hpp"

#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace maic {

enum class Origin;

// Directory trust (docs/harness.md, Trust). A project directory is one on the config chain (from the project
// root, or just under $HOME, down to the workspace; config_chain) that holds .maic/ or instruction files (its own,
// or nested ones below it that no project directory above it already covers). Until it is trusted
// its .maic/settings.* are not applied, its instruction files are not given to the model and its .maic/tools/
// are not loaded. $HOME and / are never project directories. The user's global config is always trusted.
//
// Trust is remembered in <state>/trust.json (0600) per absolute path, with a SHA-256 over the path and content
// of each of its settings, instruction and tool files, sorted, and what the directory could do then (its git
// HEAD, its permission entries, its tools). When the files change, the directory's tier decides:
//   strict    any change asks again
//   standard  the user's own changes pass (an uncommitted edit in a git working tree, or commits whose author
//             is one of the user's identities); anything else asks again (the default)
//   relaxed   edits pass; a change that widens what the project can do asks again
// The tier comes only from the user: `maic trust PATH --level L` (kept in trust.json), trust_levels and
// trust_strictness in the global settings. A decision can also hold for this process only (`--trust`,
// "not now"). Nothing here is reachable by the agent: the harness refuses its writes to trust.json and its
// `maic trust` commands.
struct ProjectDir {
    std::filesystem::path dir;
    std::vector<std::filesystem::path> settings;      // .maic/settings{,.local}.{lua,json}
    std::vector<std::filesystem::path> instructions;  // the instruction files in it: instructions.files and their local variants
    std::vector<std::filesystem::path> nested;        // the same names in its subdirectories (on-demand loading), bounded
    std::vector<std::filesystem::path> imports;       // files inside it that those import (@path)
    std::vector<std::filesystem::path> tools;         // every file under .maic/tools/
    bool tool_dir = false;                            // .maic/tools/ exists
    bool empty() const { return settings.empty() && instructions.empty() && nested.empty() && tools.empty() && !tool_dir; }
    std::vector<std::filesystem::path> files() const;  // the hashed files, sorted
};

enum class Trust {
    Trusted,  // remembered with these contents (or changes its tier lets pass), or trusted for this process
    Changed,  // remembered, but changed in a way its tier asks about
    NotNow,   // untrusted for this process
    Never,    // remembered as untrusted
    Unknown,  // never answered
};

struct TrustStatus {
    Trust trust = Trust::Unknown;
    std::string level;                           // strict, standard or relaxed
    std::vector<std::filesystem::path> changed;  // files added, removed or edited since it was trusted
    std::vector<std::string> reasons;            // Changed: why its tier asks again
};

// What the global settings say about trust and about where project files are read from; load_settings sets it
// from the user's own file (never a project's).
struct TrustConfig {
    std::string strictness = "standard";
    std::vector<std::string> identities;              // author emails that are the user's; empty: git config --global user.email
    std::map<std::string, std::string> levels;        // directory (~ expanded) -> tier
    std::vector<std::string> project_markers = {".git", ".maic", "MAIC.md"};  // instructions.project_markers
    std::string bound = "project";                    // instructions.bound: "project" or "home"
    InstructionOptions instructions;                  // which files count as instruction files, for the hash
};
void set_trust_config(TrustConfig config);
bool valid_trust_level(const std::string& level);

std::filesystem::path trust_path();
std::filesystem::path trust_audit_path();  // remote trust changes, one line each

// The directories project settings and instruction files are read from, outermost first: from the workspace up
// to the project root, the nearest directory holding a project marker; with no project above the workspace, or
// with bound "home", up to just under $HOME. Outside $HOME, the workspace alone. Never $HOME or /.
std::vector<std::filesystem::path> config_chain(const std::filesystem::path& workspace);

// What `dir` holds (dir made absolute).
ProjectDir project_dir(const std::filesystem::path& dir);
// The project directories for `workspace`, outermost first.
std::vector<ProjectDir> project_dirs(const std::filesystem::path& workspace);
// $HOME or /: never a project.
bool never_project(const std::filesystem::path& dir);

TrustStatus trust_status(const ProjectDir& p);
// May `dir`'s settings, instructions and tools be used? False for $HOME and /.
bool trusted(const std::filesystem::path& dir);

// Remembers the directory as trusted with its current contents, keeping its tier unless `level` names one.
// The record decides from then on; settle_trust or grant_trust fix it for the process.
// A remote origin cannot grant trust here: throws (remote_trust_change is the stepped-up way).
// `lua` is the directory's Lua level, how its settings.lua runs (full, sandbox, restricted); "" keeps what it
// had, and a new directory is full: "trust it" means as yourself.
void trust_dir(const ProjectDir& p, Origin origin, const std::string& level = "", const std::string& lua = "");
// How a trusted directory's settings.lua runs: this process's --trust=LEVEL, else its remembered level, else full.
LuaTier trust_lua_tier(const std::filesystem::path& dir);
// Remembers it as untrusted (asked never again). A remote origin cannot decide this here either.
void never_trust(const std::filesystem::path& dir, Origin origin);
// Forgets what was remembered; the directory is untrusted for this process and asked about at the next start.
void untrust(const std::filesystem::path& dir);
// For this process only: trusted (`--trust`) or untrusted without asking again ("not now").
void trust_for_session(const std::filesystem::path& dir, LuaTier lua = LuaTier::Full);
void not_now(const std::filesystem::path& dir);
// Fixes each project directory's answer for this process, so a file edited mid-session does not switch it.
// Changes a directory's tier let pass are recorded as its new contents; one line each says which files.
std::vector<std::string> settle_trust(const std::filesystem::path& workspace);

// The project directories still to be asked about: Unknown or Changed, not decided in this process.
std::vector<ProjectDir> trust_to_ask(const std::filesystem::path& workspace);
// Auto mode at start: a session starts in auto only in a workspace covered by project directories that are all
// trusted with full Lua, and at least one; anywhere else it starts in manual, and `:mode auto` still turns auto on.
// "" when auto may start here, else the one line that says why not.
std::string auto_held(const std::filesystem::path& workspace);
// One line per untrusted project directory (what was skipped and how to trust it), and for $HOME or / as the
// workspace, that their files are ignored. Empty when there is nothing to say.
std::vector<std::string> trust_notices(const std::filesystem::path& workspace);
// The files of `p` grouped for a person: "settings: .maic/settings.lua\ninstructions: MAIC.md" (indented).
std::string describe_project(const ProjectDir& p, const std::string& indent);
// "standard; `maic trust DIR --level relaxed` to stop asking about your own edits, ..."
std::string tier_hint(const std::filesystem::path& dir, const std::string& level);
// "Lua full: its settings.lua runs as you; `maic trust DIR --lua sandbox` ..."
std::string lua_hint(const std::filesystem::path& dir);

// `:trust [PATH] [--level L]`, `maic trust [PATH] [--level L]`: trusts PATH's directory, or every untrusted
// project directory of the workspace; returns what was done. `:untrust [PATH]`, `maic untrust PATH`.
// `maic trust --list`.
std::string grant_trust(const std::filesystem::path& workspace, const std::string& path, Origin origin, const std::string& level = "", const std::string& lua = "");
std::string revoke_trust(const std::filesystem::path& workspace, const std::string& path);
std::string trust_listing();
// The whole command as typed at this machine: "trust" with [PATH] [--level L] or --list, "untrust" with [PATH].
std::string trust_command(const std::string& command, const std::vector<std::string>& args, const std::filesystem::path& workspace);

// Asks about each project directory trust_to_ask finds, once, on `in`/`out` (the terminal before the TUI draws):
// what it holds, why it asks, its tier, and t (trust fully: its Lua runs as you), s (trust sandboxed: its Lua
// runs in a child process), n (not now: untrusted this session) or v (never).
// Anything else, or the end of input, is "not now".
void ask_trust(const std::filesystem::path& workspace, std::istream& in, std::ostream& out);
// The pieces of that prompt, for the TUI's own (`:cd`): what the directory holds and why it asks, and an answer
// carried out (t, s, v; anything else is not now), returning what was done.
std::vector<std::string> trust_prompt(const ProjectDir& p);
std::string answer_trust(const ProjectDir& p, const std::string& answer);

// Imports your own instruction files (the system directory and ~/.config/maic) make from outside the trusted
// directories (docs/instructions.md, Imports from your own files). Each one is read only once you approved the
// (importer, target) pair; <state>/trust-imports.json (0600) keeps the pairs with the target's SHA-256. A changed
// target is asked about again under the global trust_strictness: strict asks, standard lets your own edit or
// commit pass (as for trust), relaxed lets it pass. The agent never reaches the file (touches_trust).
std::filesystem::path import_exceptions_path();
// Trusted (approved, and unchanged or changed in a way the tier lets pass, then recorded), Changed (with the
// reasons) or Unknown.
TrustStatus import_exception_status(const std::filesystem::path& importer, const std::filesystem::path& target);
// Remembers the pair with the target's current contents. A remote origin cannot: throws.
void approve_import(const std::filesystem::path& importer, const std::filesystem::path& target, Origin origin);
// Every approved target, for self-protection: an agent's write to one is asked (smart) or refused (dumb).
std::vector<std::filesystem::path> import_exception_targets();
// The question for one pair: the importing file, the target, its size, what approving means, one line each.
std::vector<std::string> import_prompt(const std::filesystem::path& importer, const std::filesystem::path& target, const TrustStatus& status);
// `maic trust imports` (the list), `--remove PATH` (every pair whose importer or target is PATH).
std::string trust_imports_command(const std::vector<std::string>& args);

// A remote device changing trust (POST /api/trust): only with a step-up proof the registered verifier accepts.
// Until accounts exist (docs/design/accounts.md) none is registered and every request is refused. `action` is
// trust, untrust, never, level or lua (`lua`: full, sandbox or restricted); `path` is absolute. Each request, done or refused, is a line in
// trust_audit_path() naming the device. Returns what was done; throws with the reason otherwise.
using StepUpVerifier = std::function<bool(const std::string& device, const std::string& proof)>;
void set_step_up_verifier(StepUpVerifier verifier);
std::string remote_trust_change(const std::string& device, const std::string& proof, const std::string& action,
                                const std::string& path, const std::string& level, const std::string& lua = "");

}  // namespace maic
