# Roadmap

What MAIC should become, beyond what is built. Ordered roughly by value. Anything here is fair game to pick up; the rules in [harness.md](harness.md) and [cleanroom.md](cleanroom.md) apply to all of it.

Status of the built parts: [README.md](../README.md), [cli/README.md](../cli/README.md).

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

Done: repeated-call refusal and trip, deny with a reason, a change preview at approval, undo points, token budgets, `workdir`. Still from [harness.md](harness.md): an additive `permission` block in settings; role profiles (orchestrator, builder, scout, reviewer); cost budgets; Landlock and resource limits; per-tool network grants; forkable transcripts with `--fork-at`; Docker as a service runtime.

## 3. Quick wins from opencode

[opencode-quick-wins.md](opencode-quick-wins.md) lists 23 small items. Done: 1 to 9 (tool-result quality for small models), 18 (token usage and the context readout), 19 (foldable tool output), 23 (`:compact`, with Micaiah's two-stage design below rather than opencode's whole-history summary). Also done: retry with backoff (17); the per-turn footer (10); markdown export and `:copy` (11); sessions grouped by day with titles (12); `}` `{` `]]` `[[` message jumps (13); timestamps (14); `workdir` (15); `:stash` / `:pop` (16); nested AGENTS.md on read (20); `:rename` and opt-in auto-titles (21). Left: DeepSeek reasoning replay (22), verify-first against the public API reference.

## 3a. Compaction

Built as Micaiah specified it: tool results are what fill a context and dialog is cheap, so compaction stubs old tool results first and keeps every word of the dialog; only when that is not enough does it summarise the oldest turns into a handover note, moving the compaction point forward each time so the recent conversation is always verbatim. `:compact all` is the traditional whole-history summary, available but never the default. Consistent flow is preferred over rewind-ability. Still to do: a `:rewind`/`--fork-at` that starts a fork from an earlier record, and letting the cai-tools transcript work (below) reuse the same handover-note format.

## 4. Absorbing cai-tools

`~/dev2/cai-tools` (`cai`, a uv-installed Python tool) is Claude-specific tooling: `trans-fairy` rebuilds a claude.ai chat export into a resumable transcript, `redact` removes credential material from a transcript, plus registered subtools (commit, document, edit, fabricate, flow, grammar, grant, hook, name, notation, read, reflow, sync, timekeeping, ...). None of it calls a model itself; the `claude-opus-5` strings are metadata written into transcripts.

Plan: bring the transcript work into MAIC as `maic sessions import` (claude.ai exports and Claude Code JSONL into MAIC sessions) and `maic sessions redact` (credential scrubbing, same patterns), then look at the other subtools one by one; any that need a model use MAIC's providers, Ollama by default. Deprecate each `cai` command as its MAIC replacement lands.

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
