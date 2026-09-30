# Roadmap

What MAIC should become, beyond what is built. Ordered roughly by value. Anything here is fair game to pick up; the rules in [harness.md](harness.md) and [cleanroom.md](cleanroom.md) apply to all of it.

Status of the built parts: [README.md](../README.md), [cli/README.md](../cli/README.md).

## 1. Remote access

The reason `server/` exists. Micaiah wants to chat with agents from her phone.

* **`server/`**: a MAIC server process that owns agent sessions and speaks a small JSON protocol over a socket (local Unix socket first, then TLS on a port). The CLI becomes one client of it; the current in-process agent stays for offline use.
* **Rendezvous**: the server does not have to run the models. It can forward to Ollama on the same machine, to a remote provider, or act as a meeting point between a phone and a workstation behind NAT (an outbound connection from the workstation to a small relay; the relay never sees plaintext).
* **Harness rules for remote clients**: every request from a client that is not the local terminal is `Origin::Remote` and is always asked; approvals can be answered from the client, but `unlock` cannot (it needs the local sudo password).
* **A client**: a web app first (works on any phone, no store), then Android/iOS if the web app is not enough. Shows the conversation, streams replies, answers approvals, lists sessions, switches models.
* **Auth**: a per-device token created on the workstation, TLS with a pinned certificate, rate limits, and an audit line per remote request.

## 2. Harness: the planned layers

From [harness.md](harness.md): doom-loop detection wired to the tripwire; reject-with-feedback at the approval prompt; a diff shown before an edit is approved; git undo points; an additive `permission` block in settings; role profiles (orchestrator, builder, scout, reviewer); token and cost budgets (usage tracking is in place); Landlock and resource limits; per-tool network grants; forkable transcripts with `--fork-at`; Docker as a service runtime.

## 3. Quick wins from opencode

[opencode-quick-wins.md](opencode-quick-wins.md) lists 23 small items. Done so far: token usage and the context readout (18), foldable tool output (19). Next: items 1 to 9 (tool-result quality for small models), then retry with backoff (17), then `:compact` (23).

## 4. Absorbing cai-tools

`~/dev2/cai-tools` (`cai`, a uv-installed Python tool) is Claude-specific tooling: `trans-fairy` rebuilds a claude.ai chat export into a resumable transcript, `redact` removes credential material from a transcript, plus registered subtools (commit, document, edit, fabricate, flow, grammar, grant, hook, name, notation, read, reflow, sync, timekeeping, ...). None of it calls a model itself; the `claude-opus-5` strings are metadata written into transcripts.

Plan: bring the transcript work into MAIC as `maic sessions import` (claude.ai exports and Claude Code JSONL into MAIC sessions) and `maic sessions redact` (credential scrubbing, same patterns), then look at the other subtools one by one; any that need a model use MAIC's providers, Ollama by default. Deprecate each `cai` command as its MAIC replacement lands.

## 5. Tools and the polyglot spokes

* The `tools/` design from `programming-lang-for-agentic-cli.md`: tools as manifests plus scripts in Lua, Perl, Python, TypeScript, Go, WASM and shell, run through the harness with per-tool network grants.
* More built-in tools: `glob`, `question` (ask the user something structured), a todo/plan tracker, `web_fetch` behind an explicit grant.
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
