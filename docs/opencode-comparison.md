# opencode vs MAIC

> Note (2026-10-01): Ollama was removed from MAIC in favour of llama.cpp (docs/llamacpp.md). The Ollama references below are history.

Read-only comparison of `anomalyco/opencode` against MAIC, written for Micaiah as input to MAIC's roadmap. opencode was read at commit `2fa3363c924c5c3e367b84a87ae478296a0ed59b` (2026-09-29, "docs(web): correct GPT 6.1 Sol cache pricing"), checked out at `~/dev2/tools-and-things/opencode`. MAIC was read at commit `4477370b` ("Add C++ core, agent CLI, harness and service control"). Paths below are relative to each repo root unless absolute.

MAIC's own design principles take precedence over anything here: a root-owned tripwire that any process can trip but only a sudo password can reset, with the session surviving a trip; a bubblewrap sandbox for every model-run command; the manual / auto-read / edit / auto / plan modes; requests from a remote origin always asked; secrets never readable; JSONL session transcripts. Where opencode does something differently, the question is only whether the idea can be adopted *inside* those constraints.

## 1. Summary

opencode is a TypeScript coding agent built on Bun and the Effect library, organised as a monorepo (`packages/opencode` is the core server plus CLI, `packages/core` holds shared services and the SQLite schema, `packages/tui` is an OpenTUI/SolidJS terminal UI, `packages/schema` the wire types, `packages/sdk` a generated client, plus `desktop`, `app`, `web`, `server`, `slack`, `console` and `enterprise` packages). It is server-first: the CLI starts an HTTP server (`packages/opencode/src/server/`), the TUI is one client of it over the SDK, and `opencode serve`, `opencode attach`, an ACP server (`src/cli/cmd/acp.ts`), the desktop app and a hosted web UI are other clients. State lives under the XDG data dir in SQLite (`packages/core/src/database/`, `packages/core/src/session/sql.ts`) with an older JSON key-value store beside it (`src/storage/storage.ts`). MAIC is a C++20 library (`core/`) with an `AgentEvents` interface that front ends implement (`core/include/maic/agent.hpp`); the FTXUI TUI and the headless printer are the two front ends today, and the planned server is meant to be a third client of the same core (`README.md`, "Structure and Build"). Both put the agent loop and tools in a core library and keep the UI thin, so the shapes are compatible; the differences are that opencode is network-native (HTTP API, share, model catalog, web search, auto-update) with no sandbox and no lock, while MAIC is local-first with a sandbox and a lock and no network at all from tools.

## 2. Harness / permissions in opencode

### How a tool gets permission

There is no central gate. Each tool calls `ctx.ask(...)` itself from inside its own `execute` (`src/tool/tool.ts:45` defines `Context.ask`). A tool that forgets to ask is not checked. The calls are:

| Tool | Permission key | Pattern asked | Where |
| :--- | :--- | :--- | :--- |
| bash | `bash` | the exact command text of each parsed command node | `src/tool/shell.ts` (`collect`, `ask`) |
| bash, touching a path outside the project | `external_directory` | `<dir>/*` | `src/tool/shell.ts`, `src/tool/external-directory.ts` |
| edit, write, apply_patch | `edit` | the file path | `src/tool/edit.ts:102,145`, `src/tool/write.ts:54` |
| read | `read` | the file path | `src/tool/read.ts:255` |
| webfetch | `webfetch` | the URL | `src/tool/webfetch.ts:40` |
| task (subagent) | the subagent's id | | `src/tool/task.ts:120` |
| repeated identical tool call | `doom_loop` | the tool name | `src/session/processor.ts:29,340-380` |
| any other tool (todowrite, question, lsp, skill, MCP tools) | its own name | `*` | per tool |

### The rules

Config key `permission` in `opencode.json(c)` (global `~/.config/opencode/`, then every `.opencode/` directory walking up from the cwd, `src/config/paths.ts`). The schema is `packages/core/src/v1/config/permission.ts`: the value is either one action string, or an object whose keys are `read`, `edit`, `glob`, `grep`, `list`, `bash`, `task`, `external_directory`, `todowrite`, `question`, `webfetch`, `websearch`, `lsp`, `doom_loop`, `skill`, or any tool name. Each value is `"ask" | "allow" | "deny"` or a `{ "<glob pattern>": action }` map. Key order is preserved on parse because it is the precedence.

`fromConfig` turns that into a flat list of `{permission, pattern, action}` rules (`src/permission/index.ts`), expanding `~` and `$HOME` in patterns. `evaluate(permission, pattern, ...rulesets)` picks the **last** rule whose permission glob and pattern glob both match (`findLast` with `Wildcard.match`), defaulting to `ask` on `*`. So later rules override earlier ones and a user's config overrides the agent defaults.

Per-agent rulesets are built in `src/agent/agent.ts`: defaults are `"*": "allow"`, `doom_loop: ask`, `external_directory: ask` (with the tool-output truncation dir, tmp, skill and reference dirs allowed), `question / plan_enter / plan_exit: deny`, and `read: { "*": allow, "*.env": ask, "*.env.*": ask, "*.env.example": allow }`. The `build` agent adds `question` and `plan_enter` allow; the `plan` agent sets `edit: { "*": deny }` except for plan files and denies the `general` subagent; the `explore` subagent starts from `"*": deny` and allows `grep glob list bash webfetch websearch read`; `compaction`, `title` and `summary` are `"*": deny`. User agents from the `agent` config key get `defaults + user` and can add their own `permission`. Subagents spawned by `task` inherit only the parent's `deny` rules and `external_directory` rules (`src/agent/subagent-permissions.ts`), so a parent cannot widen a child but a child's own ruleset decides what it can do. A rule of `deny` on `*` for a tool hides the tool from the model entirely (`disabled()`, `visibleTools()` in `src/permission/index.ts`); `tools: { name: false }` in config does the same.

### bash specifically

`src/tool/shell.ts` parses the command with tree-sitter-bash (tree-sitter-powershell on Windows), walks every `command` node, and for each one records the node's source text as the pattern to check and an "always" pattern built from `BashArity.prefix(tokens) + " *"` (`src/permission/arity.ts`, a generated table: `git` is 2 tokens so `git checkout main` becomes `git checkout *`, `npm run dev` becomes `npm run *`, `rm` is 1). For commands in the `FILES` set (`rm cp mv mkdir touch chmod chown cat cd ...`) it resolves path arguments; any that land outside the project become an `external_directory` ask for `<dir>/*`. Arguments containing `$`, `$(`, backticks or `${` are treated as dynamic and **skipped**, not denied (`dynamic()` in `shell.ts`), so `rm $HOME/x` gets no external-directory check. The process then runs with the user's full environment (`shellEnv`), full network, the user's uid, in a detached process group with a timeout (default 2 minutes, `RuntimeFlags.bashDefaultTimeoutMs`). There is no sandbox: no bwrap, seatbelt, landlock or firejail anywhere in `packages/opencode/src` or `packages/core/src` (the only `sandbox` hits are unrelated worktree naming).

### Asking, answering, denying

`Permission.ask` (`src/permission/index.ts`) evaluates every pattern in the request against agent ruleset + the session-approved list. Any `deny` short-circuits into `DeniedError`, whose message to the model is "The user has specified a rule which prevents you from using this specific tool call" plus the relevant rules. If everything is `allow` it returns. Otherwise it publishes `permission.asked` and blocks on a `Deferred` until `reply` is called with `once`, `always` or `reject` (`packages/schema/src/v1/permission.ts:38`), optionally with a `message`. `reject` fails the call with `RejectedError` ("The user rejected permission to use this specific tool call") or, with a message, `CorrectedError` carrying the user's feedback, and also rejects every other pending request in that session. `always` appends the request's `always` patterns as `allow` rules to the instance's in-memory `approved` list and auto-resolves any pending request those rules now cover. The TUI prompt is `packages/tui/src/routes/session/permission.tsx`; the headless prompt is `src/cli/cmd/run/permission.shared.ts` (its own text says "until OpenCode is restarted").

Denial is an error tool result. The turn continues, the model sees the message, the session is unaffected. Nothing is locked.

A plugin can intercept: the `permission.ask` hook (`packages/plugin/src/index.ts:261`) receives the request and an `output.status` it may set to `allow`, `deny` or `ask`; `tool.execute.before/after` hooks can rewrite arguments and output. Plugins are npm packages or local files named in config (`plugin` key).

### Persistence across sessions

V1 (`src/permission/index.ts`, what the tools use): `always` lives in memory for the life of the server process, shared by every session in that instance, gone on restart.

V2 (`packages/core/src/permission.ts` with `packages/core/src/permission/saved.ts` and the `permission` SQLite table in `packages/core/src/permission/sql.ts`): a request may carry `save` patterns; an `always` reply writes `(projectID, action, resource)` rows that are loaded back into the ruleset on every later evaluation for that project, with no expiry. `list`/`remove` exist on the service; there is no CLI or TUI surface for revoking that I found.

Config itself is persistent by nature: a `permission` block in `.opencode/opencode.json` is the durable allow/deny list. There is also a managed config channel (`src/config/managed.ts`: `/etc/opencode`, `/Library/Application Support/opencode`, `%ProgramData%\opencode`, plus a `remote_config` key) intended for an administrator to force settings.

### Lock, kill switch, session survival

There is no lock or kill-switch concept. The nearest things are: `escape` interrupts the running turn (`session_interrupt` in `packages/tui/src/config/keybind.ts`), `deny` rules in config, and the `doom_loop` ask after three identical consecutive calls to the same tool with the same input (`DOOM_LOOP_THRESHOLD = 3` in `src/session/processor.ts`). The opposite direction exists: `opencode run --auto` / `--yolo` / `--dangerously-skip-permissions` auto-approves everything not explicitly denied (`src/cli/cmd/run.ts:244-274`).

Sessions are durable and resumable regardless of what happened in them: `session`, `message`, `part`, `todo` tables in `packages/core/src/session/sql.ts`, `opencode run --continue | --session ID | --fork`, `opencode session list | delete`, `opencode export | import` (`src/cli/cmd/`), and the TUI session list, timeline and fork dialogs (`packages/tui/src/routes/session/`). Snapshots for undo are git objects in a hidden repo under the data dir (`src/snapshot/index.ts`, pruned after 7 days).

### Remote origin

The server can require HTTP basic auth (`OPENCODE_SERVER_PASSWORD`, `src/server/auth.ts`). Beyond that a permission reply is a permission reply: the request record has no notion of which client answered or where the prompt came from, and a remote TUI, the desktop app, the web UI, a Slack bot or an ACP client all answer through the same `reply` endpoint with the same standing as the local terminal.

### Assessment against MAIC's tripwire + sandbox model

MAIC's design takes precedence. Read this as "what can be layered inside it", not "what to swap out".

Where MAIC is stronger, and should stay as it is:

* **One gate.** Every MAIC tool call goes through `Agent::run_tool_call` (`core/src/agent.cpp`): tripwire check, `Harness::check`, approval, then `run_tool`. opencode's per-tool `ctx.ask` means safety depends on every tool author remembering, and MCP or plugin tools never ask about paths at all.
* **Sandbox instead of parsing.** opencode has to guess what a shell command will touch from its syntax, and explicitly gives up on dynamic arguments. MAIC decides less from the text (trip patterns and the read-only classifier in `core/src/harness.cpp`) and lets bwrap enforce the rest: workspace-only writes, no network, no privileges, secrets hidden (`core/src/sandbox.cpp`). A wrong guess in opencode runs unrestricted; a wrong guess in MAIC hits a read-only mount or a missing directory.
* **Trip and lock.** opencode has nothing that stops a runaway agent other than the user pressing escape in time. MAIC's automatic trips, the `t` answer at the prompt, `maic trip`, and the root-owned lock that survives restarts and needs a password to clear have no counterpart, and the session keeps running while tripped so the user can work out what happened.
* **Secrets never readable.** opencode's default is `*.env: ask`; `~/.ssh`, `~/.aws` and the rest are only protected by `external_directory: ask`, which the user can `always`-allow with one keypress for the whole directory glob. MAIC denies (`Harness::check_read`) and trips on writes (`check_write`), and search skips them.
* **Origin.** MAIC downgrades allow to ask for `Origin::Remote` in every mode (`Harness::check`). opencode does not track origin.
* **No network from the agent.** opencode's `webfetch`, `websearch`, share upload to `opncd.ai`, models.dev catalog fetch, MCP remote servers, OAuth flows and auto-update are all outbound. MAIC's only network is the model API itself, flagged as `REMOTE` in the status line when the provider is off-machine (`Agent::remote()`).
* **Session-scoped "always".** MAIC forgets `Always` on `clear` and on exit (`Agent::always_allowed_`). opencode's V1 list outlives the session (whole server process, all sessions in it) and V2 persists forever per project with no revoke UI.
* **No plugin can flip a verdict.** opencode's `permission.ask` hook can turn `ask` into `allow`. MAIC's docs state that nothing a model or tool says can switch the harness off; keep that true for any future plugin system.

What opencode does that MAIC should adopt (each of these fits under the tripwire and the sandbox without weakening either):

* **Pattern rules with last-match-wins and a user-extendable config.** MAIC's secret, system and sensitive lists are compiled in (`Harness::Harness`). A `permission` block in `settings.json` shaped like opencode's (`tool -> {pattern -> allow|ask|deny}`) would let Micaiah add her own deny patterns and per-project allowances. The trip patterns and the secret/system paths must stay non-overridable; config should only be able to add restrictions or lift an `ask` to `allow` inside the workspace.
* **Command-prefix "always" keys.** MAIC's `approval_key` for a shell action is the first word (`Harness::approval_key`), so "always allow `git`" covers `git push --force`. opencode's arity table (`src/permission/arity.ts`) gives `git checkout *` granularity. The table is a plain map and ports directly.
* **Per-command evaluation of compound commands.** MAIC's read-only classifier already splits on `; && || |` (`is_read_only_command`); the approval key should do the same, so `ls && rm -rf build` asks about `rm`, not `ls`.
* **Reject with a reason.** opencode's `reject` + `message` becomes a `CorrectedError` the model reads. MAIC's `Approval::No` returns a fixed string. Adding an optional line of text to the `n` answer is cheap and steers the model better than "ask what they want".
* **Doom-loop detection.** Three identical consecutive calls trigger an ask in opencode. MAIC's planned "repeated denials trip the lock" (`docs/harness.md`) is the same signal; count identical calls and identical denials in `run_tool_call`, ask on the first threshold, trip on the second.
* **A durable, revocable allow-list, if one is ever wanted.** If Micaiah decides some `always` answers should outlive a session, copy the V2 shape (per-project rows with a `list` and `remove`) and add a `maic permissions` command to show and revoke them. Do not copy the V1 behaviour of one process-wide list.
* **Hide denied tools.** `visibleTools()` removes tools the ruleset fully denies from the schema the model sees. MAIC's plan mode could omit `write_file` and `edit_file` from `tool_schemas()` instead of only denying them, which saves the model a wasted call.

## 3. Feature comparison table

"opencode" column cites the implementing file. "MAIC" is what exists at commit `4477370b`. Recommendations assume the tripwire, sandbox and no-network rules stay.

| Feature | opencode | MAIC | Recommendation |
| :--- | :--- | :--- | :--- |
| Session persistence and resume | Yes. SQLite tables in `packages/core/src/session/sql.ts`; `opencode run --continue/--session/--fork` (`src/cli/cmd/run.ts`); TUI session list and timeline (`packages/tui/src/routes/session/`) | Partial. Append-only JSONL transcript per session (`core/src/session.cpp`), `maic sessions` lists files; no resume | Adopt now. Replay the JSONL into `messages_` on `maic --resume`; the transcript already records user, assistant and tool entries. Keep JSONL as the format. |
| Compaction / summarisation | Yes. `src/session/compaction.ts` (prune old tool output, then summarise with a hidden `compaction` agent, `src/agent/prompt/compaction.txt`), overflow check in `src/session/overflow.ts`, `<leader>c` manual compact | No. `kMaxSteps = 40` per turn and `num_ctx` only | Adopt now. Local models with 16k context need this more than cloud ones. Prune tool outputs first (cheap, deterministic), summarise second. |
| Multiple agents / subagents | Yes. `src/agent/agent.ts` (build, plan, general, explore, custom), `src/tool/task.ts` spawns child sessions, `src/agent/subagent-permissions.ts` | No | Later. Worth it once resume and compaction exist. When done, children must run under the same `Harness` and never get a wider mode than the parent. |
| MCP support | Yes. `src/mcp/index.ts` (stdio, SSE, streamable HTTP, OAuth), TUI dialog `dialog-mcp.tsx` | No | Later, local stdio servers only. Every MCP tool call must map to a MAIC `Action` (or be treated as a shell action) so it passes the same gate; remote MCP is network and is out. |
| LSP integration | Yes. `src/lsp/` (diagnostics after edits, hover, definition, references), `lsp` tool | No | Skip for now. Clangd diagnostics after an edit would be nice for a C++ user, but it is a large, process-spawning subsystem; a `run_shell` of the build is the cheap substitute. |
| File diff display | Yes. `src/tool/edit.ts` produces a unified diff in tool metadata (`createTwoFilesPatch`), TUI diff viewer with hunk navigation (`keybind.ts` `diff_*`), `packages/tui/src/component/dialog-workspace-file-changes.tsx` | No. Tool result is "edited path" plus an 8-line preview in the TUI | Adopt now. Show the diff at the approval prompt, not just after: `edit_file` has old and new strings before it asks, so the prompt can show exactly what will change. |
| Undo / revert of edits | Yes. `src/snapshot/index.ts` (git snapshots in a hidden repo), `src/session/revert.ts`, `<leader>u` / `<leader>r` | No. Planned as "undo points" in `docs/harness.md` | Adopt later. A `git stash create`-style snapshot before the first write of a turn, stored under `~/.local/state/maic`, is the version that fits. |
| Git integration | Partial. Status/diff helpers in `src/git/index.ts`, worktrees in `src/worktree/index.ts`, `opencode pr`, `opencode github` | Partial. Read-only git subcommands are classified read-only (`core/src/harness.cpp`) and run in a read-only sandbox | Skip beyond snapshots. Git writes should stay ordinary sandboxed commands the user approves. |
| Share / export | Yes. `src/share/share-next.ts` uploads to `opncd.ai`; `opencode export` writes JSON with a `--redact` option (`src/cli/cmd/export.ts`), `opencode import` | Partial. The JSONL file is the export; `maic artifacts` lists and cleans | Adopt export only: a `maic sessions export ID [--redact]` that copies or redacts the JSONL. Never share (network). |
| Custom commands | Yes. `src/command/index.ts`: markdown templates with `$ARGUMENTS`, `$1..$n`, optional agent/model, plus MCP prompts and skills as commands; `packages/tui/src/component/command-palette.tsx` | Partial. Built-in `:` commands only (`cli/src/tui.cpp`) | Adopt later. `~/.config/maic/commands/NAME.md` expanded into the prompt is small and useful. |
| Modes | Different. Agents (`build`, `plan`) switched with tab; no per-tool auto/ask ladder, permissions come from rules | Yes. manual / auto-read / edit / auto / plan (`core/include/maic/harness.hpp`), Shift-Tab cycles | Keep MAIC's. The mode ladder is clearer than opencode's agent-as-mode, and plan mode in MAIC is enforced by a read-only sandbox, not by a deny rule. |
| Keybindings / vim | Configurable leader-key bindings (`packages/tui/src/config/keybind.ts`, `keybinds` config); no vim mode; external editor via `$EDITOR` (`packages/tui/src/editor.ts`) | Yes. Modal vim editor with operators, registers, visual mode, search (`cli/src/editor.cpp`, `cli/src/tui.cpp`); bindings fixed | Later. A small `keys` block in `settings.json` for the non-vim chords (Shift-Tab, Ctrl-W) is enough; keep the vim layer as is. |
| Themes | Yes. 20+ JSON themes in `packages/tui/src/theme/assets/`, light/dark, `<leader>t` picker | Partial. Role styles in `settings.json` (`core/src/settings.cpp`) | Skip. Styles per role already cover it; a `theme` file that is just a styles map is trivial if wanted. |
| Providers / models | Yes. Vercel AI SDK, models.dev catalog fetched at runtime (`packages/core/src/models-dev.ts`), OAuth logins, `auth.json` in the data dir (`src/auth/index.ts`), model picker with favourites and variants | Yes. Ollama, Anthropic, any OpenAI-compatible endpoint (DeepSeek, OpenRouter) in `core/src/llm.cpp`; keys from env or a command, never a file (`core/include/maic/llm.hpp`) | Keep MAIC's. Do not fetch a model catalog; `:models` already lists Ollama tags. |
| Tools given to the model | bash, edit, write, apply_patch, read (with images), grep, glob, list, webfetch, websearch, task, todowrite, question, skill, lsp, plan_exit, plus MCP and plugin tools (`src/tool/registry.ts`) | read_file, list_dir, search_files, write_file, edit_file, run_shell (`core/src/tools.cpp`) | Adopt `glob` (file-name search; `search_files` only greps contents) and `question` (structured ask-the-user). Skip the rest; `apply_patch` and `todowrite` are model-preference tools with no safety value. |
| Image input | Yes. `read` returns image parts for jpeg/png/gif/webp (`src/tool/read.ts:19`), `--file` attachments in `run`, `attachment` config | No | Later. Only matters once a vision-capable local model is in use. |
| Web fetch / search | Yes. `src/tool/webfetch.ts` (ask by URL), `src/tool/websearch.ts` (Exa or Parallel, network) | No. The sandbox has no network | Skip. Violates the no-network rule; if ever needed, it is a separate tool with its own network grant per the planned per-tool manifest. |
| Headless / print mode | Yes. `opencode run` streams events, `--format json`, `--continue`, `--attach` to a server, `--auto` | Yes. `maic -p PROMPT`, `--json`, denies prompts when stdin is not a tty (`cli/src/headless.cpp`) | Keep. Add `--resume` with the session work. Do not add an `--auto` flag; `--mode auto` plus the sandbox is the bounded version. |
| Instruction files | Yes. `AGENTS.md`, `CLAUDE.md`, `~/.config/opencode/AGENTS.md`, `~/.claude/CLAUDE.md`, config `instructions` globs and URLs (`src/session/instruction.ts`) | Yes. `MAIC.md`, `AGENTS.md` from `$HOME` down to the workspace plus a global file (`core/src/instructions.cpp`), re-read every turn | Keep. Optionally add `CLAUDE.md` to the default `instruction_files` list since it is already a settings key. |
| Skills | Yes. `SKILL.md` discovery (`src/skill/index.ts`), `skill` tool loads one on demand | No | Later. Same mechanism as custom commands; a skill is a command the model can invoke. |
| Queued messages mid-turn | Yes. `<leader>q` manages queued prompts | Yes. `Agent::post_message` / `deliver_now` (`core/src/agent.cpp`) | Keep. |
| User-run shell | Yes. `!` prefix in prompt and a terminal component | Yes. `!cmd` runs in the user's shell, unsandboxed, output added as context (`cli/src/tui.cpp` `run_user_shell`) | Keep. |
| Tool output truncation to a file | Yes. `src/tool/truncate.ts` writes long output to a temp dir the model may `read` later | Partial. Head + tail with a byte count (`core/src/sandbox.cpp` `trim_output`) | Adopt later. Write the full output to `~/.local/state/maic/tool-output/` and tell the model the path; reads there are in-workspace-equivalent. |
| Plugins | Yes. npm or local plugins with lifecycle hooks (`packages/plugin/src/index.ts`) | No | Skip as designed. If ever added, hooks must not be able to change a harness verdict. |
| Server / remote access | Yes. HTTP API with optional basic auth (`src/server/`), ACP, desktop, web | Planned (`server/`), with `Origin::Remote` already in the harness | Later, as a client of core, keeping the always-ask rule. |
| Service management | No | Yes. `maic up/down/status/logs`, PID plus start-time identity (`core/src/service.cpp`) | Keep. |

## 4. Top 10 things MAIC should take from opencode

Ordered by value to Micaiah's stated goal (replace a paid agent CLI with local and cheap cloud models, safely).

1. **Resumable sessions from the JSONL transcript.** The transcript already has every user, assistant and tool entry with the harness decision (`core/src/session.cpp`, `Agent::run_tool_call`). Add a `maic --resume [ID]` that replays the file into `messages_` (assistant `raw` blocks included, since Anthropic needs them replayed unchanged) and a `:sessions` picker. Read `packages/opencode/src/cli/cmd/run.ts` (`--continue`, `--session`, `--fork` handling), `packages/core/src/session/sql.ts` for what they consider worth persisting per message and part, and `packages/tui/src/component/dialog-session-list.tsx` for the picker.

2. **Compaction in two stages.** Prune first: replace old tool outputs with a marker once the conversation passes a token budget, protecting the most recent N tokens. Summarise second, with the model, only when pruning is not enough. Read `packages/opencode/src/session/compaction.ts` (`PRUNE_MINIMUM`, `PRUNE_PROTECT`, `serialize`), `packages/opencode/src/session/overflow.ts`, and the prompt in `packages/opencode/src/agent/prompt/compaction.txt`. For MAIC the summariser must run with no tools and be logged to the transcript as its own entry type.

3. **Finer "always allow" keys for shell commands.** Replace the first-word key in `Harness::approval_key` with a command-prefix key from an arity table, and compute one key per command in a compound line. Read `packages/opencode/src/permission/arity.ts` (the table, directly portable) and `collect` in `packages/opencode/src/tool/shell.ts` (per-node `patterns` vs `always`). MAIC does not need tree-sitter for this; the existing splitter in `is_read_only_command` is the right starting point.

4. **Reject with feedback.** Let the `n` answer take an optional line of text that becomes the tool result ("DENIED by the user: <text>"). Read `reply` in `packages/opencode/src/permission/index.ts` (`CorrectedError`) and the `reject` stage in `packages/opencode/src/cli/cmd/run/permission.shared.ts` for the prompt flow. This is a few lines in `Agent::run_tool_call` and the two `ask` implementations.

5. **Doom-loop detection wired to the tripwire.** Count consecutive identical tool calls and consecutive denials; ask at three, trip at a higher threshold. Read `packages/opencode/src/session/processor.ts` (`DOOM_LOOP_THRESHOLD`, the `tool-input-end` case) for the comparison they use (same tool, same JSON input, all recent parts). This implements the "repeated denials tripping the lock" item already planned in `docs/harness.md`.

6. **A `permission` block in `settings.json`.** Patterns per action kind, last match wins, additive only: config may add `deny`, may turn `ask` into `allow` for paths inside the workspace, and can never touch trip patterns, secrets or system paths. Read `packages/core/src/v1/config/permission.ts` (shape), `fromConfig` and `evaluate` in `packages/opencode/src/permission/index.ts` (flattening and precedence), and `packages/core/src/util/wildcard.ts` for the glob matcher. Keep it small; MAIC's `Decision` struct already carries the reason string the prompt needs.

7. **Show the diff before asking.** `edit_file` knows old and new text before `Harness::check` runs; render a unified diff in the approval prompt and in the transcript entry. Read `packages/opencode/src/tool/edit.ts` (`createTwoFilesPatch`, `trimDiff`, the `filediff` metadata) and, for the viewer, `packages/tui/src/config/keybind.ts` `diff_*` bindings to see which navigation actions they found worth binding.

8. **Git snapshot undo points.** Before the first approved write in a turn, snapshot the workspace into a private git object store under `~/.local/state/maic/snapshots/<workspace-hash>` and record the hash in the transcript; `:undo` restores. Read `packages/opencode/src/snapshot/index.ts` (`track`, `patch`, `restore`, `revert`, the 7-day prune and the 2 MB file limit) and `packages/opencode/src/session/revert.ts` for how a revert maps back to a message. This is also planned in `docs/harness.md`.

9. **Structured questions and a glob tool.** A `question` tool lets the model ask a multiple-choice question instead of guessing (`packages/opencode/src/tool/question.ts`, `packages/opencode/src/question/`), and it is the one tool that can safely be `allow` in every mode. A `glob` tool (`packages/opencode/src/tool/glob.ts`) finds files by name; MAIC's `search_files` only greps contents, so models fall back to `find` through `run_shell`.

10. **Custom commands from markdown templates.** `~/.config/maic/commands/NAME.md` and `.maic/commands/` with `$ARGUMENTS` and `$1..$n`, invoked as `:NAME args` or `/NAME args`. Read `packages/opencode/src/command/index.ts` (`hints`, template loading, `subtask` flag) and `packages/opencode/src/config/command.ts`. Skills (`packages/opencode/src/skill/index.ts`) are the same idea invoked by the model, and can come later on the same code.

Honourable mentions, not in the ten: tool output spilled to a file the model can read later (`packages/opencode/src/tool/truncate.ts`); "file not found, did you mean" suggestions in `read` (`packages/opencode/src/tool/read.ts` `miss`); hiding fully-denied tools from the schema (`visibleTools` in `packages/opencode/src/permission/index.ts`); the leader-key keybind config shape (`packages/tui/src/config/keybind.ts`).

## 5. Things MAIC should NOT copy

* **`--auto` / `--yolo` / `--dangerously-skip-permissions`** (`packages/opencode/src/cli/cmd/run.ts:244-274`). MAIC's `--mode auto` is already the bounded version: automatic inside the workspace, inside the sandbox, with trips still armed. A flag that approves everything not explicitly denied has no place next to a tripwire.

* **Permission checks inside each tool.** opencode's `ctx.ask` per tool (`src/tool/tool.ts:45`) means MCP tools, plugin tools and any tool that forgets to ask are unchecked. MAIC's single gate in `Agent::run_tool_call` must stay the only path; new tools get a `tool_actions` mapping (one action per path they touch), not their own approval code.

* **Unsandboxed bash with syntax-based path guessing.** `src/tool/shell.ts` skips dynamic arguments and runs with the user's full environment and network. MAIC should keep deciding as little as possible from command text and letting bwrap enforce it; the text checks stay as the trip layer.

* **`*: allow` as the default ruleset** (`src/agent/agent.ts:120`) and `bash: allow` in the read-only `explore` subagent. MAIC's default is manual, and its plan mode is enforced by a read-only mount. If MAIC ever grows an explore agent, it runs in plan mode.

* **Process-wide or permanent "always".** V1 keeps approvals for the whole server process across sessions (`src/permission/index.ts` `approved`); V2 writes them to SQLite forever with no revoke surface (`packages/core/src/permission/saved.ts`). MAIC's session-scoped `always_allowed_` is correct. If persistence is ever added, it needs a listing and a revoke command first.

* **Plugin hooks that can change a verdict.** `permission.ask` in `packages/plugin/src/index.ts:261` can set `allow`. Code fetched from npm must never be able to lower a harness decision. The same goes for `tool.execute.before` rewriting arguments after the check.

* **Anything that talks to the internet from the agent.** Share upload (`src/share/share-next.ts` to `opncd.ai`), models.dev catalog (`packages/core/src/models-dev.ts`), `websearch` via Exa/Parallel, `webfetch`, remote MCP with OAuth (`src/mcp/`), `autoupdate`, `remote_config`, the OpenTelemetry option in `experimental`, and the account/console integration. MAIC's only outbound traffic is the chosen model endpoint, and the status line already says when that is remote.

* **Reading `~/.claude/CLAUDE.md` and project `CLAUDE.md` silently by default** (`src/session/instruction.ts:62-66`). Instruction files change the model's behaviour; MAIC lists the ones in effect at startup (`cli/src/tui.cpp` "instructions:" notice) and should keep the list explicit in `settings.json` rather than picking up another tool's files unasked.

* **Managed config from `/etc`** (`src/config/managed.ts`). It is meant for MDM fleets. MAIC's root-owned surface is deliberately tiny (the lock file and `maic-lock`); adding a root-owned config path widens what a compromised root-owned file could do.

* **The server-first architecture and the Effect/TypeScript stack.** opencode's TUI is an HTTP client of its own server, which is why it has a web UI, a desktop app and a Slack bot, and also why permission replies have no origin. MAIC's `AgentEvents` interface is the right boundary: the server comes later as one more implementation of it, and `Origin::Remote` is decided by the core, not by whoever answered the prompt.

* **Agent-as-mode.** opencode switches between `build` and `plan` agents with tab and expresses everything else as rules. MAIC's five-step mode ladder with a read-only sandbox for the lower rungs is simpler to reason about and to explain in a status line; keep it.
