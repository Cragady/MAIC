# PIPELINE — what the tool does at each stage

**This is the source `--agent` reads its stage text from.** `DESIGN.md` specifies that the agent-facing procedure has one source rather than a copy that rots; this is that source.

**Provenance.** Re-authored 2026-08-28 from a draft recovered out of a session transcript at `EXTRACT:879-893`, which was the only version that had ever existed. The draft was written in second person — *do not execute*, *before touching anything*, *never combine* — and that voice is why it read as procedural residue rather than as design. **Nothing was cut. It is restated as what the program does.**

**Two things changed in the re-authoring.** The draft was written against one job's paths (`tmp/bak/`, `conversations-000.zip`); this is written against the tree and sub-commands `DESIGN.md` now specifies. And the draft's approval gate was an exact string the operator sent; that mechanism is now the `--agent` forced-read flag described in `../CONVENTIONS.md`.

## What `--agent` mode is

**One stage per invocation.** The tool performs a single stage, reports, and exits. It does not continue to the next stage, and there is nothing to interrupt.

**Exit 0 at each boundary.** A completed stage is a success. **A boundary is not a failure**, and a harness that treats a non-zero exit as an error must not see one here — otherwise it retries or aborts a run that is working correctly. This is the line the entire agentic flow rests on.

**The exact next command is printed.** Every stage ends by printing the invocation that runs the following one, with its flags filled in. The agent's next action is a command it was handed, not one it composed.

**No prompt is ever presented to an agent.** Where the interactive path would ask, the agent path stops and prints. **An agent never answers a `y/N`**, because by the time it could, the process has exited.

## Invariants across every stage

**The flow is create-only, so it carries no backup stage.** Nothing here overwrites — install writes a new session beside the existing ones (see below) — so there is no destructive write for a backup to guard. Backup is a beside-the-flow, developer's-call step; see *Backup and snapshot, beside the flow* in `DESIGN.md`.

**Any copy the tool does make is fail-closed.** Where a destination already exists, the tool compares it: identical succeeds, different refuses and changes nothing. This governs `install` and the beside-the-flow snapshot alike.

**`cp -n` is never used.** It fails open: it declines silently when the destination exists, returns success, and lets a chained write proceed as though the backup happened.

**Every copy is verified with `cmp` immediately after.** A partial write that would otherwise pass as success does not survive the comparison.

**A build mints a new `sessionId` from the pool.** An install therefore adds a transcript beside the existing ones and cannot replace one.

**Ordering authority is the `parentUuid` chain.** Not timestamps, not file metadata.

**`cp -p` is the install verb, and `mv` is never used.** `mv` consumes the source and swaps the inode out from under any process already reading it.

**Nothing is written to a Claude projects directory except by `install`.**

## Stages

> **Resolved 2026-08-28 — the numbers no longer collide.** `backup` and `snapshot` were both popped out of the numbered flow: a create-only tool needs no in-flow backup, and snapshot is a beside-the-flow capability. With both gone the two documents agree — `00 init · 01 split · 02 build · 03 install · 04 resume` — and stages may be cited by number again.

### 00 — `init`

**Reports every path the run will write to**, and for each, whether it currently exists, its size and its mtime.

Scaffolds the working tree. Writes nothing outside it.

**Prints the inventory and exits.** The list is the point of the stage: it exists so the set of write targets is known before anything is written.

### 01 — `split`

**Extracts the target conversation out of the export.**

**Verifies three things before reporting success:** the target uuid is present in the extract, absent from the remainder, and the remainder count is exactly one fewer than the original.

### 02 — `build`

**Runs the transcript builder. Writes to the staging directory and never to a projects directory.**

**Verifies the built transcript before reporting success:**

- the parent chain is unbroken across every message line.
- no two consecutive turns carry the same role.
- every uuid is unique.
- the first line is a `user` record.
- `cwd` is uniform across all records.
- **the emitted key set per record type matches the reference transcript**, with nothing missing and nothing extra.

**The sixth check is the one that catches a file Claude Code will refuse to load despite it being structurally valid.** The original validation ran it against the model transcript and reported *missing vs model* and *extra* per record type. It is the only one of the six that was ever dropped, and it is the only one testing conformance rather than internal consistency.

**Reports the folded and dropped block counts.** Thinking blocks dropped, tool blocks folded, attachments inlined, off-branch messages discarded — **these are printed, not passed over silently.** A transformation that does not disclose what it changed is the failure this stage exists to avoid.

### 03 — `install`

**Copies the staged transcript into the projects directory with `cp -p`**, and compares the result with `cmp`.

**Install only ever creates.** It writes a new session beside the existing ones and never overwrites a transcript in a projects directory (invariant 8), so there is no destructive write and nothing to back up first.

### 04 — `resume`

**The tool does not run this stage.** It prints the session id and the exact `claude -r` command and exits.

**`claude -r` is a human step and is never automated.**

**Which resume shape depends on whether a pristine copy exists elsewhere.** A plain `claude -r` appends to this transcript in place and it becomes the live session — correct when the source still exists, as it does after a truncation (the original is untouched) or a graft (the base is never modified). `--fork-session` writes a *separate* transcript and leaves this one byte-unchanged — correct when this is the only copy of the as-built. **Forking when a pristine copy already exists just makes a third file.**

**If you want the as-built preserved, snapshot it before this — the tool will not do it for you.** `claude -r` appends to the transcript **in place**, so the moment it resumes the pristine build stops existing, and the staged copy lives in disposable temp. A beside-the-flow snapshot to `bak/`, taken right before resume, is the only way to keep an as-built to diff a live transcript against later. **It is the developer's call; skip it and the as-built is unrecoverable** — stated here so the skip is informed.

**The resume must be launched from the target cwd.** The project directory name is the cwd with `/` replaced by `-`, so resuming from a different directory maps to a different project directory and the picker does not show the transcript at all. Printing the command with the directory in it is what prevents this.

## On any verification failure

**The tool stops, reports what failed, and changes nothing further.**

**It does not attempt repair.** A repair is a write that nobody inspected, performed at the moment the run has already demonstrated it does not understand the state it is in.

## What is still missing

**`split-conversation.py`.** The draft flagged it as *a heredoc run inline, never saved*. It has since been recovered from the build session's transcript and is held at `reference/transfairy/old-heredoc-might-have-info.sh` as a reference exemplar — **not as an implementation.** It carries both anti-patterns this document forbids: `cp -n`, and a split that consumes its own source. The split stage is written against this specification, not against that file.

**The `--agent` plus `--yes` error path**, unspecified in `DESIGN.md` and unspecified here.

**The queued tiered-protection model**, which would supersede invariants 7 and 8 in `DESIGN.md` and would change what this document says about write targets.
