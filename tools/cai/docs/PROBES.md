# Probes — measured facts about Claude Code

**What this is.** Facts about the platform, established by running something and reading the result, that the designs in this repository depend on. Each entry says what was measured, how, and what rests on it.

**Why it is separate from everything else here.** `DESIGN.md` holds what we *decided*. `BUILDING-OF-TRANSFAIRY-STEPS.md` holds what construction *taught*. `IMPROVEMENTS.md` holds what is *deferred*. This file holds what is *true about the platform* — none of which is ours to decide, all of which can change under us when the client changes.

**Every entry carries the client version it was measured against.** A probe is a fact plus a scope; cited outside that scope it is a guess again. Where a probe has a positive control, the control is named — an empty result is not a finding until a query known to return something returns something through the same path.

**Nothing here is designed.** If an entry starts arguing for a design, it belongs in `DESIGN.md` and only its measured half belongs here.

## Method, for anything added later

**The harness has its own document — [`isolated-harness.md`](isolated-harness.md)** — which carries the alternate-root method, the lighter throwaway-cwd form, and the discipline both share. In short: craft a transcript in a **throwaway cwd**, install it, resume it headless with `claude -p --resume <sid> --fork-session`, and ask a question whose answer can only come from the crafted content. `--fork-session` is what makes this safe — the resume writes a new session and leaves the crafted original untouched. Clean up the throwaway project directory afterwards.

---

## UNVERIFIED — three load-bearing claims that were never measured

**Not probes. Listed here because this is where a reader comes to find out what is measured**, and these three are stated flatly as fact throughout the design while never having been checked. Self-reported as unverified by the agent during the build, 2026-08-24; the operator's answers are recorded with each.

**U1 — `cp -n` declines silently and returns success.** Invariant 2 rests on this entirely. *Her answer: she can check it against the man pages herself.* **Nothing depends on it being true**: the tool never shells out to `cp` at all, and a test asserts that. If the claim were false, invariant 2 would lose its rationale and keep its behaviour.

**U2 — single-dash multi-char flags collide with argparse abbreviation handling.** The project-wide no-multi-lettered-short-forms rule rests on it. *Her answer: her own C experience covers it* — and that argument, stated in the nomenclature table, is independent of the argparse claim.

**U3 — `CLAUDE.md` is discovered by walking up from cwd.** The whole memory/cwd-keying analysis and open item 2 rest on it. *Her answer: "I kinda ignored the CLAUDE.md one. I figured you didn't understand my ask."* **Open item 2 is unruled anyway**, so nothing built depends on it.

**All three are one command each and none is urgent**, precisely because each supports a rationale rather than a behaviour. Recorded so the difference between *measured* and *asserted* stays visible in the file whose job is that distinction.

## P1 — Meta records do not reach the model's context

**Measured 2026-08-28, claude 2.1.236.** A canary string placed only in a `system` record with `isMeta: true` was **not** seen by the resumed model (it answered NONE). The same canary in a `user` record's message content **was** echoed back — that is the positive control, and it is what makes the negative result mean something rather than indicating a broken harness.

**What rests on it:** the whole graft/inject regime split. `graft` puts its seam in message content because the agent *should* see it; `inject` puts its announcement in a meta record because the operator should see it and the agent should not. Closed `DESIGN.md` open item 11.

**Scope, stated honestly:** one meta shape was tested (`system` + `isMeta` + a custom `subtype`). It is the shape `inject` uses. It is not a proof about every record type the client may treat as meta.

## P2 — Unknown `origin.kind` and unknown `system.subtype` both load

**Measured 2026-08-28, claude 2.1.236.** A transcript carrying `origin.kind: "graft"` on a user record loaded and resumed cleanly, as did one carrying a `system` record with an unknown `subtype`. Neither broke loading.

**Extended 2026-08-31:** `origin.kind` on an **assistant** record also loads *and* the record still reaches context — item 12 had only tested a user record. So graft and inject may mark rows of any role.

**What rests on it:** `origin.kind: "graft"` / `"inject"` and the `graft-boundary` / `inject-boundary` seam records are safe to emit. Closed open item 12.

## P3 — An unknown record `type` is dropped; an unknown `message.role` is not

**Measured 2026-08-31, claude 2.1.236, with a positive control.** Two variants of the same content record:

| variant | result |
| --- | --- |
| normal `assistant` record (control) | canary echoed |
| top-level `type: "injected-responder"` | **record dropped, content never reached the model** |
| `type: "assistant"` with `message.role: "injected-responder"` | kept, canary echoed |

**This refines P1.** It is the unknown *type* the client ignores, not an unknown *role* inside a known type.

**What rests on it:** the turn-role ruling. Injected content keeps its natural role and is marked in `origin.kind`; renaming the record `type` would silently destroy the priming that is the entire point of `inject`. A role-level label remains *possible* at `message.role` only, and is parked in `IMPROVEMENTS.md`.

## P4 — A real transcript is much richer than anything this repo builds

**Measured 2026-08-31** against a live 7,191-record transcript. Fifteen record types, against the seven our builder emits: `ai-title`, `agent-name`, `custom-title`, `mode`, `permission-mode`, `atis-latch`, `queue-operation`, `system`, `user`, `assistant`, `attachment`, `last-prompt`, `file-history-snapshot`, `file-history-delta`, `bridge-session`.

**Frame records are per-turn, not once per file** — 215 `ai-title` records in that transcript, not one. **The `parentUuid` chain runs through every type**, not only `user`/`assistant`. Real records also carry fields our grammar does not model (`sessionKind`, `isCompactSummary`, `isVisibleInTranscriptOnly`, and a richer `usage`).

**What rests on it:** `truncate` keeps a prefix **verbatim** and runs cut-specific verification, because the six build checks are build-output verification and reject healthy real transcripts. Anything else reusing `verify()` as general validation carries the same defect. See `IMPROVEMENTS.md`.

## P8 — `--fork-session` writes a new transcript in the projects dir; a plain resume appends in place

**Measured 2026-08-31, claude 2.1.236, both halves of the pair.**

| | original file | result |
| --- | --- | --- |
| `claude -r <sid>` | **1,218 B → 14,554 B, appended in place** | same file, same session id, no new transcript |
| `claude -r <sid> --fork-session` | **byte-unchanged** | a **new** file with a **new** session id, whose internal `sessionId` matches its filename |

**Both write to the projects directory keyed to the cwd.** There is no path form: `-r` takes a session id (or opens a picker), so a transcript outside a projects directory cannot be resumed at all — which is why `install` exists and why it is the only thing here that writes there.

**A fork cannot clobber.** The new id is minted by the client, the original is untouched, and the new file is self-consistent. That is the same guarantee `build` gives by minting from the pool, arrived at from the client's side.

**What rests on it:** the pre-resume snapshot is **belt-and-braces rather than required**, provided the resume forks. A plain resume genuinely does destroy the pristine as-built — that claim is now measured rather than repeated. `install`'s printed command uses `--fork-session` for exactly this reason.

**Incidental:** a resume also creates a `memory/` directory in the projects dir if absent. Worth knowing when counting what a run produced, and it is the auto-loading channel P-nothing-here covers — see the contamination note in `transfairy/DESIGN.md`.

## P10 — The ending meta records are not required for a resume

**Measured 2026-08-31, claude 2.1.236, against a real truncated transcript.** A 1,231-record verbatim prefix of a live transcript — carrying **no** `ai-title`, **no** `last-prompt`, and **no** closing `file-history-snapshot`, because those cluster later in the file — was installed and resumed successfully. Asked what had last been discussed, the resumed model answered correctly from the final records of the prefix.

**So minimal is correct.** A transcript does not need the ending frame records to load; the message chain carries the context. This is the answer to *"create the new session minimally, and with the correct shape if the ending meta tags are hard to generate"* — they are not hard, they are **unnecessary**, and a verbatim prefix is already the correct shape.

**What rests on it:** `truncate` keeps a prefix verbatim and regenerates nothing. It also means the cut point is the only thing that determines where a resumed session believes it is — no tail repair, no re-pointing, nothing to get subtly wrong.

**Scope:** a *prefix* is safe because every record's parent is earlier. This says nothing about a transcript assembled some other way; a built transcript still emits its frame records, and `build`'s conformance check still governs there.

## P11 — A native `--fork-session` records no lineage to its parent

**Measured 2026-08-31, claude 2.1.236.** A transcript was resumed with `--fork-session` and the resulting child searched for any reference to the parent session id: **zero records mention it**, and no field name in the fork carries fork/ancestor/parent-session semantics. The child is a complete, self-consistent transcript with no way to establish where it came from **from inside the file**.

**What rests on it:** there is no native shape for us to mirror, so trans-fairy's own lineage records are an addition rather than a copy. A `graft` seam already carries `baseSessionId`/`graftSessionId`, and a **reflow** records what it was reflowed from the same way. **These are strictly more traceable than a real fork** — which is the argument for emitting them rather than leaving a reflow to look like an unrelated transcript that happens to share content.

**Reading it the other way:** if a file's provenance matters, it must be written into the file, because the client will not do it. That is the same conclusion the graft design reached from the honesty side, arrived at here from the client's behaviour.

## P12 — A turn's visible text usually reaches the transcript before that turn's tool call, but not reliably

**Measured 2026-09-01 across two sessions, claude 2.1.236.** A message emitted before a tool call in the same turn was found in the transcript **by that same tool call**, confirmed with a canary phrase planted in the visible text and grepped for immediately after. **The write normally lands first.**

**It is a race, not a fixed ordering, and both outcomes were observed within two turns of each other.** On one turn the preamble was present when the tool read the file; on the immediately preceding turn the same operation ran while the file was still 9,402 lines and that turn's preamble did not appear until line 9,404 — after the read. **Same operation, opposite results.**

**What rests on it:** a carry that assumes its own final message has landed is right most of the time and silently short by one turn when it is not. **So the carry verifies rather than assumes** — it checks for its own text in what it collected, and the check is the difference between a correct carry and a quietly truncated one. This is why the ending-text capture is possible at all, and why it cannot be taken on faith.

## P13 — A transcript's conversation is roughly 7% of its raw size, and the post-compaction slice is bounded by construction

**Measured 2026-09-01 on a 9,435-record session, claude 2.1.236.**

| | ~tokens | share of raw |
| --- | --- | --- |
| whole file, raw JSONL | 3,925,000 | 100% |
| all message content, tools included | 1,842,000 | 47% |
| — of which tool_use and tool_result | 706,000 | 18% |
| — of which thinking blocks | 841,000 | 21% |
| **user and assistant text only** | **281,000** | **7.2%** |
| user turns only | 85,000 | 2.2% |
| **past the most recent compaction boundary, text only** | **16,600** | — |

**Reading the file whole is not wasteful, it is impossible** — 3.9M tokens is roughly four times a 1M window, and even message content alone is nearly double it. **84% of the content is thinking blocks and tool traffic**, which is the bulk a naive read would spend its budget on.

**The structural point outlives the numbers.** Compaction fires at a context threshold, so the material accumulated since the last one is **bounded by construction**, and its text projection is under a tenth of that bound. **Re-reading what a compaction just dropped does not get more expensive as a session ages** — it stays permanently affordable, while a full-history read grows until it stops fitting.

**A compaction boundary is a field, not a phrase.** The summary record carries **`isCompactSummary: true`** and a `compactMetadata` object, on a `user` record. **That transcript has three**, and a text search for *compact* finds a fourth that is a task notification mentioning the word — so `--since-compaction` is a field lookup, and searching for the summary's opening sentence would over-count.

**What rests on it:** this is the measurement behind the projection operation, and it is what makes a post-compaction re-read a routine step rather than a last resort. **Compaction becomes deferred rather than lossy** — the context is not gone, only the in-context copy is. The caveat is that this excludes tool traffic, so anything whose meaning lived in a tool result rather than in what was said does not come back this way.


## P9 — There is no path-based transcript loader; `CLAUDE_CONFIG_DIR` relocates the whole root instead

**Measured 2026-08-31, claude 2.1.236.** The full flag surface carries **no way to load a transcript file by path.** `-r/--resume` takes a session id or opens a picker; `--session-id` *assigns* an id to a new session rather than loading one; `--file` downloads file resources by id and is unrelated to transcripts; `--continue` resolves the most recent session for the cwd.

**What does work is moving the root.** With `CLAUDE_CONFIG_DIR` pointed at an alternate directory, a transcript placed only at `<alt>/projects/<slug>/<sid>.jsonl` was **found and resumed**, and `--fork-session` wrote the fork **into the alternate tree**. Nothing appeared in `~/.claude/projects/` for that cwd. So the projects root is relocatable, and that is the only supported way to resume a transcript living somewhere else.

**The catch, and it is not small: the config dir holds credentials too.** The run failed at inference with *Not logged in · Please run /login* — the transcript loaded and the fork was written before it needed the API. So relocation gets you loading and forking for free, and costs you authentication, which has to be solved separately and deliberately. **Nothing here recommends copying credential material around**; that is an operator decision with its own risks.

**What rests on it:** a genuinely isolated test harness is possible — an alternate root means an experimental transcript never touches the real projects directory at all, which is a stronger form of the rule that silent injections are never installed. It also confirms `install`'s design: since no path loader exists, writing into a projects directory is the only way to make a built transcript resumable, which is why that is the one thing the tool writes there.

## P5 — The projects-directory key replaces `/` and `.` with `-`

**Measured 2026-08-31.** `/home/cragady/dev2/Cascade/SOPIA` keys to `-home-cragady-dev2-Cascade-SOPIA`; a path containing a dot has the dot replaced too, which is why an early probe under a `.claude` path failed to resolve.

**What rests on it:** `paths.slug()`, and therefore where `install` writes and where a resume must be launched from.

**Scope:** verified on plain POSIX paths. One project directory on this machine carries a doubled dash no existing path reproduces, and Windows keying — drive letters, backslashes — is unverified. Recorded in `IMPROVEMENTS.md` under Windows.

## P6 — git does not preserve a read-only seal

**Measured 2026-08-28, with a positive control.** A file at `444` beside one at `755`, committed and cloned:

| | source | git index | clone (umask 002) |
| --- | --- | --- | --- |
| sealed file | `444` | `100644` | `664` |
| executable | `755` | `100755` | `775` |

**The executable bit survived and the read-only bit did not** — the control that makes the zero meaningful. And the clone comes back **group-writable**, which is looser than the `644` git actually recorded, because checkout takes the mode from the umask.

**What rests on it:** the argument against a git-backed `bak/` in `DESIGN.md`. A seal survives `cp -rp` and does not survive a clone.

## P7 — `cat >` takes the umask; `cp -p` carries the source mode

**Measured 2026-08-31, umask 002.** A `600` source file: `cat src > dest` produced `664`, `cp -p src dest` produced `600`.

**What rests on it:** `cp -p` as the install verb, and the mode half of the `mv`/`cp`/`cat` comparison in `BUILDING-OF-TRANSFAIRY-STEPS.md` lesson 4.

## P14 — compaction is marked TWICE, and a composed transcript carries only one

**Measured 2026-09-03 across 220 transcripts in the projects directory.**

```
  isCompactSummary   a FIELD on the summary record      71 records, 66 files
  compact_boundary   a system subtype ONE record before 16 records, 12 files
  files with compact_boundary and no isCompactSummary          0
```

**They are a pair, and the ordering is exact**: `compact_boundary` at index N, `isCompactSummary` at N+1, in every file carrying both — no exceptions across the set.

**One transcript breaks the pattern, and the reason is the useful part.** `b5d42ad9` carries `isCompactSummary` at 366 and 3286 but `compact_boundary` only at 3285. **The first summary was carried in by a re-root**: `compose` brought the predecessor's summary record across and not the boundary meta record that preceded it. So a composed transcript can hold a summary with no boundary beside it.

**Which marker a tool reads therefore matters, and the suite reads the safe one.** `compaction_boundaries()` keys on `isCompactSummary`, which is present in every case measured — including the composed one. **Keying on `compact_boundary` would have silently missed a real compaction in exactly the transcripts this suite produces**, which is the worst place to have a blind spot.

**Not verified:** whether `compact_boundary` is newer than `isCompactSummary` or simply emitted under narrower conditions. 16 against 71 is consistent with either, and nothing here distinguishes them.
