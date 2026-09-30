# Roadmap

What MAIC should become, beyond what is built. Ordered roughly by value. Anything here is fair game to pick up; the rules in [harness.md](harness.md) and [cleanroom.md](cleanroom.md) apply to all of it.

Status of the built parts: [README.md](../README.md), [cli/README.md](../cli/README.md).

## 0. User accounts on maic-server (later)

Micaiah's decision (2026-09-30): accounts belong to MAIC's own server, not to llama.cpp (which has API keys only, no identities). Planned: user accounts with email verification (a signup that sends a code to the address and activates on confirmation), per-user tokens replacing the per-device ones, per-user session ownership and audit lines, an admin list. The mail step is the one outbound request MAIC's server would make, on the user's explicit action, and it must be configurable to a local relay. Not started.

## 1. Remote access

The reason `server/` exists. Micaiah wants to chat with agents from her phone. Design and reference: [remote.md](remote.md).

**Built:**

* **`maic-server`** (`maic server start`): owns agent sessions, one `Agent` per session id with its own transcript (kind `server`), and serves a JSON API with server-sent events for streaming. The CLI's in-process agent stays for offline use.
* **Harness rules for remote clients**: every request is `Origin::Remote` and is asked about in every mode; approvals are answered through the API; there is no route for `unlock`, only for `trip`.
* **The web client** (`server/web/index.html`): one file, no build, phone-friendly. Streams replies, shows tool calls, answers approvals (yes, no, no with a reason, always, trip), lists and resumes sessions, switches modes, reattaches after a lost connection.
* **Auth**: per-device bearer tokens (hashes on disk, constant-time compare, rate limited per source), TLS mandatory off loopback with a self-signed certificate made on first use or a configured pair, an audit line per request.
* **Workspace containment**: a remote session opens only inside `server.workspaces`.

**Still to do:**

* **Rendezvous relay**: an outbound WebSocket from the workstation to a small relay, end-to-end encrypted after a one-time pairing on the LAN, so the phone reaches home without an open port and the relay sees nothing. Designed in remote.md.
* **A native client** (Android first) when the web app is not enough: notifications for pending approvals, pinning and pairing in the app, background reattach.
* **Sessions across restarts in the client**: the API can `resume` a transcript by id; the client should list past server sessions, not only the live ones.

## 2. Harness: the planned layers

Done: repeated-call refusal and trip, deny with a reason, a change preview at approval, undo points, token budgets, `workdir`, forkable transcripts with `--fork-at` ([sessions.md](sessions.md)). Still from [harness.md](harness.md): an additive `permission` block in settings; role profiles (orchestrator, builder, scout, reviewer); cost budgets; Landlock and resource limits; per-tool network grants; Docker as a service runtime.

## 3. Quick wins from opencode

[opencode-quick-wins.md](opencode-quick-wins.md) lists 23 small items. Done: 1 to 9 (tool-result quality for small models), 18 (token usage and the context readout), 19 (foldable tool output), 23 (`:compact`, with Micaiah's two-stage design below rather than opencode's whole-history summary). Also done: retry with backoff (17); the per-turn footer (10); markdown export and `:copy` (11); sessions grouped by day with titles (12); `}` `{` `]]` `[[` message jumps (13); timestamps (14); `workdir` (15); `:stash` / `:pop` (16); nested AGENTS.md on read (20); `:rename` and opt-in auto-titles (21); DeepSeek reasoning replay (22), verified first against api-docs.deepseek.com/guides/thinking_mode: while a request carries `tools`, every earlier assistant turn must carry its `reasoning_content` back or the API answers 400 (without tools the field is ignored), so the streamed reasoning is kept in the message's `raw` and replayed for models whose id contains `deepseek`. All 23 are done. One caveat to watch: compaction drops `raw` from assistant turns by design, so a DeepSeek thinking model may reject the first request after a compaction; if it does, the fix is an empty `reasoning_content` on those turns, once the API's behaviour on an empty value is confirmed.

## 2a. Prompt profiles per backend

Measured on 2026-09-30 with qwen3.5:4b: under Ollama, with MAIC's tool schemas attached, a short operator rule was ignored in every system-side placement and followed only when it closed the user turn. The `operator_note` provider option came out of that. If llama.cpp (see [llamacpp.md](llamacpp.md)) measures differently, the next step Micaiah named is two comms modes: a prompt profile per backend (where operator text goes, whether a per-turn note is sent, how tools are described, prefill), chosen by provider kind and overridable in settings, rather than one layout for all.

## 3a. Compaction

Built as Micaiah specified it: tool results are what fill a context and dialog is cheap, so compaction stubs old tool results first and keeps every word of the dialog; only when that is not enough does it summarise the oldest turns into a handover note, moving the compaction point forward each time so the recent conversation is always verbatim. `:compact all` is the traditional whole-history summary, available but never the default. Consistent flow is preferred over rewind-ability. `maic -r ID --fork-at N` now starts a fork from an earlier record ([sessions.md](sessions.md)); an in-session `:rewind` on top of it, and letting the cai-tools transcript work (below) reuse the same handover-note format, are still to do.

## 4. Absorbing cai-tools

`~/dev2/cai-tools` (`cai`, a uv-installed Python tool) is Claude-specific tooling. None of it calls a model itself; the `claude-opus-5` strings are metadata written into transcripts.

**Now in MAIC** ([sessions.md](sessions.md)), written from the behaviour of the cai originals, not from their code:

* `maic sessions import`: what `cai trans-fairy split` + `build` + `install` did for a claude.ai export, and it also reads Claude Code's own JSONL. The target is a MAIC session rather than a Claude Code transcript, so there is no uuid pool, no version stamping and no install step: the file is a normal session that `maic -r` resumes.
* `maic sessions redact`: what `cai redact` did, with cai's opaque shapes (uppercase hex, long numbers, base64 blobs) plus named credential shapes (private keys, URL and command-line passwords, `KEY=value`, Bearer/Basic, JWTs, vendor prefixes). Like cai it reports shape and count, never a value, and never writes over an existing file. Not carried over: cai's uuid and version shapes (ordinary in tool output here), the `--map` file, `--substitutions`, `--blank-lines` and `--verify`.
* `--fork-at N`: the cut trans-fairy's `split --truncate --at-line` made, as a fork pointer instead of a rewritten copy.

**Still in cai**, each a candidate for its own `maic` command once it is clear what it would do against a MAIC session rather than a Claude Code one: `trans-fairy` `compose`, `install --graft-onto`, `install --inject` and `state` (composing and grafting transcripts, the lineage ledger and audit); `redact --project` (the conversation as readable text, close to `maic sessions export`); `trans-fairy-write`; and the registered subtools `notation`, `grant`, `commit`, `enroll`, `hook`, `edit`, `time`, `document`, `name`, `fabricate`, `sync`, `flow`, `read`, `reflow`. Any that end up needing a model use MAIC's providers, Ollama by default. Deprecate each `cai` command as its MAIC replacement lands.

## 5. Tools and the polyglot spokes

* Done: user-defined Lua tools the model can call, each in its own sandboxed LuaJIT state with the harness in front of every `maic.*` call ([tools.md](tools.md)); the built-in `glob`, `question` and `todo` tools.
* The rest of the `tools/` design from `programming-lang-for-agentic-cli.md`: tools as manifests plus scripts in Perl, Python, TypeScript, Go, WASM and shell, run through the same authorisation step, with per-tool network grants. `web_fetch` behind an explicit grant.
* Subagents: a scout/reviewer that runs with its own profile, reporting back into the main session.

## 6. Editor and UI

* nvim as the highlighter for the input (an embedded `nvim --embed` over msgpack-rpc, one instance kept alive) for people who have it; the built-in highlighter stays the default.
* `f`/`t`/`;` motions, `.` repeat, macros, marks, more text objects (`ip`, `is`), `gq` wrapping in the input.
* Diff rendering for edits, `{`/`}` message jumps, timestamps, session titles and `:rename`, markdown export and `:copy`, `:compact`.
* A settings key to make Enter send on one-line inputs, if it ever turns out to matter.

## 7. Services

* Docker as a service runtime (`"runtime": "docker"`), shown in `:status` the same way.
* `maic doctor` growing into `maic setup`: pull the recommended models, write the settings, install the tripwire, in one guided run.
* ComfyUI and Ollama health beyond "port open": model loaded, VRAM in use.

## 8. Windows

The tripwire design for Windows is in [harness.md](harness.md); the rest of MAIC needs a Windows port of the sandbox (AppContainer), the service manager (job objects) and the terminal layer.

## 9. Tests and tooling

* Make the timing-sensitive agent tests (cancel, mid-turn delivery) robust under load; they have flaked once under a parallel build.
* A UI test harness that drives the TUI through a pty in CI, like the ad-hoc driver used during development.
* Sanitizer and fuzz runs of the provider parsers and the markdown renderer.
