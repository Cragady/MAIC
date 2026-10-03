#pragma once

#include "maic/audit_trail.hpp"
#include "maic/bans.hpp"
#include "maic/llm.hpp"
#include "maic/agent_def.hpp"
#include "maic/instructions.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace maic {

// One visual style. Colors are FTXUI names ("red", "gray_dark"), "#rrggbb", or 0-255 palette indexes.
struct Style {
    std::optional<std::string> fg;
    std::optional<std::string> bg;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool inverted = false;

    Style merged_over(const Style& base) const;
};

// The built-in style of every role MAIC paints with (docs/settings.md lists them); the role names are its keys.
const std::map<std::string, Style>& default_styles();

// Layered settings, every key optional:
//   1. $XDG_CONFIG_HOME/maic/settings.lua (default ~/.config/maic/settings.lua), the user's own
//   2. <dir>/.maic/settings.lua and then <dir>/.maic/settings.local.lua for every trusted directory on the
//      config chain, from the project root (or just under $HOME) down to the workspace (maic/trust.hpp); the
//      nearest file wins. settings.lua is meant to be
//      committed with a project, settings.local.lua is personal. They run in restricted Lua (maic/lua.hpp).
// At each location a .json file with the same stem is the fallback when no .lua exists.
// `maic server`: where it listens, which directories remote sessions may open, and the TLS pair.
struct ServerSettings {
    std::string listen = "127.0.0.1:7373";           // loopback needs no TLS; any other address gets it
    std::vector<std::filesystem::path> workspaces;  // allowed roots for remote sessions; empty = ~/dev2 if it exists, else the current directory
    std::filesystem::path cert;                     // PEM pair; empty = a self-signed one generated under state/server on first use
    std::filesystem::path key;
    std::string relay;                              // https://host:port of a maic-relay the server dials out to; empty = none
    std::filesystem::path relay_cert;               // PEM that pins the relay's certificate; empty = the system CA store
};

// A model preset: one short name that sets the model, its context window, the reviewer the smart harness
// uses with it, and whether to think. `maic --model opus-5.5`, `:model opus-5.5`. Built-in ones can be
// changed field by field and new ones added under `models` in settings.
// The rest is about the models it works with: `subagents` lists the presets a subagent of this model may run
// on (higher or lower tiers; the model itself is always allowed), and `limited` marks a model the user's plan
// caps, which the picks below step aside from. `metered` marks one billed per token to an API account (by default
// every preset on a metered provider, Provider::metered): no automatic pick moves onto one from another provider.
// docs/settings.md has the rules.
struct ModelPreset {
    std::string name;      // "opus-5.5"
    std::string model;     // "anthropic/claude-opus-5-5"
    int context = 0;       // tokens; 0 = the provider's own figure
    std::string reviewer;  // "same" (the model itself), a preset or provider/model, or "" (the small model)
    int think = -1;        // -1 unchanged, 0 off, 1 on
    int tier = 0;          // higher is stronger, and costs more
    bool limited = false;  // the user's plan caps this model's usage
    std::vector<std::string> subagents;  // presets a subagent may run on
    std::string subagent;  // the preferred pick: "same", a preset, or "" for the rule (subagent_pick)
    std::string on_limit;  // where a subagent continues when this model hits its usage limit; "" for the rule
    bool metered = false;  // billed per token: picked only by name, never by a rule from another provider's model
};
std::vector<ModelPreset> default_presets();
// Finds a preset by name, ignoring case and treating spaces, dots and underscores like hyphens ("Opus 5.5").
std::optional<ModelPreset> find_preset(const std::vector<ModelPreset>& presets, const std::string& query);
// The preset whose model is exactly `model` ("anthropic/claude-fable-5-1").
std::optional<ModelPreset> preset_for_model(const std::vector<ModelPreset>& presets, const std::string& model);
// Sets the preset's context window on its provider (and nothing when it has none).
void set_preset_window(std::vector<Provider>& providers, const ModelPreset& preset);

// The presets on `p`'s subagents list, `p` included, strongest first.
std::vector<ModelPreset> subagent_presets(const std::vector<ModelPreset>& presets, const ModelPreset& p);

// A chosen model and why, for transcripts and the TUI. `model` is "" when nothing is left to choose.
struct ModelPick {
    std::string preset;  // "" when the model is not a preset
    std::string model;
    std::string reason;
};
// A subagent's model under a parent on `p`: its `subagent` setting; else the same model when it is not limited;
// else the strongest non-limited preset on its list below its tier, else the strongest non-limited one, else
// the same model.
ModelPick subagent_pick(const std::vector<ModelPreset>& presets, const ModelPreset& p);
// Where a subagent on `p` continues after a usage limit: `on_limit`, else the rule above without "same".
std::optional<ModelPreset> on_limit_pick(const std::vector<ModelPreset>& presets, const ModelPreset& p);
// opencode's small_model for a session on `p` when settings name none: a local model is its own (free and
// already loaded); otherwise the lowest-tier non-limited preset on its list, else the model itself.
ModelPick default_small_model(const std::vector<ModelPreset>& presets, const std::vector<Provider>& providers, const ModelPreset& p);
// The smart harness's reviewer for a session on `model`, first match wins: `pin` (reviewer_model), the
// preset's `reviewer`, `small_model`, the default small model, the model itself. A pick in `failed` (models
// that hit their usage limit this session) falls back to the failed preset's on_limit, else the cheapest
// non-limited preset on the list, never above the tier that failed; "" when none is left.
ModelPick reviewer_pick(const std::vector<ModelPreset>& presets, const std::vector<Provider>& providers, const std::string& model,
                        const std::string& pin, const std::string& small_model, const std::set<std::string>& failed);

// One judge on the smart harness's checker panel: a preset or provider/model, whether it thinks, and how long it
// may take before it counts as timed out.
struct Checker {
    std::string model;
    int think = -1;     // -1 the preset's own, 0 off, 1 on
    int timeout = 30;   // seconds
};
// `checkers` (docs/harness.md, Checkers): the judges that review in place of the single reviewer, in order, and how
// their verdicts combine: "primary" (the first alone), "escalate" (the next is asked only while the call is not
// settled: a DENY, an ASK, a timeout, an error or no clear verdict), "both" (every one must allow). No judges: the
// reviewer of reviewer_pick alone. `ask_before_metered`: a metered judge on another account than the session's is
// called only once the user says yes, for each call; where nobody can be asked it is skipped.
struct Checkers {
    std::string setup;  // the shipped setup it came from ("" for one written out)
    std::vector<Checker> judges;
    std::string combine = "escalate";
    bool ask_before_metered = true;
};
// A shipped setup by name: "dual-9b" or "dual-4b"; nullopt for any other name.
std::optional<Checkers> checker_setup(const std::string& name);

// `steering` (docs/design/engine-protocol.md section 11): what the six steering actions may do in a session. Per
// agent, `agents.NAME.steering` takes the same keys and only narrows (AgentDef::steering).
struct SteeringSettings {
    std::vector<std::string> actions = steer_actions();  // what a session accepts; anything else is maic_steer_disabled
    std::string halt_message = "The user halted this turn. What was in progress was discarded; do not continue it. Wait for the next message.";
    std::string drop_trim = "paragraph";   // none, sentence, paragraph, all
    std::string on_running_tool = "cancel";  // what steer and drop do to a running tool unless they say: cancel or wait
    std::vector<std::string> clients_local = steer_actions(), clients_remote = steer_actions();  // global file only
    std::vector<std::string> ban_actions = {"steer", "drop", "interrupt", "keep", "halt"};  // what a ban entry may name
    std::map<std::string, std::string> from;  // key -> the file that set it, for :steering

    static std::vector<std::string> steer_actions() { return {"steer", "drop", "further", "interrupt", "keep", "halt"}; }
    bool allows(const std::string& action, bool remote) const;  // in actions and in the client side's list
};
// Reads one `steering` table over `into`: `narrow_only` for a project layer or an agent (actions and ban_actions
// only lose entries), `global` for the file that may set `clients`. `where` names it in errors and warnings.
void read_steering(SteeringSettings& into, const nlohmann::json& table, const std::string& where, bool global, bool narrow_only, std::vector<std::string>& warnings);
// A session's steering as it runs as agent `name`: `agent` (AgentDef::steering, checked when settings loaded) over
// `session`, the lists only losing entries.
SteeringSettings agent_steering(const SteeringSettings& session, const nlohmann::json& agent, const std::string& name);

// What becomes of a session you leave (docs/settings.md, `leave`): bg, park or stop for each case, and for idle and
// working "ask" (the client asks). `after` is what a session left working becomes once its work ends with no client in focus.
struct LeaveCase {
    std::string idle, working, after;
};
struct LeaveSettings {
    LeaveCase switching{"park", "bg", "park"};  // leave.switch: :new, :switch and :fork; its `after` also ends a background task
    LeaveCase quitting{"stop", "bg", "park"};   // leave.quit: :q, or a client that goes
    std::string no_daemon = "park";             // what a quit does, park or stop, to a session it would leave running where no daemon can
};

struct Settings {
    std::string model = "llamacpp/current";  // the vendored llama-server serves the linked GGUF as `current`
    std::string mode = "auto";  // held at manual at start where auto_held says so (an untrusted workspace)
    bool think = false;
    bool markdown = true;   // render markdown in the conversation window
    bool mouse = true;      // scroll wheel (terminal text selection then needs Shift+drag)
    bool sound = false;
    // Where new transcripts go: "auto" (project when the workspace has a MAIC.md, else general), "general",
    // "project", or a name under sessions/.
    std::string sessions_home = "auto";
    int init_move_outside_reads = 3;  // :init moves a session into the project home without asking when it read at most this many files outside
    bool full_output = true;          // keep a command's whole output beside the session when the model gets it capped (Agent::full_output)
    int full_output_max_mb = 64;      // at most this much of it per call; past that its head and tail
    std::string leader = " ";
    std::string highlight = "builtin";  // the input's highlighter: "builtin", or "nvim" (an embedded nvim --embed, when it is installed)
    bool enter_sends = false;           // Enter sends a one-line input in insert mode (Shift+Enter / Alt+Enter then insert a newline)
    LeaveSettings leave;                // what becomes of a session you leave, case by case; nearer files replace the cases they name
    int max_tasks = 4;  // background tasks (task with background = true) one session may have running at once; a project layer only lowers it
    std::string models_dir;
    int context = 16384;       // the local server's context window in tokens (--ctx-size for llama.cpp) and the readout
    int context_2 = 8192;      // the same for the side server (services/llamacpp-2.json, ${MAIC_CONTEXT_2})
    // opencode's small_model: the cheap model for auxiliary calls. It titles a session after its first turn
    // ("" = no titles) and is the reviewer's default. `title_model` is its older name.
    std::string small_model;
    long budget_tokens = 0;    // per-session token budget, 0 = unlimited
    bool timestamps = false;   // a time beside each conversation entry
    bool record = true;
    double compact_at = 0.75;      // auto-compact at this share of the context window; 0 turns it off
    int compact_keep_results = 4;  // tool results that never get pruned (the most recent)
    std::string compact_model;     // writes the summaries ("" = the session's model; a remote one only for a remote session)
    std::vector<std::filesystem::path> sources;  // the files that were read, in order
    nlohmann::json layered = nlohmann::json::object();  // every file's table merged in order, for :cd to say what changed
    std::vector<Provider> providers = default_providers();
    std::vector<ModelPreset> presets = default_presets();  // `models` in settings adds or overrides by name
    std::map<std::string, Style> styles;  // by role, in effect: built-in default < theme < style_overrides (docs/settings.md)
    std::map<std::string, Style> style_overrides;  // the `style` entries of the settings files, merged across layers
    std::string theme = "default";  // a theme by name (docs/themes.md); `:theme NAME` switches live
    std::string theme_error;        // why the theme could not be loaded (the built-in default is then in effect)
    bool follow_nvim_theme = true;  // inside a connected host nvim (maic.nvim): the theme follows its colorscheme live
    std::string ui = "tui";         // "tui": MAIC's own interface; "nvim": nvim with maic.nvim as the interface (maic --ui nvim)
    std::string daemon = "attach";  // "attach": the TUI and maic --rpc use the daemon when one runs (maic daemon start); "off": their own engine
    bool bare = false;              // nothing from nvim: no host, no nvim highlighter or theme, no lazy-lock notice or keymap check (--bare, MAIC_BARE=1)
    std::string colors = "auto";    // colour depth: auto, truecolor, 256 or 16
    bool load_instructions = true;  // false: no instruction file anywhere
    std::string system_prompt;      // text placed first in the system prompt; "@path" reads a file (~ expands)
    std::string prefill;            // text every reply starts with (the model continues it); "@path" reads a file
    std::vector<std::string> rules; // standing one-line instructions, carried with system_prompt; layers add up
    // Terms no tool call may contain, any letter case; /.../ is a POSIX extended regex. The default halts the
    // word (with any prefix or plural) and the bare gender combos as whole words. Halted before running; layers add up.
    // The regex halts the bare two-gender-letters-plus-o combos, o in any position, as whole words; ffm, mmf
    // and the like are deliberately not on it. The word itself, with any prefix or plural, is the substring.
    // Micaiah's rule: any three letters from f, m, o with at least one o, in any order, as a whole word
    // (fmo, moo, omo, oom, ooo, oof, foo, ...). ffm and mmf carry no o and stay allowed.
    std::vector<std::string> forbid = {"threesome", "/(^|[^a-z0-9])([fm][fm]o|[fm]o[fmo]|o[fmo][fmo])s?([^a-z0-9]|$)/"};
    // `permission`: allow / ask / deny over `tool:argument` patterns (docs/harness.md); layers add up. MAIC's own helpers
    // are pre-approved. The old `allow` key still works: its command patterns land here as `run_shell:` entries.
    Permission permission{{"run_shell:maic-storyboard*", "run_shell:maic-workflow-edit*", "run_shell:maic-danbooru-tags*", "run_shell:maic-panel-check*", "run_shell:maic path*", "run_shell:maic status*", "run_shell:maic artifacts*", "run_shell:maic sessions*",
                          // cai's read-only invocations (docs/cai.md), each under both spellings of the one script: `read`
                          // (a reader with no write path; its `--out` is asked, below), `time` (clock arithmetic), the
                          // dispatcher's listing, trans-fairy's help and its plain `state` report. Nothing that writes.
                          "run_shell:cai read*", "run_shell:maic-cai read*", "run_shell:cai time*", "run_shell:maic-cai time*",
                          "run_shell:cai --help", "run_shell:maic-cai --help", "run_shell:cai trans-fairy --help", "run_shell:maic-cai trans-fairy --help",
                          "run_shell:cai trans-fairy --man-help", "run_shell:maic-cai trans-fairy --man-help",
                          "run_shell:cai trans-fairy state", "run_shell:maic-cai trans-fairy state",
                          "run_shell:cai trans-fairy state --audit", "run_shell:maic-cai trans-fairy state --audit"},
                         {"run_shell:cai read* --o*", "run_shell:maic-cai read* --o*"}, {}};
    std::vector<AgentDef> agents = default_agent_defs();  // `agents` in settings (older: `profiles`) adds or narrows, by name
    Bans bans;                      // strings, patterns and tokens the model must not produce (docs/bans.md)
    SteeringSettings steering;
    nlohmann::json sampling = nlohmann::json::object();  // sampler keys for every provider; a provider's options.sampling overrides
    std::string tripwire = "machine";  // "machine": the root-owned lock (default); "session": a lock beside this transcript, no sudo; "isolated": session lock and the machine lock ignored (needs allow_isolated)
    bool allow_isolated = false;       // may a session opt out of the machine lock (tripwire = "isolated")? Confined when it does
    std::string browser = "default";   // default | firefox | chrome: what `maic open SERVICE` uses
    std::string remote;                // a maic-server you subscribe to (https://host:7373); `maic open` prefers its services when it is up
    std::string lazy_lock;             // nvim's lazy-lock.json; "" = $XDG_CONFIG_HOME/$NVIM_APPNAME/lazy-lock.json (docs/lazy-lock.md)
    bool lazy_lock_notice = true;      // the start notice and the status strip's lock≠ when it is out of sync
    std::string harness = "dumb";   // "smart": a model reviews commands and writes the rules would allow; "dumb": rules only
    std::string reviewer_model;     // a pinned reviewer ("" = the preset's reviewer, else small_model; see reviewer_pick)
    long reviewer_budget_tokens = 0;  // the reviewer's own token cap; past it, what it would review is asked. 0 = none
    Checkers checkers;              // global file only: the checker panel (no judges: the reviewer alone)
    bool dumb_auto_ok = true;       // false: entering auto mode under a dumb harness warns and asks first
    // Read from the global file only (a project's copy is ignored with a warning; docs/settings.md):
    std::string global_lua = "full";         // the tier of your own Lua data files: full, sandbox or restricted (written literally)
    int lua_memory_mb = 256;                 // the memory cap of settings Lua below full trust
    std::string trust_strictness = "standard";  // the default trust tier: strict, standard, relaxed (docs/harness.md, Trust)
    std::vector<std::string> trust_identities;  // author emails that are yours; empty: git config --global user.email
    std::map<std::string, std::string> trust_levels;  // a tier per directory ("~/dev2/app" = "relaxed")
    // The protocol tier (docs/design/protocol-security.md): open, guarded or airtight. Global file only: the default,
    // and one per directory ("~/scratch" = "open"); `maic trust DIR --protocol TIER` records one that comes first.
    std::string protocol_tier = "guarded";
    std::map<std::string, std::string> protocol_tiers;
    AuditSettings audit;  // audit.lua beside the global settings file, never a project's (docs/audit-trail.md)
    // `instructions = { project_markers = {...}, bound = ... }`: project settings and instruction files are read
    // from the workspace up to the project root (the nearest directory holding a marker), or up to $HOME with
    // bound "home" or outside any project (maic/trust.hpp, config_chain).
    std::vector<std::string> project_markers = {".git", ".maic", "MAIC.md"};
    std::string instructions_bound = "project";
    // The rest of `instructions`: files, read, local_files, imports.depth, extra_dirs (docs/instructions.md).
    InstructionOptions instructions;
    std::vector<std::string> warnings;        // keys a project file set that only the global file may
    ServerSettings server;

    const Style& style(const std::string& name) const;
};

std::filesystem::path settings_path();

// Loads the layers for `workspace` over the defaults. Missing files are fine; a broken one throws with the line.
Settings load_settings(const std::filesystem::path& workspace);
Settings load_settings();  // for the current directory

// The text of a system_prompt setting or --system argument: as given, or the file's contents for "@path".
std::string resolve_system_prompt(const std::string& value);

// Resolves "auto" for a workspace: the project home when a MAIC.md is in effect there, else general.
std::filesystem::path resolve_sessions_home(const Settings& settings, const std::filesystem::path& workspace);

// Writes the settings file with a documented default for every key. Never overwrites an existing file.
// Lua by default (settings.lua); `json` writes settings.json instead. `models_dir` fills that key (maic setup asks).
void write_default_settings(bool json = false, const std::string& models_dir = "");

}  // namespace maic
