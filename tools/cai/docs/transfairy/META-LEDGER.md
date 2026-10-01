# Meta ledger

Outstanding improvements to the **development flow** — how the work gets done, not what gets built.

Elsewhere: program design in `DESIGN.md`, lessons already learned in `BUILDING-OF-TRANSFAIRY-STEPS.md`, parked takeaways in `undecided-fairy-dust.md`.

When an item resolves it becomes a lesson in the building doc, or it is deleted. Resolution is not separately recorded — the lesson is the record.

Rebuilt 2026-08-24 20:35 MDT.

## Sizing

Meaningful items, no bloat. Roughly ten is comfortable, thirty is a slog, a hundred goes unread. There is no lower bound: a floor on record count rewards volume and nothing else. If the list is short, that is information.

## Importance

**3** blocks other work · **2** costs something real if left · **1** tidy

---

## 1. The agent cannot flag a weakening ask mid-task

*Importance 3.*

The operator's stated test — "if the task is weakened away by context while working on other things, then I know the task was a bad ask" — only reports after the fact. There is no way for the agent to surface *"this ask is degrading as I work it"* while it is still cheap to change course.

This session produced a live example: the meta-log ask degraded across four exchanges and no one said so until the operator spotted it.

## 2. Operator-stated vs agent-derived constraints are indistinguishable

*Importance 3.*

Nothing marks which rules came from the operator and which the agent invented to reconcile them. Derived rules acquire a permanence they never earned, and get defended instead of dropped. See lesson 15e.

Wanted: a convention that makes provenance visible on the rule itself — the same argument that put `origin.kind` on a grafted record.

## 3. No entry point for a new reader

*Importance 3.*

Five documents, no stated reading order, no statement of which is authoritative for what. An agent or person arriving cold has to reconstruct the structure by reading all of it.

**Resolved 2026-08-28 — `README.md` now exists** (a routing/entry-point doc), so this item is closed. It is left in place rather than deleted because item 4 below references items by position; the resolution is recorded here per the ledger's rule against silent renumbering. See the housekeeping ledger, item A3.

## 4. Cross-doc references are positional and break silently

*Importance 3.*

Lessons cite each other by number. Any renumbering invalidates every reference past that point while the prose still reads as correct, and nothing would catch it.

**Partly resolved 2026-08-31.** The 11-into-10 fold that prompted this item is **abandoned**: sixteen numeric cross-references exist and eleven are past that point. The duplication it was meant to remove was collapsed by pointing instead — lesson 10 defers to lesson 11 — so the numbers stay stable identifiers, matching the owner's ruling on `DESIGN.md`'s open items. **The general hazard remains open**: nothing enforces that a future renumber re-anchors its references.

## 5. `PIPELINE.md` does not exist — RESOLVED 2026-08-28

*Importance 2.* **`PIPELINE.md` was re-authored from the recovered Flow B draft and now exists; this item is closed. Left in place, not deleted, because later items reference by position (the ledger's rule against silent renumbering).**

The `--agent` flow is specified to read its stage text from `PIPELINE.md` so the agent-facing procedure has one source rather than a copy that rots. The source is missing, so there is currently nothing to rot and nothing to follow.

**Updated 2026-08-28 — a draft exists.** Recovery from a session transcript turned up the Flow B stage content at `EXTRACT:879-893`, and it is the only version of this file that has ever existed. **It is marked as residue and is load-bearing at once**, because the procedural voice that makes it read as residue is the same thing that makes it stage text.

**So the item is no longer *write it from nothing*. It is *re-author what exists in program-behaviour voice*** — say what the program does at each stage rather than what the reader must do. The design survives that rewrite and the residue does not. Cutting the passage instead destroys the only copy and strips two `DESIGN.md` invariants of their stated rationale.

## 6. Mode and gate exit-conditions are behavioral, not structural

*Importance 2.*

A long-lived mode is supposed to re-announce its exit condition every turn. This held only because it was recent. Nothing enforces it in a session where it is not — and by lesson 12, a rule that depends on remembering is the weak form.

## 7. Foreclosure acceptance has no defined step

*Importance 2.*

Five foreclosures are marked "pending acceptance." Nothing defines what acceptance is, who performs it, or what changes when it happens. The append prohibition binds them, so they sit frozen in a state with no exit.

## 8. Retrospectives run only at phase boundaries

*Importance 2.*

The design phase produced roughly sixteen lessons and one retrospective, held at the end. Several lessons were nearly lost between their occurrence and the retrospective, and were recovered only because the operator remembered them.

Wanted: a cheap mid-phase capture that does not require the full ceremony.

## 9. Document conventions arrived late and are unevenly applied

*Importance 1.*

Headings-not-attention-bold and datetime stamping were adopted after most text was written. `undecided-fairy-dust.md` follows both; the others follow neither.

## 10. Meta and program docs share a directory

*Importance 1.*

`trans-fairy/` holds both the toolset's design and the process retrospective on building it, distinguished by filename alone. Blocked on item 4.
