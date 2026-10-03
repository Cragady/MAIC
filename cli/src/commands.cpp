#include "commands.hpp"

#include "maid/agent.hpp"
#include "maid/paths.hpp"
#include "maid/service.hpp"
#include "maid/http.hpp"
#include "maid/places.hpp"
#include "maid/status.hpp"
#include "maid/theme.hpp"
#include "maid/vendor.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <regex>

namespace maid {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "Ctrl-W", "C-w", "<C-w>", "^W", ":w", "Alt+Enter", "M-CR" all reduce to a comparable key.
std::string normalize(std::string s) {
    s = lower(s);
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (const char* junk : {"<", ">"}) {
        for (size_t p; (p = s.find(junk)) != std::string::npos;) s.erase(p, 1);
    }
    for (const char* pre : {"ctrl-", "ctrl+", "control-", "c-", "^"}) {
        if (s.rfind(pre, 0) == 0) s = "ctrl-" + s.substr(std::char_traits<char>::length(pre));
    }
    for (const char* pre : {"alt-", "alt+", "meta-", "m-"}) {
        if (s.rfind(pre, 0) == 0) s = "alt-" + s.substr(std::char_traits<char>::length(pre));
    }
    if (s == "alt-cr" || s == "alt-return") s = "alt-enter";
    if (s == "cr" || s == "return") s = "enter";
    if (s == "esc") s = "escape";
    return s;
}

struct Topic {
    std::string name;
    std::vector<std::string> aliases;
    std::string summary;
    std::string text;
};

const std::vector<Topic>& topics() {
    static const std::vector<Topic> t = {
        {"modes", {"mode-list", "manual", "auto-read", "edit", "auto", "plan"}, "what the agent may do without asking",
         "*modes*\n"
         "Shift-Tab cycles them; `:mode NAME` sets one; `--mode` on the command line; `mode` in settings.\n\n"
         "- **manual**: asks before every edit and every command. A session starts here instead of auto where auto waits (below).\n"
         "- **auto-read**: reads anywhere and read-only commands (ls, cat, grep, git log, ...) run on their own, in a sandbox where even the workspace is read-only; edits and other commands ask.\n"
         "- **edit**: edits inside the workspace apply on their own; commands ask.\n"
         "- **auto** (default): edits and sandboxed commands inside the workspace run on their own; writes outside it ask.\n"
         "- **plan**: read-only. Reads and read-only commands only; the model proposes a plan.\n\n"
         "Auto at start: a session starts in auto only where every project directory from the project root down is trusted fully (trusted, with full Lua; `:h trust`), and at least one is. Anywhere else, a directory with no .maid/ or instruction file included, it starts in manual and says so once; `:mode auto` turns auto on. `--mode auto` on the command line starts in auto anywhere: you asked for it then.\n\n"
         "In every mode: secrets are never read, system paths are never written, startup files and MAID's own harness are always asked about, dangerous commands trip the harness, and a request from another origin is always asked. See `:h harness`."},
        {"harness", {"tripwire", "sandbox", "trip", "lock", "dumb", "smart", "reviewer"}, "what protects the machine; :harness smart|dumb",
         "*harness* *:harness* *--harness*\n"
         "Every tool call is checked before it runs: tripwire, then policy for the mode, then the reviewer, then approval, then the sandbox.\n\n"
         "- **reviewer** (the **smart** harness): before any command or write that the rules would let through *without asking* (auto and edit modes), a model reads the last few things you said, the agent's last words and the action, and answers ALLOW, ASK or DENY. ASK becomes an approval prompt, DENY refuses with the reason, and a reviewer that cannot answer means ASK. Reads are never reviewed; what you approved yourself is not reviewed either. "
         "Its model, first match wins: `reviewer_model` in settings (your pin), the preset's `reviewer` (the local presets review with themselves), `small_model`, then the small model of the preset's family (the lowest non-limited tier on its `subagents` list: haiku-4.5 for the Anthropic presets). A reviewer that hits its plan's usage limit is replaced for the rest of the session by a cheaper one (never a higher tier), with one notice; when none is left the reviewer is off and every action it would review is **asked** instead. Its tokens count toward `:budget`, and `reviewer_budget_tokens` caps them on their own, after which the same holds. `:harness` shows its model, why, and what it has spent.\n"
         "- **checkers**: `checkers = \"dual-9b\"` (or `\"dual-4b\"`) in your global settings puts a panel of judges in the reviewer's place: the local Qwen without thinking judges every call, and Claude Haiku 4.5 on your Claude plan is asked only when Qwen does not allow it, is unsure, times out, errors or gives no clear verdict. A denial is never turned into a silent run: if the second judge would allow it, you are asked. Every checker down means you are asked. Before a metered judge (Claude Code, Anthropic's API, any metered preset) is called you are asked each time; no, no answer or nobody to ask (headless, a background task) skips it, and `checkers = { setup = \"dual-9b\", ask_before_metered = false }` calls it unasked. `combine` may also be `primary` or `both`. Each reviewed call gets a `checked:` line naming every verdict and who decided (`judged_by` in the transcript).\n"
         "- **dumb harness** (the default): the reviewer is off; the rule list alone decides and nothing reads the conversation. `:harness smart`, `--harness smart` or `harness = \"smart\"` turns the reviewer on. `dumb_auto_ok = false` in settings makes entering **auto** under the dumb harness show a warning once per session and ask you to confirm (`--accept-dumb-auto` skips it for one run); headless runs then refuse dumb + auto without the flag. The status strip shows DUMB HARNESS.\n"
         "- **the harness protects itself**: writing MAID's settings, its lock files, the server's tokens, or running the lock helper is not the agent's to do; under the smart harness that trips the machine lock (a request from another agent that tries it is what the global lock is for), the dumb harness asks.\n"
         "- **tripwire**: a root-owned lock. `:trip REASON` (or the [t] answer at an approval, or a dangerous command) sets it instantly with no password. While tripped nothing runs, but the session survives. `:unlock` resets it and asks for your sudo password. `tripwire = \"session\"` in settings scopes a trip to the session instead: the lock is a file beside the transcript, respected by that session only, and `:unlock` removes it without sudo (the machine lock is still honoured when set). `tripwire = \"isolated\"` also ignores the machine lock, which needs `allow_isolated = true` and confines the session: no reads outside its directory, no remote requests, no work through the server; the status strip shows ISOLATED. A project's `.maid/settings.lua` can pick the scope per project. `maid unlock` with no argument lists the machine lock and every session's lock with its task, directory and model, and unlocks what you pick.\n"
         "- **sandbox**: every model-run command executes in bubblewrap: only the workspace writable, secrets hidden, no network, no sudo, a timeout.\n"
         "- **approval**: y / n / N (no, and type a sentence the model gets as the reason) / a (always this file or program, this session) / t (trip). Edits show the lines that would change.\n"
         "- **repeated calls**: the same call three times in a row is refused; five times trips the lock when it is a write or a command that could change something, and just ends the turn when it is harmless (a read, a read-only or allow-listed command). Three denials by you in one turn end the turn.\n"
         "- **allow list**: commands you pre-approved run without asking or review; see `:h allow`. The wider `permission` block (allow / ask / deny over `tool:pattern`) adds restrictions or pre-approvals after the fixed rules; see `:h permission`.\n"
         "- **subagents**: the model's `task` tool runs a child agent as one of the agents (`:h agent`), which only narrow (mode capped by the session's, write paths, tools, budgets); its approvals come through you with the agent named. See `:h task`.\n"
         "- **the card**: `:gpu` shows who holds it, `:gpu free` releases it; see `:h gpu`.\n"
         "- **forbidden terms**: a tool call containing one is halted in every mode, under every harness; see `:h forbid`.\n"
         "- **undo points**: every file the agent changes is saved first; `:undo` restores. See `:h undo`.\n\n"
         "Details and the planned layers: docs/harness.md."},
        {"sessions", {"session", "resume", "transcript", "transcripts"}, "transcripts, -c, -r, forking",
         "*sessions*\n"
         "Every session is a JSONL file under ~/.local/state/maid/sessions (0600), in a home: `general/` by default, `projects/<encoded workspace>/` when settings say `\"sessions_home\": \"project\"`, or any name. `:session` shows this one and its home; `maid sessions` lists them with where each was started and last opened; `maid sessions rehome ID [ID...] project|general|NAME` moves them.\n\n"
         "- `maid sessions rehome` takes several ids (a unique prefix or a path each), HOME last. Subagent sessions move only when asked: by default their `sub` sessions stay where they are and still find their parent by id; `--subagent` moves them too (children of children as well), `--subagent-only` moves only them, `--children-of ID` picks one parent's without naming them. An ambiguous prefix (the candidates are listed) or a running session refuses the whole command and nothing moves; each move prints `from -> to` and leaves a `rehomed` record; `-n` (`--dry-run`) prints the plan only.\n"
         "- `maid -c` continues the newest session from the current directory; `maid -r` picks from a list; `maid -r ID` (a unique prefix is enough); `maid -r PATH` resumes any transcript file by path, including a temporary one under $XDG_RUNTIME_DIR from `--no-record` (those are never listed, so `-c` cannot find them).\n"
         "- Interactive resumes append to the same file. `--no-append` writes a new file that only points at the old one and the number of records loaded, which is also how a session forks. `--no-record` (or `\"record\": false` in settings, or `maid -p` without `--record`) keeps the transcript in the runtime directory instead, where it disappears at logout; it is never listed.\n"
         "- `maid -r ID --fork-at N` (also with `-c`, and with `-p`) continues from the first N records of that file only, in a new file that points at them; the old file is never changed. `maid sessions path ID` finds the file to count records in.\n"
         "- `maid sessions import FILE` turns a claude.ai export (JSON) or a Claude Code transcript (JSONL) into a session here and prints its id; `--as` names the format when detection guesses wrong, `--home` picks where it goes, `--conversation UUID` picks one out of a full export.\n"
         "- `maid sessions redact ID` writes `./<id>.redacted.jsonl` with credential material replaced by `[REDACTED:kind]` and reports counts per kind; `-o FILE` or `--in-place` choose where (`--in-place` first copies the original to `sessions/.backups/<id>/`, where `cai trans-fairy-write restore ID` finds it). Record types and the redaction kinds: docs/sessions.md.\n"
         "- Looking at one: `maid sessions read ID [--range A-B] [--tools]` prints the conversation as text; `state ID` is a one-screen summary (turns, tool calls per tool, files touched, tokens against the budget, compactions, forks, subagents); `time ID` shows how long each turn took and the slowest tool calls; `name ID` titles it with the title model.\n"
         "- A command's whole output, when it was longer than what the model got, is kept beside the session (`<id>.d/<call>.out`, with a timing index; `full_output`, `full_output_max_mb`): `maid sessions output ID` lists them, `maid sessions output ID CALL` prints one and `--replay` plays it back as it ran. It is display only: the model saw the capped result.\n"
         "- Building one from others, never changing them: `maid sessions inject ID --text T [--at N] [--role user|system]` forks at N and adds one note marked as injected; `graft ID --onto TARGET [--at N]` forks TARGET at N and copies ID's conversation in after a note saying where it came from; `compose ID --from N [--root FILE]` copies ID's records from N on, after an optional root text and a note that the earlier part is missing (a cheap re-root of a long session).\n"
         "- A session that ended mid tool call resumes from the last complete step. The model is told it resumed, with the current mode and instructions."},
        {"headless", {"-p", "print", "cli", "command-line", "context", "-C", "--context", "interactive", "-i"}, "maid -p, stdin, --context files, --interactive",
         "*headless* *-p* *--context* *--interactive*\n"
         "`maid -p \"prompt\"` runs one turn without the UI: the reply streams to stdout, tool activity to stderr. `maid -p -` takes the prompt from stdin (`cat dialog.txt | maid -p -`). `--json` prints events as JSON lines. Approvals are asked on the terminal when there is one, otherwise denied; `--mode auto-read` is the usual choice for scripts.\n\n"
         "**Context.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several; `-C -` reads stdin (then the prompt must be an argument). Combine with `-c` / `-r` to put a file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maid`.\n\n"
         "**Transcripts.** A headless run writes its transcript to the runtime directory ($XDG_RUNTIME_DIR/maid/sessions, gone at logout) unless `--record` (with `-c`/`-r`: a new file that points at the old one, a fork) or `--append` (writes into the old file). `maid --no-record` does the same for an interactive session; `\"record\": false` in settings makes it the default.\n\n"
         "**--interactive** (`-i`) with `-p` opens an interactive session that starts with the prompt already sent. It follows interactive rules whatever the order of the flags: a transcript is always kept, `-c`/`-r` continue in the same file unless `--no-append`; `--record` is redundant and `--json` is ignored.\n\n"
         "Short flags cluster: `maid -pi -` is `-p -i -`; a flag that takes a value (`-m`, `-C`) goes last in a cluster. `maid help TOPIC` prints these pages outside a session."},
        {"queue", {"queued", "mid-turn", "interrupt"}, "sending while the agent works",
         "*queue*\n"
         "Sending while the agent is busy steers the running response (OpenAI's `response.steer`): the message reaches the model at its next step, the response ends there and a successor carries your message, in the same turn. `:w now` delivers it immediately: the current output is abandoned and the model is asked again with your message included. A message that arrives after the last step still gets a successor. Ctrl-C interrupts the turn instead; Ctrl-S pauses it (`:h steer`)."},
        {"providers", {"provider", "remote", "llamacpp", "anthropic", "deepseek", "openrouter"}, "local and remote models",
         "*providers*\n"
         "Models are `provider/model`: `llamacpp/current` (the vendored llama.cpp serving the linked GGUF; local, the default; docs/llamacpp.md), `llamacpp/NAME` for any GGUF under the models directory, `anthropic/claude-opus-5-5`, `deepseek/deepseek-v4-pro`, `openrouter/...`, or any OpenAI-compatible API added in settings (docs/models.md, API models). A bare name goes to the first provider. `:model` alone lists providers; `:models` lists what the current one serves.\n\n"
         "A remote provider receives your prompts, every file the agent reads and every command's output; MAID says so when you switch and shows REMOTE in the status strip. Keys come from an environment variable or a command, never from the settings file. See docs/settings.md."},
        {"server", {"remote", "phone", "token", "tls", "maid-server", "relay", "maid-relay", "pair"}, "remote access from a phone: the server, tokens, TLS, the relay",
         "*server* *maid-server*\n"
         "`maid server start [--listen ADDR:PORT] [--model M] [--mode MODE]` serves agent sessions over HTTP with server-sent events, and a one-file web client at `/` for a phone. Loopback (127.0.0.1:7373) by default; any other address turns TLS on, with a self-signed certificate made on first use (its fingerprint is printed) or the pair set in `server.cert` / `server.key`.\n\n"
         "Every request needs a per-device bearer token: `maid server token new NAME` prints one once and keeps only its hash; `token list` and `token revoke NAME` manage them. Failed attempts are rate limited per source and every request is written to `~/.local/state/maid/server/audit.log`. `maid server status` shows the configuration and whether a server answers.\n\n"
         "Away from the LAN: `server.relay = \"https://host:port\"` names a `maid-relay` (built beside maid, run on any machine with a port) that the server dials out to and holds open; no port is opened at home. `maid server pair` prints a one-time code and a `maid://pair/...` string; the phone, on the LAN, pastes it under Sessions > Pair with a relay, the keys are exchanged directly with the server, and from then on the phone opens the relay's address and talks through it, end to end encrypted (X25519, HKDF-SHA256, XChaCha20-Poly1305): the relay sees pairing ids, sizes and times. `maid server pairs` lists phones, `unpair NAME` removes one, `status` shows whether the link is up. The relay has no route that reaches the tripwire.\n\n"
         "Every tool call from a remote client is asked about, whatever the mode; approvals are answered from the client. The tripwire can be tripped from a client and never reset: there is no route for it, on the server or the relay. Sessions are recorded like any other (kind server) and `maid -r ID` continues one at the terminal. Workspaces are limited to `server.workspaces` in settings. See docs/remote.md."},
        {"artifact", {"artifacts", "pages", "sandbox", "channel", "agent-kit"}, "pages maid-server serves sandboxed, with their data beside them",
         "*artifact* *maid artifact*\n"
         "An artifact is a built page folder (index.html at its top, its scripts and styles beside it) that maid-server serves at `/a/ID/` from `~/.local/state/maid/artifacts/ID/`. `maid artifact add DIR [--id ID]` copies one in (again to update it; its saved `data/` is kept), `maid artifact list` shows them, and `maid artifact open ID` prints a one-time link for a browser on this machine, valid two minutes, that logs the browser in to artifacts for 12 hours.\n\n"
         "Every artifact response is sandboxed: an opaque origin with no access to the server's cookies or storage, scripts only from the artifact's own files and `/a/_vendor/` (the vendored Vue), and no network but its own data documents. The page reads and writes `data/NAME.json` beside itself, with a revision in `ETag`: a write sends `If-Match` (or `If-None-Match: *` to create), a stale one is a 409, and a document is at most 1 MiB. Inline scripts are always refused, so a page loads its scripts from its own files. Runtime template compilation is refused too, unless you turn on `maid artifact allow-insecure ID` for one artifact (you type `allow insecure` at a terminal; `--off` undoes it): that adds `'unsafe-eval'` to its script policy only, the sandbox and the network limits stay, every load of its index.html is written to the audit log, and `maid artifact list` shows ALLOW_INSECURE. docs/artifacts.md\n\n"
         "When a page submits: `maid artifact watch ID [--doc NAME] [--once]` prints a line per event an agent acts on (`submitted`, `side_prompt N`, `after_prompt N`, `split ID`), and `maid channel` sends the same events to Claude Code as channel notifications (an MCP server on stdio). Neither carries the document. Every event names the notify protocol the user approved for that artifact (`maid artifact protocol ID`, `--approve` at a terminal, `--verify HASH` re-hashes it), or says `none` or `unapproved`. docs/agent-kit.md"},
        {"vendor", {"vendored", "install-services", "artifacts-tree"}, "services MAID installs for itself, and the artifact tree",
         "*vendor*\n"
         "`maid vendor` lists the services MAID can install at pinned versions (ComfyUI as a submodule at a release tag, llama.cpp and whisper.cpp as submodules at release tags built out of tree). `maid vendor add NAME` fetches and installs one the way MAID wants it (own Python, telemetry off, models on the external drive); `maid vendor adopt NAME PATH` uses an install you already have; `maid vendor use llamacpp PATH` picks the GGUF llama-server loads (docs/llamacpp.md), `maid vendor use whisper FILE` the ggml model whisper-server loads (docs/diction.md); `maid vendor wire NAME` redoes the links without the network and, for comfyui, rewrites only the `maid:` block of `extra_model_paths.yaml` from `models_dir` and the manifest's models map (checkpoints, diffusion_models, loras, text_encoders, vae, clip_vision, embeddings, controlnet, upscale_models, style_models, model_patches), keeping every other key; `maid vendor unlink NAME` stops using it. `maid setup` chains these for a first run. Everything lives under ~/.local/state/maid/vendor/, workflows under ~/.local/state/maid/workflows/, and `maid artifacts` shows where each thing really is. See docs/vendor.md."},
        {"models", {"catalog", "model-catalog", "fim", "completion", "llama.vim", "llamacpp-fim"}, "the model catalog: what each model is for, install by id, code completion",
         "*models* *maid models* *catalog* *llamacpp-fim*\n"
         "`maid models` lists the catalog (`models/catalog.json`, plus your own entries or overrides by id in `~/.config/maid/models.json`): id, role (agent, vision, scribe, completion, speech, vad), size, whether it is installed under `<models_dir>`, which one is current for its server, and the presets that use it. `maid models info ID` prints its brief first (what it is good and bad at, when to pick it), then its license, source repository and pinned commit, files with SHA-256, VRAM estimates for an 8 GB card and notes.\n\n"
         "`maid models install ID [--link]` fetches each file with curl and keeps it only when its SHA-256 matches; a file already there with the right size is kept and not fetched again, so the models on your drive count as installed. An entry that shares another's weights (`qwen3.5-9b-text`, the 9B without its vision projector) is a relative link to them, installing the other first; nothing is copied. `--link` makes it the current model of its server: llamacpp's `current-model.gguf`, whisper's `current.bin`, or llamacpp-fim's `current.gguf`. `maid models verify ID` hashes what is there; `maid models remove ID` asks first (off a terminal it needs `--yes`) and never removes weights another installed entry links to; `maid models check` validates the catalog offline.\n\n"
         "**API models** (DeepSeek's) are listed after the catalog with their window and output, from the API's `GET /models` where it was read (`maid models refresh` reads it again) or the catalog's, and the average cost per session, an estimate from the catalog's prices kept only on this machine.\n\n"
         "**Code completion**: `maid up llamacpp-fim` serves the linked Qwen2.5-Coder base model on 127.0.0.1:8084 for llama.vim's `/infill` (router mode, model name `current`, autoload off: `maid up` loads it, `maid gpu free llamacpp-fim` unloads it, starting ComfyUI or whisper unloads it like the other llama servers, and it stays unloaded until `maid gpu load llamacpp-fim` or until that service stops through MAID; completions pause meanwhile). The 7B is the default; the 3B and 1.5B trade suggestion quality for speed and memory. Nothing leaves the machine. See docs/models.md."},
        {"diction", {"dictation", "whisper", "voice", "narrate"}, "narrate out loud into a markdown document, on this machine",
         "*diction* *maid diction* *whisper*\n"
         "Micaiah's dictation tool, moved into MAID: `maid diction` (or `cai diction`) listens on the mic, splits speech on pauses, has whisper-server (127.0.0.1:8083) transcribe each utterance and a scribe model on llama-server write it into `<directory>.md` as cleaned prose, a numbered procedure (`--steps`) or raw text (`--no-agent`). Normal and insert editing, voice commands, the triple tap with \"diction normal / insert / toggle\", recap and logs work as they always did; `maid help diction` prints its own `--help`.\n\n"
         "Setup: `maid vendor add whisper`, a model (`maid vendor model whisper URL SHA256`; docs/diction.md has the URLs and hashes), `maid up whisper`, `maid up llamacpp` (or `llamacpp-2`, which the default scribe prefers so the main model stays loaded). `-m` names a whisper ggml file or a name under `<models_dir>/whisper/`; `--agent-model` takes a preset (default `qwen-4b`) or `provider/model` (`maid model resolve NAME` shows what it means).\n\n"
         "Nothing leaves the machine: audio goes to whisper-server and text to llama-server, both on loopback, unless `--agent-model` names a cloud preset, which diction says at start. `maid path diction/logs` is where its logs go. See docs/diction.md."},
        {"settings", {"config", "styles", "style", "settings.lua", "settings.json"}, "the settings file",
         "*settings*\n"
         "Lua files returning a table (JSON works too). Layered: ~/.config/maid/settings.lua, then `.maid/settings.lua` and `.maid/settings.local.lua` in each trusted directory on the chain from the project root (or under $HOME) down to the workspace (nearest wins; settings.lua is for the project, settings.local.lua is personal; `:h trust`). A file is code: `os.getenv`, `maid.hostname`, `maid.home` for per-machine choices; your own files run with full Lua unless `global_lua` says `sandbox` or `restricted`, a project's at the Lua level you trusted it with. Keys: model, mode, think, markdown, mouse, record, compact_at, sessions_home (auto/general/project/name), models_dir, leader, instructions (global file only; docs/instructions.md), providers, theme, colors, style (single roles over the theme; `:h theme`). `maid settings init` writes the global one, `:init` scaffolds a project's, `:settings` shows what is in effect. diction's own settings are diction.lua beside the global one (`maid settings read diction`, at `global_lua`); the audit trail's are audit.lua there, yours alone and off by default (`maid audit-trail init`, docs/audit-trail.md). See docs/settings.md."},
        {"tools", {"tool", "lua-tools", "script-tools", "manifest", "glob", "question", "todo-tool", "user-tools"}, "the model's tools, and writing your own in Lua or any language",
         "*tools*\n"
         "Built in: `read_file` (`grep` for only the matching lines), `list_dir` (`depth` for a tree), `glob` (files by name pattern), `search_files` (grep -E), `write_file`, `edit_file`, "
         "`multi_edit` (several replacements in one file, all or none), `apply_patch` (a unified diff, all or none), `move_file`, `copy_file`, `delete_file`, `make_dir`, `run_shell` (bubblewrap sandbox), "
         "`question` (asks you something, with options; a number picks, or type an answer, Esc gives none) and `todo` (the model's plan; `:todo` shows it, the status strip counts it). "
         "Inside nvim with a connected host (`:h nvim`) there is also `diagnostics` (`path` optional): the LSP diagnostics nvim has, judged as a read of that file or of the workspace. "
         "Every one goes through the harness; the file tools are judged as writes to each path they touch (a move out of the workspace asks, a delete under ~/.ssh trips, a patch with one such file is refused whole). `:tools` lists them with any tools of your own.\n\n"
         "**Your own tools** are Lua files: `.maid/tools/<name>.lua` in the workspace or `~/.config/maid/tools/<name>.lua`, loaded when a session starts. A file returns a table: "
         "`name`, `description`, `parameters` (a JSON schema as a Lua table) and `run = function(args) ... end` returning a string or a table. The model calls it like any other tool. "
         "Each call runs in its own LuaJIT state with only the base, string, table, math and bit libraries: no io, os, require or load. Inside, `maid.read(path)`, `maid.write(path, text)`, "
         "`maid.list(path)`, `maid.search(pattern, path)` and `maid.shell(cmd, opts)` each go through the harness exactly as the built-in tool would (policy, your approval, the sandbox); a denial "
         "is a Lua error carrying the reason, so the tool fails and the model sees why. `maid.json_encode` / `maid.json_decode` convert. A tool is stopped after 60 s or on Ctrl-C, and its output is capped at 64 KB. "
         "A file that fails to load is skipped with a notice. `maid tools` lists them outside a session. Format and a complete example: docs/tools.md and tools/examples/word-count.lua.\n\n"
         "**Script tools** are a directory each, `.maid/tools/<name>/` or `~/.config/maid/tools/<name>/`, holding `tool.json` and the script: `name` (snake_case, not a built-in), `description`, `parameters` (a JSON schema), "
         "`run` (argv: `[\"python3\", \"main.py\"]`, `[\"sh\", \"main.sh\"]`, `[\"perl\", \"main.pl\"]`, `[\"node\", \"main.js\"]`, `[\"deno\", \"run\", \"main.ts\"]`, `[\"./tool\"]` for a Go binary, `[\"wasmtime\", \"tool.wasm\"]`; the program must be on PATH, a file named in the tool's directory is passed by its absolute path), "
         "`timeout_s` (default 60), `network` (false; true is refused: per-tool network grants are not implemented yet), `reads` and `writes` (globs over the workspace). MAID checks the model's arguments against the schema, judges every declared read and write through the harness "
         "before the script starts (a write glob outside the workspace is refused; the mode decides what is asked), then runs the script in the same bubblewrap sandbox as run_shell with the arguments as JSON on stdin, the workspace writable only when `writes` is non-empty, "
         "no network, killed at the timeout. stdout is the result (capped like command output); a non-zero exit fails the call with stderr attached. `maid tools` lists them with language and declared reads/writes, `maid tools check` validates every manifest, "
         "`maid tools new NAME --lang python|sh|perl|node` scaffolds one. Examples: tools/examples/word_count (python3) and tools/examples/json_pick (sh with jq)."},
        {"task", {"delegate", "subagent", "subagents", "explore"}, "subagents: the task tool, what a child may do, its model, where its transcript goes",
         "*task* *subagents*\n"
         "The model's `task` tool (opencode's name; it was `delegate`) hands one job to a subagent: a second agent in the same workspace, running as one of the **agents** (`:h agent`) whose role lets task run it (`subagent` or `all`). The parent's conversation is not shared: the prompt and an optional context are all the child gets, and its final answer comes back as the tool result, ending with how many steps and tokens it used. "
         "The model is briefed to use `explore` for long reads or searches it does not want in its own context, `plan` for a read-only review of its own change before calling the work done, and `general` for a self-contained piece of editing.\n\n"
         "**Its model**, first match wins: the agent's `model` in settings (your pin); the call's `model`, which may name any preset on the session preset's `subagents` list, higher or lower (anything else is an error listing them with their tiers); the preset's own pick (the same model, or, when it is `limited`, the strongest non-limited one below it: fable-5.1 hands subagents to opus-5.5); the session's model when it is not a preset. The model is told which presets it may use, the default first, and to go lower for wide reads, searches and mechanical work and higher only for a hard reasoning subtask. The call shows as `↳ explore on opus-5.5 (fable-5.1 is limited)`. "
         "A child whose model hits its plan's usage limit continues the same job on that preset's `on_limit` model with the conversation so far (`explore: fable-5.1 hit its usage limit; continuing on opus-5.5`); a second limit ends it with an error naming both. See `:h model`.\n\n"
         "What a child may do is its agent's, capped by the session: a child never gets a wider mode than the session is in, so under a manual session explore's read-only commands are asked about like anything else. Approvals come to you through the parent with the agent named (`explore: $ git log`); the child's tool calls show indented under the task call (`↳ explore: ...`). A child has no `task`, `question` or `todo` tool: one level only, and it reports to the parent. Ctrl-C and a trip stop the child along with the parent; a child stops at its agent's step and token budget, and its tokens count against the session's `:budget`.\n\n"
         "Each child gets its own transcript of kind `sub` in the same home as the parent's, with the parent's id, the agent and why it ran on its model in its start record; `maid sessions` lists it indented under the parent, and `maid -r ID` opens it like any other. The parent's transcript records the task call with the child's path, model, steps and tokens."},
        {"agent", {"agents", "profile", "profiles", "role", "roles"}, "agents for subagents: build, plan, general, explore",
         "*agent* *agents*\n"
         "An agent (opencode's term; MAID called them profiles) is a named narrowing of what an agent may do, used by `task` (`:h task`). Fields: `mode` (the most it allows; the session's mode caps it), `role` (who may run it: `primary` the session only, `subagent` task only, `all` both; opencode calls this `mode`, MAID says `role` because `mode` already means the harness mode here), `description`, `write_paths` (globs over the path relative to the workspace, `src/**`; empty means the whole workspace, and never outside it), `read_outside` (reads beyond the workspace, as the mode allows), `budget_tokens`, `max_steps`, `tools` (an allow-list of tool names; an agent with no write tool on it is read-only: writes and commands that could write are denied), `reviewer` (the smart harness reads its commands and writes, when the session's is on) and `model`. No agent has the network.\n\n"
         "Built in, as in opencode: **build** (primary: everything the session has), **plan** (all: plan mode, read-only tools, reads inside the workspace only, 50k tokens), **general** (subagent: edit mode, writes anywhere in the workspace, all tools), **explore** (subagent: auto-read, read-only tools plus read-only commands, 50k tokens). The older names orchestrator, reviewer, builder and scout still find build, plan, general and explore, in settings and in task calls. `agents = { ... }` in settings (`profiles` is the older key) adds agents by name or narrows the built-in ones; asking for more than the built-in has (a wider mode, a tool it lacks, a bigger budget, reads outside) is an error when settings load, and the fixed rules (trip patterns, secrets, system paths, `permission`, `forbid`) apply to every agent unchanged. A denial names the agent: `DENIED: the explore agent is read-only`."},
        {"permission", {"permissions", "permission-block"}, "allow / ask / deny patterns over tool:argument, in settings",
         "*permission*\n"
         "`permission = { allow = { ... }, ask = { ... }, deny = { ... } }` in settings holds patterns of the form `tool:pattern`, a glob over the tool's argument: `run_shell:pytest *`, `write_file:src/**`, `read_file:/etc/**`, `edit_file:docs/*.md`; `write:` and `read:` stand for any writing or reading tool, a file pattern matches the path as given and relative to the workspace, `~` expands. **deny** wins over **ask** wins over **allow**. An allow entry runs without an approval prompt and without the reviewer in every mode but plan (that is the allow list, `:h allow`; the old `allow = { ... }` key still works and means `run_shell:` entries; a `run_shell:` allow entry matches only one simple command, with no `;`, `&`, `|`, line break, backtick, `$(`, `<` or `>`, so a chained command gets the mode's own decision); an ask entry turns an action the mode would run silently into an approval prompt; a deny entry refuses it with a reason the model reads. A `run_shell:` deny or ask entry matches the whole line and also each command in it: the line split on `;`, `&`, `|`, `&&`, `||` and line breaks, plus what is inside `$(...)`, `(...)` and backticks, each without a leading `{`, `!`, shell keyword or `VAR=value`, so `true; git push`, `x | git push`, `$(git push)` and `GIT_TRACE=1 git push` all meet a `run_shell:git push*` entry.\n\n"
         "The block is additive: it runs after the fixed rules, so it cannot lift a trip pattern, a write to a secret or system path, a forbidden term or an isolated session's fence, and allow entries are ignored for a remote origin, which is always asked. Layers add up, so a project can add restrictions or pre-approve its test command without touching your global file."},
        {"instructions", {"maid.md", "agents.md", "claude.md"}, "standing instructions the model always sees",
         "*instructions*\n"
         "/etc/maid/, then ~/.config/maid/, then the trusted directories (`:h trust`) from the project root (or just under $HOME) down to the workspace, each with its CLAUDE.md, AGENTS.md and MAID.md (lowest priority first) and their .local.md variants; later ones take precedence. `@path` in a file imports another (four hops). Re-read at the start of every turn (32 KB each). An instruction file deeper in the tree is attached once the first time a file under it is read, when a trusted directory's hash covers it. `instructions = { files, read, local_files, imports, extra_dirs }` in your global settings changes all of this (docs/instructions.md). `:instructions` shows what is in effect; `:instructions off`, `--no-instructions` or `load_instructions = false` loads none, and `:system` / `--system` places operator text ahead of all of them (see `:h system`)."},
        {"keys", {"keybindings", "bindings", "vim"}, "the key map",
         "*keys*\n"
         "The input is a small vim and starts in normal mode.\n\n"
         "- **insert**: `i a I A o O` enter it (a count repeats what you type); Enter = new line (`enter_sends` in settings makes it send a one-line input); Esc = normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register, Ctrl-R {reg} a named one; Ctrl-O runs one normal-mode command; ↑ ↓ or Ctrl-P / Ctrl-N prompt history.\n"
         "- **normal**: `h j k l w b e W B E ge gE 0 ^ $ % gg G` move (Enter = down a line); `f{c} F{c} t{c} T{c}` to a character on the line, `;` and `,` repeat; `}` `{` paragraphs, `)` `(` sentences; `x X D C S s J r R ~`; `d c y > < gq gu gU g~` + motion, doubled for the line (`dd`, `>>`, `gqq`, `gUU`); text objects `iw aw ip ap is as i\" i( i[ i{ i<`; `v V`; `p P`; `.` repeats the last change; `q{a-z}` records a macro, `@{a-z}` runs it, `@@` again; `m{a-z}` marks, `'a` / `` `a `` jump; `\"a`-`\"z` registers; `u` undo, Ctrl-R redo; counts (`3w`, `2d3w`, `5.`, `3@a`); `:` commands; `/` searches the conversation, `*` / `#` for the word under the cursor.\n"
         "- **send**: Alt+Enter or `:w` from any mode. `:e` or Ctrl-X Ctrl-E edits the input in nvim.\n"
         "- **conversation window**: Ctrl-W k enters it, Ctrl-W j (Esc, i, Enter) returns; motions including `f t ; ,`, `H M L`, `v V`, `y` yanks to the clipboard, `yy`, `/ n N`, `*` `#`, `}` `{` between messages.\n"
         "- **anywhere**: Shift-Tab cycles modes; Ctrl-C interrupts, then clears, then quits; Ctrl-S pauses a running turn and Ctrl-Q resumes it (`:h steer`); the scroll wheel scrolls.\n\n"
         "`:h KEY` works for single keys too: `:h u`, `:h f`, `:h .`, `:h m`, `:h gq`, `:h J`, `:h r`, `:h Ctrl-W`, `:h Alt+Enter`; `:h motions`, `:h macros`, `:h diff`, `:h highlight`."},
        {"motions", {"motion", "word", "ge", "%", "percent", "bracket", "hml", "*", "#", "star", "hash"}, "word and WORD motions, %, H M L, * and #",
         "*motions* *w* *W* *b* *B* *e* *E* *ge* *gE* *%* *H* *M* *L* *\\** *#*\n"
         "`w` `b` `e` move by words (letters, digits and `_` are one word, a run of punctuation another), `ge` back to the end of the previous word. "
         "`W` `B` `E` and `gE` do the same by WORDs: anything between blanks is one, so `foo-bar.baz` is a single WORD. All take counts and operators (`dW`, `cE`, `yB`, `3W`); `cW` on a WORD changes to its end, as `cw` does.\n"
         "`%` finds the first of `( ) [ ] { }` at or after the cursor on the line and jumps to its partner, across lines and nested pairs; `d%` and `y%` take both brackets and what is between them. It is a jump, so `''` goes back.\n"
         "In the conversation window `H`, `M` and `L` put the cursor on the top, middle and bottom line of what is shown (`3H`: the third line from the top). "
         "`*` searches forward for the word under the cursor and `#` backward, in either window: from the input it searches the conversation and moves there, like `/`; `n` and `N` continue."},
        {"macros", {"macro", "record", "q", "@", "@@"}, "q{a-z} records keys, @{a-z} replays them",
         "*macros* *q* *@* *@@*\n"
         "`q{a-z}` starts recording every key you press into that register (the status line shows `recording @a`); `q` again stops. `@{a-z}` replays it, `3@a` three times, `@@` repeats the last replay with a count of its own. "
         "A motion that finds nothing (`j` on the last line, `f` with no match) stops the replay, so `100@a` on a macro that ends in `j` runs down to the last line and stops, as in vim.\n"
         "The macro is the register's text (`:reg` shows it; special keys are stored as the bytes the terminal sends), so `\"ap` pastes it and a yank into `\"a` is a macro too. Macros live for the session; `.` repeats the last change the macro made, not the macro."},
        {"diff", {"diffs", "diff_added", "diff_removed", "diff_hunk", "preview"}, "how edits and diffs are shown",
         "*diff*\n"
         "The approval prompt for `edit_file`, `multi_edit`, `write_file` and `apply_patch` shows the lines that would change: removed lines in the `diff_removed` style (red by default), added ones in `diff_added` (green), `@@` hunk headers and file headers dim (`diff_hunk`). "
         "Tool output that is a diff (a `git diff` through `run_shell`, a `!git diff` of yours, a patch the model echoes) is coloured the same way in the conversation window. `:set markdown off` shows all of it as plain text. The three styles are set under `style` in settings."},
        {"nvim", {"maid.nvim", "host", "follow_nvim_theme", "diagnostics", "maidsend"}, "MAID inside nvim: maid.nvim and the host connection",
         "*nvim* *maid.nvim* *host*\n"
         "maid.nvim (`maid.nvim/` in the repository) runs MAID in an nvim terminal: `:Maid` opens it, `:MaidSend` (a range sends those lines as a fenced snippet with path and line numbers), `:MaidDiagnostics`, `:MaidQuickfix`, `:MaidToggle`, `:MaidInterrupt` (`<leader>mc`, and `<C-c>` in MAID's own buffers: stops the running turn as the first Ctrl-C does); its own help is `:h maid` in nvim.\n\n"
         "**The host.** nvim sets `$NVIM` to its socket for every job it starts. MAID connects to it as a msgpack-rpc client named \"maid\" only when the socket belongs to this user and the nvim behind it is one of MAID's own parent processes (checked with SO_PEERCRED and /proc); any other socket is refused with the reason at start. The status strip shows `nvim` while connected; `:nvim` says more.\n\n"
         "While connected: text from `:MaidSend` lands in the input (never sent by itself), commands from the plugin run as if typed; `maid_interrupt` (`:MaidInterrupt`) does what the first Ctrl-C does to a running turn, shell command or question, and when MAID is idle only says so; `:e FILE` and `e` at an approval prompt open the file in nvim's editing window; `d` at an approval prompt for a write opens a diff of the proposed change in a new tab (the file against a read-only scratch buffer); "
         "User autocmds `MaidTurnStart`, `MaidToolCall`, `MaidApproval`, `MaidFileWritten`, `MaidTurnEnd` fire in nvim with `data` (session, tool, path, summary, verdict); an approved write runs `:checktime` there so the buffer reloads; "
         "the theme follows nvim's colorscheme live as `nvim:NAME` (`follow_nvim_theme = false` in settings keeps `theme`; `:theme NAME` stops following for the session, `:nvim theme` resumes). `maid --bare` turns all of it off (`:h bare`).\n\n"
         "**Keys.** In MAID's terminal maid.nvim passes `<Esc>` and `<C-c>` through to MAID even over a global terminal-mode mapping (`<C-\\><C-n>` leaves terminal mode there), and it never overwrites a mapping of yours. `:checkhealth maid` in nvim, or `maid nvim keymaps` here, checks maid.nvim's keys, the keys MAID's input needs and llama.vim's against your mappings; after a lazy-lock.json change MAID runs that check once and says if a plugin update added a collision.\n\n"
         "**llama.vim.** `maid nvim setup llama-vim` (in the shell, never as a tool call) writes llama.vim's spec for code completion (docs/models.md) as `maid-llama-vim.lua` in the directory your lazy.nvim spec imports, after showing it and asking; `--dry-run` shows it, `--remove` deletes it, `--yes` answers off a terminal. Without lazy.nvim or an import directory it explains, prints the spec to add by hand and writes nothing; it refuses a file of that name it did not write and a llama.vim already in your spec.\n\n"
         "**Lua.** Your own Lua (settings files, `:lua`, `:luafile`) gets `maid.nvim.exec(code, ...)`, `maid.nvim.buffers()`, `maid.nvim.diagnostics(path)`, `maid.nvim.current()`, which run in nvim. The model gets none of that: only the `diagnostics` tool, judged as a read, and in Lua tools a read-only `maid.nvim.diagnostics` / `maid.nvim.buffers`, each authorised as a read. A model's edit is always a file write through the harness. docs/nvim.md."},
        {"bare", {"--bare", "maid_bare"}, "MAID with nothing from nvim: --bare, MAID_BARE=1, bare = true",
         "*bare* *--bare* *MAID_BARE*\n"
         "`maid --bare` (also `MAID_BARE=1` in the environment, or `bare = true` in settings) starts MAID with nothing from nvim: no connection to the `$NVIM` host even inside nvim's terminal (maid.nvim then falls back to a bracketed paste for `:MaidSend` and to Ctrl-C for `:MaidInterrupt`), "
         "the built-in input highlighter even with `highlight = \"nvim\"` (`:set highlight nvim` is refused), no theme following nvim's colorscheme, `:theme nvim:NAME` refused with a message saying why, no lazy-lock.json notice or `lock≠`, and no keymap check after a lazy-lock change. "
         "MAID's own settings, themes (a saved `nvim-NAME.lua` theme is MAID's own file and loads), Lua and script tools load as usual, and `:e FILE` still runs `$VISUAL` / `$EDITOR` when you ask. The status strip shows `bare`; `:nvim` says what is off.\n\n"
         "`--bare` and `MAID_BARE=1` never connect. `bare = true` in a settings file is known only once the files are read, and the host connection is made before that (so their Lua can use `maid.nvim`): it is dropped right after. "
         "`--ui nvim` (nvim as MAID's interface, `:h ui`) is refused together with `--bare`, since one is all nvim and the other none."},
        {"daemon", {"maid-daemon", "maid daemon", "background-sessions"}, "the daemon: sessions that outlive the window they started in",
         "*daemon* *maid daemon*\n"
         "`maid daemon start` runs one engine in the background (its log in `~/.local/state/maid/engine/daemon.log`), listening on a socket only you can reach (`$XDG_RUNTIME_DIR/maid/engine.sock`). While it runs, `maid` and maid.nvim open their sessions in it rather than in their own process: quitting leaves a working session working (parked once it is done; an idle one is stopped; `leave.quit` in settings, `:h leave`), `:switch` reaches every session the daemon holds, and the next `maid` or `:Maid` picks them up. `maid daemon status` lists them, `maid daemon stop` parks them all (asking first when a turn is running; `--yes` does not ask). `daemon = \"off\"` in settings keeps every session in its own process.\n\n"
         "A command line with a flag only this process's engine can honour (`--system`, `--context`, `--rule`, `--no-record`, `--trust`, ...) runs its session here and says so. The daemon has no nvim host (`:drop` of written files, nvim diffs), and `!cmd` runs in the daemon's environment, not this terminal's.\n\n"
         "`maid daemon unit` prints a systemd user unit, `maid daemon unit install` writes it (enable it yourself: `systemctl --user enable --now maid-daemon.service` starts it now and at each login), `maid daemon unit remove` takes it away. See docs/daemon.md."},
        {"liaison", {"maid liaison"}, "another agent's turns on a session the daemon holds",
         "*liaison* *maid liaison*\n"
         "`maid liaison send ID TEXT` (or `--file FILE`, `-` for stdin) hands a turn to a session the running daemon holds, so another agent can work in a long-lived session while your own window stays attached to it. It connects as the client `liaison`, resumes the session if it is parked, sends the text as ordinary input (behind a running turn, in order) and prints the turn's last reply; `--out FILE` writes it there instead (through FILE.partial, renamed). It never moves a window's focus and never starts a daemon.\n\n"
         "Its turns are not yours. `--as NAME` names the sender (default `liaison`; 1 to 32 of letters, digits, space, `. _ -`; names you go by, such as user, local or owner, are refused). Your window shows the turn under `◆ NAME (liaison)` (the `liaison` style role, and `voices` in settings by name), `maid sessions read` prints it as `[liaison:NAME]`, and maid puts a line of its own above the text the model gets: from NAME through the liaison, not from you, a request rather than your instruction. The sender's lines are quoted under it, so none of them can pass for that line. A voice cannot steer.\n\n"
         "Exit codes: 0 done; 2 the response failed; 3 no daemon runs; 4 the turn waits for an approval, said on stderr as `approval<TAB>ID<TAB>TOOL<TAB>SUMMARY` and left waiting (never with `--unattended`); 5 no reply within `--timeout` seconds (default 600), the turn keeps running. "
         "`--unattended` marks this one turn unattended (the owner is away): every approval in it is denied at once, marked as such and told to the model as a design decision rather than your refusal. The event's `by` is `unattended`; the record's `judged_by` says `unattended`, not `user`. "
         "`maid liaison approve ID APPROVAL yes|no` answers an approval (never always), `maid liaison status ID` prints one line: idle, working, paused or parked, the model, `queued=N` and `approvals=` with their ids. "
         "The harness, approvals and trust judge what it sends as a window's, and it never reads a key. See docs/daemon.md."},
        {"ui", {"--ui", "interface", "maid.nvim-ui"}, "nvim as MAID's interface: --ui nvim, ui = \"nvim\"",
         "*ui* *--ui*\n"
         "`maid --ui nvim` starts nvim with your config and mappings, and maid.nvim (this MAID's copy) as the whole interface: the conversation and the input are nvim buffers (filetypes `maid` and `maid-input`, markdown with treesitter, folds over tool output), approvals, questions and the pause menu are floats, and the engine (`maid --rpc`, this binary, with the same agent flags) is nvim's job. `-c` and `-r` resume the session through the engine. "
         "In the input, Enter (normal mode) or Alt-Enter (insert mode) sends; `/cmd` runs an engine command, `!cmd` a shell command; a message sent while a turn runs goes to that turn at its next step. Ctrl-C cancels the turn, Ctrl-S pauses it, Ctrl-Q resumes it (`:MaidSteer ACTION [NOTE]` for the rest). Inside nvim, `:Maid` opens the same interface in a split. maid.nvim's `:h maid-interface` has the details.\n\n"
         "`ui = \"nvim\"` in settings makes it the default; `--ui tui` runs MAID's own interface once. It is never used inside nvim (`:Maid` is the interface there) or with bare, and a run with `--context`, `--image`, `-i` or `--fork-at` stays in MAID's own interface (`--ui nvim` with those is an error). When nvim is installed MAID says so once at start."},
        {"highlight", {"highlighter", "treesitter", "hl", "builtin", "nvim-highlight"}, "the input's highlighter: builtin or nvim",
         "*highlight*\n"
         "The input is highlighted as markdown while you type. `highlight = \"builtin\"` (the default) is MAID's own renderer. `highlight = \"nvim\"` starts one `nvim --embed --headless` for the session on the first keystroke and asks it, over msgpack-rpc, for treesitter's highlight captures of the text as a markdown buffer: headings, inline and fenced code, bold, italic, links, lists, quotes, and inside fenced blocks the keywords, strings and comments of every language nvim has a parser for (lua, vim, c and query out of the box; it runs with `-u NONE`, so parsers your config installs are not seen). "
         "The captures map to the `hl_heading`, `hl_code`, `hl_keyword`, `hl_string` and `hl_comment` styles and the `md_*` ones. nvim gets 50 ms per keystroke; a late reply paints when it arrives. When nvim is not installed or fails, a notice says so and the built-in highlighter stays. `:set highlight nvim|builtin` switches for the session; the child is reaped on exit. Unrelated to `:e`, which opens your own nvim on the input."},
        {"conversation", {"window", "ctrl-w", "focus", "yank", "clipboard", "search", "/"}, "the conversation window as a vim buffer",
         "*conversation* *Ctrl-W*\n"
         "Ctrl-W k moves the cursor into the conversation window; Ctrl-W j, Esc, `i` or Enter bring it back (Ctrl-W in insert mode deletes a word, so press Esc first unless the input is empty). Inside: `j k h l w b e 0 $ gg G`, Ctrl-D/U/F/B; `}` / `{` next / previous message, `]]` / `[[` next / previous message of yours; `v` / `V` select, `o` swaps the ends; `y` yanks the selection to the register and the system clipboard (wl-copy, xclip, and the terminal through OSC 52); `yy` a line; `/pattern` then `n` / `N` search, smart case. Ctrl-Shift-C in your terminal still copies mouse selections; with the scroll wheel on, select with Shift+drag."},
        {"alt-enter", {"send"}, "sends the input", "*Alt+Enter*\nSends the input from any mode; the same as `:w`. Enter is a new line. nvim has no default Alt mappings, so nothing is lost."},
        {"enter", {"enter_sends", "shift-enter"}, "a new line", "*Enter* *Shift+Enter*\nInsert mode: a new line. Normal mode: down a line. To send, use Alt+Enter or `:w`: the default stays vim-like.\n`enter_sends = true` in settings (or `:set enter_sends on`) makes Enter send a one-line input in insert mode; Shift+Enter or Alt+Enter then inserts the line break, and once the input has more than one line Enter is a line break again (Alt+Enter from normal mode, or `:w`, sends)."},
        {"escape", {}, "back to normal mode", "*Esc*\nInsert or visual mode to normal mode; in the command line, cancels; in the conversation window, back to the input."},
        {"u", {"redo"}, "undo", "*u* *Ctrl-R*\n`u` undoes the last change in the input, `Ctrl-R` redoes (for the agent's file changes see `:h :undo`). Two hundred levels. An insert session counts as one step, and so does a change operator (`cw`, `cc`, `C`, `S`) together with what you typed after it."},
        {"ctrl-r", {"redo"}, "redo", "*Ctrl-R*\nRedo. See `:h u`."},
        {"ctrl-z", {"c-z", "suspend", "^z", "fg"}, "suspend to the shell; fg resumes",
         "*Ctrl-Z*\nSuspends MAID to the shell that started it, like vim; `fg` brings it back with the screen redrawn. A running turn or command is paused with it (the model call resumes on `fg`; a very long pause can time the connection out, which is then retried like any failed call). Not in command-line mode."},
        {"ctrl-c", {}, "interrupt, clear, quit", "*Ctrl-C*\nWhile the agent works: interrupts the turn. While a `!command` runs: stops it. Otherwise: clears the input; pressed twice on an empty input: quits (or `:q`)."},
        {"ctrl-s", {"ctrl-q", "pause"}, "pause and resume a turn", "*Ctrl-S* *Ctrl-Q*\nWhile the agent works, Ctrl-S pauses the turn (the `interrupt` steer): the reply so far is kept, a running tool is stopped, a waiting approval is withdrawn, and the turn waits for you with no timeout. The pause menu then takes Ctrl-Q (resume where it stopped), `s` steer, `d` drop, `f` further (what is in the input goes with them as the note), `k` keep (the reply so far is the answer), `h` halt (it is thrown away); Esc leaves the menu to type a message, which resumes the turn with it. Ctrl-C ends a paused turn. MAID turns the terminal's flow control off, so these never freeze the screen. See `:h steer`."},
        {"shift-tab", {"tab"}, "cycle the mode", "*Shift-Tab*\nCycles manual → auto-read → edit → auto → plan. See `:h modes`."},
        {"ctrl-x", {"ctrl-x ctrl-e", "editor"}, "edit the input in nvim", "*Ctrl-X Ctrl-E*\nOpens the input in $VISUAL, $EDITOR or nvim as a markdown file and loads it back when you quit. Same as `:e`."},
        {"v", {"visual", "visual-mode"}, "visual selection", "*v* *V*\n`v` selects by character, `V` by line, in the input or the conversation window. Then `y` yanks, `d` deletes (input only), `c` changes, `o` swaps the ends, Esc leaves."},
        {"i", {"insert", "a", "o", "ctrl-o", "s"}, "insert mode", "*i* *a* *I* *A* *o* *O* *s* *Ctrl-O*\n`i` inserts before the cursor, `a` after, `I` at the first non-blank, `A` at the line end, `o` opens a line below, `O` above; `s` deletes the character first (`3s` three), `S` the line. A count repeats what you typed: `3ix<Esc>` gives `xxx`, `2ofoo<Esc>` two lines. In insert mode Ctrl-O runs one normal-mode command and comes back (`Ctrl-O $`, `Ctrl-O dw`), Ctrl-R then a register name pastes it. Esc returns to normal mode."},
        {"p", {"paste", "register", "reg", "registers", "ctrl-y"}, "paste; named registers", "*p* *P* *Ctrl-Y* *Ctrl-R* *\"+p* *\"a*\n`p` pastes the register after the cursor, `P` before, `3p` three times; lines (from `dd`, `yy`, `yj`, `dap`) go on their own line below or above. In insert mode Ctrl-Y pastes the register and Ctrl-R {reg} a named one (`Ctrl-R a`, `Ctrl-R \"`, `Ctrl-R +`). `\"+p` (or `\"*p`, or Space then `p`) pastes the system clipboard (wl-paste, xclip or xsel).\n"
         "Registers: `\"a` to `\"z` before a yank, delete or paste use that register (`\"ayy`, `\"ap`, `\"bdiw`); a capital letter appends (`\"Ayw`). The unnamed register (what plain `p` uses) always holds the last yank or delete, from the input or the conversation window. `:reg` shows them all."},
        {"y", {"yank", "leader", "space", "text-objects", "iw", "aw", "ip", "ap", "is", "as"}, "yank: motions, text objects, the clipboard", "*y* *Y* *\"+y* *<leader>y* *text-objects* *ip* *is*\n"
         "`y` + motion yanks (`yw`, `y$`, `yy`, `Y`, `yj`, `y}`); `d`, `c`, `>`, `<`, `gq`, `gu`, `gU` and `g~` take the same motions. Text objects work after an operator and in visual mode: `iw` `aw` (word), `iW` `aW`, `is` `as` (sentence), `ip` `ap` (paragraph: the lines up to an empty line, `ap` with the empty lines after it), `i\"` `a\"`, `i'` `a'`, `` i` ``, `i(` `a(` (also `ib`), `i[` `a[`, `i{` `a{` (also `iB`), `i<` `a<`. So `ciw`, `dap`, `gqip`, `di\"`, `ya(`, `vip`.\n"
         "The system clipboard: `\"+y` (or `\"*y`) before any yank, or the leader (Space by default, `leader` in settings) then `y`: `<leader>y` yanks the line in normal mode or the selection in visual mode. In the conversation window every yank already reaches the clipboard, and `yiw`, `yw`, `y$`, `Y`, `yy` work there too."},
        {"f", {"t", "F", "T", ";", ",", "find-char"}, "f t F T to a character, ; and , repeat", "*f* *F* *t* *T* *;* *,*\n"
         "`f{c}` moves to the next `c` on the line, `F{c}` to the previous; `t{c}` and `T{c}` stop one short of it. `;` repeats the last one, `,` repeats it the other way; a count picks the nth match (`3fa`, `2;`). As operator targets they take the character with `f` and `t` (`df)`, `ct,`, `yf.`) and stop before the cursor with `F` and `T` (`dF(`). Work in the conversation window too."},
        {".", {"dot", "repeat"}, "repeat the last change", "*.*\n"
         "`.` repeats the last change: an operator with its motion or text object (`dw`, `ciw` and the text typed after it, `>>`, `gUiw`), `x`, `p`, `J`, `r`, `~`, or a whole insert session (`A!<Esc>` then `.` appends `!` to another line). Its count is kept (`2x` then `.` deletes two); a count on `.` replaces it (`5.`). `u`, Ctrl-R, motions, yanks and the prompt history are not changes."},
        {"m", {"mark", "marks", "'", "`"}, "marks", "*m* *'* *`*\n"
         "`m{a-z}` sets a mark at the cursor. `'a` jumps to the first non-blank of its line, `` `a `` to the exact position; `''` and ``` `` ``` go back to where the last jump (`G`, `gg`, `}`, `)`, a mark) started. Marks are motions too: `d'a` deletes the lines between here and the mark, `` y`a `` yanks up to it exactly. Marks follow the text when you edit before them and are cleared when the input is sent."},
        {"gq", {"format", "textwidth", "gu", "gU", "g~", "~", "case", "wrap"}, "gq re-wraps, gu gU g~ change case", "*gq* *gu* *gU* *g~* *~*\n"
         "`gq{motion}` re-wraps the lines the motion covers at 80 columns (`textwidth`): `gqq` or `gqgq` the line, `gqip` the paragraph, `gqj` two lines, `gq` in visual mode the selection. Words are re-flowed, the first line's indent is kept, and empty lines stay as paragraph breaks. The cursor lands on the last formatted line.\n"
         "`gu{motion}`, `gU{motion}` and `g~{motion}` lowercase, uppercase and toggle (`gUiw`, `guu`, `gUU`, `g~~`, `gugu`); `~` alone toggles the character under the cursor and moves right (`5~`). In visual mode `u`, `U` and `~` do the same to the selection."},
        {"J", {"join"}, "join lines", "*J*\n"
         "`J` joins the next line onto this one: its leading white space goes and one space is put between them, except when the line already ends in white space, the next line is empty, or it starts with `)`. `3J` joins three lines; in visual mode `J` joins the selected lines. The cursor sits on the join."},
        {"r", {"R", "replace", "replace-mode"}, "r replaces a character, R enters replace mode", "*r* *R*\n"
         "`r{c}` replaces the character under the cursor with `c` and stays put; `3rx` replaces three, and fails when the line is shorter; `r<Enter>` breaks the line there. In visual mode `r{c}` replaces every selected character.\n"
         "`R` enters replace mode: what you type overwrites the text (Enter still breaks the line), Backspace puts the original character back, Esc returns to normal mode; the session undoes as one step and `3Rab<Esc>` writes `ababab`."},
        {">", {"<", "shift", "shiftwidth", "indent"}, "> and < shift lines", "*>* *<*\n"
         "`>{motion}` indents the lines the motion covers by four spaces (`shiftwidth`), `<{motion}` removes up to four; `>>` and `<<` do the line, `3>>` three lines, `>ip` the paragraph. In visual mode `>` and `<` shift the selection, and a count shifts that many times (`2>`). Empty lines are left alone."},
        {"!", {"shell", "bang"}, "run a command in your shell", "*!* *:!*\n`!cmd` as a message, or `:!cmd`, runs cmd in your own shell (not the sandbox) in the workspace. The output shows in the conversation and is handed to the model as context. It starts no turn: on an idle session the agent sees it with your next message, and a line under the output says so; during a turn it reaches the model at the turn's next step. Ctrl-C stops it."},
        {"leave", {"leaving", "leave.switch", "leave.quit", "leave.task", "leave.no_daemon"}, "what becomes of a session you leave: the leave setting",
         "*leave* *leave.switch* *leave.quit* *leave.task* *leave.no_daemon*\n"
         "`leave` in settings says, case by case, what becomes of a session you leave: `bg` (it stays loaded and keeps working), `park` (it stops for now and resumes where it was) or `stop` (it ends; its transcript stays, `maid -r`). The defaults:\n\n"
         "- `leave.switch` (`:new`, `:switch`, `:fork`): `idle = \"park\"`, `working = \"bg\"`, `after = \"park\"`. `ask` is allowed for `idle` and `working`: MAID asks each time.\n"
         "- `leave.quit` (`:q`): `idle = \"stop\"`, `working = \"bg\"`, `after = \"park\"`. A quit mid-turn is a switch to the void: with the daemon the session keeps working. `ask` is allowed for `idle` and `working` here too; a client that closes unasked gets the shipped value.\n"
         "- `after`: what a session left working becomes once its work ends with no window on it.\n"
         "- `leave.task`: `after = \"park\"`, the same for a background task's session, whichever way it was left. A task is its own session: nothing comes from its parent.\n"
         "- `leave.no_daemon = \"park\"`: what a quit does (`park` or `stop`) to a session it would leave running where no daemon can keep it, and to the other sessions in this MAID's background.\n\n"
         "`--bg`, `--park` or `--stop` on `:q`, `:new`, `:switch` or `:fork` decides for that one leave; from a remote client only one that tightens the case. Without the daemon a second window on a session another has open is refused: one engine per transcript. See docs/settings.md."},
    };
    return t;
}

}  // namespace

const std::vector<CommandInfo>& commands() {
    static const std::vector<CommandInfo> c = {
        {"w", {"write", "send"}, "[now]", "send the input (also Alt+Enter)",
         "*:w* *:write* *:send*\n`:w` sends the input, the same as Alt+Enter. `:w now` sends even while the agent is working: the current output is abandoned and the model is asked again with your message included. Without `now`, a message sent while the agent is busy waits for its next step. See `:h queue`."},
        {"ww", {}, "", "send now, even mid-turn (= :w now)",
         "*:ww*\nThe same as `:w now`: sends immediately even while the agent is working. See `:h w`."},
        {"e", {"edit"}, "[FILE]", "edit the input in nvim, or open FILE",
         "*:e* *:edit*\n`:e` opens the input in $VISUAL, $EDITOR or nvim as a markdown file; when you quit, the file becomes the input (one undo step). A non-zero exit leaves the input unchanged. Also Ctrl-X Ctrl-E.\n\n"
         "`:e FILE` opens a file (relative to the workspace). Inside nvim with a connected host it opens there, with `:drop` in the window you edit in, not inside MAID's terminal; otherwise $VISUAL, $EDITOR or nvim runs on it in MAID's place until you quit. `e` at an approval prompt does the same for the file being asked about (`:h nvim`)."},
        {"nvim", {"host"}, "[theme]", "the nvim MAID runs inside: connected or not",
         "*:nvim*\n`:nvim` says whether MAID is connected to the nvim it runs inside and what that gives; `:nvim theme` follows its colorscheme again after a `:theme`. See `:h nvim` for maid.nvim."},
        {"h", {"help", "topics"}, "[topic]", "this help, or :h TOPIC",
         "*:h* *:help* *maid help*\n`:h` alone lists every topic. `:h TOPIC` shows one: a command (`:h w`), a key (`:h u`, `:h Ctrl-W`, `:h Alt+Enter`) or a concept (`:h modes`, `:h harness`, `:h sessions`). A unique prefix is enough; several matches give a list.\n\nOutside a session `maid help` prints the command summary and `maid help TOPIC` one of these pages, both on stdout so they pipe (`maid help lua | less`, `maid help | grep vendor`). `maid help topics` prints the index."},
        {"steer", {"steering", "further", "keep"}, "ACTION [NOTE]", "steer the running turn: steer, drop, further, interrupt, keep, halt",
         "*:steer* *:steering* *steering*\n"
         "`:steer ACTION [NOTE]` acts on the running (or paused) turn. **steer** stops the reply now and goes on with your note (\"The user redirected you: NOTE\"). **drop** stops it, removes the paragraph being written (`steering.drop_trim`: none, sentence, paragraph, all) and tells the model to leave that topic, with your note if any. **further** lets the model finish its step, then asks it to go deeper. **interrupt** pauses (Ctrl-S). **keep** ends the turn with the reply so far as the answer. **halt** throws the reply away and tells the model so (`steering.halt_message`). steer, drop, interrupt, keep and halt withdraw a waiting approval (the call says \"not run: the user redirected\") and stop a running tool (steer and drop wait for it with `steering.on_running_tool = \"wait\"`).\n\n"
         "`:steering` shows the settings in force and which file set each: which actions a session accepts and from which clients, drop's trim, the halt message, what a ban entry may name. A ban entry can steer by itself: `{ \"helm chart\", steer = \"drop\", note = \"...\" }` (`:h ban`, docs/bans.md). The protocol side is OpenAI's `response.steer` and MAID's `maid.steer` (docs/design/engine-protocol.md, section 11)."},
        {"tier", {"protocol-tier", "protocol_tier", "tiers"}, "[open|guarded]", "the session's protocol tier: how closely the engine checks itself",
         "*:tier* *protocol tier*\n"
         "Every message between the engine and an interface is checked against MAID's protocol at the session's tier. **guarded** (the default) runs every check and logs what fails to `~/.local/state/maid/engine/protocol.log`, telling you once per kind. **open** checks nothing: for experiments where speed matters more; the status strip shows PROTOCOL OPEN. **airtight** refuses what fails; it needs a build that passed conformance, and this one does not run it yet.\n\n"
         "`:tier` shows the session's tier and where it came from; `:tier open|guarded` changes it for this session: tightening from any client, loosening only from this machine and never below the tier it opened at. Where it comes from: `protocol_tier` in your global settings (the default), `protocol_tiers = { [\"~/scratch\"] = \"open\" }` or `maid trust DIR --protocol TIER` for a directory (the recorded one first; an enrolled directory is a floor), `agents.NAME.protocol_tier` for an agent. A session keeps the tier it started at when resumed. See docs/design/protocol-security.md."},
        {"harness", {}, "[smart|dumb]", "the reviewer on (smart) or the rule list alone (dumb)",
         "*:harness*\n`:harness` shows which is in force, and under the smart harness the reviewer's model, why it was chosen and the tokens it has spent; `:harness smart` turns the model reviewer on, `:harness dumb` off. Switching to dumb while in auto mode drops to edit until you confirm auto again. See `:h harness`."},
        {"mode", {}, "NAME", "set the agent mode",
         "*:mode*\n`:mode manual|auto-read|edit|auto|plan`. Shift-Tab cycles them. See `:h modes`."},
        {"model", {}, "[NAME]", "switch model, or list presets and providers",
         "*:model* *presets*\n`:model NAME` switches (when the agent is idle). NAME can be a **preset**: one short name that sets the model, its context window and thinking: `fable-5.1`, `opus-5.5` (Opus 5.5, 1M context, thinking on), `sonnet-5`, `haiku-4.5`, `qwen-4b`, `qwen-9b` (text, 16k), `qwen-9b-vision` (with its projector, 8k); written loosely (`Opus 5.5`, `opus55`) is fine. "
         "A preset also has a **tier** (higher is stronger: fable 50, opus 40, sonnet 30, haiku 20, the 9Bs 12, the 4B 10), **limited** (your plan caps its usage; fable-5.1 ships limited), **subagents** (the presets a subagent of it may run on, higher or lower: the four Anthropic ones for each other, the three local ones for each other, so a local session's data stays here unless you add a cloud preset), **subagent** and **on_limit** (where a subagent goes, and where it continues after a usage limit; empty means the rule in `:h task`). `:model` alone lists each preset with its tier, whether it is limited, where its subagents run and its reviewer, then the providers. "
         "When the session's own model hits its usage limit nothing switches by itself: the error says which `:model` continues on the next tier. `models = { ... }` in settings adds presets or changes these field by field (`models = { [\"opus-5.5\"] = { limited = true } }`; see docs/settings.md). Or a plain model: any GGUF under the models directory by name (`llamacpp/Qwen3.5-9B-Q4_K_M`; the server loads it on demand and unloads the previous one), `llamacpp/current` (the one `maid vendor use` linked), `anthropic/claude-opus-5-5`, `deepseek/deepseek-v4-pro` (or the presets `deepseek-pro`, `deepseek-flash` and their `-nothink` twins, metered: billed per token to $DEEPSEEK_API_KEY), ... Switching to a remote provider prints what will leave this machine, and to a metered one says METERED. See `:h providers`."},
        {"models", {}, "", "models the current provider serves", "*:models*\nLists the models the current provider serves. On llama.cpp that is every GGUF under the models directory, by file name (a subdirectory holds a GGUF plus its mmproj); one is resident at a time and `:model llamacpp/NAME` switches. `maid models` is the catalog of models MAID can install (`:h models`)."},
        {"think", {}, "on|off", "let the model reason first", "*:think*\n`:think on` asks the model to reason before answering: slower, better on hard problems. Anthropic models then use the provider's `think_effort`."},
        {"set", {}, "markdown|mouse|enter_sends on|off, highlight nvim|builtin", "rendering, mouse and input toggles",
         "*:set*\n`:set markdown off` shows the conversation as raw text (diffs too); `on` renders it. `:set mouse on` also makes a left click on a tool call or result fold or unfold it (like `za`); `:set mouse off` stops the scroll wheel and gives the terminal its normal mouse selection back. `:set tooldetails on` shows tool output in full instead of an 8-line preview (in the conversation window `za` folds or unfolds one result, `zR` unfolds all, `zM` folds all). `:set highlight nvim|builtin` picks the input's highlighter (`:h highlight`); `:set enter_sends on|off` makes Enter send a one-line input (`:h enter`). markdown, mouse, highlight and enter_sends persist through settings.lua."},
        {"status", {"services", "docker", "ready_pattern", "health"}, "", "harness, services, model, session",
         "*:status* *maid status*\nThe harness state, every service with where it runs and a quick action, then this session: its id and title, transcript file, the model and whether it is remote (and the model that answered the last call, when that is another), thinking and its reasoning effort, the context used of the window with the turns and model calls so far, the cost estimate, the mode, queued messages, the harness, the protocol tier, the workspace and whether its project files are trusted, whether a daemon holds the session, the background tasks, the user-defined tools and the model's todo list. `:usage` has the figures per model.\n\n"
         "A service line reads `comfyui: running [host]  pid 1234 · http://127.0.0.1:8188`; `[docker]` means a container MAID started (`container maid-comfyui`), from a `services/*.json` with `\"runtime\": \"docker\"`, an `image`, optional `volumes` (host paths only under the state, models or vendor trees), `env` and `\"gpu\": true` for `--gpus all`. Containers bind `127.0.0.1:<port>` only, like host services; `maid logs` reads `docker logs`. `services/comfyui-docker.json.example` is the shape; MAID never pulls an image.\n\n"
         "Under a running service is what it holds: a llama server's resident model (from `/v1/models`) or `no model resident`; for ComfyUI the VRAM in use and whether its queue is busy (`/system_stats`, `/queue`). `starting` means the port is not open yet, or the service file's `ready_pattern` (a POSIX extended regex: `To see the GUI go to` for ComfyUI, `listening on` for llama-server) has not shown up in its output since this start; `maid up` waits for both. `maid doctor` adds whether ComfyUI's torch and the NVIDIA driver agree on CUDA; `maid setup` walks a first install.\n\n"
         "In `maid status` on a terminal the state is coloured by the theme: `running` green, `starting` yellow, `stopped` dim, `failed` red (it exited without `maid down`); a service that uses the GPU ends its line with a `GPU` tag, taken from `needs_gpu` in its file. Colour is off when `NO_COLOR` is set and not empty, when stdout is not a terminal, and with `--text-base`, which also prints one tab-separated record per line instead (`service NAME STATE RUNTIME gpu|cpu WHO URL DETAIL`, `-` for an empty field). `maid gpu`, `maid models` and `maid daemon status` take `--text-base` too (docs/settings.md, Colour and plain output in commands)."},
        {"usage", {}, "", "tokens, estimated spend, concurrency and rate limits per model",
         "*:usage*\n"
         "What the status strip shows (context used, total tokens, `~0.0123 USD est.`) and more. Per model this session used: the requests, the tokens (input as cache hit and cache miss, output) and the estimated spend, costed at the catalog's prices by the model that was asked for, peak or off-peak by the UTC time of each call (`:h models`); a local model shows tokens and no spend, a model the catalog does not price says so. Then the account's concurrency (requests open of `max_concurrent`, and how many wait), and per provider the environment variable that supplies its key and whether it is set (never the key), with any rate-limit hold or open circuit breaker; then the session's total, with a subagent's and the reviewer's calls in it, and the context used.\n\n"
         "Every figure is an estimate: the providers' own bill is the truth. MAID keeps no token totals across sessions; the last line gives what it does keep, each session's cost estimate in `<state>/costs.json` (never sent anywhere). Open requests and holds are shared by every session in the same process (the daemon's, or this one's). Reasoning tokens are counted in the output, not shown apart. Plain text: `--rpc` answers it as lines through `maid.session.command`, and a remote client may run it."},
        {"todo", {"plan"}, "", "the model's plan (the todo tool)",
         "*:todo*\nShows the list the model keeps with its `todo` tool during multi-step work: `[x]` done, `[ ]` not yet. The status strip shows `todo n/m done` while there is one; `:clear` drops it. See `:h tools`."},
        {"tools", {}, "", "the model's tools, built in and yours",
         "*:tools* *maid tools*\nLists the built-in tools, MAID's helpers, every user-defined Lua tool with its file and description, and every script tool with its language, manifest and declared reads/writes, plus files that were skipped and why. Outside a session `maid tools` does the same; `maid tools check` validates the manifests and `maid tools new NAME --lang LANG` scaffolds one. Writing one: `:h tools` (the topic) and docs/tools.md."},
        {"up", {}, "SERVICE", "start a service", "*:up*\n`:up llamacpp` starts a service MAID manages (llamacpp, comfyui). Refused while the harness is tripped."},
        {"down", {}, "SERVICE", "stop a service MAID started", "*:down*\n`:down llamacpp` stops it. MAID only stops what it started."},
        {"init", {}, "", "scaffold MAID.md and .maid/settings.lua, then draft the MAID.md",
         "*:init*\nCreates `.maid/settings.lua` and a `MAID.md` placeholder in the workspace, then asks the agent to look over the project and write the MAID.md (it will ask before writing in manual mode). A project with a MAID.md keeps its transcripts under sessions/projects/. `maid init` does the scaffolding only.\n\n"
         "This session joins them: when it worked in this workspace throughout (nothing written outside it, at most `init_move_outside_reads` files read outside, 3 by default; work before a `:cd` here counts as outside), `:init` moves its transcript into `projects/<encoded workspace>/` and says so. With more outside work it asks first; a `--no-record` session or one already there stays. Records written during the move are held in a `.pending` file beside the destination and appended once it is in place. `maid sessions rehome ID project` does the same from the shell, and `maid sessions rehome ID general` moves it back. docs/sessions.md"},
        {"cd", {"cwd", "pwd"}, "[PATH|-|PLACE]", "change the session's workspace",
         "*:cd* *:cwd* *:pwd*\n`:cd PATH` moves this session's workspace: an absolute path, one relative to the workspace, `~/...`, or a place name from `:path` (`:cd workflows`; Tab completes them). `:cd -` goes back to the previous one; `:cd` alone or `:pwd` shows where it is.\n\n"
         "In one step the harness root moves (what counts as inside, the sandbox's writable directory, relative paths), so do shell commands and `!`, the project settings layers are read again as a start there would read them (command-line flags still win; one notice lists the settings that changed), and the instruction files there replace the old ones: the model gets a system note saying the workspace moved and what instructions now apply. The transcript gets a `workspace` record. The tripwire, the forbid list, the harness choice and the locks are not changed by a directory's settings.\n\n"
         "Only while the agent is idle, and only by you: no tool changes it and a remote request cannot. A confined session (tripwire = \"isolated\") moves only within the directory it was started in. The status strip shows the directory after a `:cd`."},
        {"settings", {}, "", "which settings files are in effect",
         "*:settings*\nLists the settings files that were read, nearest last: the global file, then `.maid/settings.lua` and `.maid/settings.local.lua` (or their .json fallbacks) from just under $HOME down to the workspace. Shows where this session's transcript home resolved to. See `:h settings`."},
        {"ban", {"bans", "banned", "logit_bias", "logit-bias", "pattern", "patterns", "regex"}, "[add TEXT|pattern REGEX|token ID|remove N|clear]", "strings, regex patterns and tokens the model must not produce",
         "*:ban* *--ban* *--ban-pattern* *bans*\n"
         "**Files**: wherever a ban is given, `@path` stands for a file with one entry per line (`#` lines and blank lines skipped, `~` expands): `--ban @~/bans/phrases.txt`, `--ban-pattern @~/bans/tics.re`, `:ban add @file`, `:ban pattern @file`, `:ban token @file`, and `strings = { \"@~/bans/phrases.txt\" }` in settings.\n\n"
         "**Regex bans** (`:ban pattern REGEX`, `--ban-pattern`, `bans.patterns` in settings; POSIX extended, so `(as an ai|i cannot|certainly!)` with `:ban case off`) are matched over the streamed reply the same way as string bans: the reply is cut before the match reaches the screen, the model is told what it started and asked to continue, and after `retries` attempts the match is replaced. A regex cannot say how much more text might complete a match, so the last `window` characters (64) are held back until more text arrives; keep the window longer than any phrase you ban. One regex holding every tic a model is known for is the intended use: see docs/bans.md for a starter set. This is done by MAID after the fact, not by constrained decoding: grammar-guided decoding on llama.cpp or vLLM can say what a reply must look like, not what it must not contain.\n\n"
         "**String bans** work with every provider: the reply streams through a filter that holds back a short tail and cuts the call the moment a banned phrase would appear, before it reaches the screen; the clean part is kept, the model is told which phrase was banned and asked to continue; after `retries` attempts (3) the phrase is replaced by `replacement` (\"[banned]\") instead. `:ban add TEXT`, `--ban TEXT` (repeatable), or `bans = { strings = { ... } }` in settings; layers add up. `:ban case off` matches regardless of case.\n\n"
         "**Token bans** map to `logit_bias` (the token's probability goes to minus infinity, so the model takes another path) on OpenAI-compatible providers: llama.cpp server, vLLM, LM Studio and the like. `:ban token 1234` bans an id; `:ban token TEXT` bans text, which llama.cpp-style servers accept in logit_bias and which is a string ban everywhere else. Anthropic has no logit bias, so ids are ignored there with one notice.\n\n"
         "**Samplers**: see `:h sampling`."},
        {"sampling", {"sampler", "xtc", "temperature", "top_k", "min_p"}, "[KEY VALUE|xtc P [T]|unset KEY|reset]", "sampler settings: temperature, top_k, min_p, XTC, ...",
         "*:sampling* *sampling* *xtc* *--xtc* *--sampling*\n"
         "On the command line: `--xtc 0.5` or `--xtc 0.5,0.1` (probability, threshold) and `--sampling KEY=VALUE` (repeatable) set them for one run, over the settings. In a session: `:sampling` shows what is sent with every request; `:sampling temperature 0.7`, `:sampling min_p 0.05`, `:sampling seed 7` set a key for this session (over `sampling = { ... }` in settings and `providers.<name>.options.sampling`, which wins over the global table); `:sampling unset KEY`, `:sampling reset`.\n\n"
         "**XTC** (exclude top choices) is a sampler that, with probability P, drops every token above threshold T except the least likely of them, so the model is pushed off its most predictable path, which is where refusals and stock phrases live. `:sampling xtc 0.5 0.1` (or `sampling = { xtc_probability = 0.5, xtc_threshold = 0.1 }`). It exists in llama.cpp's server, koboldcpp, text-generation-webui and other llama.cpp-based OpenAI-compatible servers, where MAID sends it as `xtc_probability` / `xtc_threshold`. Anthropic's current models take no sampling parameters, so nothing is sent there; the keys are kept for when you switch provider. Bans and XTC combine: XTC changes what the model is likely to say, bans catch what it says anyway."},
        {"image", {"img", "picture", "attach", "drop", "drag"}, "[FILE|clear]", "a picture for the next message (vision models)",
         "*:image* *--image* *drag and drop*\n"
         "`:image FILE` attaches a picture (png, jpg, webp, gif, up to 20 MB) to your next message; `:image` lists what is attached, `:image clear` drops it; `--image FILE` (repeatable) does it for the first prompt from the command line. **In the text**: write a markdown image or link whose target is an image file, `![the panel](output/panel_1.png)` or `[this](~/Pictures/ref.jpg)`, anywhere in the message, and MAID attaches the file on send and leaves an `[image: alt]` marker in its place; relative paths are from the workspace. **Drag and drop**: a file dragged from a file manager onto the terminal arrives as its path, quoted or with escaped spaces; when a message is that path alone, or starts with one in that dropped shape, it is attached the same way. A plain path typed or pasted inside a sentence stays text, so the agent can be asked to read it instead. The model has to be a vision one: both Qwen3.5 GGUFs here carry their projector (`llamacpp/Qwen3.5-9B-Q4_K_M` is the stronger reader), and Anthropic takes images. Old pictures are the first thing compaction removes after old tool results."},
        {"forbid", {"forbidden", "blocklist", "halt"}, "[TERM|remove N]", "terms no tool call may contain; halted in every mode",
         "*:forbid* *forbid*\n"
         "A list of terms the agent may not search for, run, read, write or pass in any argument, in any letter case; a term written `/.../` is a POSIX extended regex (case-insensitive). The built-in list halts one word with any prefix or plural, and any three letters from f, m, o with at least one o, in any order, as a whole word (fmo, moo, omo, oom, ooo, foo, ...); ffm and mmf carry no o and are not on it. A tool call containing one is halted before anything runs, with a denial the model reads and a notice you see; the rule sits in front of the tripwire patterns, the mode, the allow list and the reviewer, so the dumb harness enforces it exactly as the smart one does, and a Lua tool's inner actions are covered too. `:forbid` lists, `:forbid TERM` adds for this session, `:forbid remove N`; `forbid = { ... }` in settings keeps them (layers add to the built-in list)."},
        {"allow", {"allowlist", "whitelist", "permission"}, "[PATTERN|remove N]", "commands pre-approved: no asking, no review",
         "*:allow* *allow*\n"
         "A list of command patterns (glob over the whole command line: `pytest *`, `npm test`, `git status*`) that run in every mode but plan without an approval prompt and without the reviewer. MAID's own helpers are on it by default (`maid-storyboard*`, `maid-workflow-edit*`, `maid-danbooru-tags*`, `maid path*`, `maid status*`, `maid artifacts*`, `maid sessions*`); their looking-only shapes count as read-only, so they run in plan and auto-read modes too. A command with a prefix (`cd x && maid-storyboard ...`) does not match a pattern that starts with the program name; add such a pattern with `:allow` if you want it. Trip patterns (sudo, rm -rf /, ...) are checked first and still win, and the sandbox still applies. `:allow` lists, `:allow PATTERN` adds for this session, `:allow remove N`; `allow = { ... }` in settings keeps them (layers add up). A repeated identical call that is harmless (a read, a read-only or allowed command) is refused after three and ends the turn after five; only a repeated write or other command trips the lock."},
        {"rule", {"rules", "standing-rule"}, "[TEXT|remove N|clear]", "a standing instruction, reminded every turn",
         "*:rule* *--rule* *rules*\n"
         "A one-line instruction the model is asked to follow: `:rule Always answer in French`, `--rule TEXT` (repeatable), `rules = { ... }` in settings (layers add up). Rules travel with the operator text: they lead and close the system prompt and are appended to each of your messages as the model sees them, which is what a small model needs once tools are attached. `:rule` lists them, `:rule remove N`, `:rule clear`; a change mid-session is appended as a system note. A rule is a request the model can still drop; when the first words of every reply must be exact, use `:prefix` instead. `:system` is the same mechanism for a whole block of operator text."},
        {"ctx", {"context-size", "context-window", "ctx-size"}, "[N]", "the context window in tokens; restarts the local server to match",
         "*:ctx* *--ctx* *context*\n"
         "One number drives both the local llama.cpp server (`--ctx-size`, via `${MAID_CONTEXT}` in services/llamacpp.json) and MAID's readout and auto-compaction. `:ctx` shows it; `:ctx 32768` sets it and, if the server is running with another size, restarts it (every model it serves gets the new size). `--ctx N` on the command line does the same at startup, including `maid up llamacpp --ctx N`; `context = N` in settings makes it the default. The 4B at Q4_K_M fits 32k on an 8 GB card; the 9B is tighter. The side server has its own number: `:h ctx2`."},
        {"ctx2", {"context-2", "context_2"}, "[N]", "the side server's context window (llamacpp-2); restarts it to match",
         "*:ctx2* *--ctx2* *context_2*\n"
         "The same for the second llama server, `llamacpp-2` on port 8082 (`${MAID_CONTEXT_2}` in services/llamacpp-2.json, default 8192): `:ctx2` shows it, `:ctx2 16384` sets it and restarts that server when it runs with another size, `--ctx2 N` at startup, `context_2 = N` in settings. Two models resident at once share the card, so the side server's window is the one to lower first; `maid gpu` says whether the pair fits. See docs/llamacpp.md, Two servers."},
        {"prefill", {"prefix", "assistant-prefill"}, "[TEXT|@FILE|off]", "every reply starts with these literal words (the model continues them)",
         "*:prefix* *:prefill* *--prefix* *--prefill* *prefill*\n"
         "Puts TEXT in the model's mouth: it is sent as the opening of the assistant turn, so the model continues from it instead of being asked to comply. Give the words themselves, not a rule about them: `:prefill Sure thing! ` makes every reply begin \"Sure thing!\"; `:prefill Always start with Sure thing!` makes every reply *be* that sentence, because the model completes an instruction-shaped opening with an end of turn. Rules go in `:system`. Where a system prompt is a request a small model may drop, this is a guarantee: the reply starts with TEXT every time. `--prefill TEXT` (or `@file`), `prefill = \"...\"` in settings, `:prefill TEXT` in a session, `:prefill off` clears, `:prefill` shows. The prefill is shown and stored as the start of the reply. Two things to know: a prefilled turn almost never calls a tool (the model is already answering), so use it for chat-style rules rather than agentic work; and with thinking on, the prefill skips the thinking, since the answer has begun. Works on llama.cpp and Anthropic."},
        {"system", {"system-prompt", "operator"}, "[TEXT|@FILE]", "operator instructions placed first in the system prompt",
         "*:system* *--system* *system_prompt*\n"
         "Text that leads every system prompt, before MAID's own briefing and before any instruction file, marked as operator instructions that take precedence, repeated at the very end of the prompt, and appended to each of your messages as the model sees them (the transcript keeps your words as typed). That last part is what makes a small model obey: measured with a local 4B, every system-side placement was ignored once tool schemas were attached, and the rule closing the user turn was followed every time. The way to front-load behaviour. Set it with `--system TEXT` or `--system @~/prompts/reviewer.md` on the command line, `system_prompt = \"...\"` or `\"@path\"` in settings, or `:system TEXT` / `:system @file` in a session (idle only; it applies from the next turn and is appended to a resumed conversation). `:system` alone shows it. Independent of instruction files: combine with `--no-instructions` to run on the operator text alone."},
        {"instructions", {"no-instructions", "load_instructions"}, "[on|off]", "the instruction files in effect, or switch them off",
         "*:instructions* *--no-instructions*\nLists the instruction files the model sees, re-read every turn. `:instructions off` stops loading them (global, project and nested) for the next turns; `on` brings them back. `--no-instructions` on the command line or `load_instructions = false` in settings starts that way. Independent of `:system`. See `:h instructions`."},
        {"trust", {"trusted", "--trust", "maid-trust", "trust-level", "strictness", "global_lua", "restricted-lua", "sandbox", "lua_memory_mb"}, "[PATH] [--lua full|sandbox|restricted] [--level strict|standard|relaxed] | --list", "trust a project directory: its settings, instructions and tools",
         "*:trust* *maid trust* *--trust* *trust* *sandbox*\n"
         "A project directory is one on the chain from the project root (or just under $HOME) down to the workspace holding `.maid/` or an instruction file (`CLAUDE.md`, `AGENTS.md`, `MAID.md`, their `.local.md` variants, or the names in `instructions.files`; for the workspace, also ones in its subdirectories). Its trust hash covers every instruction file it holds, nested ones and what they import inside it. Until you trust it, its `.maid/settings.*` are not applied, its instruction files are not given to the model and its `.maid/tools/` are not loaded; a notice at start names what was skipped. "
         "The first time MAID starts in one, it asks on the terminal before the screen is drawn, listing its settings, instruction and tool files: **t** trust fully (its Lua runs as you), **s** trust sandboxed (its Lua runs in a child process that cannot reach the system), **n** not now (untrusted this session), **v** never (remembered). A directory MAID used before this check existed is asked about like any other, and says so. `:cd` into a project directory asks the same, in a modal, before its settings are read. "
         "`$HOME` and `/` are never projects; their files are ignored with a notice. Your global config (`~/.config/maid/`) is always trusted.\n\n"
         "**Lua level** (`--lua`), how a trusted directory's settings.lua runs: **full** (the default for \"trust it\": the whole standard library, as you), **sandbox** (a child process with a memory cap, a CPU limit, no file descriptors and the restricted environment; its table comes back as data), **restricted** (the restricted environment in MAID's own process, with early memory checks). Your own files (settings.lua, themes, diction.lua) run at `global_lua`, `full` by default.\n\n"
         "**Tier** (`--level`), how often a trusted directory is asked about again when its files change (kept with a SHA-256 of each file in `<state>/trust.json`, 0600): "
         "**strict** asks about every change; **standard** (the default) lets your own changes pass (an uncommitted edit in a git working tree, or commits whose author is one of your identities) and asks about anything else (a commit by someone else, a checkout that moved the history, a new untracked file, any change outside a git working tree); "
         "**relaxed** lets edits pass and asks only when a change widens what the project can do (a new `permission.allow` entry, a removed ask or deny entry, a looser mode, `harness = \"dumb\"`, a tripwire other than machine, `allow_isolated`, a provider, a new tool or a tool manifest whose run, reads or writes changed, a new instruction file, or any settings.lua change in a fully trusted directory). A change that passes is named in a one-line notice. "
         "Your identities are `trust_identities` in the global settings, else `git config --global user.email`; never a repository's. Both axes come only from you: `trust_strictness` and `trust_levels` in the global settings, `:trust --level L --lua L` / `maid trust PATH --level L --lua L`, kept in trust.json. A project's own `trust_*`, `global_lua` or `lua_memory_mb` key is ignored with a warning.\n\n"
         "`:trust` trusts every untrusted project directory of this workspace, `:trust PATH` one directory; `:untrust [PATH]` forgets it (asked again at the next start; settings already applied stay until then). Outside a session: `maid trust [PATH] [--lua L] [--level L]`, `maid trust --list`, `maid untrust PATH`. "
         "A headless run (`maid -p`) or a run off a terminal asks nothing: an untrusted directory stays untrusted unless `--trust` (fully) or `--trust=sandbox` is given, for that run only. "
         "The agent can never change trust: a write to trust.json or a `maid trust` command trips the smart harness and is refused by the dumb one. A remote device (maid-server) uses only directories trusted here; `POST /api/trust` needs a step-up proof, refused until accounts exist.\n\n"
         "**The restricted environment** (sandbox and restricted): base, string, table, math and bit; `os.getenv`, `os.time`, `os.date`, `os.clock`; `load` for text only; no io, require, dofile, ffi or jit; 2 s at most; a heap of `lua_memory_mb` (256 MB) at most, with `string.rep` and `table.concat` refusing a bigger result first; a table of data only (a function in it is an error naming its key). Using anything else is an error naming the file and line. See docs/harness.md and docs/settings.md."},
        {"untrust", {"distrust"}, "[PATH]", "forget a project directory's trust (asked again at the next start)",
         "*:untrust* *maid untrust*\n`:untrust` forgets the trust of every project directory of this workspace, `:untrust PATH` of one. Its instructions stop from the next turn; settings and tools already loaded stay until MAID restarts, when it is asked about again. See `:h trust`."},
        {"session", {}, "", "where this transcript is", "*:session*\nThis session's file and the sessions directory. See `:h sessions`."},
        {"new", {}, "[--bg|--park|--stop] [DIR]", "start another session in this MAID and go to it",
         "*:new*\n`:new` starts another session here (in DIR with `:new DIR`, as `:cd DIR` would move it) and puts it in focus. The one you leave goes to the background when it is working (and is parked once its work is done) and is parked when it is idle, unless `--bg`, `--park` or `--stop` says otherwise or `leave.switch` in settings does (`\"ask\"` asks each time). See `:h switch` and `:h leave`."},
        {"switch", {"sessions-menu", "switcher"}, "[--bg|--park|--stop] [ID|TITLE]", "go to another session; alone, the switcher",
         "*:switch* *:bg* *:park* *:stop* *:fork* *switcher*\n"
         "One MAID holds several sessions at once, each with its own model, mode and workspace; their turns run in parallel (a single-slot local server still takes them one at a time). The top strip counts the others and says when one is waiting for you or has finished.\n\n"
         "- `:switch` opens the switcher: every other session, what it is doing (working, waiting, finished, idle, parked), its model and the directory you land in; j/k and Enter go there, Esc stays. `:switch ID` (a unique prefix, or the title) goes straight there; a parked session, or any transcript by id, is resumed.\n"
         "- `:fork` forks this session into a second one (both stay open; the fork points at this transcript as it stands) and goes to the fork.\n"
         "- `:bg` sends this session to the background, where it keeps working, and opens the switcher to pick where to go.\n"
         "- `:park` ends this session for now (it leaves memory, stays in the switcher and resumes where it was, messages that waited to run included); `:stop` ends it outright (it leaves the switcher and stays an ordinary transcript, `maid -r ID`). On this session both open the switcher first; `:park ID` and `:stop ID` end another one. A working session is asked about first, since its turn is interrupted.\n"
         "- Leaving a session through `:new`, `:switch` or `:fork`: `--bg`, `--park` or `--stop` says what happens to it; without one, `leave.switch` in settings does (`:h leave`).\n\n"
         "Without the daemon, background sessions live in this MAID: `:q` with one still working says so first, and quitting parks them all (their turns interrupted, each resumes where it stopped; `leave.no_daemon`)."},
        {"fork", {}, "[--bg|--park|--stop]", "fork this session into a second one and go to it", "*:fork*\nSee `:h switch`."},
        {"bg", {"background"}, "", "send this session to the background and pick another", "*:bg*\nSee `:h switch`."},
        {"park", {}, "[ID]", "end this session (or ID) for now; it resumes where it was", "*:park*\nSee `:h switch`."},
        {"stop", {}, "[ID]", "end this session (or ID); it stays a transcript", "*:stop*\nSee `:h switch`."},
        {"gpu", {"vram", "memory"}, "[free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui] | load llamacpp-fim]", "who holds the card; free memory without stopping anything",
         "*:gpu* *maid gpu* *vram* *out of memory*\n"
         "`:gpu` (and `maid gpu`) shows who holds the card: the model resident in each llama server (llamacpp on 8081, llamacpp-2 on 8082, the code completion server llamacpp-fim on 8084), the whisper server's model (8083), ComfyUI's own view of VRAM used and total, and one sentence on whether the models fit the card (each GGUF's size on disk plus an estimated KV cache for its context, whisper's model plus its buffers, against the card's total from ComfyUI or nvidia-smi). `:gpu free` unloads every llama server's model (they reload on the next request) and asks ComfyUI to unload its models and release its caches; `:gpu free llamacpp`, `:gpu free llamacpp-2`, `:gpu free llamacpp-fim` or `:gpu free comfyui` does one side. whisper-server cannot unload without stopping, so `:gpu free whisper` only says what it holds and that `maid down whisper` releases it. Nothing is stopped. The completion server never loads its coder by itself (llama.vim asks at every pause, which would take the card back from ComfyUI): `maid gpu` shows it as loaded, unloaded or not linked, `:gpu load llamacpp-fim` loads it, and stopping the service that unloaded it (`maid down comfyui`, `maid down whisper`) loads it again when it was loaded before. When a service fails to start, `maid up` reads its log and says why in plain words: a CUDA out of memory names who holds the card and this command; a port in use, a missing Python module and a driver mismatch are recognised too. Starting a service marked `needs_gpu` (ComfyUI, whisper) frees the llama servers' models first by itself, and starting whisper also asks a running ComfyUI to unload its models; a llama server loads nothing at start, so starting one never evicts the other."},
        {"lazylock", {"lazy-lock", "lazy_lock"}, "[record|diff]", "is nvim's lazy-lock.json as recorded? record it, or diff per plugin",
         "*:lazylock* *maid lazy-lock* *lazy-lock.json* *lock≠*\n"
         "MAID keeps a SHA-256 of nvim's lazy-lock.json (lazy.nvim's plugin pins) so a plugin or package manager update never slips by. The lock file is `lazy_lock` in settings, else `$XDG_CONFIG_HOME/$NVIM_APPNAME/lazy-lock.json` (`~/.config/nvim/lazy-lock.json`). MAID never runs nvim, never writes the lock file and uses no network.\n\n"
         "- `:lazylock` (`maid lazy-lock`): `in sync`, `changed since DATE` with how many plugins were updated, added and removed, `not recorded yet`, or `no lazy-lock.json at PATH`.\n"
         "- `:lazylock record`: writes the hash to `~/.config/maid/nvim-lazy-lock.sha256` (one sha256sum line, to commit with your dotfiles; `sha256sum -c` checks it from `~`) and keeps a snapshot of the lock file under MAID's state directory for diffs. Prints the old and the new hash.\n"
         "- `:lazylock diff`: per plugin, added, removed, commit changed (old..new) or branch changed; lazy.nvim's own entry is called out as `package manager updated`. Without a snapshot here (a fresh clone of the dotfiles) it says so and still compares the hashes.\n\n"
         "While it is out of sync a notice shows at start and `lock≠` sits in the status strip, re-checked at each turn end and on `:status` by the file's mtime and size. `lazy_lock_notice = false` turns those two off; `maid status`, `maid doctor` and the command still report. A machine with no lock file and nothing recorded stays quiet. Exit codes of `maid lazy-lock`: 0 in sync, 1 changed or not recorded, 2 no lock file or an error. docs/lazy-lock.md"},
        {"path", {"paths", "places", "mcd"}, "[NAME] [copy]", "a place maid knows: show it, or copy it to the clipboard",
         "*:path* *:open* *maid path* *mcd*\n"
         "MAID keeps a registry of every place it knows by a short name: `workspace`, `session` (this transcript), `sessions`, `state`, `config`, `settings`, `instructions`, `logs`, `root`, `tools`, `models`, `models/llamacpp`, `vendor`, `vendor/<service>`, `workflows`, `templates`, and every `maid artifacts` entry as `owner/name`. A unique prefix is enough (`:path work`, `:path sess`).\n\n"
         "In a session: `:path` lists them, `:path NAME` shows one, `:path NAME copy` puts it on the clipboard (and in the register, so `p` pastes it), `:open NAME` opens it in your file manager.\n"
         "In the shell: `maid path` lists, `maid path NAME` prints one (so `cd \"$(maid path workflows)\"` works), `maid path NAME --copy`, `maid open NAME`. Since a program cannot change its parent shell's directory, `eval \"$(maid shell-init)\"` in your rc file adds `mcd NAME` (cd there), `mpath NAME` and `mcp NAME` with tab completion of the names (zsh, bash, fish)."},
        {"open", {"xdg-open", "browser"}, "NAME|SERVICE [firefox|chrome]", "a service in the browser, a place in the file manager",
         "*:open* *maid open* *browser* *remote*\n`:open comfyui` opens that service's URL in your browser; `:open server` the maid web client; `:open workflows` a place in the file manager. The browser is `browser` in settings (`default` = the system's, `firefox`, `chrome`) or given after the name. With `remote = \"https://host:7373\"` in settings and that maid-server answering, a service opens the remote's copy (the same port on the remote host; its service must be reachable from here, by listening beyond loopback there or through your tunnel). `maid open` with no name shows a menu. `:open NAME folder` (or `maid open NAME --folder`) opens the containing folder in the file manager instead: a file's parent, a service's vendored checkout. `maid cd NAME` prints the place's directory (a file's parent), so `cd \"$(maid cd NAME)\"` works anywhere; `maid cd NAME --subshell` opens a shell there instead (`exit` returns); `mcd NAME` from `maid shell-init` changes the current shell."},
        {"artifacts", {}, "", "where everything is kept, with sizes", "*:artifacts*\nEvery place MAID and its services leave things (transcripts, service logs, ComfyUI outputs, ...) with sizes. Clean with `maid artifacts clean OWNER/NAME [--older-than DAYS]`."},
        {"reg", {"register", "registers"}, "", "show the registers", "*:reg*\nShows the unnamed register and every named register `\"a`..`\"z` that holds something. See `:h p`."},
        {"undo", {}, "[N]", "restore the file(s) the agent changed last",
         "*:undo*\nEvery write_file / edit_file / multi_edit / apply_patch (one point per file) saves the file's previous content first, delete_file keeps the deleted file's content, and move_file keeps the reverse move; a deleted directory is not kept. `:undo` restores the newest point (`:undo 3` the newest three); a file that did not exist is removed, a moved file is moved back. The model is told what was undone; the transcript records it. Points live for the session."},
        {"copy", {}, "", "copy the last reply to the clipboard", "*:copy*\nCopies the newest assistant reply to the register and the system clipboard."},
        {"export", {}, "[FILE]", "write the transcript as markdown",
         "*:export* *maid sessions export*\n`:export` writes this session as markdown (## User / ## Assistant, tool calls in fenced blocks) to `<session id>.md` in the workspace, or to FILE. Outside a session: `maid sessions export ID [FILE]` (stdout without FILE)."},
        {"stash", {"pop"}, "", "park the input draft; :pop brings it back",
         "*:stash* *:pop*\n`:stash` saves the input draft to ~/.local/state/maid/prompt-stash.jsonl and clears the input, so you can ask something else first; `:pop` restores the newest one. Survives restarts; `:q` with a draft stashes it automatically. Sent prompts are also kept in prompt-history.jsonl, so ↑ / Ctrl-P in the input reach earlier sessions' prompts."},
        {"rename", {"title"}, "TITLE", "title this session",
         "*:rename*\nSets the title `maid sessions` and `:export` show. With `small_model` in settings (opencode's name for the cheap auxiliary model; `title_model` is the older name, for example `small_model = \"qwen-4b\"`) a title is generated after the first turn; a remote title model is never used for a local session."},
        {"budget", {}, "[N|off]", "token budget for this session",
         "*:budget*\n`:budget` shows tokens used; `:budget 200000` stops the agent once input plus output over the session reaches that; `:budget off` removes it. `budget_tokens` in settings sets a default."},
        {"theme", {"themes", "colorscheme", "colo"}, "[NAME|reload|nvim:NAME]", "list themes, switch one live, or import a neovim colorscheme",
         "*:theme* *themes* *theme* *colors*\n"
         "`:theme` lists the themes with the active one marked and where each comes from; `:theme NAME` switches to one live; `:theme reload` re-reads the active one's file, for editing a theme while looking at it. "
         "A theme that fails to load is an error naming the file and line, and the current theme stays. `theme = \"NAME\"` in settings picks one at start (nearest layer wins).\n\n"
         "Themes are Lua files returning `{ name = ..., background = \"dark\"|\"light\", styles = { role = { fg = \"#rrggbb\", bg = ..., bold = true, italic = true, underline = true, dim = true, inverted = true } } }`, "
         "found in `~/.config/maid/themes/NAME.lua` first, then the ones shipped with MAID: `default` (the built-in look), `gruvbox-dark`, `gruvbox-light`, `mono` (no colours, only bold, dim, underline and inverse). "
         "A theme sets any of the roles; the rest keep the default. Precedence: the built-in default, then the theme, then `style` entries in settings, which merge over single roles.\n\n"
         "`:theme nvim:NAME` imports a neovim colorscheme: a headless nvim with your own configuration (and `g:maid_theme_import` set, so a config can skip heavy plugins) applies it, its highlight groups are mapped onto the roles, and the result is written to `~/.config/maid/themes/nvim-NAME.lua`, which loads without nvim from then on; then it is switched to. Tab after `nvim:` lists the colorschemes nvim has. `maid themes import NAME [--as FILE]` does the same outside a session.\n\n"
         "Colours: truecolor when `COLORTERM` is truecolor or 24bit, else the nearest xterm-256 colour, else (no 256 in `TERM`) the nearest of the 16 ANSI colours; `colors = \"truecolor\"|\"256\"|\"16\"` in settings overrides the detection. Writing one, the role list and the nvim mapping: docs/themes.md, docs/settings.md."},
        {"lua", {"luafile", "luajit", "repl", "chat"}, "[CODE]", "run Lua (LuaJIT) here, or enter Lua mode; :chat returns",
         "*:lua* *:luafile* *:chat* *maid lua*\n"
         "`:lua CODE` runs Lua in the workspace with LuaJIT (vendored, pinned to the revision Neovim uses); an expression shows its value, `=expr` forces that. `:luafile PATH` runs a file. `:lua` with nothing after it enters **Lua mode**: the input box becomes a REPL (prompt `lua❯`), every send runs in Lua, and `:chat` (or `:lua` again) returns to the model. Globals persist for the session. Output shows in the conversation and is handed to the model as context, like `!cmd`.\n\n"
         "Outside a session: `maid lua` is a REPL (`=expr`, multi-line continuation, Ctrl-D leaves), `maid lua FILE [args]` runs a file (`arg` holds the arguments), `maid lua -e CODE` a snippet.\n\n"
         "The `maid` table: `maid.workspace`, `maid.version`, `maid.read(path)`, `maid.write(path, text)`, `maid.shell(cmd)` (returns output and exit code), `maid.notice(text)`. The standard library is available: this runs as you, like your shell, and is never given to the model."},
        {"compact", {}, "[prune|head|all]", "free context: old tool results first, then the oldest turns",
         "*:compact*\n"
         "Frees context without losing the thread. `:compact` (and the automatic compaction at `compact_at`, 75% of the window by default) does it in this order:\n"
         "1. **prune**: every tool result except the most recent few (`compact_keep_results`, 4) is replaced by a one-line stub saying what it was; the dialog stays word for word. Tool output is what fills a context; dialog is cheap.\n"
         "2. **head**: only if pruning was not enough, the oldest half of the turns is summarised into a handover note (objective, details, work state, next move, files) and the rest stays verbatim. Each time this happens the compaction point moves forward, so the recent conversation is always intact.\n\n"
         "It also fires when a byte estimate of the history says the window is full (a single large tool result can outgrow it in one step, before any token count comes back), and when the server refuses a request as too long: then MAID compacts and retries, twice at most, before showing the error. `:compact prune`, `:compact head` run one stage; `:compact all` is the traditional whole-conversation summary. The session file keeps everything that was said: compaction is recorded, never edited into the past. After any compaction, assistant turns replay as plain text (provider thinking blocks are dropped)."},
        {"clear", {}, "", "start a new conversation", "*:clear*\nForgets the conversation (the session file keeps everything). Waits until the agent is idle."},
        {"trip", {}, "[reason]", "trip the harness now", "*:trip*\nSets the tripwire immediately with no password; nothing runs until `:unlock`. See `:h harness`."},
        {"unlock", {}, "", "reset the harness (sudo password)", "*:unlock*\nResets the tripwire without leaving the session; asks for your sudo password every time."},
        {"!", {}, "cmd", "run cmd in your shell", "*:!*\nSee `:h !`."},
        {"q", {"quit", "exit"}, "[--bg|--park|--stop]", "quit (an unsent draft is stashed)",
         "*:q* *:quit*\nQuits. The session becomes what `leave.quit` in settings says (an idle one is stopped; a working one keeps working in the daemon and is parked once it is done, and without the daemon its turn is interrupted and it is parked), or what `--bg`, `--park` or `--stop` says (`:h leave`). The session file is complete at every moment, and an unsent draft in the input is stashed (`:pop` in the next session brings it back), so nothing is lost. On exit the transcript path and its `maid -r` command are printed."},
        {"wq", {}, "", "send, then quit when the reply is in",
         "*:wq*\nSends the input like `:w` and quits once the reply has arrived, as vim's write-and-quit would. Ctrl-C while waiting keeps the session open. With an empty input it is `:q`."},
    };
    return c;
}

std::vector<const CommandInfo*> match_commands(const std::string& word) {
    std::string w = lower(word);
    std::vector<const CommandInfo*> exact, prefix;
    for (const auto& c : commands()) {
        bool is_exact = c.name == w, is_prefix = c.name.rfind(w, 0) == 0;
        for (const auto& a : c.aliases) {
            is_exact = is_exact || a == w;
            is_prefix = is_prefix || a.rfind(w, 0) == 0;
        }
        if (is_exact) exact.push_back(&c);
        else if (is_prefix) prefix.push_back(&c);
    }
    exact.insert(exact.end(), prefix.begin(), prefix.end());
    return exact;
}

std::vector<std::string> complete_argument(const std::string& command, const std::string& partial, const CompletionContext& ctx) {
    std::vector<std::string> candidates;
    std::string cmd = lower(command);
    if (cmd == "mode") candidates = {"manual", "auto-read", "edit", "auto", "plan"};
    else if (cmd == "nvim") candidates = {"theme"};
    else if (cmd == "set") candidates = {"markdown", "mouse", "tooldetails", "timestamps", "highlight", "enter_sends"};
    else if (cmd == "budget") candidates = {"off"};
    else if (cmd == "instructions") candidates = {"on", "off"};
    else if (cmd == "ban") candidates = {"add", "token", "remove", "tokens", "clear", "retries", "case", "list"};
    else if (cmd == "harness") candidates = {"smart", "dumb"};
    else if (cmd == "allow") candidates = {"remove", "list"};
    else if (cmd == "forbid") candidates = {"remove", "list"};
    else if (cmd == "image") candidates = {"clear"};
    else if (cmd == "gpu") {
        // The servers this machine defines (llamacpp-fim only where its service file is).
        candidates = {"free"};
        for (const auto& s : ctx.services) {
            if (is_llama_server(s) || s == "whisper" || s == "comfyui") candidates.push_back("free " + s);
            if (is_fim_server(s)) candidates.push_back("load " + s);
        }
        if (ctx.services.empty()) candidates = {"free", "free llamacpp", "free llamacpp-2", "free llamacpp-fim", "free whisper", "free comfyui", "load llamacpp-fim"};
    }
    else if (cmd == "lazylock" || cmd == "lazy-lock") candidates = {"record", "diff"};
    else if (cmd == "path" || cmd == "open" || cmd == "cd") candidates = {"workspace", "session", "sessions", "state", "config", "settings", "instructions", "logs", "root", "tools", "models", "vendor", "workflows", "templates", "comfyui/outputs", "comfyui/workflows", "comfyui/templates", "maid/sessions", "maid/service-logs", "diction/logs"};
    else if (cmd == "sampling") candidates = {"xtc", "temperature", "top_k", "top_p", "min_p", "seed", "repeat_penalty", "dry_multiplier", "top_n_sigma", "unset", "reset"};
    else if (cmd == "compact") candidates = {"prune", "head", "all"};
    else if (cmd == "think") candidates = {"on", "off"};
    else if (cmd == "w" || cmd == "write" || cmd == "send") candidates = {"now"};
    else if (cmd == "up" || cmd == "down") candidates = ctx.services;
    else if (cmd == "theme") {
        candidates = ctx.themes;
        candidates.push_back("reload");
        candidates.push_back("nvim:");
        for (const auto& c : ctx.nvim_colors) candidates.push_back("nvim:" + c);
    }
    else if (cmd == "model") {
        candidates = ctx.models;
        for (const auto& p : ctx.providers) candidates.push_back(p + "/");
    } else if (cmd == "h" || cmd == "help") {
        for (const auto& c : commands()) candidates.push_back(c.name);
        for (const auto& t : topics()) candidates.push_back(t.name);
    }
    // :cd also completes directories on disk: absolute, ~/..., or relative to the workspace; hidden ones once a
    // dot is typed.
    if (cmd == "cd" && !ctx.workspace.empty()) {
        size_t slash = partial.rfind('/');
        std::string dir_part = slash == std::string::npos ? "" : partial.substr(0, slash + 1);
        std::filesystem::path base = dir_part.empty() ? std::filesystem::path(".") : std::filesystem::path(dir_part);
        const char* home = std::getenv("HOME");
        if (partial == "~") candidates.push_back("~/");
        if (dir_part.rfind("~/", 0) == 0 && home) base = std::filesystem::path(home) / dir_part.substr(2);
        if (base.is_relative()) base = std::filesystem::path(ctx.workspace) / base;
        bool hidden = partial.compare(dir_part.size(), 1, ".") == 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (!it->is_directory(ec) || (name[0] == '.' && !hidden)) continue;
            candidates.push_back(dir_part + name + "/");
        }
    }
    std::vector<std::string> out;
    std::string p = lower(partial);
    for (const auto& c : candidates) {
        if (lower(c).rfind(p, 0) == 0) out.push_back(c);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace {

// The page `topic` names, normalised and trimmed; when it names several, their names go to `several`.
std::optional<HelpPage> find_page(const std::string& topic_in, std::vector<std::string>& several) {
    std::string topic = normalize(topic_in);
    while (!topic.empty() && topic.back() == ' ') topic.pop_back();
    // Exact matches first, then prefixes, across commands and topics.
    std::vector<HelpPage> exact, prefix;
    auto consider = [&](const std::string& name, const std::vector<std::string>& aliases, const std::string& summary, const std::string& text, const std::string& shown) {
        std::vector<std::string> names = aliases;
        names.push_back(name);
        for (const auto& n : names) {
            if (normalize(n) == topic) {
                exact.push_back({shown, summary, text});
                return;
            }
        }
        for (const auto& n : names) {
            if (normalize(n).rfind(topic, 0) == 0) {
                prefix.push_back({shown, summary, text});
                return;
            }
        }
    };
    for (const auto& c : commands()) consider(c.name, c.aliases, c.summary, c.help, ":" + c.name);
    for (const auto& t : topics()) consider(t.name, t.aliases, t.summary, t.text, t.name);
    if (!exact.empty()) return exact.back();  // a topic and a command of the same name: the topic's page
    if (prefix.size() == 1) return prefix.front();
    // Several matches that are one name (a `:harness` command and a `harness` topic): the topic's page.
    bool one_name = true;
    auto bare = [](std::string n) { return n.rfind(":", 0) == 0 ? n.substr(1) : n; };
    for (const auto& page : prefix) one_name = one_name && bare(page.name) == bare(prefix.front().name);
    if (!prefix.empty() && one_name) return prefix.back();
    for (const auto& page : prefix) several.push_back(page.name);
    return std::nullopt;
}

bool is_index(const std::string& topic_in) {
    std::string topic = normalize(topic_in);
    while (!topic.empty() && topic.back() == ' ') topic.pop_back();
    return topic.empty() || topic == "topics";
}

}  // namespace

std::optional<HelpPage> help_page(const std::string& topic) {
    std::vector<std::string> several;
    if (is_index(topic)) return std::nullopt;
    return find_page(topic, several);
}

std::string help_text(const std::string& topic_in) {
    if (is_index(topic_in)) {
        std::string out = "*help* Type `:h TOPIC` (or `maid help TOPIC`) for one of these; a unique prefix is enough:\n\n**commands**\n";
        for (const auto& c : commands()) out += "  :" + c.name + (c.args.empty() ? "" : " " + c.args) + "  " + c.summary + "\n";
        out += "\n**topics and keys**\n";
        for (const auto& t : topics()) out += "  " + t.name + "  " + t.summary + "\n";
        return out;
    }
    std::vector<std::string> several;
    if (auto page = find_page(topic_in, several)) return page->text;
    if (several.empty()) return "no help for '" + topic_in + "'. `:h` lists the topics.";
    std::string out = "'" + topic_in + "' matches several topics:\n";
    for (const auto& name : several) out += "  " + name + "\n";
    return out;
}

namespace {

// The `*tag*` tokens a line starts with, one space apart (`\*` is an asterisk inside one); `rest` is what follows them.
std::vector<std::string> leading_tags(const std::string& line, std::string& rest) {
    std::vector<std::string> tags;
    size_t i = 0;
    while (i < line.size() && line[i] == '*') {
        std::string tag;
        size_t j = i + 1;
        for (; j < line.size() && line[j] != '*'; ++j) {
            if (line[j] == '\\' && j + 1 < line.size() && line[j + 1] == '*') ++j;
            tag += line[j];
        }
        if (j >= line.size() || tag.empty() || (j + 1 < line.size() && line[j + 1] != ' ')) break;
        tags.push_back(tag);
        i = j + 2;
    }
    rest = line.substr(std::min(i, line.size()));
    return tags;
}

struct Painter {
    const Settings* settings;
    ColorDepth depth;
    explicit Painter(const Settings* paint) : settings(paint), depth(paint ? color_depth(paint->colors) : ColorDepth::Ansi16) {}
    std::string operator()(const std::string& text, const char* role) const { return settings ? ansi_paint(text, settings->style(role), depth) : text; }
};

// Where the code span opened by the backticks at `open` ends: the next run of as many backticks (two or more let
// the span hold a single one).
size_t code_end(const std::string& line, size_t open) {
    size_t ticks = line.find_first_not_of('`', open);
    ticks = (ticks == std::string::npos ? line.size() : ticks) - open;
    for (size_t at = open + ticks; (at = line.find('`', at)) != std::string::npos;) {
        size_t run = line.find_first_not_of('`', at);
        run = (run == std::string::npos ? line.size() : run) - at;
        if (run == ticks) return at;
        at += run;
    }
    return std::string::npos;
}

std::string render_inline(const std::string& line, const Painter& in) {
    std::string out;
    for (size_t i = 0; i < line.size();) {
        size_t end = std::string::npos;
        if (line.compare(i, 2, "**") == 0 && (end = line.find("**", i + 2)) != std::string::npos && end > i + 2) {
            std::string bold = line.substr(i + 2, end - i - 2);
            bold.erase(std::remove(bold.begin(), bold.end(), '`'), bold.end());
            out += in(bold, "md_bold");
            i = end + 2;
        } else if (line[i] == '`' && (end = code_end(line, i)) != std::string::npos) {
            size_t ticks = line.find_first_not_of('`', i);
            if (ticks == std::string::npos) ticks = line.size();
            ticks -= i;
            std::string code = line.substr(i + ticks, end - i - ticks);
            if (code.size() > 1 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
            out += in(code, "md_code");
            i = end + ticks;
        } else {
            out += line[i++];
        }
    }
    return out;
}

std::string render_text(const std::string& text, const Painter& in) {
    std::string out;
    bool first = true;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (first) {
            std::string rest;
            auto tags = leading_tags(line, rest);
            for (size_t k = 0; k < tags.size(); ++k) out += (k ? "  " : "") + in(tags[k], "md_link");
            if (!tags.empty() && !rest.empty()) out += " ";
            line = rest;
            first = false;
        }
        out += render_inline(line, in);
        if (nl == std::string::npos) break;
        out += "\n";
        pos = nl + 1;
    }
    return out;
}

// The distinct first groups of `re` in `text`, in order.
std::vector<std::string> mentions(const std::string& text, const std::regex& re) {
    std::vector<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
        std::string m = (*it)[1];
        while (!m.empty() && m.back() == '.') m.pop_back();
        if (!m.empty() && m.find("..") == std::string::npos && std::find(found.begin(), found.end(), m) == found.end()) found.push_back(m);
    }
    return found;
}

}  // namespace

std::vector<std::string> help_tags(const HelpPage& page) {
    std::string rest;
    return leading_tags(page.text.substr(0, page.text.find('\n')), rest);
}

std::string render_markdown(const std::string& text, const Settings* paint) {
    return render_text(text, Painter(paint));
}

std::string render_help(const HelpPage& page, const std::string& usage, const Settings* paint) {
    Painter in(paint);
    auto trimmed = [](std::string s) {
        while (!s.empty() && s.back() == '\n') s.pop_back();
        return s;
    };
    std::vector<std::string> sections;
    auto section = [&](const char* heading, const std::string& body) { sections.push_back(in(heading, "md_heading") + "\n" + trimmed(body)); };
    section("NAME", in(page.name, "md_bold") + (page.summary.empty() ? "" : " - " + page.summary));
    if (!usage.empty()) section("SYNOPSIS", usage);
    section("DESCRIPTION", render_text(page.text, in));
    static const std::regex path_re(R"((?:^|[^\w.~$/])((?:~|\.maid|\$XDG_[A-Z_]+)/[\w.<>{}*$/+-]*))");
    std::string files;
    for (const auto& f : mentions(page.text, path_re)) files += (files.empty() ? "" : "\n") + in(f, "md_code");
    if (!files.empty()) section("FILES", files);
    static const std::regex doc_re(R"((docs/[\w./-]*\.md))");
    static const std::regex topic_re(R"((?::h|maid help) ([\w.:+-]+))");
    std::string also;
    auto add = [&](const std::string& item) { also += (also.empty() ? "" : ", ") + item; };
    for (const auto& d : mentions(page.text, doc_re)) add(d);
    std::vector<std::string> seen = {page.name};
    for (const auto& t : mentions(page.text, topic_re)) {
        if (t.size() > 1 && std::none_of(t.begin(), t.end(), [](unsigned char c) { return std::islower(c); })) continue;  // a placeholder: TOPIC
        auto other = help_page(t);
        if (!other || std::find(seen.begin(), seen.end(), other->name) != seen.end()) continue;
        seen.push_back(other->name);
        add("maid help " + t);
    }
    if (!also.empty()) section("SEE ALSO", also);
    std::string out;
    for (const auto& s : sections) out += (out.empty() ? "" : "\n\n") + s;
    return out;
}

namespace {

// Does the remote maid-server answer? 401 counts: it is up, we just did not send a token.
bool remote_up(const std::string& url) {
    httplib::Client c(url);
    c.set_connection_timeout(3);
    c.set_read_timeout(3);
    auto r = c.Get("/api/status");
    return r && (r->status == 200 || r->status == 401);
}

std::string host_of(const std::string& url) {
    size_t a = url.find("://");
    std::string rest = a == std::string::npos ? url : url.substr(a + 3);
    size_t b = rest.find_first_of(":/");
    return b == std::string::npos ? rest : rest.substr(0, b);
}

}  // namespace

std::pair<std::string, std::string> open_command(const std::string& name, const Settings& settings, const std::filesystem::path& workspace,
                                                 const std::vector<ServiceDef>& services, const std::optional<std::filesystem::path>& session,
                                                 const std::string& browser_override, bool folder) {
    std::string browser = browser_override.empty() ? settings.browser : browser_override;
    if (folder) {
        auto places = known_places(settings, workspace, services, session);
        std::string key = name;
        for (const auto& def : services) {
            if (def.name == name) key = "vendor/" + name;  // a service's folder is its vendored checkout
        }
        const auto& p = find_place(places, key);
        std::filesystem::path dir = p.is_file ? p.path.parent_path() : p.path;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) throw std::runtime_error(dir.string() + " does not exist yet");
        return {"xdg-open '" + dir.string() + "' >/dev/null 2>&1 &", "the folder " + dir.string() + (p.is_file ? " (holding " + p.path.filename().string() + ")" : "")};
    }
    if (name == "server" || name == "maid-server") {
        std::string url = !settings.remote.empty() && remote_up(settings.remote) ? settings.remote : "http://" + settings.server.listen;
        if (url.rfind("http", 0) != 0) url = "http://" + url;
        return {browser_command(browser, url), "the maid web client at " + url};
    }
    for (const auto& def : services) {
        if (def.name != name || def.port == 0) continue;
        std::string url = "http://127.0.0.1:" + std::to_string(def.port);
        std::string where = "local";
        if (!settings.remote.empty() && remote_up(settings.remote)) {
            // The remote's copy: the same port on the remote's host. Its service must listen beyond loopback
            // there, or a tunnel must carry it; MAID's own services bind to loopback unless told otherwise.
            url = "http://" + host_of(settings.remote) + ":" + std::to_string(def.port);
            where = "remote (" + settings.remote + ")";
        }
        return {browser_command(browser, url), def.name + " at " + url + " (" + where + ", " + browser + " browser)"};
    }
    auto places = known_places(settings, workspace, services, session);
    const auto& p = find_place(places, name);
    std::error_code ec;
    if (!std::filesystem::exists(p.path, ec)) throw std::runtime_error(p.path.string() + " does not exist yet");
    return {"xdg-open '" + p.path.string() + "' >/dev/null 2>&1 &", p.name + " (" + p.path.string() + ")"};
}

}  // namespace maid
