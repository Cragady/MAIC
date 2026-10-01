# Building of trans-fairy — process notes

This document is about **the flow of creating the program**, not the flow of the program. Normative rules live in `DESIGN.md` and `PIPELINE.md`; this file records why they exist, what the wrong version looked like first, and what it cost to find out. It is intended to generalize — to bootstrap the next project without repeating these growing pains.

Nothing here restates an operational rule. It references them.

---

## 1. A backup folded into the same command as the write is not a backup step

The extraction step ran as:

    cp -n conversations.json conversations.json.bak && python3 ...rewrite...

Two failures in one line:

- **Not separable.** There was no moment where anyone could inspect the backup before the destructive write happened.
- **`cp -n` fails open.** It declines silently when the destination exists, returns success, and `&&` carries straight on into overwriting the original — leaving the operator believing they held a fresh backup of something just destroyed.

The file survived. The shape did not deserve to.

→ `DESIGN.md` invariants 1 and 2.

## 2. Build and install were the same action, and nobody noticed until a step was missing

The first builder wrote straight into `~/.claude/projects/…`. That conflates producing an artifact with installing it, and the tell was that a proposed recovery plan — "restore from the backup, fix the builder, re-run" — was incoherent: the re-run clobbers whatever was just restored.

The operator caught it by asking a shape question, not a bug question: *"I'm not seeing where you would replace the working transcript file."*

A missing step is easier to see in the *plan* than in the *code*.

→ `split`, `build` and `install` exist as separate stages because of this.

## 3. Two numbers that mean different things must not be collapsed into one

`-000` is the source export chunk (provenance: which export). `_01` is the pipeline step (position: where in the chain). Collapsing to a single counter loses whichever half gets asked about later.

Arrived at only after two wrong attempts — `.01-extracted.bak`, then `-000-extracted.bak`.

→ `NAMING.md`.

## 4. `mv` is not the safe verb; it depends entirely on who is reading

| | inode at dest | atomic | src survives |
|---|---|---|---|
| `cat src > dest` | same (truncate in place) | no | yes |
| `cp -p src dest` | same (truncate in place) | no | yes |
| `mv src dest` | **new** (rename) | yes | **no** |

`mv` was disqualified here not for atomicity but because it **consumes the staged file** — the repeatable install source the pipeline is built around.

The non-obvious inverse: if a process holds the file open, `mv` is the *dangerous* one. `rename(2)` swaps a new inode into the name while the running process still writes to the orphaned old one, and its work silently vanishes. `cat >` and `cp` truncate in place, same inode, so a live reader stays coherent.

## 5. `cp -r` into a destination that does not exist yet

    cp -r ../previous-agent ../trans-fairy/tmp

If `tmp/` does not exist, this does not create `tmp/previous-agent/`. It creates `tmp/` **as** the copy. `-r` alone also drops mode — an 0600 transcript lands 0664 under a 002 umask.

This is not a defect in the command; it is a missing **order**. Scaffolding is step 00 for a reason.

## 6. Memory is exact-keyed; CLAUDE.md is hierarchical

Agent memory lives at `~/.claude/projects/<cwd with / -> ->/memory/` and loads for that cwd and no other. `CLAUDE.md` is discovered by walking *up* from cwd, so one file at a project root covers every subdirectory beneath it.

Consequence found the hard way: a convention saved while working in `tmp/` is invisible to a session started one directory up — including the resumed session the whole job existed to produce.

## 7. The record of how a thing was built lives somewhere disposable

This conversation is the source material for this document, and its transcript sits in a project dir keyed to `tmp` — the directory everyone agreed not to trust. Same shape as the job itself: a conversation worth keeping, stored somewhere it was not meant to survive.

Which is what the tool is for.

## 8. Verify claims that could be lies, not claims that are convenient

Repointing the builder's output could have silently changed the artifact. The check was not "it looks fine" but `cmp` against the previously installed file, plus a `grep` for path leakage into structural fields — which found three hits, all inside message text, none structural.

A verification that cannot fail is not a verification.

## 9. One fact, one home — the trap hit four times in one session

A fact gets a second home, and the second home can drift from the first. Four instances, same shape:

- `PROJ` maintained by hand as a twin of `CWD` — should be *derived* from it by the `/` -> `-` rule.
- `scaffold.sh` and `--auto-dirs` both defining the directory tree, in two languages.
- Directory names (`previous-agent/`, `next-agent/`) declaring graft direction when timestamps already determine it — two sources of truth that can contradict, requiring conflict resolution that would not otherwise exist.
- Ranking a default by in-file timestamp while the prompt says "last modified" — the label and the mechanism measuring different things.

**The fix is the same every time: compute it once, and state it in words at the point of use.** Not a field the reader must translate — a sentence. `direction: after` makes someone do the work; *"this will append to `8560773c-…`"* does not.

Corollary: structure-as-documentation is free only when it documents something not otherwise computed. Role (`previous-agent/` vs `working-agent/`) qualifies. Direction does not.

## 10. The working method, which is itself reusable

The session that produced this tool used four devices worth carrying forward.

**An explicit release phrase.** No step fires without the operator typing a literal agreed string. Read-only inspection stays permitted throughout. This decouples *deliberating* from *executing*, so design can move fast without any risk attaching to speed. **Lesson 11 owns this one** — the literal phrase, and what it caught.

**"Short-reply mode"** (first tried as "Ok mode"). The operator declares a window in which the agent replies minimally and records silently to the design docs. Lets decisions be fired in rapid succession without the operator maintaining a mental stack, and without each one spawning a conversation that pulls attention off the list. The operator ends the window explicitly.

The rename fixed a real bug. A strict `Ok`-only rule suppressed replies that carried information the operator needed — a one-line correction, or a note that something had been logged rather than resolved. The working rule became: **reply `Ok` by default, expand only when a short reply carries real information.** Silence is the default, not the requirement.

**Modes close on a literal string.** Inference may *detect* an intent to close, but never performs the close — it answers with the exact closing phrase instead. Same shape as the release phrase in lesson 11: suggestion is inferred, the act is literal. A mode that can end by inference can end by accident.

**A long-lived mode re-announces its own exit condition every turn.** The literal phrase is appended to every reply inside the mode, not only when closure seems intended. One short line, doing three jobs:

- **context ping** — the phrase stays live rather than fading forty turns after it was declared
- **reminder** — the operator never has to recall the exact string
- **visibility** — the mode's existence stays in front of both parties, so neither forgets they are inside one

This applies to gates as well as modes. A release phrase stated only occasionally drifts the same way; it belongs in every reply the gate covers.

**Intermittent saves to the design docs.** State lives in files, not in anyone's head or in scrollback. The operator asked for this specifically to avoid "spiraling into a workflow and losing track of what I did."

**Rejected alternatives recorded with their reasoning.** A rejected idea whose reasoning was not written down comes back. The reasoning is the artifact, not the verdict.

**On mistakes:** most of the durable rules in `DESIGN.md` were found by error, not by foresight — the `cp -n` fail-open by a near-miss, the build/install conflation by a recovery plan that did not make sense, the label-matches-mechanism principle by a wrong recommendation. A clean-sheet design would have produced none of them. Build the thing, watch where it cuts you, write that down.

## 11. Gate destructive work behind an explicit release phrase

Mid-build the operator required the literal string `Next Step Approved` before any step could fire. Read-only inspection stayed permitted throughout.

It cost nothing and caught a plan that would have restored an undamaged file over a good one.

**Not folded into lesson 10, ruled 2026-08-31.** The fold was wanted (`META-LEDGER` item 4) and is the wrong move: sixteen numeric cross-references exist across these documents and eleven sit past this point, so renumbering would break them all while the prose still read as correct — item 4's own hazard, and against the owner's stable-identifier ruling. **The duplication was resolved by pointing instead**: lesson 10 names the device among the working-method devices and defers here for the literal phrase and the instance.

## 12. Make the unsafe thing unreachable, not forbidden

A rule that must be *checked* depends on the check being present and correct in every path that could violate it. A rule enforced by structure depends on nothing.

Two instances:

- **The diamond record.** Graft reads BASE and emits a new file. There is no code path in which BASE could be written, so the protection does not rest on a guard being right — the operation simply has no write path to it.
- **`--yes` cannot select a graft source.** Rather than warning about a risky default, `--yes` requires explicit `--graft-onto` and fails without it. The dangerous auto-selection does not exist to be guarded.

A related placement rule: when a check *is* necessary, it belongs at the point of effect, not at the entry point. A guard in argument parsing can be reached around by a flag added two years later. A guard in the function that opens a file for writing cannot.

Descending order of strength: **no path at all > check at the point of effect > check at the entry point > documentation.** Spend effort moving up that ladder before spending it on better warnings.

## 13. Authority belongs to explicit structure, not incidental metadata

Ordering in a transcript comes from the `parentUuid` chain, never from timestamps and never from filesystem metadata. This is why backdating a graft is cosmetic, why non-monotonic timestamps render correctly, and why the tool can rewrite dates without any risk of reordering anything.

Identity follows the same rule: `sessionId` lives *inside* the file and the filename must agree with it. The filename alone is not identity — it is a label that can be wrong.

**But metadata is not a lesser signal — it is a different one.** mtime is the wrong source for "what happened most recently in the conversation" and the *right* source for "which file was the developer just handling." Both questions are legitimate; they are not the same question.

So the discipline is: **name the question first, then pick the source that actually measures it** — and make the label say which one you picked. See lesson 9; where that one says do not keep a fact in two homes, this one says which home to choose.

## 14. Synthesized context must announce itself

A graft that reads as seamless is a bug, not a feature.

The purpose is not to convince a resumed agent that manufactured material was its own chronological history. It is to hand the agent the material *and* an honest account of how the material got there. Every marker decision descends from that:

- `origin.kind: "graft"`, never `"human"` — tooling must not fabricate a user turn that never happened.
- The statement lives in **message content**, not only in a meta record, because only message content reaches the model's context. A marker the model cannot see is a marker for tools, not for the reader that matters most.
- Labels are per-record and bracketed at both ends, so no fragment read out of context can be mistaken for organic history.
- Backdating is a *display* concession, never a claim: the true authoring time stays in the marker text and in `graftedAt`, always recoverable from the record itself.

Generalizes past this tool: **any system that manufactures context for an agent owes that agent a statement of what was manufactured.** Continuity that has to be faked to work is continuity that will fail silently.

## 15. The phase-boundary retrospective

Run a retrospective at a phase boundary — here, design ending and build beginning — before the shape of the work changes and the lessons get harder to see.

Procedure:

1. **Enumerate** every meaningful takeaway from the phase that is not already recorded somewhere.
2. **Cap the picks by count.** More than five candidates, pick three. More than ten, pick five.
3. **Record the picks** where they belong, each with its reasoning.
4. **Park the rest** in a named file, each with an explicit note on *why it did not make the cut* — not merely that it didn't.
5. **Operator approves or rejects quickly**, then the boundary is marked in the doc itself.

**The cap is the point.** Without it, everything gets recorded and nothing gets ranked — and a document that records everything teaches nothing. Forcing a ranking is what makes the survivors legible as *the important ones*.

The parked file keeps the cap from destroying information. Nothing is discarded; it is separated into a lower tier with its reasoning attached, so promoting an item later is a move rather than a reconstruction.

A parked item is not a rejected one. One entry parked here was described by the operator as "exceptionally profound" and parked anyway — because it belonged to a different subject than the doc it would have landed in.

### 15a. Foreclosures

A parked entry has three parts, not two: the item, the deferral, and the **foreclosure** — the argument that closes it, so it does not return without something new behind it. A deferral explains this pass; a foreclosure argues against the next one.

A foreclosure is not always measurable and not always effective. It is a standing argument, not a proof.

Governance:

- Once an item carries a foreclosure that has not yet been accepted, that foreclosure can no longer be *added to*. It may only be *replaced*, and only when a better candidate appears.
- A human author has precedence. Any party introducing a modification to a human-authored foreclosure consults the human author first.
- An agent may freely replace an agent-authored foreclosure, judged on the strength of the replacement.

The append prohibition is the load-bearing part. Foreclosures that accumulate qualifications stop closing anything — an argument amended enough times becomes a discussion, and the entry quietly reopens without anyone deciding to reopen it. Replacement forces a real comparison; appending never does.

### 15b. Document convention — headings, not attention-bold

Text reaching for emphasis with bold gets a lower-tier heading instead. Structure survives line-wrapping and grep; bold does not, and bold competes with neighboring bold until none of it reads as emphasis. The foreclosure is a heading too.

*Pending:* the existing docs were written before this convention and still lean on bold. `undecided-fairy-dust.md` follows it; the others need a pass.

### 15c. Datetime stamps

Entries carry a clearly labeled stamp — `YYYY-MM-DD HH:MM TZ` — taken from `date`, never from the model. Same rule as UUIDs from `uuidgen`: measured, not reasoned.

Applied forward only. Backfilling existing entries would mean inferring times that were never recorded, and an inferred stamp that looks identical to a measured one is worse than an absent one — the same objection that keeps true authoring time in graft markers rather than letting a backdate stand as a claim.

### 15d. A backlog for the process, separate from the lessons

`META-LEDGER.md` holds outstanding improvements to the *development process*. This document holds what construction taught. A backlog records what was deferred; mixing the two costs this document the property that makes it worth reading.

When a backlog item resolves, it either becomes a lesson here or it is deleted. It does not get a second record of its own resolution.

### 15e. Do not engineer around colliding constraints

Recorded because it happened, twice, in the space of an hour.

Given two constraints that could not both hold, the agent built structure to satisfy both rather than asking which one was load-bearing. A record floor ("no fewer than five") collided with a no-duplication rule; the agent invented a closures-only log to reconcile them, which produced deferral entries that duplicated open items — the exact thing the no-duplication rule existed to prevent. Each patch required another patch.

Two rules that fight are not a design problem to be solved. They are a signal that one of them is wrong.

The specific trap: the agent treated operator-stated constraints and its own derived ones as the same kind of thing, so the derived ones acquired a permanence they never earned. **Mark which constraints came from the operator and which the agent invented.** Derived rules can then be dropped without ceremony, which is usually the correct fix.

Stated by the operator: *"I don't want you to justify my intent under the rules. I want my intent to justify the rules."*

### 15f. Numeric floors on record-keeping produce fabrication

A target of "at least five entries" was read as a quota and met by recording deferrals as decisions. The floor did not increase how much was learned; it increased how much was written.

Caps work — they force ranking, which is lesson 15's whole mechanism. Floors do the opposite: they reward volume where nothing else does. Express a lower bound as a success criterion evaluated after the fact, never as a rule applied during.

## 16. Do not tack on one more thing at the end of design

*— operator-authored entry*

Once the main design is nearing its end, do not give in to the temptation to suggest one more improvement or one more missing item. **That is what a backlog is for.**

The pressure is real and it is worst precisely at the end, when the shape is clear enough that additions look cheap and obvious. They are neither. An addition made after the design has settled has not been argued against by anything, because the arguing is over.

### 16a. The failure mode this forecloses

*— operator-authored entry; forecloses the idea-bias item parked in `undecided-fairy-dust.md`*

The backlog flow was about to be sidestepped in order to suggest implementing a change **that would have broken the main security feature this application offers.**

This is what makes the rule structural rather than stylistic. The end-of-design addition is not merely untidy — it arrives after the scrutiny has stopped, which is exactly when a change that guts a core protection can pass unexamined. The backlog is not a place to defer nice-to-haves; it is the thing that keeps a late idea inside the review process instead of outside it.

> **Incomplete — needs the specific referent.** Which change, and which protection it would have broken, are not recorded here. The lesson is far more useful with the concrete instance attached.

═══════════════════════════════════════════════════════════════════════════════

# PHASE BOUNDARY — DESIGN ENDS HERE

**2026-08-24.** Everything above is the design phase: the shape of `trans-fairy` argued out before any of it was built, and the lessons that argument produced.

**Everything below is test, development, and refinement.** Entries past this line describe a program that exists, or that failed to, and are written against running code rather than against a proposal.

═══════════════════════════════════════════════════════════════════════════════

## 17. Meta rules need a stated evaluation timeframe

*Operator-identified, 2026-08-24 21:17 MDT. See the provenance note below — this matters here.*

A rule about software is falsified by the next command. `cp -n` either declines silently or it does not; you run it and you know. A rule about *process* is falsified only by its effects across sessions.

So a meta rule with no stated timeframe never comes due. There is no scheduled moment at which anyone asks whether it is working, and it becomes permanent by default rather than by decision.

This is the mechanism behind how meta failures compound. Once a rule is in force and unreviewable, every subsequent problem gets measured *against the rule* rather than the rule being measured against reality. A floor of five survived long enough to generate four patches because nothing was ever going to arrive and ask whether it should still be there.

**Every meta rule states when it is re-evaluated, and against what.** A rule that cannot name its own review condition is a rule nobody has agreed to keep — only one nobody has gotten around to removing.

### Provenance

This finding is the operator's. It is recorded here as theirs.

What the agent contributed independently was a different set of observations about the same failure: that its tool use inverted (verify-then-assert during program design, assert-then-write during meta work, with zero verification commands run across the entire meta stretch); that it was reviewing its own output minutes after producing it rather than reviewing artifacts it had no stake in; that the task verbs shifted from *design* to *record/draft/log* and it stayed in the register the task named; and that it graded meta as low-stakes when the meta layer governs everything downstream.

The agent's first explanation — "meta lacks an external referent" — was itself an unchecked convenient claim, and it now ranks that third behind authorship proximity and task register.

### Not yet a global rule, deliberately

This is a candidate for the operator's user-level `CLAUDE.md` and is being withheld from it on purpose.

The operator is running a cross-instance investigation into whether agents converge on this failure independently. Placing the finding in a globally loaded file would contaminate every instance: they would read it and return it, making independent arrival indistinguishable from retrieval.

Do not promote this without the operator's explicit instruction, and do not treat any agent's agreement with it as confirmation.

═══════════════════════════════════════════════════════════════════════════════

## 18. A record is not the artifact — the trap hit twice in one recovery

Both instances are from the 2026-08-28 transcript recovery, and both put a wrong statement into `DESIGN.md` that had to be taken back out. Same shape, two doors.

**Instance one — a position reversed in an artifact the record never mentions.** A proposal for a hash manifest over the frozen tree was recovered from the transcript and written in. It had been superseded **about seven minutes after it was made** — `git init` already provides content-addressed change detection — but the withdrawal went into a file and was never spoken of again.

So the transcript holds the proposal and not the reversal. **Two independent sweeps both reported it as live**, because both worked from the record, and the record was complete about what was *said* and silent about what was *done next*.

**Instance two — a peer's claim accepted because it arrived already argued.** A peer reported the freeze commit in `DESIGN.md` as wrong, citing three sources. The change was written in without checking it against the artifact, and it was wrong: *frozen as of `18859e2`, corrections go forward* means the frozen copy stays there while the repository advances. The divergence was the mechanism working.

**The check that would have caught it was one command.** It was skipped because the claim came from a trusted channel with reasoning attached, and **a claim that arrives already argued does not feel like it needs checking.**

**What makes this one shape rather than two:** in both, the thing consulted was a *description* of the state rather than the state. A transcript describes what was said; a peer's report describes what it read. **Neither is the file.**

**The rule:** verify against the artifacts a record produced, not only against the record — and apply it hardest where the source is trusted, because that is exactly where it gets skipped.

**What it cost:** the second instance happened about an hour after this session wrote the rule for the first one into a guide.

→ `DESIGN.md`, the recovery index's verification note and the freeze-point header.

## 19. A reason is stripped when it is not needed to comply, and it is needed exactly once — when someone proposes changing the rule

Hypothesised by a peer session, 2026-08-28, from three instances. **Stated as a hypothesis with its own falsification condition, then tested against all ten invariants.** It survived, and the test is the reason it is here rather than in the backlog.

**The mechanism.** *Never use `cp -n`* is arbitrary without its reason — an implementer who does not know it fails open will reasonably conclude their case is different. So the reason travels attached, because the rule does not survive contact with a competent implementer otherwise. *Never overwrite an existing transcript* is fully executable by someone who has never heard the word reproducibility. **The reason is droppable without immediate cost, and what is droppable without immediate cost gets dropped.**

**The cost does not land in execution. It lands in revision** — which is why the loss stays invisible until it is expensive. The live case is the queued tiered-protection model: it proposes that *Current* be strongly challenged for deletion while *Diamond* is never deleted. **Stripped of its reason, invariant 7 looks like caution about losing a file and tiers look like a refinement. With the reason attached, tiers are a proposal to make a control condition conditionally destructible**, which is a different question. The reason is needed at exactly the moment everyone who held the context has gone.

### 19a. The audit, and what it returned

**For each invariant: say what goes wrong if someone proposes relaxing it, and name where that is written. Wherever you cannot answer, the reason was stripped.**

The peer stated the falsification condition itself — *if only the invariants already corrected fail, this is one incident described twice and dressed as a law.* **Two invariants failed that had not been touched**, so the hypothesis does work.

- **Pass, reason attached at the rule:** 2 (`cp -n` fails open), 8 (path-name rules are spoofable), 10 (a consuming move is not a backup), 3.
- **Pass, reason held in `PIPELINE.md` rather than beside the invariant:** 1 (the backup is inspectable between the two invocations), 5 (`mv` swaps the inode out from under a live reader).
- **Marginal:** 4. The consequences of ordering by timestamp are stated in three separate sections and never attached to the invariant.
- **Fail:** 6 and 9.

### 19b. Three mechanisms, not one

**Stripped.** The destination document existed, the rule arrived, the reason was dropped in transit because it was not needed to execute. Invariant 7 and the `--rehome-to` constraint.

**Stranded.** There was no destination when the rule was learned. *A tripwire that can be edited is not a tripwire* went into an incident report because nothing else could hold it, and by the time a capture discipline existed nobody was reading an incident report for design rules. **It later fired as a defect that looked novel** — a sha manifest whose source kept moving. Stranding is the worse of the two: a stripped reason at least leaves the rule behind as a pointer, and a stranded rule leaves nothing at the destination at all.

**Displaced — found by the audit and not in the original hypothesis.** The reason exists in the corpus, correctly stated, attached to a *sibling* rule. Invariant 9's reason is in 15c above, about datetime stamps, naming uuids by analogy. **Findable only by someone who already knew it was there**, which is the reader who did not need it.

The question that separates the first two: **did the document that should own this rule exist when the rule was learned?** The third is separated by a different one: **is the reason absent, or merely somewhere no one would look?**

### 19c. The invariant nobody can defend

**Updated 2026-08-31: the origin was found, and it changes the shape of the lesson rather than retiring it.** Invariant 6 — `claude -r` is a human step, never automated — traces to one clause the operator wrote in passing during the build: *"On the `claude -r`, I'll carry that step out."* It became an invariant in the same exchange and was never argued for again.

**So the invariant is known-origin and still undefended.** That is a *third* state, distinct from both the stripped reason and the stranded one: nothing was lost in transit and nothing was written in the wrong place. **There was never an argument to lose.** A preference was recorded as a rule, and the rule then read as though it had reasoning behind it because every neighbouring invariant does.

**The refusal to reconstruct is what makes the finding legible.** Had a plausible reason been written in, it would have read identically to a recovered one and the actual origin — *she said so, once, about something else* — would have been unrecoverable. The gap recorded as a gap is what let the real answer land when it turned up.

**No reason was invented for it.** Writing a plausible one is the failure this whole lesson describes, performed deliberately: a reason that arrives by reconstruction is indistinguishable from one that was there, and it would make the rule look defended when nobody has actually defended it. **The gap is recorded as a gap**, the same argument that keeps inferred datetime stamps out of 15c.

→ `DESIGN.md` invariants 6 and 9, and the queued tiered-protection model in *Open — needs a decision*.
