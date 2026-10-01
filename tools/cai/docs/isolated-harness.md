# The isolated harness — running a transcript without touching the real projects directory

**A method, established 2026-08-31.** Everything trans-fairy tests, and everything that resumes a crafted or experimental transcript, can be run against an **alternate root** so the operator's real `~/.claude/projects/` is never written to at all.

**Why it matters here.** The rule that silent injections and experimental transcripts are never installed in a projects directory is a discipline — it depends on the party doing the installing honouring it. An alternate root makes the same guarantee **structural**: the experimental transcript is not in the real tree because the process never had the real tree. That is the preferred shape for every protection in this repository, applied to the test surface.

## The mechanism

`CLAUDE_CONFIG_DIR` relocates the client's whole configuration root, and the projects directory moves with it. Place a transcript at:

    <alt-root>/projects/<slug>/<session-id>.jsonl

where `<slug>` is the target cwd with `/` and `.` replaced by `-` (see `PROBES.md` P5), then resume from that cwd with the variable set:

    CLAUDE_CONFIG_DIR=<alt-root> claude -p --resume <session-id> --fork-session "<probe>"

**Measured behaviour (P9):** the transcript is found and resumed from the alternate tree, `--fork-session` writes the fork **into that tree**, and nothing appears in `~/.claude/projects/` for that cwd.

## What it costs

**Authentication lives in the config directory too.** A relocated root has no credentials, and the run fails at inference with *Not logged in* — after loading and forking successfully. So the isolation is free for everything up to the API call, and the auth question has to be answered deliberately.

**Nothing here recommends copying credential material into an alternate root.** That is an operator decision with its own risks, and this repository's whole history with credential material argues for thinking about it rather than defaulting to it.

## The lighter alternative, when isolation of the *tree* is enough

Most probes in `PROBES.md` did not need an alternate root. They used a **throwaway cwd**: a fresh directory that has never been used as a project, so its slug maps to a projects directory that did not previously exist, and removing that directory afterwards leaves the real tree exactly as it was.

    CWD=$(mktemp -d); PROJ=~/.claude/projects/$(echo "$CWD" | sed 's#[/.]#-#g')

This keeps authentication working, and it is the harness that produced every transcript probe recorded here. **It isolates by name rather than by root** — weaker, since the real tree does gain and lose a directory, and adequate whenever the concern is not writing over something real.

## Keeping a test out of an influenced environment

**A throwaway cwd is also what keeps a run clean of environmental influence, not only clean of the real tree.** A cwd that has never been used carries no `CLAUDE.md` above it that anyone wrote for a project, no `memory/` keyed to it, and no history — so a subject started there is closer to a controlled condition than one started in a working repository, whatever its transcript says.

**This matters for the emulated-root contract.** The rule that a synth-root transcript should not sit in a stability-needing projects directory has a positive counterpart: **there is a place it can go.** A throwaway cwd, or an alternate root, is that place, and naming it is what makes the restriction followable rather than merely prohibitive.

## Discipline for both

**Always `--fork-session`.** A plain resume appends to the crafted transcript in place and destroys the thing under test (P8). A fork leaves the original byte-unchanged.

**Clean up the project directory afterwards**, and check: a resume also creates a `memory/` directory, which is an auto-loading channel and should not be left lying in a tree where a later session might pick it up.

**Ask a question only the crafted content can answer.** A probe that could be answered from general knowledge measures nothing.

**Positive-control every zero.** An absent canary is not a finding until a canary known to appear does appear through the same path.
