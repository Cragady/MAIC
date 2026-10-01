# trans-fairy

**A toolset for moving a conversation out of one body and getting it to take in another** — a claude.ai export becomes a Claude Code session transcript that `claude -r` will resume.

**This is the entry point.** Six documents, roughly 1,900 lines, and until now no stated reading order and no statement of which is authoritative for what. That gap is `META-LEDGER.md` item 3, and this file closes it.

**To run it, not read it:** the quickstart is in the repository [`README.md`](../../README.md) under *Usage — trans-fairy* (`init → split → build → install → resume`, `bin/run-all.sh`, and the `--agent` path). `cai trans-fairy --man-help` is the full flag reference. This file is the map to the design; the sections below say which document to open for what.

## Which document owns what

| File | Owns | Read it when |
| --- | --- | --- |
| `DESIGN.md` | the program's shape — invariants, tree, CLI, graft, truncation, and every open decision | you need to know what the program *is*, or what is still unruled |
| `PIPELINE.md` | what the program does at each stage, in program-behaviour voice | you are implementing a stage, or `--agent` is printing from it |
| `NAMING.md` | how files are named | you are creating a backup, snapshot or artifact |
| `BUILDING-OF-TRANSFAIRY-STEPS.md` | how the design came to be — eighteen lessons, each pointing at the rule it produced | you want to know *why* a rule exists, or you are bootstrapping another project |
| `META-LEDGER.md` | outstanding improvements to the development process, not to the program | you are deciding how the work gets done |
| `undecided-fairy-dust.md` | takeaways parked below the retrospective's cap, each with a deferral and a foreclosure | you are about to propose something that may already have been closed |
| `../PROBES.md` | measured facts about Claude Code that these designs rest on, each with its method and client version | you are about to assume how the client behaves, or a probe result needs re-checking against a new version |
| `../isolated-harness.md` | how to run or probe a transcript without touching the real projects directory | you are building a test arm, or about to resume something experimental |

**The split that matters.** `DESIGN.md` and `PIPELINE.md` are normative. `BUILDING` explains and never restates. **A rule stated normatively in two of them is a defect rather than redundancy** — and the one that drifts is the one nobody re-reads.

## Reading order

**Cold, and you need the shape:** this file, then `DESIGN.md`'s *Invariants*, *Tree* and *Stages*, then `PIPELINE.md` end to end. That is about 250 lines and it is the whole contract.

**Implementing a stage:** `PIPELINE.md` first, then the `DESIGN.md` section for the stage you are on, then `NAMING.md` at the moment you write a file. **Check *Open — needs a decision* before assuming anything is settled** — fourteen items sit there and four of them touch code.

**Deciding something:** `DESIGN.md`'s *Open — needs a decision*, then *Rejected alternatives*, then `undecided-fairy-dust.md`. The last two exist so a closed question does not reopen without something new behind it.

**Arriving to argue with a rule:** `BUILDING` first. Most rules here were learned by something going wrong, and the lesson names what it cost.

## Status — what exists

**The core is implemented, 2026-08-28.** `cai trans-fairy` runs init, split, build, install (create-only), graft, inject (loud/silent), the `--agent` one-stage-per-invocation path, and the help family, on `src/cai/{transfairy,grammar}` with tests (`tests/test_transfairy.py`, `tests/test_grammar.py`). `state --split` now extracts project-dir state into the role dirs; a few inject sub-decisions remain.

**Two referents get called *exists* and they are not the same thing**, which is worth stating because the documents do not:

| | means |
| --- | --- |
| `DESIGN.md:31` — *nothing is built* | there is no `trans-fairy` program. True. |
| `DESIGN.md:240` — `build` *exists* | `reference/transfairy/build-transcript.py` exists — hand-driven code that predates the spec and is kept for its field shapes, not as an implementation |

**The same holds for `split`.** `reference/transfairy/old-heredoc-might-have-info.sh` was recovered and is a **reference exemplar, not an implementation** — it carries both anti-patterns the design forbids, `cp -n` and a split that consumes its own source. Stage `split` is written against `PIPELINE.md`, never against that file.

**What is implemented and tested is `src/cai/grammar/`** — the shared record grammar, four checks including a negative control. It exists because the grammar was previously known in two places and drifted into a defect.

## The 2026-08-28 recovery, in one page

**What prompted it.** `DESIGN.md` described `--agent` plus `--yes` as unspecified, and the operator remembered ruling on it. **A decision she held against a document that called it open** is the mismatch that started the whole thing.

**What was recovered.** A build-session transcript, 254 verbatim records across 439 KB, swept three times by a peer session. The material is catalogued in `DESIGN.md`'s *Recovered material — index* rather than copied in — the recovery is larger than the design it belongs to, and pasting it would bury the thing it was for.

**What came back that was load-bearing and simply absent:**

- **The founding inversion for `--agent`.** Stdin prompting was rejected outright: an agent drives a program by invoking it and reading what comes back. The flag reached the document; the reasoning behind it did not.
- **`Exit 0 at each boundary`.** The line the entire agentic flow rests on. Without it a harness reads a stage boundary as a failure and retries or aborts a run that is working.
- **`PIPELINE.md`'s only draft.** The file had never existed, and `--agent` is specified to read its stage text from it.
- **The scoped-token grammar**, which had been removed as collateral by an unrelated edit while the document went on claiming it was retained.
- **The snapshot's actual purpose** — a record of as-built before any live turn, so imported history can be separated from real turns months later. Not a crash-restore, which is what the name suggests.

**What it cost, and the lesson that came out of it.** Two wrong statements were written into `DESIGN.md` from the record and had to be taken back out. Both are `BUILDING` lesson 18: **a record is not the artifact.** A transcript captures what was said and not what was done immediately afterwards; a peer's report describes what it read. **Neither is the file.**

**What is closed.** The extract corpus. Three sweeps, no new material on the third.

**Read the extract's residue markers with caution.** They are miscalibrated with a direction — over-marked on the CLI and `--agent` text, under-marked elsewhere — so a reader trusting them alone over-discounts exactly the material the recovery was for.

## Still missing

`split-conversation.py`, written as an inline heredoc and never saved. The `--agent` plus `--yes` **error text and exit code** — the exclusion itself is decided. The queued tiered-protection model, which would supersede invariants 7 and 8.

## What this file is not

**It is not normative and it restates no rule.** Where it looks like it does, the owning document wins.

**Its job, ruled 2026-08-28: this file is the map.** `PIPELINE.md` carries the in-depth flow, `DESIGN.md` holds the rules, `BUILDING-OF-TRANSFAIRY-STEPS.md` holds the history. This one exists so a reader gets the eagle-eye on which of those to open, and nothing here competes with them for authority.
