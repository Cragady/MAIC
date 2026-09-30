# Roadmap

This is the driver: work is picked from the top of the "Next" list, and nothing lands that is not on it or a quick fix. The rules in [harness.md](harness.md) and [cleanroom.md](cleanroom.md) apply to all of it. What is built is listed at the end, so the list above it stays honest.

Status of the built parts in detail: [README.md](../README.md), [cli/README.md](../cli/README.md).

## Next, in order

### 1. Two models at once: the second llama-server port

The Story chat deep pass wants the 9B while the 4B handles the quick pass. On an 8 GB card the router keeps one model resident (`--models-max 1`), so today the deep pass runs on whatever is loaded. Plan: a second `llamacpp-2` service on port 8082 with its own models root (or the same root and `--models-max 2` on a card that fits both), a `llamacpp-2` provider, and the ComfyUI node's second server node pointed at it; `maic doctor` says which layout the card can hold. Small, unblocks the workflow. Depends on nothing. Until then the card is shared by turn-taking: a service marked `needs_gpu` (ComfyUI) unloads llama-server's resident model when it starts, and llama-server reloads on the next request.

### 2. Rendezvous relay for the phone

An outbound WebSocket from the workstation to a small relay, end-to-end encrypted after a one-time pairing on the LAN, so the phone reaches home without an open port and the relay sees nothing. Design in [remote.md](remote.md). The tripwire stays local-only: no route on the relay can unlock. Depends on nothing; the web client works over it unchanged.

### 3. Accounts on maic-server

Micaiah's decision (2026-09-30): accounts belong to MAIC's own server, never to llama.cpp (which has API keys only, no identities). Design document first, then build; security-sensitive, so the design is reviewed before code.

* `maic join SERVER`: tells a client which maic-server to use (the `remote` setting plus a login); `maic open` and the apps then work against it.
* Login: username and password, or OAuth (sign in with a provider instead of a password; the provider configured per server; tokens never stored in plain text).
* Two-factor authentication on login (TOTP first, passkeys when the client can), per account, with a server setting that enforces it for everyone or leaves it optional.
* Email verification on signup: a code to the address, activation on confirmation. The mail step is the one outbound request MAIC's server makes, on the user's explicit action, configurable to a local relay.
* Best practice throughout: **argon2id** for password hashing (a hard requirement), short-lived session tokens with refresh and revocation, rate limits on login and codes, no secrets in URLs, a recovery path that does not weaken 2FA, the audit log naming the user.
* Per-user session ownership, an admin list, the per-device tokens folded into per-user ones.

Depends on 2 for phone use away from the LAN, not for the LAN itself.

### 4. One remote interface: MAIC plus llama.cpp

The web app and the phone apps present one clean interface that combines the MAIC server (sessions, approvals, the harness, services) and the llama.cpp server (models, loading and switching, sampling and XTC, a plain chat with the loaded model), so remote access gives both. llama.cpp stays behind MAIC's server, never exposed on its own; MAIC's server proxies what the app needs. A native client (Android first) follows the web app: notifications for pending approvals, pairing in the app, background reattach. Depends on 3.

### 5. Subagents and role profiles

A scout or reviewer that runs with its own permission profile (mode, write paths, budget) and reports back into the main session; profiles (orchestrator, builder, scout, reviewer) narrow only. The additive `permission` block in settings (allow / ask / deny per tool or command pattern) that can add restrictions or pre-approve harmless commands, never touch trip patterns, secrets or system paths. The `allow` list built on 2026-09-30 is the first slice. Depends on nothing.

### 6. Prompt profiles per backend

Measured on 2026-09-30 with qwen3.5:4b: under Ollama and under llama.cpp alike, with MAIC's tool schemas attached, a short operator rule was ignored in every system-side placement and followed when it closed the user turn; the `operator_note` provider option came out of that. Next: a profile per backend (where operator text goes, whether a per-turn note is sent, how tools are described, prefill) chosen by provider kind and overridable in settings, measured rather than assumed, and re-measured with the 9B. Depends on nothing.

### 7. Services: Docker, setup, health

* Docker as a service runtime (`"runtime": "docker"`), shown in `:status` the same way, with the same loopback-only rule.
* `maic doctor` growing into `maic setup`: pull the recommended model with `maic vendor model`, write the settings, install the tripwire, in one guided run.
* Health beyond "port open": model loaded and VRAM in use for llama.cpp and ComfyUI; the torch-versus-driver check from the Stability Matrix assessment; a `ready_pattern` beside the port check.
* From [assessments/stability-matrix.md](assessments/stability-matrix.md): a regenerated `maic:` block in `extra_model_paths.yaml` from a models map, an offline missing-node check for workflows, adopt verification against the git remote.

### 8. Tools and the polyglot spokes

* The rest of the `tools/` design from `programming-lang-for-agentic-cli.md`: tools as manifests plus scripts in Perl, Python, TypeScript, Go, WASM and shell, run through the same authorisation step, with per-tool network grants declared in the manifest.
* More MAIC-owned helpers in the style of `maic-storyboard`: single-entry drivers that do the mechanical part and hand a small model one decision per turn.

### 9. Absorbing the rest of cai-tools

`~/dev2/cai-tools` is Claude-specific tooling; import, redact and fork-at are in MAIC ([sessions.md](sessions.md)). Still in cai, each a candidate once it is clear what it means against a MAIC session: `trans-fairy compose`, `install --graft-onto`, `install --inject`, `state`, `trans-fairy-write`, `redact --project`, and the subtools notation, grant, commit, enroll, hook, edit, time, document, name, fabricate, sync, flow, read, reflow.

### 10. Windows

The tripwire design for Windows is in [harness.md](harness.md); the rest needs a port of the sandbox (AppContainer), the service manager (job objects with kill-on-close, junctions instead of symlinks, portable git on PATH, `%LOCALAPPDATA%\maic` for state and the uv cache), the runtime directory for temporary transcripts, and the terminal layer.

### 11. Editor and UI

* nvim as the highlighter for the input (an embedded `nvim --embed` over msgpack-rpc, one instance kept alive) for people who have it; the built-in highlighter stays the default.
* Macros, `W B E` and `%` in the input; diff rendering for edits in the conversation window.
* A settings key to make Enter send on one-line inputs, if it ever turns out to matter.

### 12. Tests and tooling

* Make the timing-sensitive agent tests (cancel, mid-turn delivery) robust under load; they have flaked once under a parallel build.
* A UI test harness that drives the TUI through a pty in CI, like the ad-hoc driver used during development.
* Sanitizer and fuzz runs of the provider parsers, the ban filter and the markdown renderer.
* The build gate: `ctest` passes on stale binaries when a test target fails to compile; the release and commit scripts must gate on the build's exit code first (done by hand since 2026-09-30, to be scripted).

## Parked

* Compaction through a Lua hook from a helper script: the storyboard steps are small enough that a 4B does not need it; revisit if a helper ever does.
* A `:compact` that the model calls itself: the byte estimate and the too-long retry cover the cases seen so far.

## Done

Built, in the order it landed, so the list above is only what is left.

* **Core agent and TUI** (beta.1 to beta.3): vim-style input with text objects, marks, registers, `.` repeat, `gq`, `J r R s > <`, Ctrl-R/Ctrl-O; the conversation window with folds, yank, search, `}` `{` `]]` `[[` jumps, timestamps; Alt+Enter sends, `:w`, `:ww`, `:wq`; prompt history across sessions; `:stash`/`:pop`; Ctrl-Z suspend.
* **Sessions**: JSONL transcripts with homes (general, project, named), forks by pointer, `--fork-at`, `--record`/`--no-record` with temporary transcripts in the runtime directory, resume by id or path, titles and `:rename`, markdown export, import from claude.ai and Claude Code, redaction, the exit line with the way back.
* **Compaction, Micaiah's flow**: prune old tool results first, summarise the head only when that is not enough, `all` never the default; a byte estimate before each call and compact-and-retry on a too-long refusal.
* **Providers**: Ollama, Anthropic, OpenAI-compatible; llama.cpp vendored at b11284 as the default, router mode over the models directory, `maic vendor model` checked downloads, thinking per request, later system messages as user notes; bare model names never reach Ollama; `--ctx` / `:ctx` / `context`; retry with backoff.
* **Harness**: modes, tripwire (machine-wide, session-scoped, or isolated opt-out gated by `allow_isolated`), read-only classifier, sandbox, approvals with reasons and diffs, undo points, repeated-call guard that trips only for writes, the model reviewer (smart) or rules only (dumb, with the auto warning), the allow list, self-protection of MAIC's own files and locks, budgets, `workdir`, nested AGENTS.md on read, operator instructions (`--system`, `:rule`, `--no-instructions`), prefill, bans (string, regex, token) and sampling passthrough with XTC.
* **Tools**: read, list (tree), glob, search, write, edit, multi_edit, apply_patch, move, copy, delete, make_dir, run_shell, question, todo; user-defined Lua tools behind the harness; `maic-workflow-edit` and `maic-storyboard` for ComfyUI.
* **Services and vendoring**: pinned submodules and checksum-verified releases, `maic vendor add|adopt|use|model`, ComfyUI with MAIC's own llama.cpp node (the Ollama node retired), the artifact tree with workflows and templates, `maic path`/`open`/`shell-init` (`mcd`), menus for `up`, `down`, `unlock`, `open`, browser choice and a subscribed remote.
* **Remote access, first slice**: `maic-server` with per-device tokens, TLS off loopback, audit log, SSE streaming, approvals over the API, a one-file phone-friendly web client; `Origin::Remote` always asked.
