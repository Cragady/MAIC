# Roadmap

This is the driver: work is picked from the top of the "Next" list, and nothing lands that is not on it or a quick fix. The rules in [harness.md](harness.md) and [cleanroom.md](cleanroom.md) apply to all of it. What is built is listed at the end, so the list above it stays honest.

Status of the built parts in detail: [README.md](../README.md), [cli/README.md](../cli/README.md).

## Next, in order

### 1. Rendezvous relay for the phone

An outbound WebSocket from the workstation to a small relay, end-to-end encrypted after a one-time pairing on the LAN, so the phone reaches home without an open port and the relay sees nothing. Design in [remote.md](remote.md). The tripwire stays local-only: no route on the relay can unlock. Depends on nothing; the web client works over it unchanged.

### 2. Accounts on maic-server

Micaiah's decision (2026-09-30): accounts belong to MAIC's own server, never to llama.cpp (which has API keys only, no identities). Design document first, then build; security-sensitive, so the design is reviewed before code.

* `maic join SERVER`: tells a client which maic-server to use (the `remote` setting plus a login); `maic open` and the apps then work against it.
* Login: username and password, or OAuth (sign in with a provider instead of a password; the provider configured per server; tokens never stored in plain text).
* Two-factor authentication on login (TOTP first, passkeys when the client can), per account, with a server setting that enforces it for everyone or leaves it optional.
* Email verification on signup: a code to the address, activation on confirmation. The mail step is the one outbound request MAIC's server makes, on the user's explicit action, configurable to a local relay.
* Best practice throughout: **argon2id** for password hashing (a hard requirement), short-lived session tokens with refresh and revocation, rate limits on login and codes, no secrets in URLs, a recovery path that does not weaken 2FA, the audit log naming the user.
* Per-user session ownership, an admin list, the per-device tokens folded into per-user ones.

Depends on 1 for phone use away from the LAN, not for the LAN itself.

### 3. One remote interface: MAIC plus llama.cpp

The web app and the phone apps present one clean interface that combines the MAIC server (sessions, approvals, the harness, services) and the llama.cpp server (models, loading and switching, sampling and XTC, a plain chat with the loaded model), so remote access gives both. llama.cpp stays behind MAIC's server, never exposed on its own; MAIC's server proxies what the app needs. A native client (Android first) follows the web app: notifications for pending approvals, pairing in the app, background reattach. Depends on 2.

### 4. Prompt profiles per backend

Measured on 2026-09-30 with qwen3.5:4b: under Ollama and under llama.cpp alike, with MAIC's tool schemas attached, a short operator rule was ignored in every system-side placement and followed when it closed the user turn; the `operator_note` provider option came out of that. Next: a profile per backend (where operator text goes, whether a per-turn note is sent, how tools are described, prefill) chosen by provider kind and overridable in settings, measured rather than assumed, and re-measured with the 9B. Depends on nothing.

### 5. Tools and the polyglot spokes

* The rest of the `tools/` design from `programming-lang-for-agentic-cli.md`: tools as manifests plus scripts in Perl, Python, TypeScript, Go, WASM and shell, run through the same authorisation step, with per-tool network grants declared in the manifest.
* More MAIC-owned helpers in the style of `maic-storyboard`: single-entry drivers that do the mechanical part and hand a small model one decision per turn.

### 6. Absorbing the rest of cai-tools

`~/dev2/cai-tools` is Claude-specific tooling; import, redact and fork-at are in MAIC ([sessions.md](sessions.md)). Still in cai, each a candidate once it is clear what it means against a MAIC session: `trans-fairy compose`, `install --graft-onto`, `install --inject`, `state`, `trans-fairy-write`, `redact --project`, and the subtools notation, grant, commit, enroll, hook, edit, time, document, name, fabricate, sync, flow, read, reflow.

### 7. Windows

The tripwire design for Windows is in [harness.md](harness.md); the rest needs a port of the sandbox (AppContainer), the service manager (job objects with kill-on-close, junctions instead of symlinks, portable git on PATH, `%LOCALAPPDATA%\maic` for state and the uv cache), the runtime directory for temporary transcripts, and the terminal layer.

### 8. Editor and UI

* nvim as the highlighter for the input (an embedded `nvim --embed` over msgpack-rpc, one instance kept alive) for people who have it; the built-in highlighter stays the default.
* Macros, `W B E` and `%` in the input; diff rendering for edits in the conversation window.
* A settings key to make Enter send on one-line inputs, if it ever turns out to matter.

## Parked

* Compaction through a Lua hook from a helper script: the storyboard steps are small enough that a 4B does not need it; revisit if a helper ever does.
* A `:compact` that the model calls itself: the byte estimate and the too-long retry cover the cases seen so far.

## Done

Built, in the order it landed, so the list above is only what is left.

* **Core agent and TUI** (beta.1 to beta.3): vim-style input with text objects, marks, registers, `.` repeat, `gq`, `J r R s > <`, Ctrl-R/Ctrl-O; the conversation window with folds, yank, search, `}` `{` `]]` `[[` jumps, timestamps; Alt+Enter sends, `:w`, `:ww`, `:wq`; prompt history across sessions; `:stash`/`:pop`; Ctrl-Z suspend.
* **Sessions**: JSONL transcripts with homes (general, project, named), forks by pointer, `--fork-at`, `--record`/`--no-record` with temporary transcripts in the runtime directory, resume by id or path, titles and `:rename`, markdown export, import from claude.ai and Claude Code, redaction, the exit line with the way back.
* **Compaction, Micaiah's flow**: prune old tool results first, summarise the head only when that is not enough, `all` never the default; a byte estimate before each call and compact-and-retry on a too-long refusal.
* **Providers**: Anthropic, OpenAI-compatible; llama.cpp vendored at b11284 as the default, router mode over the models directory, `maic vendor model` checked downloads, thinking per request, later system messages as user notes; `--ctx` / `:ctx` / `context`; retry with backoff.
* **Ollama removed (2026-10-01)**: llama.cpp does all of it. The provider kind, the vendored release, `services/ollama.json` and the setup doc are gone; a bare model name goes to `llamacpp`.
* **Harness**: modes, tripwire (machine-wide, session-scoped, or isolated opt-out gated by `allow_isolated`), read-only classifier, sandbox, approvals with reasons and diffs, undo points, repeated-call guard that trips only for writes, the model reviewer (smart) or rules only (dumb, with the auto warning), the allow list, self-protection of MAIC's own files and locks, budgets, `workdir`, nested AGENTS.md on read, operator instructions (`--system`, `:rule`, `--no-instructions`), prefill, bans (string, regex, token) and sampling passthrough with XTC.
* **Tools**: read, list (tree), glob, search, write, edit, multi_edit, apply_patch, move, copy, delete, make_dir, run_shell, question, todo; user-defined Lua tools behind the harness; `maic-workflow-edit` and `maic-storyboard` for ComfyUI.
* **Services and vendoring**: pinned submodules, `maic vendor add|adopt|use|model`, ComfyUI with MAIC's own llama.cpp node (the Ollama node retired), the artifact tree with workflows and templates, `maic path`/`open`/`shell-init` (`mcd`), menus for `up`, `down`, `unlock`, `open`, browser choice and a subscribed remote.
* **Two models at once (2026-10-01)**: a second llama-server, `llamacpp-2` on port 8082 over the same GGUFs with its own `context_2`; the `llamacpp-2` provider; `--ctx2` / `:ctx2`; `maic gpu` and `maic doctor` say whether the two models fit the card; the smart harness reviews on the side server when it is up, so the main model is never evicted; the Story chat deep pass points at it.
* **Services: Docker, setup, health (2026-10-01)**: `"runtime": "docker"` in a service file (image, volumes under MAIC's trees, env, `--gpus all`), loopback only, shown as `[docker]` with its container, started, inspected, logged and stopped through docker, `services/comfyui-docker.json.example` as the shape; `maic setup`, the first run as yes/no questions over prerequisites, settings (asks `models_dir`), llama.cpp, ComfyUI, a checked Qwen3.5 GGUF and the tripwire, the plan alone off a terminal; health beyond the port: the resident model, ComfyUI's VRAM and queue under `maic status`, `ready_pattern` in the service files, the torch-versus-driver CUDA check in `maic doctor`; from the Stability Matrix assessment: `maic vendor wire comfyui` regenerating the `maic:` block of `extra_model_paths.yaml` from the manifest's models map, `maic-workflow-edit check` for node types nobody provides (offline), adopt reading the git remote and ref.
* **Remote access, first slice**: `maic-server` with per-device tokens, TLS off loopback, audit log, SSE streaming, approvals over the API, a one-file phone-friendly web client; `Origin::Remote` always asked.
* **Tests and tooling (2026-10-01)**: the agent tests made deterministic (a workspace and state directory per run, `FakeServer` holds a stream open until the agent hangs up instead of racing a timer; twenty runs under `nice` with a build beside them, and two at once); `tests/tui_driver.py` and `tests/test_tui.py`, the TUI through a pty under a job-control stub, read back with pyte, ctest `tui` (skipped when pyte and uv are both missing); `core/tests/fuzz_parsers.cpp`, ctest `fuzz` (SSE, session JSONL, markdown, the ban filter; it found two ban-filter bugs, fixed); `scripts/check.sh` (configure, build, ctest, the python suites, gating on each; `--quick`), `scripts/check-asan.sh`, `release.sh` refusing to release when the gate fails, `.githooks/pre-push`; [testing.md](testing.md).
* **Subagents and role profiles (2026-10-01)**: the `delegate` tool runs a child agent in the workspace under a profile (`orchestrator`, `builder`, `scout`, `reviewer`, or one from `profiles` in settings) that only narrows: mode capped by the session's, write paths, tool list, step and token budgets, its own transcript of kind `sub` listed under the parent, approvals through the parent, one level only. The additive `permission` block (allow / ask / deny over `tool:pattern`) with the `allow` list folded in; it runs after the trip patterns, secrets and system paths and never touches them.
