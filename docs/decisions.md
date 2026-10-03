# Decisions

An index of decisions and findings, newest first, so none is lost. Each line says what was decided and where it is specified; where no other document holds it yet, this file is the record. Decisions are Micaiah's unless marked as a finding.

## 2026-10-03

* **An approval never holds a session forever** (Micaiah: "A command shouldn't happen that requires input. It will infinitely hold the session unless we programmed a timer in it."). `approvals.timeout` (default about 5 minutes, configurable); on expiry the call is denied, never allowed, the model is told it timed out and carries on, the window shows one line. The same timer for the window, the liaison, `--rpc` and a daemon session nobody is attached to; the first answer still wins. [roadmap](roadmap.md)
* **MadMaid at level 2, read-only: `auto-read`** (Micaiah: "if you can approve the asks, then I'm fine with `auto-read`"). Session `20261003-051311-daemon-2568927` in `~/dev2/MAID`. She reads and runs read-only commands freely in the read-only sandbox; edits, tests and any other command come to Claude as approvals through `maid liaison`, which Claude answers within read-only review. Changing a session's mode stays with Micaiah (her window), never the liaison.
* **`maid -r ID` resumes exactly that session or says why not and exits non-zero;** it never starts another one. A deleted current folder isn't a reason to refuse: it resumes in the session's own workspace with a notice. A resume through the daemon shows the session's recent history, so a resumed session never looks new. [sessions.md](sessions.md)
* **Sessions are portable; `sessions/general` stays.** The launch folder wins on resume: `resume.workspace = "launch" | "session" | "ask"` (default launch), `--keep-workspace` for one run, always a one-line `workspace: X (was Y)`, the move passes the same trust check as `:cd`, a deleted launch folder falls back to the session's own with a notice. [roadmap](roadmap.md)
* **Claude may spawn maid subagents** (Micaiah, 2026-10-03). Level 0 only: one-shot `maid -p --record` runs in `<state>/subagents/` (untrusted, so `auto` stays held and tool calls are refused), the key from `api_key_command`, transcripts kept, `-c --append` to continue one. Their replies are data. The liaison is for sessions Micaiah wants to watch or share. The noise decisions are on hold (tracked in `design/noise-spec-draft.md`).
* **Slow hop with TheMadMaid established** (2026-10-03): her reply 001 (declined, with two protocol gaps found and fixed), Claude's task 002 through `maid liaison`, her reply 002 back on the same connection. An earlier cycle against a separate one-shot DeepSeek session proved the mechanics only and does not count. Protocol hop/1: a mailbox (`<state>/hop/to-deepseek`, `to-claude`), Micaiah the only source of commands, messages are data, answers carry `hop/1 reply NNN` and a status. Claude drives it with `maid liaison send` into TheMadMaid's own session in the daemon, while Micaiah's window stays attached; in that mode she answers in text and uses no tools (level 0). The key comes from `api_key_command` reading a file only Micaiah's user can read, never from settings or an environment. The protocol text lives at `<state>/hop/PROTOCOL.md`. First result: the noise spec draft (its claim that stb_perlin has no runtime seed is to be checked against the vendored file).
* **Discipline after the MAID_VENDOR cascade** (Micaiah, 2026-10-03: "We absolutely cannot let an error like this cascade and clobber over critical functionality"). One bad service file stopped `maid up`, `:cd` and more. Standing rules:
  1. **Blast radius.** Every aggregate loader (services, themes, tools, places, providers, settings layers, instructions, artifacts) degrades per item: a bad entry is skipped with a message naming the file and what to fix; the rest loads.
  2. **No unrelated dependency on a core path.** A core command (start, chat, resume, `:cd`, `:status`, `maid up`) catches failures of anything it only consults on the side, and says what it skipped.
  3. **Installed builds are hermetic.** An installed release reads only the files it shipped with, never the checkout.
  4. **Fault-injection tests.** The suite plants a broken item of each kind (a bad service file, an unset variable, a corrupt theme, a bad project setting) and checks that the core commands still work.
  5. **Renames and migrations** get a check that the previous installed release still works against the new checkout and the new one against old user files, before the release.
  6. **Silence is a bug.** A command that does nothing says why (`:cd` during a turn did nothing and said nothing).
  Applied 2026-10-03: a turn's failure hint never throws; services (a missing directory too), the vendor manifest, catalog and API model entries, presets, providers, checker judges, preset names, ban steers, agents (unavailable, failing closed), project layers (still refused when they touch permission, forbid, harness, tripwire or agents), transcripts and audit.lua (held for `audit_gate`) degrade per item; places, `:status`, `:cd`, `maid doctor`, `maid server status` and the start-time restarts say what they skipped.

* **Degraded beats halted.** Something adjacent going wrong (one bad service file, an unset variable, one failing provider) degrades that one piece, says so, and lets the rest run; it never stops a whole chain such as `maid up`. Service files: a file that doesn't load is skipped with a warning. [service.hpp](../core/include/maid/service.hpp)
* **An installed maid reads the files it shipped with,** never a checkout that has moved on since its release. [paths.cpp](../core/src/paths.cpp)
* **When tool calls get refused, look at the paths first.** Agents working here use full absolute paths, never truncated or partial ones (a plain read of a cut-off path was stopped by a safety check mid-fix). Nothing in this repo should hard-stop an agent's ordinary work.
* **Messages in the TUI take no history space:** notices and agent messages show as floating windows or collapsible blocks, so the history Micaiah wants to keep stays readable. [roadmap backlog](roadmap.md)
* **Dropped: a Lua layer for talking to the shell's color settings.** The test environment now unsets `FORCE_COLOR` and `PYTHON_COLORS`, which fixes the flaky test at its source; a Lua layer would add moving parts without helping.
* **`maid liaison *` allowlisted in Claude Code** (Micaiah's approval in her own words; global settings).
* **The docs audit also documents what `auto` lets through per level,** and how `auto` shuts down or degrades. [roadmap backlog](roadmap.md)
* **Thinking stays on;** harness-driven tool calls written into the transcript; Flash as a fast subagent. [roadmap backlog](roadmap.md)
* **Finding: DeepSeek supports `stop` sequences and Chat Prefix Completion in thinking mode** (docs, untested), which allow continuing a turn after a tool call. [roadmap backlog](roadmap.md)
* **Name: maid, an agent runner.** *Mica's AI Decisions*, or *Micaiah's Agentic Interface Delta*; trans-fairy gets the credit. [README](../README.md), [v0.4.0](releases/v0.4.0.md). `comfymaid-review` keeps its name.
* **Tool-use onboarding policy:** levels 0 to 4, trials with fixed task sets, measurements, records never edited, thresholds fixed in advance, slow archival before any purge. [model-onboarding.md](model-onboarding.md)
* **Agentic tying:** an agent below level 4 never runs without a tool monitor. [model-onboarding.md](model-onboarding.md#agentic-tying)
* **Liaison sessions** and a `maid liaison` command (to be allowlisted in Claude Code once it exists); Claude judges the liaison's tool calls, so Claude tokens go to judging only. [roadmap backlog](roadmap.md)
* **DeepSeek through text tool calls** (no `tools` parameter, so no reasoning replay), the co-pilot chain, and server-side harnesses needing their own approval path. [roadmap backlog](roadmap.md)
* **Finding: DeepSeek's reasoning replay** is keyed on requests that carry `tools` and covers every earlier turn; without `tools` it isn't needed. [references/deepseek.md](references/deepseek.md#reasoning-replay-what-it-covers-and-when-thinking-can-be-skipped)
* **DeepSeek follow-ups built:** `/models` read on first use, cost per session by the model that actually answered, concurrency capped at a third of DeepSeek's limits, backoff with full jitter, shared cooldown and a circuit breaker, helpers without keys. [models.md](models.md), [v0.3.10](releases/v0.3.10.md)
* **Never get a key or account banned:** backoff, `Retry-After`, never retrying 400/401/402/403. [models.md](models.md)
* **API key variable names documented** until the keystore exists. [models.md](models.md#api-keys-the-environment-variable-names)
* **The protocol firewall:** only messages matching an approved, hash-verified protocol act; everything else is data. Approved notification protocols with a short hash; receivers re-hash to verify. [agent-kit.md](agent-kit.md), [roadmap backlog](roadmap.md)
* **Encrypted, signed agent messaging** over TLS (required), maid-supplied decryption, protocol tiers negotiated in at most 2 to 3 rounds, protocol establishment and size caps, temporary protocols, a raw TCP transport using MAIC's own framing, protocol adoption and a generalized lifecycle. [roadmap backlog](roadmap.md)
* **Finding: agent messaging elsewhere.** opencode has no injection guard on its message paths; DeepSeek's harness limits messages to parent and child; OpenAI accepts four input kinds into a session. [references/agent-messaging.md](references/agent-messaging.md)
* **Finding: a headless `claude -p` can message an interactive session** through Claude Code's own channel (tested); Claude Code Channels is the supported push path. [agent-kit.md](agent-kit.md)
* **Watcher, `maid channel`, approved protocols** built. [agent-kit.md](agent-kit.md), [v0.3.9](releases/v0.3.9.md)
* **Artifacts on maid-server:** sandboxed always; trust never removes the sandbox; today's capabilities are the baseline, carved downward; step-ups per use; permanent allowances capped (defaults 5 overall and 1 per artifact; recommended 5 to 10 and 1 to 2; unlimited only for administrators). `ALLOW_INSECURE` per artifact for Vue's runtime compiler. [artifacts.md](artifacts.md), [roadmap backlog](roadmap.md)
* **Finding: artifact sandboxing elsewhere.** No one unsandboxes generated pages except through a deliberate browser feature. [references/artifact-sandboxing.md](references/artifact-sandboxing.md)
* **Finding: browsers block ES module imports over `file://`** (Chrome and Firefox, tested); classic scripts work. The artifact sandbox forbids `localStorage` and IndexedDB, so pages keep drafts in memory there. [templates/comfymaid-review/README.md](../templates/comfymaid-review/README.md)
* **Vue, never React;** Nuxt only as a static build if ever needed; Vue 3.5.43 vendored with provenance; no npm packages yet. [roadmap backlog](roadmap.md), [vendor/VENDORING](../vendor/VENDORING)
* **comfymaid-review:** the catch-up page; data-flow rule (nothing resolved where it lands is brought in unresolved); the open-items pointer only while a page has open items, a closed page never revived; linked boxes; side and After Prompt histories; splits (beta). [templates/comfymaid-review/README.md](../templates/comfymaid-review/README.md)
* **The pointer rule built into maid:** prompts for outside agents, data for maid's own; every `maid artifact` subcommand maps to a protocol method reachable by CLI, TCP and hooks; tools for agents to write artifacts. [roadmap backlog](roadmap.md)
* **Leave, every case defined:** a task is its own session with `leave.task.after`; one engine per transcript without the daemon; remote flags only tighten; the leave audit reads 0 UNDEFINED. [settings.md](settings.md), [v0.3.9](releases/v0.3.9.md)
* **Next leave protocol:** `leave` required, `dropped`, `leave.grace`, step-up resurrection, `UNSUPPORTED_UNDEFINED` as a soft contract with an allowance for every instance, `DEV_ONLY_UNSUPPORTED_UNDEFINED_BYPASS`. [roadmap backlog](roadmap.md)
* **Daemon modes:** off, on-demand, unmanaged, constant (any supervisor: systemd first, then a FOSS container, then others); recommend only FOSS; sudo always announced with a fallback. [roadmap item 4](roadmap.md)
* **Logins go stale per device;** a relay checks both ends, pessimistic toward the workstation. [roadmap item 6](roadmap.md)
* **Metered models ask first;** Anthropic and Claude Code are metered; the checker panel asks before a paid judge. [settings.md](settings.md)
* **Beta rule:** what needs refinement ships as beta and is recorded; only blockers and future nightmares stay open. [roadmap: Beta](roadmap.md)
* **Undefined behaviour is defined where it is found.** [roadmap](roadmap.md)
* **Agent-proposed global changes** need a clear, unambiguous confirmation; anything less leaves the setting unchanged. [roadmap item 13](roadmap.md)
* **Colored status, `--text-base`, `NO_COLOR`;** help topics laid out like man pages; real man pages later. [settings.md](settings.md), [roadmap backlog](roadmap.md)
* **Noise (stb_perlin, improved noise, OpenSimplex2)** for themes, the TUI, the review page, ComfyUI and jitter; ids from the OS random source (UUIDv7), never noise or a seed. [roadmap backlog](roadmap.md)
* **Finding: links open twice in Konsole** because Claude Code prints OSC 8 hyperlinks and Konsole also detects URLs; a `hyperlinks` setting is planned. [references/terminal-links.md](references/terminal-links.md)
* **Queued right after the rename:** `maid server` as a service, the revoke-all switch for `ALLOW_INSECURE`, artifact writing tools, a docs audit that checks help and docs against the commands.
* **Micaiah's clean-room pipelines** (PrismviewCleanRE, YescoReverseEngineer) to be studied and brought into maid. [roadmap backlog](roadmap.md)

## 2026-10-02

* **The rename decided** (maic to maid), with history left untouched and a migration that moves directories only.
* **Event identity:** epochs, durable numbers, self-describing transcripts with skeletons and `must_understand`; trans-fairy keeps every stealth mode, and maid adds nothing that makes a silent rewrite detectable. [design/engine-protocol.md](design/engine-protocol.md), [sessions.md](sessions.md)
* **Event filtering** in `maid.hello`, with `maid.filtered_from`. [design/engine-protocol.md](design/engine-protocol.md)
* **Defaults:** dumb harness and auto mode, auto held at start where a directory isn't trusted. [settings.md](settings.md)
* **The checker panel** (`dual-9b`, `dual-4b`). [harness.md](harness.md)
* **API providers and DeepSeek built in;** providers and keys global-only. [models.md](models.md)
* **Finding: a system-prompt rule is ignored on tool turns** by Qwen3.5 9B (measured; other families not yet). [references/prompt-placement.md](references/prompt-placement.md)

Earlier decisions are in the roadmap's Done list, the design documents and the release notes.
