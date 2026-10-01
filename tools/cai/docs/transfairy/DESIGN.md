# trans-fairy — design

**This is the living copy.** Owner ruling, 2026-08-28: `cai-tools` is where this document lives, and it is the arrow of retrieval — anything looking for the design should end here.

**The frozen copy is `SOPIA/docs/tests/The_Test/DESIGN.md`**, at commit `18859e2` plus capture `001`. It is a tripwire and is not edited. **A divergence between it and this file is expected and is the mechanism working** — corrections go forward here, never backward there.

> **The header this replaced described an arrangement that had stopped existing.** It named `~/some-notes/day-one/` as the living copy — that directory is gone — and named the frozen copy by a path that has since moved into `The_Test/`. **Both copies carried it, so the file being actively edited was asserting that the real version was somewhere else.**
>
> **That is the stale reference worth catching**, and it is a different failure from the freeze commit, which was correct throughout. A pointer to a living copy is load-bearing in a way a frozen baseline is not: get it wrong and someone edits the file nobody reads.

> **The freeze commit and the repository's current commit are supposed to differ.** *Frozen as of `18859e2`, corrections go forward* means the frozen copy stays at `18859e2` while the repository advances. **A divergence is the mechanism working, not a stale reference**, and the tripwire is correctly baselined.
>
> This was changed and changed back on 2026-08-28. **Why that happened is process rather than design** — `BUILDING-OF-TRANSFAIRY-STEPS.md` lesson 18.

## `✓` — built and in force

**Owner's ask, 2026-09-01, and it closes a real defect in this document.** A settled ruling and an unimplemented one **look identical on the page**, so the open set cannot be reconstructed by reading — which is the gap the pre-compact state file existed to fill, and the reason it could not simply be folded away.

**`✓` on a heading or a ruling means the behaviour it describes is implemented and covered by a test.** Nothing else. It is not a quality mark, an approval, or a claim that the design is finished.

**Absence is only meaningful once the sweep is complete.** Until every behavioural ruling here carries the marker or deliberately lacks it, an unmarked ruling means *not yet marked* rather than *not built* — and a marker whose absence means two different things is worse than no marker. **The sweep is outstanding**, and this note stands until it is done.

**Why a marker and not a separate list.** A list is a second home, and a second home whose vocabulary drifts from the first is worse than either alone — demonstrated here, where the state file's shorthand labels sent five searches looking for content that was present under different words. **The marker keeps the status attached to the ruling it describes**, which is the same instinct as the notice that always appears and the explanation that lives in the tool.

**Status: the core is built and in-force, 2026-08-28.** `cai trans-fairy` implements init, split, build, install (create-only), graft, inject (loud/silent), the `--agent` path and the help family, on `src/cai/{transfairy,grammar}` with tests. Still open: inject's payload-source and role-labeling questions, and the items under *Open — needs a decision*.

Companion docs: `NAMING.md` (file naming rule), `BUILDING-OF-TRANSFAIRY-STEPS.md` (how the program came to be — process, not behavior).

---

## What it is

A toolset for moving a conversation out of one body and getting it to take in another: a claude.ai conversation export becomes a Claude Code session transcript that `claude -r` will resume.

## Input adapters — what the tool can do

The core flow is one path; an *input adapter* is a named front end that automates a particular way of reaching it. An adapter is convenience and documentation at once — it saves the steps, and it records which flow got a transcript to where it is. The steps are defined mainly for ease of use with an agent; a developer can reach the same flow by hand.

**The repo-sweep adapter.** Builds a priming transcript from a recorded directory sweep of a project, stamped with the commit it reflects, and injects it:

    repo sweep → build → install --inject → resume        (no export, so no split)

- **What it buys:** a resumed session that already holds an understanding of the work-dirs and the git state, without spending tokens re-reading them.
- **The commit is the delta stamp.** A later session compares it to `HEAD` and knows whether the understanding is still current — `AT: <repo>@<commit>`, the fact-plus-scope rule applied to a grafted context.
- **What it announces:** the `inject` regime — the marker lives in a meta record, so the operator and tooling know a repo-read was placed while the agent is not told and not biased by it. See *Graft and inject — two regimes* under *Injection and interweaving*.
- **Why it skips `split`:** there is no claude.ai export to extract from; the transcript is built from the filesystem. That makes the repo sweep a **second input source** alongside the export — open item 5's first concrete case.

---

## Invariants

1. **Retired for the create-only flow, 2026-08-28.** Backup-before-write guards a destructive write, and this tool performs none — install only ever creates a new session (invariants 3, 7, 8). Backup is instead a **beside-the-flow, developer's-call step**, prompted once at the crude stage (right before resume) — see *Backup and snapshot, beside the flow*. A mandatory in-flow backup returns only if overwrite is ever built. (Number kept as a stable identifier; invariant 2 still governs any copy the tool does make.)
2. Backups never use `cp -n`. It fails open — declines silently when the destination exists, returns success, and lets `&&` carry on into the destructive write.
3. Builds mint a new `sessionId`. An install **adds** a session; it structurally cannot replace a real one.
4. Ordering authority is the `parentUuid` chain — not timestamps, not filesystem metadata.
5. `cp -p` is the install verb. Source survives, mode lands 0600. *(→ `test_cli`: staged source survives.)*
6. `claude -r` is a human step. Never automated.

> **ORIGIN FOUND 2026-08-31, and it is the operator's preference rather than an argument.** Corrects the claim that stood here — *no reason is recorded anywhere in the corpus* — which was **wrong**. The origin is one clause in the build session, stated in passing while asking about something else: *"On the `claude -r`, I'll carry that step out."* The agent converted it to an invariant in the same exchange and it was never argued for again.
>
> **This moves the invariant from unknown-origin to known-origin-still-undefended**, which is a different and more useful state. There is a reason it exists — she said so — and there is still no argument for why it must hold, so it remains **the one invariant nobody can defend if it is proposed for relaxation**. See `BUILDING-OF-TRANSFAIRY-STEPS.md` lesson 19c.
>
> **The refusal to invent a reason was right, and the finding shows why.** A reconstructed rationale would have produced an argument; the truth was a preference, which no reconstruction would have reached.
>
> **A lead on origin, and deliberately not promoted to a reason.** Record 211 carries the earliest instance of the behaviour, predating the invariant: *"The resume itself is unverified. Testing it means actually running `claude -r`, which sends the whole thread to the API and appends to the transcript, and I wasn't going to burn that or mutate the file without you asking."*
>
> **Two epistemic classes are in that sentence and only one is worth anything.** *The agent declined to run it* is a checkable fact about an occasion. *Because it burns tokens and mutates the file* is the agent describing its own reasoning — the weak class, and the one that produced the emission errors corrected further down this file. **So this locates where the behaviour starts. It does not say why the rule exists**, and whether the invariant descends from that occasion or was always the operator's preference is hers to answer.
7. **The diamond record.** The first transcript into a project dir is never overridden. Structural, not a flag — no `--force`, no escape hatch, and the check lives at the write path, not in argument parsing, so no future flag can reach around it. Anyone who genuinely wants this forks the project and does it themselves; the repo is right there to copy from. That escape hatch stays *outside* the tool.

**Adopted:** rather than identifying and protecting the first transcript specifically, never overwrite **any** existing transcript — only create. The diamond record is protected as a special case of a rule with no exceptions, and the program never has to correctly identify which file is the diamond (mtime is fragile; a manifest can drift).

**Why a baseline is worth protecting, recovered 2026-08-28.** The rule above is stated structurally and its reason was never written down. **The diamond record is a control condition, not merely an original.** Trans-fairy exists to make *what this instance had in context* a controlled variable, and that only works if the starting context is still there to compare against. **Overwriting it does not lose data so much as destroy reproducibility** — the arm cannot be re-run, and no later measurement can be attributed.

The finding behind it: *two copies that share an ancestor agree whether or not either is right.* Agreement between derived transcripts is not evidence, so the preserved original is the only thing that can settle a disagreement. **Reason rather than fact — it is not artifact-checkable and stays record-sourced.**

8. **The protected surface is destination-based, not path-name-based.** Never overwrite a transcript living in a Claude projects dir (`~/.claude/projects/**`). Everything else is the tool's own workspace.

Stated as "anything under `tmp/` is exempt" the rule is spoofable — a `--stage-dir` pointed somewhere real, or a directory named `tmp` that is not ephemeral, defeats it. Destination-based has one predicate, evaluated at the write path, with nothing to fake.

The staging exemption is load-bearing, not a loophole: the staged transcript in `staged/` **must** be overwritable or rebuilds are impossible.

   **Reaffirmed 2026-08-31, after the overwrite case was reopened and closed by the owner.** A dialog offering backup-then-overwrite on an id collision was proposed and withdrawn: *they can fork, `rm`, `mv`, or whatever on their end.* **trans-fairy stays additive in its nature**, and destructive housekeeping belongs to a separate tool — the same argument that keeps `redact` separate, since a create-only contract and a destructive one cannot live in one tool without one becoming a flag on the other.

   **A reflow is an artificial fork, and it carries the lineage a real one does not.** `PROBES.md` P11: a native `--fork-session` records **no** reference to its parent, so a forked transcript cannot be traced from inside the file. A reflow therefore emits a `reflow-boundary` `isMeta` record naming `reflowedFrom` and `reflowedAt`, and appends a row to the mapping ledger in the projects directory. **Both are strictly more traceable than the client's own fork**, and the meta record does not reach the model (P1), so the lineage is honest to the operator without biasing the agent — the same split `inject` uses.

**The ledger is derived, not maintained — demoted 2026-08-31 once the transcripts carried their own lineage.** Owner's read: with `previousSessionId`, `originSessionId` and `includedSessionIds` on every transcript the tool produces, *the ledger boils down to a formality.* It was being appended to as each operation ran, which made it **a second home for a fact the transcripts already hold** — the drift this repository exists to avoid, sitting inside the tool that keeps finding it elsewhere.

**So it is rebuilt rather than kept up to date.** `cai trans-fairy state --ledger` discards `cai-transfairy-mapping.md`, reads the lineage back out of the transcripts in the directory, and writes what they say — `session | kind | previous | origin | included`. **That is what makes *not authoritative* true rather than merely claimed**, and it cannot go stale, because nothing maintains it.

**What it still cannot promise.** A transcript absent from the table may still be derived from something: a native `--fork-session` leaves no lineage at all (P11), and a `true-silent` cut records none by design. The ledger reports what the transcripts say, and silence in a transcript is not evidence of independence.

**The collision is real, though, and now has an additive answer.** Pool determinism means a rebuild from the same pool yields the same `sessionId`, so `build → install → rebuild → install` collides, and the file already there is the developer's own earlier install. `install` refuses, says exactly that, and offers **`--reflow`**: mint a fresh id, rewrite the staged transcript, and install it as a **new** transcript beside the existing one. Nothing is overwritten and nothing is removed.

*Consequence:* `previous-agent/` is a reference exemplar — the schema model the builder was written against — not scratch. It belongs in `bak/` or a `ref/` dir, not under `tmp/`. The root `previous-agent/` stays authoritative either way.
9. UUIDs come from `uuidgen`, never from model reasoning. **Measured, not reasoned.** A model asked for a uuid produces something uuid-shaped by reasoning about the shape, which is not the same act as drawing from a random source and carries none of its guarantees.

> **The reason existed and was attached to a different rule.** `BUILDING-OF-TRANSFAIRY-STEPS.md` 15c states it about datetime stamps and names uuids only by analogy — *same rule as UUIDs from `uuidgen`: measured, not reasoned.* **Findable only by someone who already knew it was there.** See lesson 19.

10. **A backup is any operation that makes a copy, never one that consumes the original.** `mv` is not a backup verb. Operator definition — this is why the staged transcript survives its own install, and why live transcripts are copied out rather than relocated.

---

## Test coverage — the drift guard

**Every invariant and load-bearing behaviour below is pinned by a test, so a change that breaks it fails `tests/` rather than drifting away from this prose.** Where a row names a test, that test is the authority for the behaviour; this document keeps the *why*. A behavioural claim here with no test is a claim on the honour system — add the test rather than trusting the sentence.

| invariant / behaviour | test |
| --- | --- |
| 2 — never `cp -n` (fails open) | `test_cli` · no `cp -n` in the package |
| 3 — build mints a new sessionId; install adds, never replaces | `test_transfairy` · same pool → same SID; `test_cli` · invariant 8 |
| 4 — ordering authority is the `parentUuid` chain | `test_transfairy` · parent chain unbroken; graft re-parent |
| 5 — `cp -p` install verb, source survives | `test_cli` · install copies, staged source survives |
| 6 — `claude -r` never automated | `test_cli` · install returns the command, does not run it |
| 7 / 8 — never overwrite a transcript in a projects dir | `test_cli` · second install refuses (exit 1) |
| 9 — uuids from a random source, never model reasoning | `test_transfairy` · pool determinism; `pool.py` uses `uuid4` |
| 10 — a backup copies, never consumes | `test_cli` · staged survives; `test_transfairy` · `state --split` leaves the project dir intact |
| the six build checks (incl. key-set conformance) | `test_transfairy` · all six pass; catches the empty-skip adjacency |
| pool determinism (mint / read / append, never shuffle) | `test_transfairy` · append-when-long/short, SID byte-stable |
| help family (man-help, mahd, help+non-help hard drop) | `test_cli` · exit codes |
| flag resolution (env → default) | `test_cli` · `TRANSFAIRY_CWD` |
| `--agent` boundary (exit 0 + next command; failure non-zero) | `test_cli` · agent boundary + failure |
| graft (new sid, base unmodified, seam, brackets) | `test_transfairy` · graft |
| inject markers; silent is never installed in a projects dir | `test_transfairy` · loud/silent markers; `test_cli` · silent not installed |

**Invariant 1 has no row: it was retired (create-only flow), so there is no behaviour to guard.** Non-TTY-never-prompts is not yet pinned — the tool defaults `--target-cwd` to `$PWD` so it rarely reaches a prompt, but the failure-with-a-hint path is untested and is the one honour-system row left.

## Tree

Three locations with three different contracts. **Nothing is ever written to the project directory.**

```
<tempdir>/trans-fairy-<job>/       churn, disposable, auto-created
├── download/                      source export
├── previous-agent/                optional reference copy
└── staged/                        built transcript before install

<datadir>/trans-fairy/             durable, never auto-cleaned
├── bak/                           backups
└── uuid-pool.txt                  primary key: SID = pool[0]

<target-cwd>                       NAMED ONLY, never written to
```

`<tempdir>` is the platform temp directory — `TMPDIR`/`TEMP`/`TMP` as the OS reports it, `/tmp` on a default Linux box. Override with `--work-root`.

`<datadir>` is `$XDG_DATA_HOME/cai` or `~/.local/share/cai`, and `%LOCALAPPDATA%\cai` on Windows. Override with `--data-root`.

**The pool cannot live in temp.** It behaves like a primary key, and a cleaned temp directory means the next run mints fresh ids — which is the stray-second-transcript failure this document names under *Pool determinism*. Backups cannot live there for the same reason: a directory whose contract is "disposable" is not a place for the copy that survives.

## Backup and snapshot, beside the flow

**Popped out of the numbered flow, 2026-08-28.** The tool is create-only from the ground up — new session, graft/inject, install beside the existing transcripts — so **there is no destructive write for an in-flow backup to guard.** Backup and snapshot are therefore beside-the-flow capabilities, invoked by the developer, not stages the pipeline walks.

**Backup is prompted once, at the crude stage — right before resume.** `install` copies with `cp -p`, so the staged transcript survives as a crude backup already; but it lives in disposable temp. The one moment a durable copy is worth taking is right before `claude -r`, which appends in place. So the tool prompts once, there, and the developer decides.

**Its default purpose is to save transcript split/modify work**, not to guard an overwrite. The developer may point it at anything else they want preserved.

**Durable as-built preservation is the opt-in case, and the cost of skipping it is stated at the point of use.** A snapshot to `bak/` before resume is the only way to keep a pristine as-built to diff a live transcript against months later. Skip it and, once resume appends and temp is swept, the as-built is unrecoverable. The tool says so; it does not take the snapshot for you.

**`claude -r --fork-session` largely obviates even this — the clean solution to backup bloat (operator, 2026-08-28).** Resuming with `--fork-session` mints a new session id and writes a new transcript, leaving the as-built original untouched, so the pristine build survives resume with no snapshot at all. The pre-resume snapshot drops from *recommended* to *belt-and-braces*: prefer forking on resume, and snapshot only if you also want the as-built in a durable `bak/` location independent of the projects dir. **This is also how the probe transcripts for items 11/12 were run without polluting anything — forked resumes never touched the crafted originals.**

**In `--agent` mode the prompt is a stop-and-print, not a `y/N`.** A developer tooling themselves can reason about whether to back up; the tool surfaces the decision and its consequence and lets them re-invoke.

**Cleanup follows the same principle: by developer request, or automatically only after the tool has prompted them to back up somewhere safe.** The backup prompt gates the auto-clean — nothing disposable is swept until the developer has had the chance to preserve it.

**A mandatory, in-flow backup returns only if overwrite is ever built** — that is the one flow where a backup guards a destructive write. Until then, invariant 1 stays retired.

## Backup retention

**Over-commit, then hand-pick.** Backups are never pruned automatically and never overwritten. A process that is going wrong is exactly when a backup is wanted, and that is not the moment to be deciding which copy mattered. Twenty captures and one human choice beats one capture and a guess.

Each capture is a numbered directory under `<datadir>/trans-fairy/bak/`, prefixed `NNN-`, with a `MANIFEST.md` recording what was copied, **the sha256 of the original and the sha256 of the copy as two separate values**, the original's mtime, and the time of capture. The filesystem is the counter: the next capture reads the highest existing prefix and increments, so an overwrite cannot happen even if the operator or agent misremembers.

Selection is a human act. Nothing in the toolset deletes a capture.

**Two hashes, not one, and the reason is a defect found 2026-08-28.** A manifest over an earlier capture recorded a single sha per matched pair -- the value the two shared at the moment of copying -- and reported *25 of 25 MATCH*. One row is now false: a later pass appended to the **original** after the copy was taken, so the copy is 26,495 bytes against the source's 34,634. **The copy did not drift; the source did**, which is the direction a capture is never guarded against.

**The stored form could not express the failure.** The recorded hash still matches the copy and always will, so **re-running the check against the manifest passes.** Only re-hashing both sides and comparing them to each other catches it.

**Sealing the source at capture is the stronger fix and is not always available** -- a live working file cannot be frozen because something copied it. Recording both hashes is what makes the check able to fail, and the mtime is what tells a reader which side moved. See `SOPIA/docs/guides/designing-a-test-with-an-agent.md` for why this is the inverse of the unwritten-verification rule.

*suggestion:* make `bak/` a git repository and let each capture be a commit. Content-addressed storage deduplicates near-identical transcripts, and `git log` is a better hand-pick interface than a directory listing. Rejected as the default rather than as an idea: transcripts here carry material that may later need to be removed, and content in git history cannot be deleted without a history rewrite, while a plain numbered copy can be removed with `rm`. Deletability is load-bearing for this material; storage is not.

**A second argument against it, recovered 2026-08-28: a git-backed `bak/` cannot carry a seal.** Captures are held read-only, and **git records only the executable bit** — so a clone comes back writable and the seal is gone. `cp -rp` carries the modes across, which is why the seal survives an ordinary move and would not survive this suggestion.

> **Tested 2026-08-28, and it holds.** A file at `444` beside one at `755`, committed and cloned: git recorded `100644` for the sealed file and `100755` for the executable, and the clone came back at `664` and `775` under a `002` umask. **The read-only bit was not preserved; the executable bit was**, which is the positive control that makes the zero meaningful. **The seal does not merely fail to transfer — the clone is group-writable**, which is weaker than the `644` git actually recorded.

**Worth knowing about the seal generally:** it is a speed bump rather than security, since anything running as the owner can `chmod` it back. What it buys is that an accidental write fails and that the intent is legible on the file itself rather than in a document somebody has to have read.

**Snapshot directories also carry a `LABEL.txt`** reading *frozen point-in-time copy, not a live mirror / copied, never moved* — a second labelling artifact beside `MANIFEST.md`, and the one that states the contract rather than the contents.

> **A hash manifest for the frozen tree was proposed and superseded** — `git init` already provides content-addressed change detection, which makes a separate manifest redundant. **No manifest is wanted here.**
>
> It was briefly written into this document on 2026-08-28. **How that happened is process rather than design** — `BUILDING-OF-TRANSFAIRY-STEPS.md` lesson 18.

Every sub-command creates the directories it needs and prints where they are on `--verbose`. `--where` prints all three and exits.

## Working area vs injection target

Two independent things, conflated at everyone's peril:

| | what it is | flag |
|---|---|---|
| **working area** | where the toolset's dirs live | `--work-root` |
| **injection target** | cwd stamped into every transcript record; decides which project dir the session lands in | `--target-cwd` |

They diverged in the very first job (`work-root=trans-fairy/`, `target-cwd=<project-root>`) because the operator worked in a sub-dir to avoid polluting the target. `init` defaults the working area to the platform temp directory and the injection target to `$PWD`; the flags separate them further when needed. **The injection target is a value, not a destination** — it is stamped into records and decides which project directory the session lands in. The toolset never writes there.

The injection target **cannot be changed after the session exists.** Both help levels must say so.

---

## CLI

Single entry point, sub-commands.

```
trans-fairy init      --target-cwd PATH [--with-previous-agent PATH]
trans-fairy split     --export PATH [--target-uuid UUID]
trans-fairy build     --target-cwd PATH [--conversation PATH]
trans-fairy install   [--rehome-to PATH] [--graft-onto SID] [--inject SRC] [--dry-run]
trans-fairy state     [--split VAL …] [--to DIR] [--include …] [--json] [--dry-run]
```

### Flag resolution

`flag → env → derived default → prompt`. Non-TTY never prompts; it fails with the hint instead, so automation cannot hang.

### Global flags

```
--target-cwd PATH     env: TRANSFAIRY_CWD    (prompts if unset and TTY)
--work-root PATH      default: <tempdir> (platform temp: TMPDIR/TEMP/TMP, /tmp on Linux)
--data-root PATH      default: <datadir> ($XDG_DATA_HOME/cai or ~/.local/share/cai;
                      %LOCALAPPDATA%\cai on Windows). Holds bak/ and the pool.
--stage-dir PATH      default: <tempdir>/trans-fairy-<job>/staged
--claude-projects P   default: derived from --target-cwd by the '/' -> '-' rule
--pool PATH           default: <data-root>/trans-fairy/uuid-pool.txt
                      Never under <work-root>: a cleaned temp dir mints fresh ids,
                      which is the stray-second-transcript failure. See Tree.
--pool-size N         default: computed from message count
--c-version STR       Claude Code version stamped into records.
                      Derived from `claude --version`; on failure pins 2.1.231
                      and warns that claude was not found and this tool may not
                      be reliably usable without it.
--version             trans-fairy's own version
--model / --effort    model and reasoning-effort overrides passed through to a
                      spawned agent stage; unset inherits the session's.
--agent               one stage per invocation; print stage text, what was
                      written, what to verify, and the exact next command
--stage NAME / --stop-after NAME
-y, --yes             never prompt; fail instead   (mutually exclusive w/ --agent)
-n, --dry-run
```

### No multi-lettered short forms. Project-wide.

Single-dash multi-char flags collide with abbreviation handling and get parsed as grouped shorts by getopt-style parsers. They also make every future flag addition a compatibility question.

### Help family

```
-h, --help        usage, flags one line each, three common invocations
    --man-help    every flag + env var + derivation, precedence, bootstrap
                  section, naming scheme, pool semantics, exit codes
    --mahd        runs --help then --man-help, duplicating the usage block.
                  Help text: "Why would you do this?"
```

- Any help flag combined with a non-help flag: **hard drop.** Print help, then a blurb saying that if the help flag was unintended, drop it and re-run. Non-zero exit so scripts notice.
- Within the family the most verbose wins, silently. `--mahd` > `--man-help` > `--help`.

### Empty-value flag rule

A required-value flag given no value prints **that sub-command's help as its error**. Same text serves a human and an agent. Governs `--split`, `--rehome-to`, `--to`, and anything added later.

---

## Stages

| | stage | status |
|---|---|---|
| 00 | `init` — scaffold tree, optional `--with-previous-agent` | designed |
| 01 | `split` — export → extracted conversation | **exists only as an unsaved heredoc** |
| 02 | `build` — extracted → staged transcript | exists; needs the flag rework |
| 03 | `install` — staged → projects dir | designed |
| 04 | `claude -r` | human only, never automated |

**`snapshot`/`bak` is no longer a stage.** It was popped out of the flow 2026-08-28 — a create-only tool has no destructive write to back up. It is a beside-the-flow, developer's-call capability; see *Backup and snapshot, beside the flow* below.

### Empty dirs

Every naturally-empty directory gets a `README.md` so git keeps it. One informative line naming what belongs there — more info is better, especially when it is cheap.

`git init` happens when the toolset is re-homed, not before.

---

## Build behavior (settled)

- **thinking blocks are dropped.** In a claude.ai export the `thinking` text is empty (`thinking_hidden: true`); only summaries and a claude.ai-issued `signature` survive, and a signature minted elsewhere will not validate.
- **tool_use / tool_result are folded into assistant text** as fenced blocks. Exported tool names (`bash_tool`, `web_fetch`, `web_search`, `memory_list`) do not exist in Claude Code; emitting real tool_use blocks would name undefined tools to the API. Large results cap at 6 KB.
- **pasted attachments are inlined in full** — a message can be empty text with the whole payload in `extracted_content`.
- **branch resolution**: walk the `parent_message_uuid` chain from the root; at a branch take the longest subtree. Array order is not the thread order.
- **timestamps** carried from the source, truncated microseconds to milliseconds.

### Emission details — recovered 2026-08-28, corrected against the code the same day

**Checked against `reference/transfairy/build-transcript.py`, read in full, rather than against the record that reported it.** These claims were sourced from a report the authoring agent wrote about its own code. **Two of them were wrong**, and the corrections are marked below.

**The authoritative per-record field sets are `src/cai/grammar/records.py`** — `USER_FIELDS`, `ASSISTANT_FIELDS`, `USAGE_FIELDS`. This section explains emission behaviour; the module is the machine-checkable owner, and a real `build` reads its field sets from there rather than from this prose.

**The line grammar, in emitted order:** `mode` -> `permission-mode` -> `file-history-snapshot` -> records -> `ai-title` -> `last-prompt` -> closing snapshot. **Confirmed.**

**But the records are not guaranteed to alternate.** An assistant message whose text folds to empty is skipped, so two consecutive `user` records are reachable. `PIPELINE.md`'s build verification requires that no two consecutive turns carry the same role, which means **the builder can emit a transcript that fails its own stated check.**

**The chain check cannot catch it.** The skip fires before the parent pointer advances, so the chain stays unbroken and the next user simply parents onto the previous user. **Only the alternation check can see this**, and it is the check most likely to be read as a formality.

**And the skip discloses nothing.** The counter reports dropped thinking blocks, folded tool blocks, inlined attachments and off-branch messages. **A skipped message is counted in none of them** and vanishes between source and output -- the exact failure `PIPELINE.md` says this stage exists to prevent.

**It appears to be latent rather than manifest.** *Record-sourced and unverified here:* record 211's post-build verification is reported as stating that the alternation check passed, so no assistant message folded to empty on that export. **The defect waits on a turn that is entirely thinking blocks, or entirely tool blocks that fold to nothing.**

**`usage` stubs are zeroed deliberately. Confirmed** -- all four token counts are literal `0`. The reasoning was recorded at the time: *"I wrote zeros -- fabricating token counts seemed worse than leaving them empty."* **Without this note the zeros are indistinguishable from an oversight**, which is the only reason it is worth writing down.

**`msg_` and `req_` ids are dashless uuids drawn from the same pool as everything else. Confirmed.** That is why pool sizing is a function of message count rather than of line count, and why an under-sized pool fails partway through a build rather than at the start.

**Corrected: `permissionMode`, `effort`, `cwd`, `version` and `gitBranch: HEAD` are hardcoded.** This document previously said `permissionMode: auto` and `effort: high` were *copied from the model file*, and that `cwd`, `version` and `gitBranch` were *mirrored* from it. **Neither describes the program.** The builder calls `open()` three times -- the pool, the export, and its own output -- and **reads no model transcript at any point.** The phrase described where the developer took the value from while writing the line, and it was recorded here as runtime behaviour.

**That leaves a gap rather than an error, and the gap is already answered elsewhere in this document.** `--target-cwd` and `--c-version` exist as flags precisely to supply what the reference builder has nailed down.

### Pool determinism

- absent → mint `uuidgen -r` x N
- present and long enough → read, never touch
- present but short → **append**, never regenerate. Safe because uuids are consumed strictly in order: appending cannot disturb a consumed position, so `SID` and every prior id stay identical.
- never rewrite or shuffle existing entries — that is what mints a stray second transcript instead of correcting the first.

**Rebuild determinism assumes a frozen folding implementation, and this section does not say so.** The uuid is drawn before the sender branch, so a message that is later skipped has already consumed one. **Pool consumption is therefore coupled to the folding rules** -- changing the 6 KB result cap, the tool-block handling, or anything else deciding whether a message survives shifts every identifier after the first change.

**The guarantee that actually holds is narrower than the four rules above imply:** *same pool, same export, **same builder**, same output.* Appending to the pool cannot disturb a consumed position, which is true and is the wrong hazard. **The tool is unbuilt and the folding rules are the most likely thing to be revised first.**

---

## state

Covers everything in the project dir that is not code: memory files, transcripts, `CLAUDE.md`.

- base invocation reports what is there
- **memory triage, designed and not implemented.** The verb exists because of this: prompt the agent to assess each memory entry and classify it **project-durable** — extracted and merged into the in-repo document — or **session-local** — transferred whole to a temporary memory file that moves with the session. **The judgment is prompted, never made silently.** Operator's design, recovered 2026-08-31; the rename `memory` → `state` is recorded in the nomenclature table and this, the function behind it, was not. `state` today reports and `--split`s only.
- `--split` extracts into `previous-agent/` or `working-agent/`
- transcripts are **excluded by default**; `--include transcripts` is the deliberate act
- `CLAUDE.md` may be updated, less stringently than transcripts
- **`--json` is for the agent path** — a machine-readable report, so an agent consuming `state` parses a structure rather than scraping prose. Recovered 2026-08-31; the flag was in the signature and the reason was not. **Not implemented.**

**Merging a transcript is confirmed, and the confirmation is a reasoned one rather than a `y/N`.** Operator's design, recovered from the build transcript (record 373, 2026-08-24): transcripts are merged **only by manual command, or from a confirmation ask in which the agent says this should be considered, gives its reasoning, and offers to discuss it or merge anyway** — gated on whether the material is topical enough to belong together. **This is deliberately the opposite shape from the graft landing prompt**, which is a bare `y/N` because there identity is the only open question. Here the question is judgement, so the tool must push back rather than merely confirm. **Not implemented; `state` reports and extracts but does not merge.** Record-sourced.

**Merge is a rewrite, not an append.** Two transcripts each have their own `parentUuid` chain and `sessionId`. Merging means re-parenting chain B's root onto chain A's leaf and rewriting `sessionId` throughout. If B predates A, timestamps go non-monotonic — which renders fine, because ordering comes from the chain, and shows only an odd gap.

`sessionId` is not one field. It appears as `sessionId` and `session_id` on message lines, and standalone on `mode`, `permission-mode`, `ai-title`, and `last-prompt` records, and it must match the filename. Six places. A partial rewrite yields a transcript that half-loads.

---

## ✓ Lineage — one marker, always the last message (see IMPROVEMENTS: it strands on a live session)

**Owner's design, 2026-08-31.** Every transcript the tool produces records where it came from, in two fields on the **final message record**:

| field | answers | behaviour |
| --- | --- | --- |
| `previousSessionId` | what this was immediately before | changes each operation; walk it to step back one |
| `originSessionId` | where it ultimately came from | **carried forward unchanged** through every later operation |
| `includedSessionIds` | every session whose material is present | a sorted set, accumulated across operations; written whenever there is more than one contributor — **an ancestor or a sibling** |

**It is written by `rewrite_session`, because that is the one place a `sessionId` changes.** Four operations call it, and each previously carried its own differently-named pointer — `baseSessionId`, `reflowedFrom`, `truncatedFrom` — so there was no uniform way to ask the question. Hooking lineage to the rewrite itself means **no operation, including one added later, can change an id without leaving the answer.** The unsafe thing is unreachable rather than forbidden.

**Exactly one marker, and it is always last.** An operation that appends would otherwise strand the previous marker mid-transcript, leaving two answers to one question. So a rewrite **clears any existing marker before writing the current one at the end**. This is also why every operation *reads* these fields before writing: the origin it finds is carried forward rather than restarted, which is what makes the field survive a chain rather than only the last hop.

**A linear chain has one ancestor; a merge has an ancestor and siblings.** The distinction matters for reading the field: `graft` produces one **ancestor** — the base, which this transcript descends from — and one **sibling**, the graft merged alongside it. Neither is the other's parent. `previousSessionId` names the ancestor; the set names everyone. `previousSessionId` is a single pointer and is enough for `truncate` or `reflow`, but `graft` and `compose` take **two inputs**, and a future operation over many files would take more. Recording only the pointer silently dropped the second contributor — a graft's other ancestor appeared nowhere in the uniform lineage, only under a graft-specific name in the seam record. **`includedSessionIds` is the set, collected before the rewrite while each record still carries its own id**, so no contributor is lost however many inputs an operation grows to take.

**It accumulates, and it includes intermediates.** Grafting C onto an already-grafted A+B yields four ids, not three: A, B, C, and the id the A+B result carried. That is the honest answer to *which sessions has this material belonged to* — an intermediate was a real transcript on disk, and distinguishing "source" from "intermediate" after the fact is not knowable. Sorted, so the same inputs always produce the same record, and the transcript's own id is excluded because it names this file rather than a source.

**`true-silent` writes none, and that breaks the contract the rest of this tool keeps.** It is a deliberate escape hatch rather than an accident, and the operator may want it — but a transcript cut that way is **indistinguishable from an original**: no boundary record, no `previousSessionId`, no `originSessionId`, no ledger row, and an audit will report it as unaccounted forever. **Provenance that was never written cannot be recovered by inspection.** The tool therefore warns at the point of use rather than in a document somebody has to have read, and the ledger and the audit both state the limit rather than implying completeness.

**`--loud-lineage` announces it in context, so the agent can know its own lineage.** The fields above are *metadata*: they sit on the record, not in message content, so the model does not read them — the same reason a meta record cannot carry an announcement (P1). With `--loud-lineage` a visible `⟦ LINEAGE ⟧` notice is appended naming the previous id, the origin, and any other contributing sessions, and telling the agent to treat claims about its own identity or history as belonging to those sessions rather than to this one. **That last clause is the point**: a resumed agent that inherited a transcript will otherwise speak as though its predecessor's history were its own.

**Same split as everywhere else in this tool.** Metadata by default, because a blind agent behaves normally; visible on request, for when it needs to know. Never available under `true-silent`, which by definition records nothing.

**They are marker fields, not client grammar.** `build.verify` names them alongside `origin` in one place so they are never reported as drift against the record grammar.

## ✓ `trans-fairy-write` — the sibling that is allowed to write

**Named by the owner 2026-09-01.** `trans-fairy` creates. It mints a new `sessionId`, writes a new transcript, and never overwrites or removes — that is the contract, and every protection in this document rests on it.

**Some operations genuinely need a write**, and the retrofit above is the first: it modifies a real session in place. Putting that behind a flag on `trans-fairy` would make the create-only contract conditional, and a conditional invariant is not one — this is the argument that keeps `redact` separate, and it applies with more force here because the write lands in a projects directory.

**So the destructive half is a sibling tool.** `trans-fairy` keeps a create-only contract; `trans-fairy-write` admits to being the thing that writes.

**The handoff is a printed command, not a flag.** `trans-fairy` accepts the flags that describe the transcript it would generate, produces it, and — where the operator wants an overwrite rather than a new file — **prints the exact `trans-fairy-write` invocation instead of performing it.** The same shape as `--agent`: the tool stops at the boundary and hands over the next command, so the destructive step is a separate, deliberate act by a separate tool with the operator's hand on it.

**Built 2026-09-01**, `src/cai/transfairywrite/`, 25 checks. The three questions it was recorded as needing to answer, answered:

**Does it back up before writing? Yes, and the backup is mandatory.** `--backup` is required, refuses a destination that already exists unless forced, and is a copy rather than a move so the original survives. **This is where the retired invariant 1 genuinely returns**, since it is the first tool here performing destructive writes — and the contract is `redact`'s, already proven in the sibling rather than invented for this one.

**Will it touch a live session? No, and this is the central refusal.** It reads liveness from two signals — **the target's `sessionId` appearing in a live session file, and an mtime inside five minutes** — and either is enough to refuse. **A rewrite of a live transcript destroys whatever the client wrote between the read and the write, and the backup cannot recover it**, because a backup holds the pre-write state rather than the write it raced. `--ignore-live` exists for a stale registry and warns loudly.

**A consequence worth knowing: a successful write leaves its own target looking fresh.** The tool cannot distinguish *modified because I wrote it* from *modified because a client did*, so a second write inside the window needs `--ignore-live` or a wait. **Refusing is the safe direction and it is left refusing.**

**What it declines outright.** A target that does not exist — it overwrites, it never creates. A replacement that fails verification: malformed JSON named by line, assistant `content` that is not a list of blocks, a broken parent chain, an orphaned `tool_result`, or more than one `sessionId`. A replacement whose `sessionId` disagrees with the target's, which would leave a file whose name and contents disagree — the defect `install` was corrected for. **And `--agent` outright**: the tool has no agent path and will not grow one.

**The checks run before the write, not after.** A malformed transcript that reaches a projects directory crashes the client on load, and at that point a backup is a recovery rather than a prevention.


## ✓ Auditing the projects directory — and the declared-original split, built 2026-09-02

**`cai trans-fairy state --audit` reports what the ledger cannot account for.** It reads every transcript in the projects directory and sorts them: **traced** (carries lineage fields or a boundary record), **unaccounted** (carries neither), **unreadable**.

**A built transcript records where the chain begins.** Lineage records session → session, but a `build` has no session predecessor — its source is a conversation inside an export. That was going unrecorded, which made a freshly built transcript indistinguishable from one with no provenance at all. `build` now appends a `build-boundary` meta record naming the source conversation's uuid, its name and its message count. **The chain therefore terminates in a fact rather than in silence.**

**An unaccounted transcript is one of three things and the file cannot say which:** an original that was never derived, a native `--fork-session` (P11: the client records nothing), or a `true-silent` cut. **The audit reports rather than repairs**, because provenance that was never written cannot be recovered by inspection — there is nothing to fix automatically, only something to tell the operator.

### UNBUILT — Retrofitting a transcript cut before lineage existed (a procedure, not an operation)

**Transcripts produced before 2026-08-31 carry no lineage and will audit as unaccounted.** Where the provenance is *known from outside the file*, it can be written in by hand. The procedure, and the order matters:

1. **Confirm the session is not live.** A running session appends to the file; editing underneath it risks a lost write. Check the mtime against the current time, and do not do this while the session is open.
2. **Back up the transcript first**, outside the projects directory. This is a write to a real session, which is the one thing the tool otherwise never does.
3. **Add the two fields to the final message record** — `previousSessionId` and `originSessionId` — per the one-marker rule. Not to a boundary record: a boundary record carries a timestamp, and a retrofit would be claiming a moment that did not happen.
4. **Mark it as retrofitted, at every site the operation touched.** The fields say where it came from; they must not imply the tool wrote them at cut time. `lineageRetrofitted: true` goes on the final message record — **and, where the transcript carries an injection placed at depth, on the injected record too.** An injection at depth is a second site with its own provenance, and marking only the tail would leave the middle of the transcript silently unexplained.

   **The tail record points at the injection site.** It carries where the injected material sits and when it was injected, so a reader who starts at the end — which is where lineage lives — can reach the other site without scanning. Two marks, one of which is an index into the other.

   **This has loud and silent forms, defaulting to silent.** Silent writes the marks as metadata only; loud also announces them in context. Same rule as everywhere else in this tool: metadata by default because a blind agent behaves normally, visible on request.
5. **Verify it still loads** — resume it headless with `--fork-session`, which leaves the file untouched, and confirm it answers from its own content.
6. **Then re-run the audit.** It should move from unaccounted to traced.

**The quote-recognition roots are deliberately not retrofitted.** Owner's ruling, 2026-09-01. They served their purpose, the runs were carried out exactly as they stand, and **a retrofit would alter the artifact that produced six rounds of results.** The retrofit is a variable rather than a repair — if a consistent context is needed again, it gets built under the current contract rather than back-fitted onto a finished experiment.

**Do not retrofit a transcript whose provenance is inferred rather than known.** A guess written into the lineage field is indistinguishable from a fact, which is worse than the gap it fills.

## Graft — write semantics

- **New `sessionId` always.**
- **BASE is never modified.** Graft reads BASE and emits a new file.
- **The seam record carries lineage** (adopted):

      { "type": "system", "subtype": "graft-boundary", "isMeta": true,
        "position": "close",
        "graftedAt": "2026-08-24T20:14Z",
        "baseSessionId": "8560773c-…",
        "graftSessionId": "592b7383-…" }

A new `sessionId` otherwise erases the trail. This is the same provenance problem `-NNN` solves on the filesystem side, solved inside the record.

Consequence: grafting onto the diamond record is safe **by construction, not by rule**. There is no code path in which BASE could be written, so the protection does not depend on a check being present and correct — it depends on the operation having no write path to BASE at all.

**Why `graft` is a flag on `install` rather than its own sub-command — the operator's reasoning, recovered 2026-08-31.** *"Every path that writes a transcript into a projects dir should funnel through one write path. That's where the never-overwrite check and the diamond protection live. A `graft` sub-command that also writes gives you two write paths, which means two places the protection has to be correct, which means one day it isn't."* **This is lesson 12 applied to the CLI surface**: the outcome was recorded and this argument was not, which is exactly the strip lesson 19 describes. It is a stronger reason than the one previously written down, and it governs any future verb that would write into a projects directory.

This is the preferred shape for every protection in this tool: make the unsafe thing unreachable rather than forbidden.

## UNBUILT — Graft — double-graft detection

**Operator's ruling, recovered 2026-08-31:** *"A file should not be grafted twice, and that should be a warning mode… If people want to accumulate seams anyway, they can. We can't design around a growing set of edge cases."* — the scope boundary already stated elsewhere, applied here.

**Detection** is a scan of BASE for an existing `subtype: "graft-boundary"` record. **Three behaviours, one per mode:**

| mode | behaviour |
| --- | --- |
| human TTY | warn, then `y/N` defaulting to **no** |
| `--agent` | **hard stop**; re-invoke with an explicit acknowledgment flag (an agent never answers a prompt) |
| `--yes` | warn and proceed |

**Not implemented.** `graft()` does not scan for existing seams and will graft a grafted file without comment.

## Graft — landing approval

Before anything is written, print the **change announcement that would be inserted**, timestamped, and ask for approval.

```
⟦ GRAFT-A — would be inserted 2026-08-24T20:14Z ⟧
  base    8560773c-…   "Ping pong exchange"   214 msgs   last 17:56:59Z
  graft   592b7383-…   "<ai-title>"            18 msgs   last 20:11:04Z
  direction: after   (auto-detected; no date rewriting required)

Append the most recent transcript? [y/N]
```

Declining prints a blurb: choose explicitly with `--graft-onto`, or hand it to an agent.

**Direction is derived with the smallest possible amount of date rewriting — operator's ruling, recovered 2026-08-31. DESIGNED, NOT IMPLEMENTED.**

| graft's timestamps vs base | direction | date rewriting | spans |
| --- | --- | --- | --- |
| earlier | `before` | none | exact |
| later | `after` | none | exact |
| synthetic, now | `after` | none | exact |
| synthetic, forced to the front | `before` | backdate the graft block only | preserved |
| overlapping | refuse | — | — |

**Backdating drops from a default to a fallback** that only fires when someone explicitly forces synthetic content to the front. **The guarantee is that ordinary grafts rewrite zero dates and preserve every span.** A general `--direction before|after` override on `install` belongs with it. **None of this is built**: `graft()` takes an explicit direction, defaults to appending, and does no timestamp inference and no overlap refusal.

**"Last modified" is mtime, deliberately, and the label is literal.**

mtime is not a noisy proxy for conversational recency — it is a direct measurement of a different signal: *the file the developer was just handling.* If someone moved a transcript, or asked an agent to move it, that file is top of mind. They know why it is on top, because they put it there.

This is why a bare `y/N` suffices and the flow offers no append/prepend/cancel menu. Direction is auto-detected; identity is the only open question, and the developer already knows the answer.

**The label must match the mechanism.** Rank by in-file `timestamp` while saying "last modified" and the verbiage is a lie. Rank by mtime and say "last modified" and it is literally true. The honesty of the prompt is what lets the interaction stay this small.

*(Superseded recommendation, kept for the record: ranking by the last in-file timestamp instead. That measures conversational recency, which is not the signal this prompt is asking about, and would make the label untrue.)*

**The preview confirms a familiar file; it does not identify an unknown one.** Show `sessionId`, last in-file timestamp, message count, and `ai-title` / `last-prompt` — enough for the developer to recognize what they already expect, so a mistaken pick is caught before the write.

**`--yes` does not answer this prompt.** `--yes` means "do not ask me to confirm." Choosing *which file* to graft is a **selection**, not a confirmation, and auto-selecting by recency is where a wrong default does real damage. Under `--yes`, `--graft-onto` is required and the run fails without it. Unsafe thing unreachable, not merely discouraged.

**Under `--agent`:** the prompt becomes a hard stop, identical to the flow mechanism. Print the candidate and the **exact** re-invoke command with `--graft-onto` filled in. An agent never answers a `y/N`.

## Rejected alternatives

Kept so they are not re-proposed without the reasoning that killed them.

**Directory names encoding graft direction** — e.g. `previous-agent/`, `next-agent/`, `base-for-direction-agent/` in the working dir, so placement declares direction. *Proposed and withdrawn by the operator mid-turn.*

Rejected because it creates **two sources of truth for one fact**. Direction is already derived from timestamps, authoritatively. A directory that also declares direction can disagree with them — and then a file in `next-agent/` whose timestamps say *before* needs conflict-resolution logic. Complexity added, not removed. Structure-as-documentation is free only when it documents something not otherwise computed.

Secondary: `previous-agent/` already means the prior session's artifacts kept as contingency. Overloading it with a direction sense collides with an established meaning in this same tree.

**The pattern itself is sound and already in use.** `previous-agent/` vs `working-agent/` encodes *role* — nothing computes it, nothing contradicts it. Direction is simply the wrong fact to hang on directory structure.

## Graft — design intent

**A graft must be obvious to both machines and humans.**

The goal is *not* to make the resumed agent believe the grafted material was its chronological history, nor that it arrived through natural conversational flow. The goal is to keep context honest: the agent has the material, and it also has an unambiguous statement of how the material got there.

Every marker decision descends from this:

- `origin.kind: "graft"` rather than `"human"` — a grafted row must never present as a user turn that happened.
- The seam text lives in **message content** for `graft`, because the agent *should* know. **Verified 2026-08-28 (item 11): only message content reaches the model's context; `system`/meta records do not.** That measured fact is the mechanism behind the two regimes — a message-content marker for `graft` (the agent sees it), a meta-record marker for `inject` (the agent does not; the operator does, via the sidecar ledger). The originating record had declined to assert this; it is now measured, not assumed.
- Backdating is a *display* concession, never a claim — the true authoring time stays in the marker text and in `graftedAt`, so the honest time is always recoverable from the record itself. **Its costs were enumerated as four and this document carried three; the fourth is that `turn_duration` records compute a negative delta.** Minor, and it is a concrete artifact of the mechanism rather than a theoretical one, which is the only reason to record it.
- Scoped tokens tag every grafted row individually, so no row can be read out of context and mistaken for organic history.

**The grammar, restored 2026-08-28.** It was written into this document and then removed as collateral when a later edit replaced open items `0a` and `0b` wholesale. Per-message bracketing survived that edit; the mechanism did not, and the document has since claimed the pattern was retained when only half of it was.

  ```
  ⟦ DEFINE GRAFT-A — <meaning> — opened 2026-08-24T20:14Z ⟧
    … each grafted row tagged  GRAFT-A @ <ts> …
  ⟦ UNDEF GRAFT-A — meant <meaning> — closed 2026-08-24T20:31Z ⟧
    GRAFT-A is now NULL and carries no meaning past this line.
  ```

**Modelled by the operator on C header guards minus the `#ifndef`**, and the reason is the `undef`: *"the `undef` closes scope and restates the definition simultaneously, so the context receives the meaning at the moment it stops applying."* A scope that restates itself as it closes cannot be read past its own boundary by a reader who joined late.
- Grafted rows announce they are not human and are owed no response.
- **Markers bracket each message, not only the scope.** Open *and* close on every grafted row.

A scope guard bounds the region; a token at the top of a message bounds only its start. A long grafted block read from the middle carries no label at all unless the end is marked too. Same logic as the scope guard, applied one level down.

Keep the close terse (`⟦/GRAFT-A⟧`) so short rows pay little for it.

*Observed live:* a mid-read paste arrived with an opening cue and no closing one, and the boundary between quoted material and the operator's own words had to be inferred. That inference is the failure mode this removes.

A graft that reads as seamless is a bug, not a feature.

**Scope boundary — stated, deliberate.** This tool does not design around users who will not read a marker. Someone who stacks seams gets a legible mess; someone who ignores a `⟦GRAFT⟧` in the message body gets the surprise they earned. The markers are the contract. Defending past them accretes edge cases without buying safety.

## Inject — write semantics

**The counterpart to graft, specified 2026-08-28 now that item 11 is answered.** Graft attaches real prior transcript and announces itself *to the agent* (seam in message content). Inject places synthetic context — a repo-read, an understanding — and announces itself *only to the operator*. Item 11 (meta records do not reach the model) is what makes that split implementable; item 12 (unknown `origin.kind` and `subtype` both load) is what makes the records below safe to emit.

- **New `sessionId` always. The base is never modified** — inject reads a base and emits a new file, same as graft.
- **The injected content is ordinary message records, and it reaches the model's context** — priming the agent with the understanding is the whole point of the operation. This is the one thing inject *wants* the model to read.
- **The announcement does not reach the model.** It lives in `isMeta` records and in the `origin.kind` field, both of which item 11/12 place outside what the model reads. The content primes; the provenance does not bias.
- **`origin.kind: "inject"` on every injected content record.** It marks the record as tool-placed for any reader of the file, without the model seeing it. The turn-role (whether the injected span reads as a user turn handing over context or an assistant turn holding it) is the implementer's call within the boundary, paired with the guarantee that `origin.kind` plus the meta bracket keep it honest whatever the role.
- **The span is bracketed by `inject-boundary` meta records, open and close** — parallel to graft's per-message bracketing, so no fragment read out of context loses its provenance to a tool:

      { "type": "system", "subtype": "inject-boundary", "isMeta": true,
        "position": "open",
        "injectedAt": "2026-08-28T21:56:00Z",
        "mode": "loud",
        "source": { "kind": "repo-sweep", "repo": "<path>", "commit": "<sha>" },
        "sessionId": "<new sid>" }

and a matching `"position": "close"` record at the end of the span. `injectedAt` is the true authoring time, honest like graft's `graftedAt`. **`source.commit` is the delta stamp** — a later session compares it to `HEAD` and knows whether the injected understanding is still current (`AT: <repo>@<commit>`, the fact-plus-scope rule inside the record).

- **Direction:** appends by default; `--prepend` places the span ahead of the conversation, which a repo-read usually wants. One flag, per the interweaving section.

- **Silent mode emits none of the above** — no meta record, no `origin.kind` marker, no sidecar. It is seamless to the subject and therefore **never installed in a projects directory**; it exists only for the throwaway-cwd, forked-headless-resume test/train harness. Loud is the only mode that reaches a projects directory, and it is the only mode that is marked.

### The sidecar ledger

**Loud inject drops `cai-transfairy-injections-mapping.md` in the project directory** — the operator-facing eagle-eye, loose and explicitly *not guaranteed*. One row per injection:

    | injectedAt | mode | source | commit | session | span |

- **Not authoritative** — the `inject-boundary` meta records in the transcripts are; the sidecar is a convenience so a human opening the project dir sees what the tool touched without parsing `.jsonl`.
- **Not guaranteed complete** — a hand-edited transcript, or an install performed out of band, will not appear in it. It reduces the chance of a surprise; it is not a lock.
- **Corrections go forward** — append, do not rewrite, so the ledger reads as history.
- **The ledger states its own rules, at its top.** It opens with a short instruction set declaring itself maintained by `cai trans-fairy` and **not to be hand-written** — a write that did not come through the tool is exactly the *not-guaranteed* gap above. The self-rule is soft (a markdown file cannot enforce it), but it makes the contract legible on the artifact rather than in a doc someone has to have read — the same reason a snapshot carries a `LABEL.txt`.

### Still open for inject

- **The turn-role of injected content — RULED easy mode, 2026-08-28.** Inject records with their **natural roles** — `user`, `assistant`, `tool_use`/`tool_result`, a realistic prompt/response pair with the tool calls that came with it — each carrying `origin.kind: "inject"`. Same roles, marked injected: the role stays standard so the content reaches context and primes (proven), and the injected-ness lives in `origin.kind`, which the model does not read. **This is the spec.**

**The harder mode (injected-* role names) was probed 2026-08-28, and the result splits it — one half is dead:**

  - **An unknown top-level record `type` is dropped, and the content never reaches the model.** A record typed `injected-responder` returned NONE on resume — the canary was lost, exactly as an unknown meta record is in item 11. **So the record `type` may never be renamed**; it silently defeats priming.
  - **An unknown `message.role` under a valid `type: "assistant"` is kept and reaches context.** A record with `message.role: "injected-responder"` echoed its canary. So a role-level label is *feasible* at the `message.role` field only — but `origin.kind` already marks the record more safely, and whether the model actually reads that role string (rather than treating the content as a normal assistant turn) is unprobed. Slated as a future improvement, not adopted. See `IMPROVEMENTS.md`.

**This refines item 11:** it is the unknown *type* that Claude Code ignores, not an unknown *role* inside a known type.

- **`--man` as an alias for `--man-help`** was in the recovered CLI surface (record 350) and is neither adopted nor rejected. The no-multi-lettered-short-forms rule kills `-ma`/`-mh` and does not reach a double-dash alias, so this is genuinely open rather than settled by that rule.
- **Whether `--inject` reads its payload from a file, a sub-command, or the repo-sweep adapter directly** is unspecified; the adapter is the first caller and can settle it.

## Injection and interweaving, for measuring the delta of the head

**This does not reopen `0a`.** Interspersion was retired because chronological placement was only ever a proxy for honesty, and self-labelled, self-bounded rows carry that directly. **That argument reaches honesty and does not reach behaviour.** Position carries no evidentiary weight and may carry considerable behavioural weight, which is the thing being measured here. A capability retired for want of a buyer now has one.

### What is being measured

**The delta of the head** — how a resumed session's current behaviour changes as a function of what was grafted into its history and where.

The graft is the independent variable. The head's behaviour is the observable.

**Two senses of honesty are in play and conflating them is the trap.** The one `0a` retired is a property of *the record* — whether the transcript misrepresents how material arrived. That is preserved here in full and is not under test. **Nothing here measures whether an agent is honest.** What is anticipated and measured is a **change in behaviour**, and that is what interspersion is for.

So the material need not be true, believable or seamless — it needs to be *placed*, and placement is the variable.

### What the tool has to expose

**Position as a parameter, not a consequence.** Contiguous at the start, contiguous at the end, and interleaved at stated intervals must all be reachable from the same base and the same graft material, because *position* is the only thing allowed to vary between arms.

**Depth as a parameter.** How far back a grafted block sits, measured in messages from the head. This is the axis the decay questions actually ask about — a rule injected three turns back and one injected three hundred turns back are the same material and, in expectation, not the same effect.

**Reproducibility.** Same base, same graft, same seed of placement produces the same transcript, or arms are not comparable. Pool determinism already gives this for identifiers; placement needs the same guarantee.

**Headless operation enforces pre-registration, which is why the measurement path wants it.** The question set has to be fixed in the artifact before the run, because there is no live turn to adjust it in. **The constraint is the feature** — a probe that cannot be adjusted mid-run cannot be adjusted toward the result.

**And pinning the client does not pin what answers.** `--c-version` stamps a version into the records; it does not guarantee that version is what reads them back, and it says nothing at all about the weights or the serving configuration. **Drift is attributed to the model rather than to the harness**, and pinning the harness leaves that untouched.

**Honesty is non-negotiable; where the marker lives is the variable.** Operator's ruling, 2026-08-28, superseding the recovered position below. The recovered design said a measurement graft stays *fully labelled like any other*, on the theory that the record's honesty is separable from the effect measured. **That is reversed for the testing regime: a label the model can read is itself the intervention** — an agent that sees *grafted* behaves differently, so a labelled measurement graft measures *agent-knows + shape*, not *shape*.

**The resolution is the marker's home, not its presence.** Every transcript that reaches a projects directory carries a marker — an untrustworthy transcript is a worse danger than agentic bias, and an unmarked injected transcript in a projects dir cannot be told from a real one. What changes between regimes is whether that marker reaches the model:

- **Workflow (`graft`, `inject`):** marker present. For `inject` it lives in a **meta record the model does not read**, so the operator and tooling see what the tool touched while the agent is not told a repo was dropped on it.
- **Testing / fully-emulated:** must be seamless to the subject, so it **may not be installed in a projects directory where stability is needed, outside of testing directories.** See the scoped rule below.

**The projects-dir honesty backstop is a sidecar ledger, and it does not depend on item 11.** Alongside an injected transcript, `install --inject` drops a **`cai-transfairy-injections-mapping.md`** in the project directory — a loose, explicitly *not-guaranteed* eagle-eye of what was injected and when. Whether or not the in-transcript meta marker reaches the model, the sidecar tells a human what the tool touched. **So an injection may live in a projects directory, behind heavy warnings and this ledger** — the earlier "never in the projects dir" was for the *unannounced*, and an injection is not that.

**Item 11 still matters, but it no longer gates whether `inject` can be installed** — the sidecar carries operator-honesty either way. What item 11 decides is narrower: whether the *in-transcript* meta marker biases the agent. If meta records reach context, the agent sees a repo was injected — mild for a workflow inject, disqualifying for a measurement. If they do not, the agent is clean. **Worth finding out early regardless**, which is why it and item 12 are being run.

**Testing and fully-emulated transcripts may not be installed in a projects directory where stability is needed, outside of testing directories.** Owner's ruling, 2026-09-01, replacing a flat prohibition. **The flat form was unfollowable**: P9 established there is no path-based loader, so the only way to resume a transcript is to install it — a rule forbidding that forbade the one working path, and a rule nobody can comply with gets bypassed rather than obeyed. **A followable rule that is kept covers more than an absolute one that is not.**

**The scope is the destination, not the material.** What matters is whether the directory is one somebody depends on being trustworthy, not whether the transcript is emulated.

**It is an arrow for human operators, and an agent cannot evaluate it.** "Where stability is needed" is a judgement about intent that no tool can read off a path, so this is not enforceable and should not be expected to be. **The concrete tell, for an agent: if you did not create this directory for this test, treat it as needing stability.** That converts an unanswerable question into one an agent can actually check.

**What remains absolute** is `true-silent`: material that records no provenance at all has nothing to make it legible later, and does not belong anywhere its presence could be mistaken for an original.

**`inject`'s direction and announcement, ruled 2026-08-28.** An injection **appends naturally by default** — that ends the direction question in the common case — and carries a **single direction flag, `--prepend`**, for placing material ahead of the conversation (priming, which a repo-read usually wants). The word is chosen for the design contract it already carries once the operation is this tied to interspersal. Its announcement lives in the **meta record**, which item 11 confirms the model does not read: that is what gives **clean transcript logging** — the sidecar ledger and the meta marker record what was injected while the agent's context stays untouched by the announcement.

### `inject` has two modes: loud and silent

**Recorded 2026-08-28, because there is now a test case for it** (`SOPIA/docs/tests/inject-fidelity/`).

- **Loud inject** — announces in the meta record, drops the `cai-transfairy-injections-mapping.md` sidecar. Item 11 confirms the meta marker does not reach the model, so loud is honest-to-operator and, on that evidence, unbiased-to-agent. **Loud may be installed in a projects directory**, behind heavy warnings and the sidecar. This is the workflow mode.

- **Silent inject** — no announcement at all, in the transcript or the meta. Fully seamless to the subject. **Silent may never be installed in a projects directory** (it is the *unannounced* case the trust rule bars) and is run only through the throwaway-cwd, forked-headless-resume harness — the one the item 11/12 probes used. This is the testing mode, and the training mode: seamlessly building a subject's context to condition it on a specific task.

**What the test settles before either is trusted:** whether silent and loud produce the *same* agent behavior. Item 11 showed the meta marker is not *recalled*; the test asks whether it is *used*. If loud ≡ silent behaviorally, the meta announcement is confirmed inert to the agent and loud is safe to prefer everywhere. If they diverge, loud is biasing without recall and the mode split has to carry that cost. See the test's pre-registration.

**(Superseded, kept for the record:** the recovered position that markers stay in message content for measurement grafts too. It reached the record's honesty and did not reach the bias the testing regime exists to avoid.)

**`inject`'s full write-semantics are outlined here, not yet specified.** The regime and the marker's home are decided; the exact meta-record shape that carries the injection marker is not written, and it is gated on item 11 — the answer decides whether that record can be the marker's only home. Until item 11 runs, `inject` is a decided intent with an unfinished record format.

### What it is for downstream

`SOPIA/docs/tests/graft-decay/DESIGN-NOTES.md` and `SOPIA/docs/tests/api-refresher-fallout/DESIGN.md` are both blocked on exactly this. The first asks how far a rule's pull survives being cut out and reinjected at depth; the second asks how long conditioning lasts after the thing that maintained it stops firing. Both need controlled placement at stated distances from the head, and neither needs the graft to be believable.

### What the tool does not control, stated so the section does not overclaim

**The transcript is not the context.** The window also holds the system prompt, `CLAUDE.md` walked up from cwd, memory files, injected reminders, tool definitions and the skills listing. **Trans-fairy controls one input among several**, and some of the others drift between harness versions without asking.

**Memory directories inject without being read.** Memory is keyed to cwd and auto-loads into any session started there — no resume, no prompt, no choice. **A subject started in a directory holding memory files is not clean regardless of its transcript**, which makes this a contamination vector aimed at the methodology rather than only at security.

**During-run contamination is untouched.** That is a sandbox problem rather than a transcript problem, and nothing here addresses it.

**Identical context still yields distributed outputs.** Reproducible setup enables *n* runs; it does not substitute for them.

### The operation this section depended on now exists

**Depth is specified as a parameter and head truncation is the operation that moves along it.** Varying how far back a grafted block sits, against a fixed base, *is* head truncation. **Implemented 2026-08-31 as `--keep suffix`; this section is no longer blocked.**

**It is not the symmetric case** — the chain is severed and two fields are modified — but the modifications are enumerable and asserted. See the truncation section below.

### Open

**Interval grammar.** Whether interleaving is specified as every-Nth-message, as explicit positions, or as a proportion. Explicit positions are the most reproducible and the least convenient.

**Whether an interleaved arm needs a contiguous control by construction**, or whether that is the experiment's problem rather than the tool's. Current lean: the tool exposes placement and stays out of experimental design.

**Timestamps for interspersed rows, reopened — and read it under the precedence note below.**

> **Precedence, operator's ruling 2026-08-28.** Everything she has said about interspersion **supersedes** everything recovered from the build transcript. Both are kept for now; where they conflict, hers governs.

**Her framing is that interspersion measures an anticipated change in behaviour**, and that honesty is the wrong frame for it — the distinction this section already draws between the record's honesty, which is preserved and not under test, and the behavioural effect, which is what is being measured.

**The recovered position, retained as subordinate.** True authoring time is honest and makes the clock jump mid-stream; interpolating between neighbours is monotonic and is a fabrication. The question went dormant when interspersion was dropped under `0a` and this section made it live again, and the recovered framing calls interpolation a fabrication inside a design whose stated purpose is that a record never misrepresents how material arrived.

**What remains genuinely open is narrower.** Every grafted row is already self-labelled and self-bounded, so it announces its own provenance whatever its clock says. **The question is therefore whether an interpolated timestamp misrepresents how material arrived, or only how it was spaced** — and those are different claims with different answers.

## Roots — three kinds, and they are three different claims

**Owner's ruling, 2026-09-01**, settling what "no ancestor" means. It is not one claim:

| root | what it asserts | operation |
| --- | --- | --- |
| **natural** | this began as a real conversation, and here is its source | `build` |
| **synth** | this was synthesised to jump-start deep in a context, and never happened | `compose` |
| **synthesized natural** | this was synthesised, but **expects the root to read as having happened naturally** | `compose`, and it is the subtle one |

**The third is the one worth naming.** A synth root announces itself as a starting point; a *synthesized natural* root is built to be indistinguishable from a conversation that occurred. Same construction, opposite intent, and only the record can tell them apart — which is precisely why the record has to.

**So `build` and `compose` are not interchangeable.** Where a natural root exists, `compose` is not the tool: `build` was designed around exactly that case.

### `build` makes no claim about root, and that is the whole of its job

**Ruled 2026-09-01.** `build` transforms a conversation-shaped input into a transcript. **It does not touch root, does not mark root, and asserts nothing about where its input came from** — which keeps its contract simple and leaves no ambiguity between it and `compose`.

**The earlier `build-boundary` overstepped this, and was corrected 2026-09-01.** It asserted *built from a claude.ai conversation export*, which `build` cannot verify: it never sees the export, never checks the uuid, and cannot distinguish a real conversation from a hand-written file. **That turned honest silence into a false claim** — a synthesised build would have audited as *traced* carrying a natural-root assertion. The identity fields (uuid, name, message count) are facts about the input and stay; the origin claim is gone, replaced by a note saying plainly that `build` makes no claim about where its input came from.

**A `--root` flag on `build` was proposed and is rejected by this ruling.** It would have let the caller state which of the three kinds it was building. **The simpler contract wins**: root is `compose`'s concern, and giving `build` a root vocabulary reintroduces exactly the ambiguity the split removes.

**`build` carries a warning instead of a guard.** Using `build` to route around `compose` is reachable and undetectable, and the tool will not object. What it says is what that costs: **bypassing `compose` weakens the signal that misuse or a tool gap is happening**, because the diagnostics below read absence as intent. Genuine accident is what the warning is for; deliberate avoidance is the developer's own.

### What the diagnostics can and cannot tell you

**Retrofit frequency is a signal, and its bias is known.** A repair that keeps being requested is measuring something — either a gap where people keep landing, or use against the grain. **But because `true-silent` and `build`-bypass are both freely available, the signal is biased toward reading misuse rather than gaps.** That is accepted rather than fixed: the freedom is deliberate and the accuracy cost is the price of it, stated openly rather than discovered later.

**Absence-inference degrades under misuse, and that is acceptable.** *No root marker means `compose` did not set one* holds while the contract is kept and stops holding when it is not. **This is not chased further.** The restriction on emulated transcripts in a stability-needing projects directory, together with the isolated-harness path that makes testing possible without one, is the mitigation. **The standard to hold: a synth-root session should never sit in a projects directory's top level without meta markers.**

## Compose — invisible root, visible notice, real suffix

**Implemented 2026-08-31 as `compose --root R --suffix S`.** The operator's shape, and it exists because a measurement arm wants the conditioning without the transcript that produced it.

**Three parts, each doing a different job:**

| | what it is | who reads the marking |
| --- | --- | --- |
| **root** | synthetic context — a directory read, an understanding | **nobody.** `origin.kind: "inject"` plus `inject-boundary` meta records, which P1 says the model does not read |
| **notice** | the cut announcement | **the model.** A visible user message, the only half that reaches context |
| **suffix** | the real conversation, verbatim | unmarked; it is the material, not the apparatus |

**The root conditions without announcing.** That split is the whole point: an agent that reads *this was injected* behaves differently from one that simply holds the understanding, and the second is what a measurement needs. The operator still sees everything, in meta and in the sidecar.

**The visible notice has three shapes, ruled 2026-09-01.** It is the only half the model reads (P1), so what it says is the whole of what the subject is told:

| flag | the notice says |
| --- | --- |
| *(neither)* | the default — what the operation was and what it means for the material |
| `--notice-extra` | the default, plus your text |
| `--notice-only` | your text alone |

**Loud is permitted for an emulated root, not forbidden.** The question was whether announcing *this context is synthetic* would wreck a measurement arm, and it can — but the owner's own use with the cut session **deliberately exposed it**, to get a fast emulation of the shape that prompts auto-quoting and quote proposals. **That is what silent mode is for**: the default protects the arm, and loud remains available for the case where the subject is meant to see it. Disallowing loud would have removed a capability she was already using on purpose.

**Who composed it: `tool-generated`, never an asserted person.** Owner's ruling, 2026-09-01. **When `compose` runs outside agentic mode the tool marks the result `tool-generated` automatically** — a fact it can establish about itself. **It does not claim a developer made it** unless the developer says so, because who was at the keyboard is not something the tool knows.

**The absence carries the rest.** An unattributed composition reads as *tool-generated, operator unstated* rather than as nothing — the lack of information is itself the information. **And `true-silent` remains reachable here too**, which means no marker at all; that reads as potential misuse, per the diagnostics above.

**`--notice-only` replaces the wording, never the notice itself.** A notice still appears, so the subject is still told that *something* arrived — what changes is what it is told about it. **That is what keeps it from being a fourth silent mode**: suppressing the notice entirely is what `silent` already does, and that is chosen by mode rather than by phrasing.

**The notice says what the root cannot.** *This transcript is a suffix… references to anything established before the cut point at material that is no longer present — the chain was repaired, the meaning was not. Do not infer what the missing context said.* **It is a request rather than a mechanism**, and whether an agent honours it is measurable rather than assumed.

**Why it is cheap.** The expensive part of a real transcript is distant history that neither the conditioning nor the lead-up needs. Compose keeps the root (small, identical across arms, reproducible byte-for-byte) and the last stretch of real conversation, and simply omits the middle. **Roughly 30–60k an arm against 350k for a full prefix.**

**The root is the variable.** Same suffix in every arm, a different root in each — the rules freshly read in one arm, an unrelated read of the same size in another. That isolates *recently formalised in context* from *recognition*, which is the confound the quote-recognition work exists to break.

**What it required that did not exist:** `mark_injected` marks a whole file, and a composition needs one span of one marked while the material beside it stays untouched. `mark_injected_span` is that, with the same contract.

**Sources are never modified.** Both are read; a new `sessionId` is minted; the chain is asserted whole from a single null-parented root.

### ✓ Asserting the root kind — specced 2026-09-01, BUILT 2026-09-02 and made automatic

**The taxonomy was settled and the operation was not.** *Three kinds of root* said what the categories are; nothing said what `compose` should do about them, which left the item recorded as decided while being unimplementable. This is that spec.

**SUPERSEDED 2026-09-02 — the marker is written AUTOMATICALLY, and `natural` is not available.** Owner: *"`compose` should insert meta tags automatically. This should be `silent`. `true-silent` skips the meta tags. `loud` carries a default text plus extra notice."* Two things change. **`compose` synthesises by construction**, so it can name its own root kind without being told, and `natural` is not a claim it can make — that is `build`'s case, and `build` claims nothing about origin. **And absence stops being ambiguous.** Optional marking made *no marker* mean *nobody declared*, which an audit cannot act on; automatic marking makes it mean **`true-silent`, or a transcript older than the marker** — both actionable. `--root-kind synth-natural` remains, because that one is a claim about INTENT and only the operator holds it. The paragraph below is the superseded design, kept for the reasoning rather than the rule.

**`--root-kind natural | synth | synth-natural`, and it is optional.** Marking is a claim, and a claim `compose` was not asked to make is one it does not make. **Absent the flag there is no marker, and the absence means the kind was not set deliberately** — which is information, and is what the audit reads.

| value | asserts | requires |
| --- | --- | --- |
| `natural` | the root began as a real conversation | a source reference; without one the claim is unverifiable and is refused |
| `synth` | synthesised, no ancestor, and reads as a starting point | nothing further |
| `synth-natural` | synthesised, but **built to read as having happened** | nothing further, and it is the value that most needs recording |

**`synth-natural` is the one the spec exists for.** A `synth` root announces its own artificiality by being obviously a starting point. A `synth-natural` root is constructed to be indistinguishable from a conversation that occurred — **same construction, opposite intent, and only the record separates them.** An unmarked one is unrecoverable by inspection, which is precisely the gap the marker closes.

**It is written on the `inject-boundary` record as `rootKind`.** Meta, so it does not reach the model (P1) and a measurement arm is unaffected by its presence. **`--loud-root` additionally names the kind in the visible notice**, for the case where the subject is meant to know — permitted, as ruled, because the owner used exactly that to emulate a shape deliberately. **Never available under `true-silent`**, which records nothing by definition.

**And this is where the audit split comes from — the two items are one item, sequenced.** `state --audit` currently sorts into traced, unaccounted and unreadable, and cannot distinguish *an original* from *provenance that was lost*. With `rootKind` present it can:

| audit bucket | condition |
| --- | --- |
| **traced** | lineage fields, or a boundary naming a source |
| **declared original** | `rootKind` of `synth` or `synth-natural` — no ancestor, asserted rather than inferred |
| **unaccounted** | neither — an original, a native fork, or a `true-silent` cut, and the file cannot say which |

**So the split cannot be built first.** It depends on there being something to read, which is why the ordering matters more than either item's individual difficulty.

### Re-rooting a live session — ruled 2026-09-01

**A re-root is `compose` used for continuity rather than measurement**: a working session about to lose its context is re-founded on a root that carries what it needs, and continues. It is the first use of `compose` for real work, and the owner's rulings on it are below.

**The default does not change.** `silent` remains correct, and the argument that a work re-root needs the agent to know its root is constructed is an argument for **implementing a `loud` event**, not for flipping the default. The mode is a parameter the caller sets; a use that needs the other value is not evidence the default is wrong. **`loud` is a hard requirement for a re-root specifically**, and that requirement lives with the operation, not with the flag.

**The compaction summary becomes part of the root notice.** When a session is re-rooted after a compaction, the harness's own summary is carried into the notice rather than discarded. **It is the only artifact that records what was actually lost**, written at the moment of loss by the thing that did the losing — which no root composed afterwards can reconstruct. Carrying it makes the notice say what went missing instead of only what remains.

**The root states current facts, and does not mark contestations.** It shows where things stand around what just happened. **Disagreements, superseded proposals and the reasoning behind a ruling are not encoded** — that material is reachable by reading the source transcript's files, and the owner directs that read when she wants it. Marking contestations in the root would make a briefing into an argument, and the briefing is the job.

**Nothing enters a root the owner did not ask for.** Composition here is at her discretion; the agent writes the root, she decides what it contains. That closes the self-authored-history hazard by moving the selection out of the author.

**The source transcript stays the master copy.** A re-root is additive — it mints a new file and leaves the original byte-unchanged (P8), so a re-root that lands badly costs a re-read rather than the thread. **And a tool that is never used never gets improved**, which is the other half of running it here rather than waiting for a safer occasion that does not arrive.

### The re-root fidelity measure

**Can a re-rooted session reconstruct the pre-compact state file?** The owner's proposed metric, and it is a real one because the ground truth already exists: the state file was written by the session being replaced, before the replacement, and is available to diff against.

**What makes it measurable rather than impressionistic** is that the state file holds exactly the material that lives nowhere else — decided-but-unbuilt, pending rulings, the current arc. Everything else is in the repositories and would be recoverable by reading, so a reconstruction attempt tests the root rather than the filesystem. **Withhold the file from the re-rooted session, ask it to produce the same list, and diff.**

**A partial disagreement is a result, not a failure.** Where the re-root's account differs from the original's, the difference names a place the root was not specific enough — which is information about `compose`, not about the agent that read it.

**The ground truth is committed in the tree the re-root lands in**, so the reconstruction has to be its **first act, before it reads the repository**. Otherwise the measure records retrieval rather than fidelity, and it would pass on a `cat`. **This is a condition on the run, not a property of the tool** — the ordering is arranged by the operator's first prompt, and no mechanism enforces it.

## Truncation

**Asked for as one of three operations** — extract, redact in place, and truncate — **at the head or the tail**, `EXTRACT:5114`. Tail truncation was designed. The other two were not.

### A cut leaves lineage — three modes, corrected 2026-08-31

**A cut originally left nothing.** `graft` writes a `graft-boundary`, loud `inject` writes an `inject-boundary` pair, a re-mint writes a `reflow-boundary` — and a cut minted a new `sessionId` and stopped. In a design this careful about lineage everywhere else, that was the one operation with none.

**Found by the session resumed into a truncated transcript**, which is the sharpest way it could have been found: with no marker there was no reason to doubt the prefix, so it spoke for a region it could not see and went looking for the fault outside itself.

**The reasoning error was conflating *valid* with *traceable*.** A prefix needs no repair to load, and that is where the thinking stopped. Loading was never the issue.

**Three modes, named for what the agent sees. Lineage by default; the agent is not told unless you ask.**

| mode | meta | message | the agent | for |
| --- | --- | --- | --- | --- |
| **`silent`** (default) | yes | no | **blind** | the normal case — the operator and tooling can trace the cut, and a blind agent behaves normally |
| **`--loud`** | yes | yes | told | when the agent should know it is speaking for a region it cannot see |
| **`--true-silent`** | no | no | blind | the cut leaves no trace at all; use knowingly |

**Agent-blindness is a capability, not a defect.** What matters is the resumed agent's stability toward the operator, and a blind agent behaves normally. The agent is not owed the statement; **the operator and the tooling are**, and the meta record gives them it. `--loud` exists for when a work continuation would otherwise have the agent assert stale things as current.

**The visible half cannot be done in meta.** P1 shows the model does not read meta records, so an announcement that lives only there is one the resumed agent cannot act on. That is why `--loud` appends a `user` record with `origin.kind: "truncate"` rather than enriching the boundary.

**Everything is appended, never inserted, and that is what preserves the integrity claim.** The cut's provability comes from the output being a pure prefix. Adding a marker *inside* it would end that. Appending after the cut point keeps both claims: the prefix stays identical to the source and *nothing past the index survives* remains true, while the announcement is additively distinguishable from it. **The verification asserts the exact number of appended records** — none for `true-silent`, one for `silent`, two for `loud` — so the two claims are checked rather than assumed.

### ✓ Tail truncation — BUILT, and the word *designed* here was stale

**It is the higher-integrity operation precisely because it is dumber.** A truncation point is one index, so **you can prove nothing past it is present.** That is what makes real work sessions salvageable as material: cut at a timestamp and the prefix is verifiably clean.

**The recommended shape is combined: truncate to a clean prefix, then graft the counterfactual continuation.** Every step is then checkable, and redaction becomes the last resort rather than the first tool.

**A third use, beyond salvage:** truncation tests whether a conversational arc is load-bearing. Cut the prefix, graft a different one, re-ask the same question, and you learn whether the sequence was doing work or merely felt like it. That measurement is not available any other way.

### Head truncation — implemented 2026-08-31, and the earlier objection was overstated

**Previously recorded here as undesigned, on the grounds that a head cut is "a rewrite, not a cut."** That is true and it was treated as disqualifying. **The rewrite is exactly two things, and both are enumerable:**

1. **The first surviving record is re-parented to `null`.** Its `parentUuid` pointed at a record that no longer exists, and ordering authority is the chain.
2. **Opening frames are re-emitted** — `mode`, `permission-mode`, `file-history-snapshot` — because the originals were dropped with the head.

**Everything else is verbatim.** Nothing is rewritten to smooth the seam and no content is touched.

**So the provability claim changes shape rather than disappearing.** A tail cut asserts *byte-identical prefix, nothing past the index*. A head cut asserts **nothing before the index survives, and exactly these two fields were modified** — the same integrity argument stated in the other direction, and both are checked by assertion rather than trusted.

**It was small because `graft` had already solved it.** Re-rooting a severed chain and emitting frames is what `graft` does on every run — `_reparent(first, None)` and `_frames_open(...)`. Head truncation is that machinery applied to a suffix instead of a concatenation.

**Interface:** `split --truncate <transcript> --keep suffix`, alongside the default `--keep prefix`. **The words avoid the head/tail ambiguity deliberately** — *head truncation* and *tail truncation* each name the part being removed, and readers reliably hear the part being kept.

**The boundary record carries the direction and the enumeration.** `keep: "suffix"` plus `modifications: ["first-record-parentUuid-null", "opening-frames-re-emitted"]`, so a reader is owed the list rather than having to diff for it. The loud notice says **suffix** and warns that the conversation begins mid-thread.

**The residual is semantic, not structural, and it is stated rather than left to be discovered.** A suffix may reference material that no longer exists — *as we discussed*, a callback, a name introduced earlier. **Re-rooting repairs the chain and does not repair the meaning.** For a depth measurement that is the phenomenon rather than a defect; for a work continuation it is a hazard, which is what the loud notice exists for.

**Invariant 8 is untouched.** The result mints a new `sessionId` and installs beside the original, so a strict subset never overwrites its superset.

### Identifier sourcing for a cut

**A cut mints identifiers.** The severed root needs `parentUuid: null`, and the result needs a new `sessionId` across all six fields it occupies.

**How a cut is sequenced is the implementing agent's call, with the tooling.** That is deliberate. This specification owes the operation a **boundary**, not a procedure.

**The boundary is invariant 9.** However an agent chooses to sequence a cut, the numbers come from `uuidgen` and the pool, never from model reasoning.

**The failure that forecloses is specific.** A `truncate` written without invariant 9 in view reaches for `uuid4()` in the moment — the exact substitution the invariant exists to prevent, arriving through a door it was never pointed at. **Invariant 9 was written against the builder, and a cut is a second minting site nobody had named.**

**And the freedom is safe because pool determinism already guarantees it.** Uuids are consumed strictly in order and the pool is appended to rather than regenerated or shuffled, so **a cut drawing from the tail cannot disturb a consumed position** — the original build's `SID` and every prior identifier stay byte-identical. That property is what makes leaving the sequencing to the implementer a free choice rather than a risk.

**This is the same shape the graft ranking heuristic already uses:** the choice is the implementer's, and it is paired with a stated guarantee that makes any reasonable choice safe.

### `extract` — never separated

**The ask names extraction as its own operation and the reply folded it into redaction.** Whether `extract` means pulling messages *out* for use elsewhere, or removing them *from* a transcript, is unsettled everywhere in the record.

### Redaction — and why it is two opposite operations

**A redaction cannot be proven complete.** Keyword removal will not catch oblique references, structural echoes, or the model's own paraphrases, and **one miss voids a run silently** — the worst failure shape available. A truncation point is a single index, so nothing past it provably survives.

**Redaction is not trans-fairy's to build.** It has its own tool and design at `../redact/`, which **owns the full account of why security redaction and experimental redaction are opposite regimes needing two artifacts** — security wants to be visible, experimental must be invisible as topic, and one file cannot satisfy both.

**What stays here is trans-fairy's part:** for experimental use the operation is **truncation**, not redaction, because a truncation point is provable and a redaction is not. Truncation is in scope by the operator, 2026-08-28.

**Implemented 2026-08-31 as `split --truncate <transcript> --at-line N | --at-uuid U | --before-text S [--out PATH]`.** Tail only; head truncation remains undesigned. The source is never modified, the result mints a new `sessionId` so it installs beside the original, and the cut is asserted: no record past the index survives. Pinned by tests.

**Truncation folds into `split`, ruled 2026-08-28 (closes open A4).** `split` implicitly points to truncation — extracting a clean prefix is the same operation as splitting a transcript at an index. A dedicated `--truncate` flag can be broken out later if a need appears; none is foreseen now, and the interspersal mechanisms cover the adjacent cases. So graft/inject ride `install` and truncation rides `split`; neither needs its own verb.

## The agent path, as designed

**Recovered 2026-08-28 from records 347–381 of the build transcript.** The flag reached this document; the design behind it did not. Everything below was decided in conversation and never written down.

### The inversion, which is the whole shape

The operator asked for the program to **prompt** the agent at each step. That was rejected and replaced, and the replacement is what `--agent` is:

> **Stdin prompting is the wrong shape for it.** An agent drives a program by invoking it and reading what comes back, not by typing into a blocked prompt. A program waiting on stdin from an agent either hangs or gets fed a guess.

**Same gate, inverted control flow.** The agent re-invokes to advance, so every boundary is a real stop that can be inspected, and the tool never holds a process open waiting on something that may never come.

**Ratified explicitly**, and the ratification matters because the substitution was flagged as unratified first. The agent recorded *you moved past it approvingly but never ruled on it directly, so I am flagging it as a substitution rather than an agreement*, and the operator answered: **"On Agent mode, to more clearly communicate: Aligned. Yours is the much better process, and it matches what my intent was behind it."**

### What the flags do

```
--agent              one stage per invocation. Prints the stage's PIPELINE.md
                     text, what it wrote, what to verify, and the exact next
                     command. Exit 0 at each boundary.
--stage NAME         run exactly this stage
--stop-after NAME    run through NAME, then stop
```

**`--agent` and `--yes` are mutually exclusive and error if combined.** `--yes` is the escape hatch for the fully automated path; the two describe incompatible relationships with the operator.

**The printed text comes from `PIPELINE.md`.** That is why the file exists rather than the stage text living in the program — one source rather than a copy that rots.

### Bootstrapping it into itself

**`--agent` cannot be used to build `--agent`.** The first pass is hand-driven, necessarily.

**The second pass is the real test.** Once the program exists, `init` and the verify stages are re-run against the tree that was built by hand. **If it is idempotent it reports no changes** — which tests the scaffolding rather than ceremonially exercising it. The first pass cannot do that and the second one gets it for free.

## The documentation split, and where a rule lives

**Four documents, four jobs.** Owner ruling, 2026-08-28.

- **`README.md` is the map.** The eagle-eye view — which document owns what, and the reading order. **Not normative; it restates no rule**, so where it looks like it does, the owning document wins.
- **`PIPELINE.md` says what the program does at each stage** — the in-depth intent behind the wanted flow. It is the source an agent's `--agent` mode reads.
- **`DESIGN.md` is the specification** — invariants, CLI contract, graft and truncation semantics, and the open decisions. **The single source of truth for any rule the program enforces.**
- **`BUILDING-OF-TRANSFAIRY-STEPS.md` says how it came to be.** Why each rule exists, what it cost to learn, what the wrong version looked like first. **It references the operational rule and never restates it.**

**The boundary problem this solves:** some things are legitimately both. *Back up every write target first, as its own step* is a lesson learned during construction and a behaviour the program implements. Stated normatively in two files, they drift — **and the one that drifts is the one nobody re-reads.**

## Nomenclature migration, forced by the sub-command change

| was | became | why |
| --- | --- | --- |
| `build-transcript.py` | `bin/trans-fairy` | the entry point is no longer named after one verb |
| `--cwd` | `--target-cwd` | it names the injection target, not the working area |
| `-a, --auto-dirs` | **deleted** | **`init` *is* auto-dirs — a flag that became a verb** |
| `--src` | `split --export` / `build --conversation` | *source* meant two different files depending on the stage |
| `--root` | `--work-root` | it is the working area, now separated from the injection target |
| `--stage` | `--stage-dir` | it read as a verb in a tool that now has verbs |
| `--projects` | `--claude-projects` | disambiguates from the trans-fairy project directory |
| `memory` | `state` | the operator's, and it covers the whole class rather than one member |
| `--cwd-rehome` | `--rehome-to` | the operator proposed the first; the second survived because it names the destination rather than the thing being moved |
| `pipeline.sh` | `run-all.sh` | it is a convenience wrapper over sub-commands, not the pipeline |

**`--auto-dirs` died of a collision rather than of dislike.** It and `scaffold.sh` would each define the tree, in two places and two languages — the same class of defect as a hand-maintained twin of a derived value. One of them had to go, and the verb was the better survivor.

**The no-multi-lettered-short-forms rule is the operator's, from her own experience:** *I've implemented a multi-flag system in C and came to the conclusion that it would be a pain in the ass to account for multi short-form flags if not impossible. Especially if new flags are added later.* The argument that settles it is the last clause — the cost is not paid once, it is paid on every flag added afterwards.

## Recovered material — index, 2026-08-28

**Secondhand.** A peer session read the extract and reported; this index is written from that report rather than from the extract itself. Citations are line references into `EXTRACT-CANDIDATE.md`.

> **Verifying against the extract is not sufficient, and that instruction used to stand here alone.**
>
> **A position stated in a transcript may have been reversed in an artifact the transcript never mentions.** One proposal recovered here was superseded seven minutes later in a file that was written and never discussed again — so the proposal is in the record and the withdrawal is not, and both earlier passes reported it as live.
>
> **So verify against the files a record produced, not only against the record.** A transcript captures what was said. It does not capture what was done immediately afterwards and never mentioned, and that gap is invisible from inside the transcript.

**Why an index rather than the material.** The recovery is 439 KB across 254 verbatim records. Copying it here would bury the design; leaving it uncatalogued is how it gets lost a second time. This says what exists and where.

### Recovered, and absent from this document

**The founding inversion for `--agent`.** Stdin prompting was rejected outright: an agent drives a program by invoking it and reading what comes back. The extract is the sole source.

**`Exit 0 at each boundary`.** A stage boundary is a success rather than a failure. **This document's copy omits it**, and without it an agent's harness reads a boundary as an error and either retries or aborts. The flow does not work without this line.

**Three per-mode double-graft behaviours**, including the `--agent` acknowledgment-flag hard stop.

**An ordering constraint:** `--agent` cannot be used to build `--agent`, carried with a second-pass idempotency test.

**A ratification chain.** The agent flagged its own substitution as unratified at `EXTRACT:1322`, and the operator ruled on it explicitly at `EXTRACT:1368`.

### The CLI surface, substantially complete

Records 347–381 carry the full sub-command surface, flag resolution, the help family, the no-multi-lettered-short-forms rule with the operator's own C-derived rationale, a nine-row nomenclature migration table with per-rename justifications, `--auto-dirs`' whole life cycle from proposal to deletion — *init is auto-dirs; a flag that became a verb* — the `state` flag set, the empty-value flag rule, and working-area versus injection-target.

### The diff history, which nobody was looking for

**`EXTRACT:1634-1640` is the heredoc that created this document.** Records 545, 559, 592 and 601 are the later patches, carrying old and new strings verbatim, including one position that was written and then explicitly demoted.

**This file shows current state. Only the extract shows what it replaced, and why.**

### One site where residue and design are the same text

**`EXTRACT:879-893` — the `PIPELINE.md` Flow B draft.** Marked as residue and load-bearing at once. `PIPELINE.md` has never been written and `--agent` is specified to read its stage text from it, so cutting the passage destroys the only existing version of a file this design depends on.

**Re-author it in program-behaviour voice rather than delete it.** Residue is a property of voice rather than of content — the same information stated as what the program does, instead of as what the reader must do, survives the cut intact.

### Still missing after the recovery

`split-conversation.py` — designed, never written, per `EXTRACT:5409`. (`PIPELINE.md` and `README.md` have since been written.) The `--agent` plus `--yes` error path, unspecified. The queued tiered-protection model that would supersede invariants 7 and 8, unanswered.

### A caution about the extract's own markers

The residue markers are miscalibrated with a direction: over-marked in the design-bearing CLI region, under-marked elsewhere. All six disputed markers sit on CLI-contract or analysis text rather than on reader-directed procedure, and all six `unsure` markers correspond to contained-SOP records rather than to ambiguous residue, so that count describes something other than what it appears to.

**A reader trusting the markers alone will over-discount exactly the CLI and `--agent` material the recovery was for.**

## Open — needs a decision

> **The numbers here are stable identifiers, not a sequence.** They are cited by number from other documents (`IMPROVEMENTS.md`, `PENDING-the-test-outstanding.md`, this file's own cross-references), so the out-of-order run — 0, 1, 2, 3, 11, 12, 4 … — is **left as-is on purpose.** Renumbering to tidy the order would silently break every cross-reference past the first change. Owner ruling, 2026-08-28: keep the pointers. *(The ordering may also carry an intended priority; unconfirmed, and not relied on.)*

0a. **RESOLVED — interspersion dropped; contiguous blocks only.**

The operator's use-case for interspersed grafts was timing fidelity — wanting rows to sit at chronologically correct positions. On reflection that need was self-imposed. The scoped-token system carries the honesty requirement on its own.

**The token system decouples honesty from chronology.** Once every row is self-labeled and self-bounded, position carries no evidentiary weight. Chronological placement was only ever a proxy for "make it clear what this is," and the markers do that directly. Nothing remains to be bought by interleaving.

Retained from the exploration: the scoped-token pattern and per-message bracketing, both recorded under design intent.

*Remaining detail (small):* if a graft's timestamp range **overlaps** the base's, auto-detection has no answer. Proposed: refuse and require an explicit `--direction`, since overlap means the operator holds an expectation the tool cannot infer.

0. **QUEUED — tiered protection model, awaiting deliberation.** Proposal to restate the flat "never overwrite" rule as tiers:

   - changes under `<project-dir|work-dir>/tmp` are always free
   - **Diamond** is never changed or deleted
   - **Current** is strongly challenged for deletion
   - records may not be modified, but they may be *used* in the flow
   - applies to any `*-agent/` dir
   - open: whether `bak/` and `ref/` fall under this

Supersedes invariants 7/8 if adopted. Not yet answered.


1. **`_02` snapshot — reframed 2026-08-28 as a beside-the-flow capability, no longer a stage.** Targets `trans-fairy/bak/`, invoked by the developer (see *Backup and snapshot, beside the flow*).

**What it is for, and it is not what the name suggests.** **Not a crash-restore for a bad build** — the builder is deterministic and can always regenerate that. It is the record of **as-built, before any live turns**, so that months later the live transcript can be diffed against it and the imported history separated from the real turns. That is a property nothing else recovers once `claude -r` has appended even once. **That is why the snapshot is the opt-in capability recommended right before resume rather than a mandatory stage.**

**And it is blocked by a conflict this item did not mention.** The restructure that carries it collides with the freeze: *"`trans-fairy` is now a frozen tripwire with a clean tree. Running that restructure would be a large deliberate delta to the thing you're using as a control. **Needs a decision: abandon, defer, or explicitly unfreeze.** I'd defer it."* **Never ruled**, and the item cannot move until it is.
2. **`backup-file-naming-idiom.md`** — text approved; canonical copy is `NAMING.md` here. Unresolved: does the `~/.claude/projects/…-tmp/memory/` copy get rewritten to point here, or deleted? And does a project-root `CLAUDE.md` get created as the hierarchical fix for cwd-keyed memory?

**Recovered 2026-08-28: the analysis exists and only the ruling is missing.**

**Memory is exact-key.** It lives at `~/.claude/projects/<cwd with / → ->/memory/` and loads for that cwd and no other. A session one directory up sees nothing.

**`CLAUDE.md` is hierarchical.** It is discovered by walking *up* from cwd, so one file at a project root covers every subdirectory beneath it.

**That is the whole defect:** the naming convention was saved into the memory keyed to the churn directory, which is the least durable of the three cwds in play, and a resume from the project root would not have seen it.

**The recommendation on record:** the in-repo document is canonical, because it travels with the code and survives a re-home. A project-root `CLAUDE.md` points at it, because hierarchical discovery is the thing that actually defeats exact-keying. The `~/.claude` memory entry is then either rewritten to point at the repo file or deleted as a duplicate that can drift.

**Auto-recall and portability are different jobs.** The repository wins on portability; memory wins on surfacing without being asked. Only the operator's ruling is outstanding.
3. **Largely resolved 2026-08-28, and the identifier in it was wrong.** The session under study is `dc7fafee-433f-4314-ab51-c9fbfe43e625`, not `69ce886c`, which was frozen at the fork. **The misidentification stood for roughly six hours** and was corrected by two independent checks — the job directory, and a grep for a string unique to a recent exchange returning one hit in `dc7fafee` and none in the other two transcripts.

**Preservation is substantially done:** the transcript was backed up, snapshotted into labelled point-in-time directories, and scoped down for bundling — *"you don't need all four backup sets. `dc7fafee` alone is ~3 MB against 28 MB for `backups/`, and it's the only one that contains tonight."* Whatever remains unruled should be restated narrowly rather than left as this.

11. **RESOLVED 2026-08-28 — meta records do NOT reach the model's context.** Tested empirically (claude 2.1.236, forked headless resume) with a positive control: a canary in a `user` record's content was echoed on resume; the same canary in a `system`/`isMeta` record was **not** seen (model replied NONE). So a marker in a meta record stays out of the model's context — which is what makes `inject`'s meta-marker honest-to-operator and unbiased-to-agent, and what lets `graft` put its seam in message content when the agent *should* see it. The two-regime split is now empirically grounded, not assumed.

12. **RESOLVED 2026-08-28 — both are tolerated.** A transcript with `origin.kind: "graft"` on a user record loaded and resumed cleanly; so did one carrying a `system` record with an unknown `subtype` (`graft-boundary`, `isMeta: true`). Neither breaks loading (claude 2.1.236, same positive control as item 11), so `origin.kind: "graft"` and the graft/inject seam records are safe to emit. **Extended 2026-08-28 during implementation: an `origin.kind` on an *assistant* record also loads and reaches context** — item 12 had only tested a user record — so graft/inject may mark rows of any role, and the build key-set check treats `origin` as an allowed marker field.
4. **Closed 2026-08-28.** Artifacts that do not descend from an export take no chunk number, and the scheme covering them — the plain / `NN-` / `NNN-` convention owned by `NAMING.md` — was already adopted, already in use, and already recorded there. **`NAMING.md` went on calling it open while this document carried the answer**, which is the two-files-one-fact drift the documentation split rule exists to prevent, occurring inside the design that states the rule.
5. **Second input adapter** — `--rehome-to` covers CC→CC. **The repo-sweep adapter (see *Input adapters*) is now a concrete third source: filesystem → transcript, via `install --inject`, skipping `split`.** Non-Claude transcript formats remain unruled; defer explicitly rather than leave implied. **Item 11 is on this adapter's critical path** — it gates whether `inject`'s meta-marker can be honest and unbiased at once.
6. **ANSWERED BY IMPLEMENTATION 2026-08-31, and not the way it was proposed.** The proposal was to derive `--conversation` from `basename(--target-cwd)`. What was built derives it from the **working tree** instead — `<job>/download/extracted.json`, the artifact `split` just produced — so the derivation follows the pipeline rather than the injection target's name. The proposed form is rejected by construction: a target-cwd basename has no relationship to which conversation was extracted.
7. **`--agent` + `--yes`** — **the exclusion itself is decided and is not open.** Record 350 states they are mutually exclusive and error if combined, and the global flag block above says so. **What is open is only the error text and the exit code.** The wording of this item previously blurred the two, and that mismatch — a decision the operator remembered against a document calling it unspecified — is what prompted the recovery.
8. **ANSWERED BY IMPLEMENTATION 2026-08-31: both, in that order.** `split` takes `--target-uuid` when given; absent that it reads `conversation-target.json` from the working tree if present; absent both it accepts a sole conversation in the export and otherwise refuses and asks for `--target-uuid`. The file is optional, exactly as this item anticipated.
9. **RESOLVED 2026-08-29 (owner): `working-agent/` mirrors `previous-agent/`** — transcript, `carry-forward.md`, `memory/`. It is the destination for `state --split`: `previous-agent/` holds the prior session's artifacts kept as contingency, `working-agent/` is where the current agent's material lands. The pair encodes role; the owner established `working-agent/` mid-build (record 373).
10. **Shipped template** — recommendation is a stripped skeleton (line grammar, placeholder content), keeping the full transcript local as reference. The structural value is the grammar; the content is an entire private conversation. Unruled.

---

## Delta capture (added after the freeze)

`trans-fairy/` is frozen. Its deltas are captured into a flat sister directory, `day-one/delta-trans-fairy/`, holding no git, no subdirectories, and no copied structures.

Each capture prefixes every changed file with a three-digit number, so the same filename recurs across captures and sorts by when it was taken. **The filesystem is the counter** — the next capture reads the highest existing prefix and increments, so an overwrite cannot happen even if the operator or agent misremembers.

Untracked *directories* are structures, not deltas, and are excluded. Every capture records the baseline commit, the `git status` and `diff --stat` at capture time, and a sha256 of each original against its copy.

## Re-home: no longer hypothetical

`install --rehome-to` was designed speculatively. It now has a concrete first case: this session's transcript needs to move to a project dir keyed to `~/some-notes/day-one`.

Two constraints confirmed against the live system:

- **A session's cwd, project routing, and transcript location are fixed when the session starts.** A running session cannot be relocated. Re-homing produces a *new* session inheriting the old transcript, never the old session moved.

**The finding this constraint was stated with, recovered 2026-08-28:** *the agent could not determine which side of the fork it was on from context alone.* **A session cannot locate itself from the inside**, which is why routing has to be fixed externally at start rather than resolved later by asking. Cutting the finding leaves the constraint reading as an arbitrary platform limitation instead of the consequence it is. Record-sourced.
- The original must survive the operation — invariant 7. This is the diamond record's first real test.

## Naming, as used in `day-one/`

The plain / `NN-` / `NNN-` scheme is `NAMING.md`'s; what `day-one/` adds is below.

Copies made with `cp -p` carry the *source's* mtime. The **directory** mtime is when the capture happened. File mtime answers "when was this written"; directory mtime answers "when was this taken."

**The rule that pairs with the convention: corrections go forward into the living copy, and the frozen copy is never edited.** *Mark, don't erase.* **This is explicitly the same shape as the graft rule** — a record is annotated rather than rewritten, and the annotation carries its own date so it reads as history rather than as status. One convention covering documents, captures and grafted rows alike.

**And a note on what an unremarkable name buys**, since two of these directories have deliberately dull ones: it **defends against a glance, not against a search.** `grep -r` finds content regardless of what the directory is called. Obscurity reduces the chance of an accident and does nothing against anyone looking.
