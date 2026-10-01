# Housekeeping pass — transfairy, 2026-08-28

**A decision log and a conflicts ledger for one cleanup pass**, kept because the owner asked that progress and the reasoning behind each call be written down rather than held in a session. **Scope: `docs/transfairy/` only.** Expansion to cai-tools top level is a separate pass, on the owner's go.

**Read at `cai-tools@6974af2..HEAD`.** A finding is a claim about a moment; this names the state it was taken at.

## How a call was made

**Direction, where two designs conflict, by the corpus's own values** — fewer sources of truth (lesson 9), unsafe-thing-unreachable (lesson 12), fail-closed, simpler predicate. Not by which reads nicer.

**Intent, where direction is a tie**, by this precedence: owner's live ruling > later conversation > later transcript > earlier transcript > pre-existing cai default. **Artifact state governs fact even when intent points elsewhere.**

**Ownership of a promoted item:** a foundational item deliberately promoted to cai top level is owned there and transfairy points to it — it is not a pre-existing default that lost an argument. Promotion intent is the distinguisher.

**Three buckets.** GREEN — apply now, reversible, no ruling needed. AMBER — needs the owner's ruling. GREY — considered and genuinely hard, or style with no governing rule; left, not forced.

**Deliberate duplication is exempt.** `CONVENTIONS.md`'s shared-surface table stays duplicated by standing owner instruction, retired by git rather than by this pass.

---

## GREEN — APPLIED at `4867575`

*(G5 folded into G1's flags edit.)*

**G1 — Global flags block contradicts the Tree section, and one default is the failure the Tree names.** `DESIGN.md` *Global flags* defaults `--pool` under `--work-root`, i.e. into temp; the Tree section puts the pool in `<datadir>` and argues by name against a pooled temp (stray-second-transcript). Also `--work-root` default (parent of `bin/` vs `<tempdir>`), `--stage-dir`, and a missing `--data-root`. **Direction: the Tree section — fail-closed, avoids a named failure. Fix the flag table to match it.**

**G2 — Three references treat `README.md` as absent/unwritten; it exists (`9da3321`).** `META-LEDGER.md:56` "does not exist"; `DESIGN.md:765` "designed, never written". **Both stale. Correct to reflect existence.** (The fourth reference, DESIGN:699, is a role conflict — see A1.)

**G3 — The `NN-`/`NNN-`/plain naming scheme is stated in three places.** Owner: `NAMING.md:37-39`. Duplicated at `DESIGN.md:874-875` (open item 4) and `DESIGN.md:941-943` (day-one section). **Lesson 9. `NAMING.md` owns; DESIGN's two collapse to pointers.**

**G4 — Foreclosure governance is stated in full twice.** `BUILDING-OF-TRANSFAIRY-STEPS.md` 15a (the lesson) and `undecided-fairy-dust.md` "Rules governing foreclosures". **15a owns; the parked-items file points to it.**

**G5 — `--model` and `--effort` appear in the flags block, explained nowhere.** **Add a one-line gloss each.** Low risk; no decision attached to them anywhere.

## AMBER — needs a ruling

**A1 — RESOLVED 2026-08-28, applied at `4ed5d21`. Is `README.md` normative?** `DESIGN.md:699` names *PIPELINE.md and README.md* as the normative pair, single source of truth. The `README.md` that now exists is a routing/entry-point doc that explicitly restates no rule. **They disagree.** *Recommendation:* DESIGN:699 names PIPELINE only; README stays the map. **Ruling yours — it decides what README is.**

**A2 — RESOLVED 2026-08-28 (`320215a`): keep pointers, no renumber. Open-item numbering runs 0,1,2,3,11,12,4,5,6,7,8,9,10.** Items 11–12 sit between 3 and 4. Reordering renumbers, and items are cited by number elsewhere — the positional-ref fragility META-LEDGER item 4 names. *Recommendation:* treat the numbers as stable identifiers and leave the order, or accept a renumber and fix refs in one sweep. **Not a silent apply.**

**A3 — RESOLVED 2026-08-28 (`320215a`): mark-in-place. Two META-LEDGER items are resolved but the file's own rule forbids update-notes.** Item 5 (PIPELINE exists now) and item 3 (entry point exists now). The rule is *becomes a lesson or is deleted*. Deleting renumbers, and item 10 says "Blocked on item 4" — a positional reference that a delete would silently repoint. **This is two rules colliding (resolve-by-delete vs stable cross-ref); by lesson 15e that is a signal, not a thing to engineer around.** *Recommendation:* mark resolved in place without renumbering. **Ruling yours.**

**A4 — RESOLVED 2026-08-28 (`22592bc`): truncation folds into `split` (implicit; `--truncate` splittable later, none foreseen); graft/inject ride `install`. DIRECTION SET, re-read done. `graft` and `truncate` may not need their own verbs.** Owner's read: `install` buckets graft into it, and `split` takes care of truncate. There was discourse on this worth a re-read of items elsewhere before ruling. Deferred by the owner until the rest of the pending items are done. **Update 2026-08-28: `install` now carries `--graft-onto` and `--inject` (`1a48139`), which is the bucket for graft; the re-read to confirm truncate⊂split is still owed.** Original finding: `graft` and `truncate` are designed with no CLI sub-command. Four graft sections and tail-truncation exist; `--graft-onto` is load-bearing; neither has a verb in the sub-command list or stage table. **A gap, not a conflict.** Adding CLI surface is design, not housekeeping. *Recommendation:* defer, or add stub rows flagged unbuilt. **Yours.**

## GREY — left, not forced

**Y1 — RESOLVED 2026-08-28 (`93b867d`): backup + snapshot popped out of the flow, both docs agree on 00–04. Stage numbering collided between `PIPELINE.md` and `DESIGN.md`** (00–05 name different stages). Already *considered and not ruled* by the owner, pending evidence. Left. **Her stated lean, recovered 2026-08-31 and previously unrecorded: number by transcript occurrence** — *"my instinct is to go with transcript occurrence, but it's hard to say… we can just say we need more evidence to consider."* **Moot as long as Y1 stays resolved**, since the two documents now agree on 00–04 and there is nothing to renumber. Recorded so a re-opening starts from her lean rather than from nothing.

**Y2 — RESOLVED 2026-08-28 (`9cb8f49`): no-wrap ruling; all nine hard-wrapped files unwrapped, word sequences verified identical. Wrapping was split.** `DESIGN`/`PIPELINE` are unwrapped; `BUILDING`/`META-LEDGER`/`NAMING`/`undecided-fairy-dust` wrap near 79 columns. **No rule in cai-tools governs this** (no `CLAUDE.md` here). Style pick, low priority, owner's call.

## CLEAN — checked, no action

**C1 — The record grammar is not duplicated into `DESIGN.md`.** DESIGN's "line grammar" is emission order; `src/cai/grammar/records.py` owns record *shape*. Different things. The promoted grammar surface is clean and `README.md` already points to it.

---

# Sweep 2 — code radius (`src/`, `reference/`, `tests/`), 2026-08-28

**Read directly this pass**, not record-sourced: `reference/transfairy/old-heredoc-might-have-info.sh`, `src/cai/grammar/records.py` (full), `src/cai/dispatch.py` (full), `src/cai/{transfairy,redact}/cli.py`, `tests/test_grammar.py`. **The code surface is clean** — two trivial stubs, one well-factored shared module, one entry point, one test. No duplicated logic and no conflict between files.

## Verified — closes prior caveats

**V1 — the `old-heredoc` anti-patterns are confirmed by a direct read**, not from the docs' description. Block B opens `cp -n conversations.json …` and the inlined python rewrites `conversations.json` in place, consuming the source. Both invariants (2 and 10) are visible in the one block, as the header claims. **The record-sourced flag on this is now closed.**

**V2 — `records.py` field sets match what `build-transcript.py` emits, exactly.** `USER_FIELDS`, `ASSISTANT_FIELDS`, `USAGE_FIELDS` line up with the builder's emitted keys. **C1 upgrades from confident to verified: the record grammar is not duplicated into DESIGN, and the two code homes agree.**

**V3 — `tests/test_grammar.py` passes 4/4, exit 0**, negative control included. The one implemented component is green.

## GREEN — APPLIED at `8ebba11` (sweep)

**G6 — `DESIGN.md` never points at `src/cai/grammar/records.py`.** The emission section describes field behaviour; the machine-checkable owner of the field sets is the module, which is a promoted foundational item. **Per the owner's ruling, transfairy points to cai top level rather than re-describing.** Add one pointer from the emission section to the module.

**G7 — capture the import rule before the builder is written.** The field-set knowledge lives in three homes: `records.py` (owner), `build-transcript.py` (frozen pre-extraction exemplar, agrees today), and DESIGN prose (explanation). They agree now; they will drift if the real `build` re-hardcodes instead of importing `records.py`. **Add a one-line implementation note to `IMPROVEMENTS.md` under trans-fairy.** Additive, no doc conflict.

## Scope note

**`diction/` was not swept.** It is a sister container with its own tracked top-item (`IMPROVEMENTS.md` item 1 — "exists in two places on disk"), which is a deployment issue rather than a transfairy/cai-core housekeeping one. Out of this pass unless the owner wants it pulled in.

---

# Sweep 3 — redact + cai top level, 2026-08-28

**Read this pass:** `docs/redact/{README,TAKEAWAY,SIDE-TAKEAWAYS,AUDIT-FINDINGS}.md`, `reference/redact/{redact-transcript.py,audit-terms.sh}`, top-level `README.md`, `docs/CONVENTIONS.md`. **redact's code surface is clean** — the README usage block and the argparse flags match exactly, and the "Preserve structural identifiers" list equals the script's `STRUCTURAL_KEYS`. Top-level `README.md` is current (one `## State`, no staleness).

## GREEN — APPLIED at `4ed5d21` (sweep 3)

**G8 — the two-regimes finding is in two places.** Stated in full in `transfairy/DESIGN.md`'s redaction section *and* in `docs/redact/README.md` (marked "Transferred from transfairy/DESIGN.md"). By the owner's ruling that **redaction is redact's, not trans-fairy's**, redact owns the finding. **DESIGN keeps only the conclusion that bears on its own decision — truncation is provable, redaction is not, so trans-fairy uses truncation — and points at `../redact/` for the rest.** The original was left in place when the finding was transferred: the promotion-leaves-a-twin pattern.

**G9 — record the redact import rule (mirror of G7).** `reference/redact/redact-transcript.py` carries its own `PAYLOAD_KEYS` and shape-recursion; `src/cai/grammar/records.py` exists precisely to own content-shape traversal (`IMPROVEMENTS.md:53`). **Add a note under redact in `IMPROVEMENTS.md`: the real redact imports `records.py` rather than re-declaring shape handling, and `records.py` may need to expose the payload/structural key sets that currently live only in the reference script.**

**G10 — diction's top item is resolved by an owner fact.** `IMPROVEMENTS.md` item 1 says diction exists in two places and will drift. **Owner, 2026-08-28: the cai copy is the latest, kept its git history on the move, and is canonical.** Update item 1 to resolved — cai is authoritative; the external venv/PATH copy is the stale one. **The repoint/delete is a disk action, the owner's to run, not an in-repo edit.**

## AMBER — needs a ruling

**A5 — RESOLVED 2026-08-28 (`add n/a` ruling): `n/a` value added, redact column filled (5 applied, rest n/a as out-of-scope for a single-purpose script). `CONVENTIONS.md`'s redact column was fillable, but the table had no value for "does not apply."** Having read redact, several rules are legitimately *out of scope* for a single-purpose script (help family, `--agent`, working-area-vs-injection-target), not *not-yet-applied*. Marking them `not applied` misreads "shouldn't" as "hasn't." **Needs a vocabulary call: add an `n/a` value, or leave those cells.** *Unambiguous now regardless:* backup-copies-never-consumes = **applied**, no-multi-lettered-short-forms = **applied**, never-prompts = **applied**.

## GREY — left

**Y3 — RESOLVED 2026-08-28: redact stays a sibling tool in cai (its home; it was saved into SOPIA from another birthing place, and the owner wanted it in cai), not folded into transfairy — their contracts are opposite (create-only vs destructive-in-place). `cai redact` can be invoked on a live transfairy job. The residual content overlap: `docs/redact/SIDE-TAKEAWAYS.md` §2 (a verification that cannot fail correctly) overlaps the SOPIA verification-scope guide.** Cross-repo, and the framing differs (redaction-incident-specific vs general). Not forced — cross-repo dedup is a larger question than this pass.

**diction code not swept.** The owner fact resolves its tracked top item; a line-by-line code sweep is low-value for a working, un-re-authored tool. Deferred unless asked.

**inject's loud/silent modes and the fidelity test** are recorded in `DESIGN.md` and drafted at `SOPIA/docs/tests/inject-fidelity/PRE-REGISTRATION.md`.


---

# Sweep 4 — empirical, and design rulings, 2026-08-28

**Items 11 and 12 run (`21eb100`).** Both were "one command, never run"; they were not one command — each needs Claude Code to load a crafted transcript — but they were run against claude 2.1.236 via forked headless resume, with a positive control (a canary in a user record echoed on resume).

- **Item 11 — RESOLVED.** A canary only in a `system`/`isMeta` record was not seen on resume. **Meta records do not reach the model's context.** This grounds the two-regime graft/inject split empirically.
- **Item 12 — RESOLVED.** `origin.kind: "graft"` and an unknown `system.subtype` both load and resume cleanly. Seam records are safe.

**Design rulings folded in (`21eb100`, `cd3dfa2`):**
- **Injections may live in the projects dir** behind heavy warnings and a `cai-transfairy-injections-mapping.md` sidecar ledger (loose, not-guaranteed). Testing/emulated still may not.
- **`inject` direction:** appends by default, one flag `--prepend`; announcement in the meta record (clean transcript logging).
- **`--fork-session` is the clean solution to backup bloat** — forking on resume leaves the as-built untouched, so the pre-resume snapshot is belt-and-braces, not required.
- **graft `prepend`/`append`** confirmed present as the graft-direction axis (landing approval, auto-detected; `--direction` on overlap).
