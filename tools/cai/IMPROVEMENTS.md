# Improvements

> **Current, as of 2026-08-26.** Top of the board: diction exists in two places on disk and they will drift. Neither `trans-fairy` nor `redact` is implemented; the shared grammar is, with a fixture. Open design questions: whether `redact` reports position as well as presence, how `trans-fairy` derives pool size, and what a project directory key looks like on Windows.

## How to read this board

**Swept 2026-09-02.** Status is marked POSITIVELY, in two forms, and the difference matters:

```
  ## ✓ …           built and covered by a test
  ## UNBUILT — …   a behaviour claim with no implementation
  ## …             neither: reasoning, a knowledge gap, or work that is not this suite's
```

**The open set is `grep '^#\+ UNBUILT'` — a positive match, not an inferred absence.** The `✓` marker was originally specced so that *absence* would mean unbuilt, which required a complete sweep before it meant anything and still left a reader unable to tell an unbuilt behaviour from a heading that never described one. **Marking both states removes the inference**, so the board is readable at any point in a partial sweep and a new section that is neither needs no marker at all.

**`UNBUILT` is a word rather than a glyph, deliberately.** It is scanned by people and by `grep`, it needs no dictionary entry, and it cannot propagate the way a marker does — which is the failure `⊘` is defined around. Glyphs are reserved for what a tool has to parse.

**The sweep found three false claims before it marked anything.** `cai fabricate`, `cai documentation` and `cai name` each carried *"Owner's proposal, 2026-09-01. Not built."* All three were built by 2026-09-02, and every one of those lines would have been read as current. That is the cost of status living in prose rather than in a marker, measured on this board.

## Planned work

In order. The first is the only one with a deadline in the sense that delay makes it worse.

**1. diction exists twice — RESOLVED 2026-08-28 (owner), disk action pending.** This repository holds it under `diction/`, and a second copy sits elsewhere on disk where the virtualenv and PATH symlinks still point. **Owner's ruling: the `diction/` copy here is the latest — it kept its git history on the move — and is canonical.** The other is the stale copy. What remains is a disk action, the owner's to run: `install.sh` to repoint the symlinks and rebuild the venv against this copy, then delete the external one. **No in-repo change is owed; this stays listed only until the symlinks are repointed.**

### The lineage marker is stranded in the live session, and this session is the proof

**Measured 2026-09-02, on `b5d42ad9` itself.** The re-rooted session carries lineage — and it is not where the rule says it must be.

```
  marker at line              400
  last message record        4506
  message records after it   2082
  rule: exactly ONE marker, always last      FAILS
```

**The rule held while `rewrite_session` was the only writer.** It clears any prior marker before writing, which is what made *exactly one, always last* an invariant rather than a hope — the owner's own correction, after a graft left a marker stranded at index 5. **It fails here because two writers came afterwards that the function does not mediate**: the carry was appended by hand, and then the live session has been appending to itself for hours.

**That is a general limit, not an incident.** A rule enforced inside one function cannot survive writes made outside it, and **a live session appends forever** — so *always last* is not a property any transcript-writing tool can maintain about a session that is still running. Anyone walking to the final message to read lineage finds none.

**What the marker says is the second finding, and it is the unbuilt emulated-origin marker showing through:**

```
  previousSessionId    00000000
  originSessionId      00000000
  includedSessionIds   ['00000000', 'cddb5f6c-...']
```

`includedSessionIds` names the real predecessor correctly. But `previousSessionId` and `originSessionId` hold `00000000` — the synthetic root's placeholder id. **To an audit that reads as a session whose ancestor is named `00000000`**, indistinguishable from a real predecessor, when the truth is *there is no ancestor; this originated here.* The placeholder is standing exactly where the positive claim was designed to go, which is the clearest available argument for building it: the gap does not present as silence, it presents as a plausible-looking id.

**Two consequences worth stating.** A tool cannot own *always last* on a live target, so the honest form is either *at least one, most recent wins* or a marker written at session close by something outside the session. And an audit reading `00000000` as an ancestor will trace a chain that terminates in a fiction rather than in a fact.

### Shell quoting has now corrupted content twice in one stretch, in two different ways

**Recorded 2026-09-02.** Both were silent, both produced output that looked fine, and neither was a tool defect.

**A heredoc terminator typo swallowed a whole script.** Opened with one terminator, closed with another; the shell consumed the rest of the command as body. Diagnosed in full above.

**Backticks in a double-quoted commit message were command-substituted.** `143c34a` was written with the phrase *tested `is None`* and the shell ran `is None` as a command, printed `command not found`, and **substituted the empty result** — so the commit landed reading *"the nulled-pointer check tested  while store.resolve returns an empty list."* The sentence still parses. Nothing failed. **The record simply has a hole where the technical detail was**, and the only visible sign was one warning line above an `ok=True`.

**Corrected forward here rather than by amending**, since the commit is pushed and a rewrite needs the owner. The missing phrase: the check tested `is None`, while `store.resolve` returns an **empty list** for an absent symbol — so a nulled alias pointer resolved as healthy.

**The shape both share: the shell edited the content on its way to the tool, and the tool then did exactly what it was told.** `cai commit` verified tests, checked the grant, matched the subject scope — every gate correct, on text that no longer said what was written. **A gate cannot audit what reached it against what was intended**, which is a real ceiling and worth stating rather than patching around. The cheap habit that avoids both: build the payload in a file or a Python string, and pass a path.

### ✓ One write doctrine, reachable from every family that writes — BUILT 2026-09-03

**Built after an audit the owner asked for: *"I was just worried that there would be a path for an overwrite with no backup."*** There were four.

```
  reflow --to      clobbered silently        now refuses
  redact --to      clobbered silently        now refuses
  fabricate --to   clobbered silently        now refuses
  cai edit         rewrote untracked files   now refuses
  read --out       already refused           written after the incident
```

**`--to` is the flag an operator reaches for TO AVOID destroying something**, and in three tools it destroyed something else instead. `redact`'s own help said `--to` *"makes the run non-destructive, so no backup is required"* — true about the source, and silent about the destination.

**Two kinds of write, and they fail differently.** A NEW file fails when the path is not new: `safewrite.write_new` refuses unless `--force`. An IN-PLACE rewrite fails when there is nothing to recover from: `safewrite.recoverable` requires git tracking, a backup, or `--force`.

**The in-place check is applied where content can be LOST, and not elsewhere** — which is the distinction that keeps it from taxing the ordinary case. `cai edit` changes content by design, so it carries the check. `reflow lines` proves the content survived with a signature that has already been verified, so an untracked reflow risks formatting and not content, and refusing it would spend adoption for a hazard that is not there.

**The audit itself is the reusable part.** Grepping for `open(..., "w")` found seventeen write sites; grepping each for a guard produced two false GUARDED results, both matching the word *exists* inside a docstring. The behaviour had to be run to be known — a temp file called `PRECIOUS`, four commands, and read what survived.

**Found by peer session `RE: Flow` while reporting the incident above, 2026-09-02, and it is the sharper half of that report.** The reasoning that would have prevented the destruction **was already in this codebase**, written for `redact`:

```
  redact/cli.py:34        "in-place rewrite truncates, so a concurrent append is
                           destroyed and a backup cannot recover it"
  redact/redactor.py:95   "Never `cp -n`, which declines silently and returns success"
  redact/cli.py:19        --backup PATH ... REQUIRED
```

**That first sentence describes the incident exactly**, down to the concurrent append and the backup being no help. It was written before the incident, by this suite, about this hazard — **and it was not reachable from `reflow`, which is the family that needed it.**

**This is not the two-homes failure. It is the opposite one.** The doctrine had exactly one home and was correct there. What it lacked was any mechanism making it apply to the next writer, so a new family got built beside it with none of it. **A rule that lives in one tool is a property of that tool, not of the suite** — and every tool added afterwards starts from zero unless the rule is somewhere it must be routed through.

**The fix is a shared write path**, the same consolidation already done for `fence`, `mode` and the source resolver: one module carrying refuse-a-live-target, require-a-backup-for-a-destructive-run, never-`cp -n`, and never-write-a-projects-directory — with `redact`, `reflow` and `trans-fairy-write` all going through it rather than each carrying a copy or, worse, not carrying one.

**`IN_PLACE` and the projects-directory exclusion are the structural half and they are built.** They stop this specific destruction. They do not stop the next family from being written without a backup, which is what this entry is for.

### INCIDENT — `cai reflow context` overwrote a transcript, and every gate passed

**2026-09-02, reported by the owner.** A session ran `cai reflow context <transcript>` — the documented command, no flags — and it **replaced the transcript with its own projection.** Reproduced on a throwaway: 307 bytes of valid JSONL became 64 bytes of prose, unparseable and unresumable.

**This was not misuse.** The member defined `read` and not `write`, so the family's default text writer ran. Nothing in the tool distinguished *transformed this file* from *produced a different view of it*.

**The gates did not fail. They answered a question nobody had asked.**

```
  changed          true    a projection never equals its source
  signature_held   true    the messages genuinely DID survive the projection
  written          true    reported to the operator as success
```

**That is the part worth keeping.** The verification was correct on its own terms and the outcome was destruction, because the invariant being checked — *content survived* — is not the invariant that matters when the output is a different KIND of data from the input. **A correct check on the wrong question is indistinguishable from a passing one.**

**Three fixes, and the ordering of the defaults is the load-bearing one.**

**`IN_PLACE` defaults to FALSE.** A member that transforms text in place declares it; a member that projects says nothing and is refused. **The safe answer must be the one you get by saying nothing**, because the member that needed the guard was written by someone who had not thought about it — me, the same day.

**A projection refuses to write over its source at all**, naming the cost: readable afterwards, never resumable.

**A projects directory is never a write target**, even for an in-place member. A live client holds those files open and appends to them, so a write underneath one loses whatever it wrote in between — and a backup cannot help, because it predates the race.

**What is recoverable for the affected session: the conversation, and nothing else.** That is what a projection keeps. Uuids, parent links, tool traffic, timestamps and session frames are gone, so the file can be read and never resumed. **There was no backup because reflow took none** — `trans-fairy-write` is the sibling that backs up before writing, and reflow did not go through it. That is the fourth fix and it is not built: **a write path that bypasses the tool built to make writes safe.**

### A tool whose object is misidentified grows guards on the wrong side

**Peer session `RE: Flow`, 2026-09-03, on the reflow incident, and it is a better account than either of the two we had.** The peer diagnosed a write-path bug. I diagnosed a name that should be retired. **Micaiah diagnosed a category error, and that is the real one**: if the CONTEXT is what gets reflowed, the transcript is the SOURCE and was never a candidate for being written to at all.

**The two wrong diagnoses were both describing symptoms one level too low**, and each implied a fix that would have held. Guards on the write path work. Retiring the name works. **Neither would have stopped the next guard being added on the same wrong side**, because both leave the tool pointed at the wrong object.

That is the generalisation worth keeping: **a misidentified object does not produce one bug, it produces a stream of them** — each individually fixable, each fix reinforcing the misidentification by making the wrong arrangement survivable.

### A refusal should teach the doctrine at the point of failure

**Same session, on the live-transcript refusal:** *"That refusal message is the best artifact in the set. It does not say 'refused'; it says what would have been lost and why a backup would not have covered it. Someone who hits that at 3am learns the doctrine from the error text."*

```
  refusing to write a transcript that looks live: written 11 seconds ago,
  under the 300-second threshold -- a write underneath a running client
  loses whatever it wrote in between, and the backup predates that.
```

**The reason this matters more than documentation is reach.** Nobody meets a doctrine document at the moment it applies; everybody meets the error. **A refusal is the only text guaranteed to be read by the person who needed it**, so it is the cheapest place a rule can live and the one place it cannot be skipped.

### The write gates, confirmed from outside

**Verified 2026-09-03 by `RE: Flow`, which did not build them** — and by running the hazard rather than reading the description, which is the standard that session itself articulated. Default emits with the source byte-identical; `--replace` without `--backup` is a usage error touching nothing; an existing backup is refused with the source intact; a legitimate run replaces the source and leaves a backup byte-identical to the original. **A live target refused and left no backup file behind**, which is the ordering claim, confirmed by a party with no stake in it being true.

### A broken instrument returns the answer that requires no further work

**Peer session `RE: Flow`, 2026-09-04, from a pair of failures on the same day.** It is a **trigger** rather than a discipline, which is what the entries around it lacked: they say *check*, this says *when*.

```
  mine    two broken checks agreed the reporter was mistaken   "nothing to fix"
  theirs  a leak scan blind to bare register addresses          "nothing to report"
```

**Both were well formed. Both returned clean. Both ended the work.**

**Theirs is the sharper instance, and the direction is why.** Mine accused a peer. Theirs **exonerated itself, about its own conduct, on a question about a wall it was responsible for** — asked whether decompiled code had leaked into subagent context, it matched mnemonics and identifiers, found none, and reported zero. Widened to bare register addresses it hit `0xD0000110` in three briefs.

> **A convenient answer is not wrong because it is convenient. It is that a convenient answer is the one nobody re-runs.**

**And the asymmetry that follows, which I would not have written:** *when a check contradicts a report, the report is not the first thing to doubt.* **The instrument was built five minutes ago; the reporter ran theirs against the real thing.** My two failed verifications both concluded the reporter was mistaken, and had I stopped at two I would have written back with a measurement to show for it.

**A claim checked badly twice is not a claim checked.**

### Having written something once is not having checked it HERE

**Two instances from unrelated systems, which is what makes it a pattern rather than an anecdote.**

**Mine:** I told a peer that two conflicting flags were already refused. The check existed in `cai read` and had never been written in `cai reflow`. **I asserted it about a code path I had never run**, on the strength of having written it once in the sibling tool.

**Theirs:** a command was placed on a read-only allowlist on the strength of a format string that lived in **a different function entirely**. Right string, wrong path, never executed — and it cost them their worst day.

**The shared shape is a proxy again, and a particularly convincing one: your own past work.** A memory of having handled something is evidence that you thought about it, and no evidence at all that this path does it — which is the same distance as prose about a guard versus the guard.

### A search is an experiment, and we already know how to run those

**Micaiah's framing, 2026-09-03, relayed through peer session `RE: Flow` at her request.** It supplies the *why* the entry below only asserts.

**The observation:** an agent authoring a test is scrupulous without being asked — controls, predictions, variables held fixed, what would falsify this, is the assertion order-invariant. The same agent then runs a search, a verifier or a build probe and **none of that machinery comes along.**

```
  the query, the path, the probe      the APPARATUS
  running it                          the TEST RUN
  "12/12 claims hold"                 the HYPOTHESIS, now believed
  proving the forward positive        the CONTROL
```

**The difference is posture, not ability.** Authoring a test feels like doing science, so the discipline engages. Running a search feels like looking something up, and looking things up has no controls in it — while being neither easier nor less consequential.

**What the posture switch specifically drops**, and this is the part that makes it actionable:

> **When authoring a test, the CODE is under suspicion. When investigating, the SUBJECT is under suspicion and the INSTRUMENT gets a free pass.**

**Every instance recorded on this board is an instrument failure reporting a clean result about an innocent subject.** A corpus path that no longer resolved — the glob was wrong, not the corpus. An include path missing from a compile probe — the build was wrong, not the gate. Two false `GUARDED` results here — the query was wrong, not the write sites. **An instrument cannot report its own failure**, which is what a control is for and exactly what falls away when the frame shifts from testing to looking.

**The habit, which carries the rule without anyone remembering the rule:** before accepting a result you wanted, ask what you would ask of a test suite that passed on its first run — **"what else would produce this output?"** There is nearly always a cheaper explanation than success: an empty glob, a missing header, a query that never matched, an assertion that cannot fail. Name it, rule it out, and the control writes itself.

**The evidence is better than the argument.** The peer reported that within an hour of writing this up it patched two verifiers, ran a check, and pushed on output reading `guard present: False` on one line and `12/12 claims hold` on the next. **It read the second.** The rule was freshly authored and in front of it. **Knowing the rule and running the check are different activities** — recorded here before, now with a third independent instance.

#### The cost, and where the discipline should NOT go

**Owner, same message:** *"I don't want to get too crazy with defining items like this and bog down simple things like `grep`. Still, it does seem like when you started forward checking the positive, it helped."*

**Both halves are load-bearing and the second does not cancel the first.** A control on every search is ceremony, and ceremony gets skipped wholesale rather than selectively — which loses the cases that mattered along with the ones that did not. **A discipline nobody can afford is not stricter than one they can, it is absent.**

**The discriminator is whether the result CLOSES a question:**

```
  looking something up        the next step reveals a wrong answer immediately
                              → no control; the work is its own check
  believing a result          nothing downstream will contradict it
                              → an experiment, and it needs a control
```

**Sharper: it is not *is this a search*, it is WHAT THE RESULT LICENSES.** A control is warranted when a negative **closes** something — ends an investigation, satisfies a gate, licenses a claim in a durable record, or permits an action. It is overhead when the result is one look among many, with more evidence coming and the cost of being wrong being that you look again.

**And the sharpest test is how DIRECT the instrument is, because every recorded instance lived in a PROXY.**

```
  a glob standing for a corpus          the glob was wrong, the corpus was fine
  a query standing for a guard          the query was wrong, the sites were fine
  a heading standing for an implementation   the heading was stale, the code was built
  a criterion standing for a behaviour  the criterion was wrong, the checksum was fine
```

**Reading a file you can see is direct and needs nothing.** The failures happen where something stands in for the thing you actually care about, because a proxy can break while still returning a well-formed answer about the subject.

**The two costs are separate, and keeping them separate is what makes this affordable:**

```
  the HABIT      "what else would produce this output?"
                 one thought · everywhere · free even on a throwaway grep
  the APPARATUS  a positive control, a refusing verifier, a must-compile file
                 real work · only where something is being closed
```

**Spend the thought freely and the apparatus narrowly.** A grep returning nothing while you are still exploring needs the thought and not the control. **The same grep used to assert in a specification that a command does not exist needs both.**

**This section was derived twice, independently, and that is why it is stated this confidently.** The owner named the cost; this session wrote a *closes-a-question* discriminator; the peer session wrote a *what-the-result-licenses* one with the proxy test and the two-weight split, without seeing the first. **Two readings arriving at the same shape without contact is the evidence class this repository already calls strongest** — the same corroboration argument the reader's map rests on, firing on the rule about checking.

**Ask: does anything depend on this being right, and would I find out if it were not?** A grep whose answer you are about to act on and be corrected by needs nothing. **An empty grep you are about to record as a finding is a hypothesis**, and every costly instance on this board was one of those — a negative accepted, written down, and built on.

**Same shape as the carrier rule in SOPIA:** a summary is fine when you would not act differently on finding something it dropped. The cost of checking is trivial; the cost of checking *everything* is not, and the tell is what the result is about to be used for.

#### Build the shape so the rule need not be remembered

**Owner's, and it is the same move made three times in this suite already.** A facility asserting an absence takes its positive control as a **required argument**, so the unsafe call cannot be written. `IN_PLACE` defaults to false so a member that says nothing is refused. `cai read` has no write path to guard rather than a guarded one. **Top of the lesson-12 ladder each time: not a rule to follow, a path that is not there.**

### Searching for the guard tests the text; only running the hazard tests the guard

**Stated by peer session `RE: Flow`, 2026-09-03, generalising the write audit.** It is the sharpest form this board has of a rule it already carried in weaker words.

**The audit is the worked example.** Seventeen write sites, grepped for a guard, produced two `GUARDED` results that were false — both matched the word *exists* inside a **docstring**. Reading the code said the guard was there. It was not. What settled it was a file called `PRECIOUS`, four commands, and looking at what survived:

```
  grep for a guard      tests whether the WORDS are present
  run the hazard        tests whether the BEHAVIOUR is present
```

**The peer reports the same shape three times outside this suite** — twice an acceptance gate agreeing on totals while the content was wrong, once a corpus verifier reporting `0/0 claims hold` because a path had gone stale on a machine move. **A check that passes while proving nothing is the common failure**, and it is not detectable by reading the check.

**This subsumes the earlier entries rather than sitting beside them.** *Prove the positive* says a negative needs a control. *A control sharing its query is not a control* says the control must differ. **This one says where the control has to live: in the behaviour, not in the text.** A control that greps for a different string is still grepping.

**And it explains why the false GUARDED results were so convincing.** Both matched real sentences, written by this suite, describing the exact hazard being checked for. **Prose about a guard is evidence that someone thought about the hazard, and no evidence at all that they handled it** — which is nearly the opposite of how it reads.

### A control that shares its query with the claim is not a control

**2026-09-02.** Verifying that a deleted file had no remaining referrers, I ran the search, got nothing, and then ran a "positive control" — **using the same search string**. It also returned nothing, which I briefly read as confirmation.

**It confirmed nothing.** A control exists to prove the query can return a positive. Searching for the *same absent thing* twice proves only that it is absent twice, and it manufactures the feeling of having checked. The rule already on this board — *validate against a known positive before trusting a negative* — is satisfied only when the control looks for something **known to be present**, which necessarily means a different string.

```
  not a control    grep X → 0 ;  "control": grep X → 0
  a control        grep X → 0 ;  control: grep Y → n, where Y is known present
```

**The claim happened to be true** — the file genuinely had no live referrers, confirmed afterwards by an unfiltered search and by `cai flow pointers` reporting 22 pointers and 0 broken. **That is the dangerous shape: a worthless check that agreed with a correct answer.** Nothing in the output distinguishes it from a check that agreed with a wrong one.

**This is the twelfth instance of the class on this board and the first of this variety** — not a bad query, but a bad *control* for a query. Which is why the enforcement belongs in `cai` rather than in the resolve to be careful: the prompt fires on the empty result, and what it has to ask for is a control that differs from the pattern it is checking.

### The heredoc that swallowed a script, and why every structural gate passed it

**Diagnosed 2026-09-02, from evidence rather than reconstruction.** A chained-heredoc shell command opened one heredoc with `<<'NEWEOF'` and closed it with `OLDEOF`. The shell went looking for a bare `NEWEOF`, never found one, and **consumed the entire remainder of the script as heredoc body** — the tool invocations after it, the next heredoc, all of it. Two files the later steps depended on were therefore never created, and the anchor file held 837 bytes of shell script instead of four lines of Python.

**`cai edit` then applied it faithfully and every gate agreed.** The anchor matched once. The definition count went 3 to 3. Delta 0, as declared. **The guard was working exactly as specified and the file would not import** — because structure counting cannot see that the replacement text was garbage. Garbage in, garbage applied.

**The fix is a post-apply parse check**, and it belongs at the point of effect rather than in a resolve to be careful with heredocs. The result must still parse as what the file claims to be; on failure the file is untouched. Its limit is stated in the code: corruption that is still valid syntax passes, since a stray terminator alone on a line is a bare name expression.

**Two reporting errors of mine are worth keeping with it.** I first said the scratchpad files had *vanished*, which implies something removed them and sent the diagnosis toward a mid-operation compaction. **They were never written.** And my first positive control for the new gate used a terminator on its own line, which parses — so the control returned `ok=True` and I nearly recorded the gate as broken. **The control was wrong, not the guard**, which is the fourth time in this stretch that a correct result was measured wrongly.

**2. Two reflow implementations.** `diction/tools-reflow.py` and a `reflow.py`/`reflowlib.py` pair in the SOPIA repository both define `reflow`. They were written independently for the same job. This is the shape that produced the defect `src/cai/grammar/` exists to prevent: one piece of knowledge held in two places, drifting quietly until one of them is wrong. A shared module here would hold it once.

**Measured 2026-09-02, and they have diverged.** `diction/tools-reflow.py` is 2,378 bytes and defines `reflow`, `is_code`, `is_structural`, `sentences`. `SOPIA/tools/reflow.py` is 3,780 bytes and defines `reflow`, `norm`. **Both implement `reflow`; neither has the other's helpers.** The board has carried this as *two implementations* since 2026-08-26 — what was not known is that they no longer agree.

**Resolved 2026-09-02, and I got the reading wrong twice before the owner corrected it.** I called it *the `fence` case one repository boundary further out* — two implementations of one concept. Then, having run both on identical input, *two concepts sharing one name*, remedy a rename. **Both were wrong, and the evidence I cited fit either reading**, which is why it decided nothing.

**The owner's reading: *"two different modes of a subset of what I would call `reflow`."*** Reflow is a family; line-breaking is one member of it; sentence and paragraph are two modes of that member. `diction` puts one SENTENCE per line, SOPIA one PARAGRAPH — opposite on the same input, three lines against one, each correct for the rule its repository keeps.

**I then restated that as "one operation, two modes" and built to the restatement.** That drops `subset`, which is the load-bearing word: it makes the pair the whole of reflow, so whitespace normalisation, hard wrapping to a column and unwrapping would each have had to arrive disguised as a line-breaking mode. **Corrected in `cai/reflow/` — a family with `lines` as its first member**, and in `reflow-operation` / `reflow-mode` in the dictionary. Only implemented members are listed; a registered-but-unbuilt name is refused **as a proposal rather than as an unknown name**, because those need opposite responses.

**My dependency objection was wrong twice over and dissolved on being checked.** Both files import only `re`, `sys`, `glob`, `pathlib`, and `tools-reflow.py` is not part of diction's package — it merely lives in that directory. **Neither repository pays a dependency**, and I asserted otherwise twice without running the one command that would have settled it.

**The merge exposed a real safety gap, which is what makes it worth having done.** SOPIA's version refuses to write when a content signature changes; diction's rewrote files with no check at all. **Under one operation that asymmetry is not defensible** — a mode is a choice about line breaks, not about whether the text may be silently altered — so the verification belongs to the operation and now runs in both modes. A bug surfaced with it: in sentence mode a blockquote continuation was space-padded instead of repeating `> `, silently turning quoted text into indented code.

**3. The rest of SOPIA's toolset.** Beyond reflow: search guards, a spiral rate detector, term archaeology, an instrument test runner, a grant-expiry script. Some are general and belong in a tools suite; some are specific to that repository and should stay. Nobody has sorted them. Two entries in the draft's own improvement notes — the `rename_mark` blockquote floor and the absence of a transfer convention for quoted spans — travel with them when they move.

**Qwen3-TTS is the candidate engine for diction's text-to-speech — owner, 2026-08-31. Preview before implementing.**

**Nothing is decided and nothing should be built yet.** The owner wants to hear it first; this row exists so the intent is recorded rather than remembered, and so a later agent does not treat a named engine as a settled choice.

**What a preview would need to answer**, since these are the questions that made diction its own distribution in the first place: whether it runs locally or calls out, what it drags in if local (diction already pulls Whisper, CTranslate2 and the CUDA runtime wheels, so a second model's weight is not free), and whether it fits the same `uv tool install --with` shape the speech-to-text side uses. **If it is hosted rather than local, that is a different dependency question and arguably a different tool.**

**Text-to-speech for diction — planned, owner 2026-08-31.** diction is speech-to-text today; the owner intends the reverse direction as well. Recorded now so the design is not written as though one direction is the whole tool: a bidirectional diction changes what the package owns (voice selection, output device handling, and whether synthesis is local or hosted, which is the dependency question that made diction its own distribution in the first place). No design yet, and nothing here decides one.

**A doc that nothing links to is nearly a doc that does not exist.** `docs/isolated-harness.md` was written, committed, and referenced by nothing for several commits — findable by grep and by no other means. That is the no-entry-point defect the README-as-map ruling exists to prevent, committed by the session that wrote the ruling. Wanted: a check that after adding a document, something points at it. Cheap forms exist — a grep for each tracked `.md` filename across the other tracked `.md` files, run as part of the form check, flagging any file nothing references. The awkward cases are the deliberate ones (an entry point points outward and is pointed at by nothing inside the repo), so the check needs an exemption list or it becomes noise.

**✓ `trans-fairy-write` — named and BUILT 2026-09-01, supersedes the housekeeping-tool row below.** The sibling that is allowed to write. `trans-fairy` stays create-only and prints the exact `trans-fairy-write` command when an overwrite is wanted, rather than growing a flag that would make its own contract conditional. First caller is the lineage retrofit, which modifies a real session in place. See `docs/transfairy/DESIGN.md`. Built at `src/cai/transfairywrite/`, 25 checks; the retrofit and projects-dir housekeeping remain unbuilt callers.

**✓ `redact` expands to carry a re-root chain — owner's ruling, BUILT 2026-09-01.** `--to PATH`, `--project` with selections, `--since-compaction`, `--neutralise-fences` and `--max-block`, at `src/cai/redact/`, 36 checks. The remaining unbuilt piece is the chain driver itself — the final-turn capture that strings these together.

**The projection is a redaction.** Stripping thinking blocks and tool traffic to reach the conversation is **removal**, which is `redact`'s verb — not a new read operation, as was first specced. `redact` today removes secrets; removing structural categories is the same operation pointed at different targets. That collapses a proposed surface into an existing one.

**The categories are selectable, and the read is not only for conversation.** Owner, 2026-09-01: *what if we just want the read on tool results instead?* The projection is named for its commonest use and specced for a general one — **the caller says which record and block categories it wants**, and *user and assistant text* is the default rather than the definition.

| selection | reaches | measured here |
| --- | --- | --- |
| default | `user` + `assistant` text blocks | ~281k, 7.2% of raw |
| `--turns` | user turns only | ~85k |
| `--only tools` | `tool_use` + `tool_result` | ~706k |
| `--with-tools` | conversation plus tool traffic | ~1.84M |
| `--since-compaction` | any of the above, past the last boundary | ~17k at the default |

**A tools-only read is the case the conversation projection cannot serve**, because debugging a run means reading exactly the material the default drops. **Thinking blocks stay excluded from every selection**: they are the single largest component, and their signatures do not validate outside the session that minted them.

**`redact` already carries the contract `trans-fairy-write` was going to need**, which is why the write half belongs here rather than being designed from scratch: a **required** `--backup`, refusal to overwrite an existing backup, `--verify` against that backup, and refusal to write invalid JSON. The sibling that admits to modifying already knows how to do it safely.

**But it may never write in place on a live transcript.** `redact` writes with `open(transcript, "w")` — truncate and rewrite. If the client appends between the read and the write, that append is destroyed, **and the backup does not save it**, because a backup preserves the pre-write state rather than the concurrent write. So the expansion is `--to PATH`: source read-only, result written elsewhere.

**Which is what lets a chain respect the base contract by funnelling through a re-root flow.** The ending text of a turn is capturable — a message lands in the transcript before that turn's tool call runs, verified in two sessions — so the final turn can read its own last words, redact to a clean projection, compose the next root, and hand off. **No live write, and nothing for the operator to copy by hand.**

**On carry, inner envelope markers are neutralised — the name and its carrier both.** A carried `---- Raw Source ----` becomes evidence that one was there rather than an active fence, so **only the outermost envelope is live**. Depth stops being a property the rule reasons about: neutralisation happens at every carry, so a chain of any length has exactly one active contract. This replaces the older *depth stops at one* clause, and it is a strictly smaller rule.

**The neutralisation is itself a redaction, and must be understood as one.** Carried history was edited. It therefore falls under the same disclosure the rest of redaction carries — the mark says what was done, and the edit is not permitted to pass as untouched material.

**The match must be positional, never textual.** A fence is a delimiter **alone on a line** — `^---- <name> ----\s*$` — while a mention is the same string embedded in prose or inside a code block. **Measured in the thread that designed the convention: 14 fence-shaped lines against 12 inline mentions**, both live and roughly equally common, so a substring redaction would have mangled the discussion that produced the rule. The form check already applies this discipline by skipping fenced regions.

**Two loose gates make one strong gate.** The same line-anchored check runs against both delimiter families, and recognition rules expand alongside the blocks. Neither family's check is strict on its own; together they bound the envelope even where one closer is missing, because different delimiters cannot collide.

**A detector that cannot tell use from mention is a class, not an incident.** It has now produced two false positives in one day on different targets — a leak check matching a quotation of its own probe string, and this one. Any scan for a marker in material that may discuss that marker needs the positional form.

**Build order, owner 2026-09-01: `trans-fairy-write` first, then `redact`.** The write sibling comes first because three recorded items queue behind it — the lineage retrofit, projects-dir housekeeping, and repairing a transcript after install — and because `redact`'s own expansion needs somewhere safe to put a result. **`redact` second**, carrying the projection, the structural stripping and fence neutralisation on carry. Neutralisation is the one written rule with no implementation behind it, so the gap closes at that point rather than earlier.

**The hand-run path, and where each step belongs — owner's principle, 2026-09-01.** *The hard ran path paves the way for automation.* What has been done by hand **and defined** should become the appropriate tool's functionality. The 2026-09-01 re-root was run end to end by hand, which is what made each step definable; this is the inventory.

| done by hand | belongs to | state |
| --- | --- | --- |
| removing a `tool_use`/`tool_result` pair from a transcript | `redact` | recorded, unbuilt |
| projecting turns to message text for a carry | `redact` | recorded, unbuilt |
| neutralising inner envelope markers on carry | `redact` | specified, unbuilt |
| overwriting an installed transcript in place | `trans-fairy-write` | recorded, unbuilt |
| keeping the backup chain by hand across seven revisions | `trans-fairy-write` | the contract already exists in `redact` |
| repairing a malformed record after an install | `trans-fairy-write` | recorded, unbuilt |
| checking that the final turn had landed before carrying it | the carry operation | verified twice, unimplemented |
| staging a correctly-named copy so `install` would accept it | `install` | see the defect below |

**Three defects the hand run exposed, all confirmed in source.**

**`compose` mislabelled a true number — FIXED 2026-09-01.** `graft.py` passed `1, len(suffix_lines)` as `cut_at, dropped`, so the notice printed the **retained** count under a **removed** label: *cut before record 1 and 921 earlier records were removed*, where 921 was what it kept. **The owner's correction: the arrow was miscommunicated rather than the arithmetic wrong** — the intent was closer to *921 selected out of 8,015*. An earlier version of this entry called it a false number understating the loss; it was a true number pointing the wrong way, which is a different defect with a different fix. **`compose` cannot know what was dropped at all**, because it receives a suffix already cut, and it now says so rather than filling the gap. `dropped` measures the past and is fixed at the moment of the cut; `retained` counts what is present; a caller that knows neither claims neither.

**`compose` validated neither its root input nor its output shape — FIXED 2026-09-01.** A root whose assistant record carried `message.content` as a string rather than an array of blocks was accepted, composed, and installed, then crashed the client on load with `content.map is not a function`. **`build` had six checks and `compose` had none**, despite `compose` being the operation that takes hand-authored input, which is where malformed records actually come from. `verify_composed` now runs **before the write**, carrying the checks the hand run performed: assistant content is a list of blocks (role-aware, since a string is valid on a user record), the parent chain is unbroken, no `tool_result` is orphaned by an excision that took its `tool_use`, exactly one `sessionId`, uuids unique, and the document ends on a message record.

**`install` derived the session id from the filename — FIXED 2026-09-01.** A transcript composed with `--out` installed under whatever name the caller chose, the name then disagreed with the `sessionId` inside it, and the client could not find it. **The owner's ruling reverses the direction**: the id is read from the records, and the destination is *renamed to it*. The id is present in every record; taking it from the file name was the guess.

**A housekeeping tool for the projects directory — owner's proposal, 2026-08-31.** trans-fairy is additive by contract and will not remove or overwrite anything, which leaves the projects directory to grow: every rebuild-and-reflow, every fork, every install adds a transcript and nothing prunes. The proposal is a **separate tool for writes and destructive actions**, so trans-fairy keeps a create-only contract and housekeeping happens somewhere that admits to being destructive.

Scope it would need to answer: what may be removed (a reflowed duplicate is the obvious candidate; the first transcript into a directory is the obvious exclusion), whether removal is ever automatic or always a human act, and whether it backs up before removing — the point where the retired invariant 1 would genuinely come back, since that tool *would* perform destructive writes. **The `--agent` contract applies: an agent never answers a deletion prompt.** No design yet.

**4. The neovim client.** Designed, unbuilt. Its design lives in the SOPIA repository under `docs/scratch/`. It is Lua rather than Python, so it does not become a distribution here, but it consumes the same headless-session pattern diction proved and should not reimplement it from scratch.

**5. Transcript parsing.** A sub-tree of `cai`, owned here — but `claude.nvim` should vendor or package it rather than depend on `cai` strictly. Operator's framing: `cai` owns it, the editor project does not require `cai` to be installed.

> **Marked for discussion before this is built.** `SOPIA` first argued against vendoring: share a fixture and a `jq` program instead, on the grounds that two copies of a parser drift the way the record grammar drifted into a defect.
>
> That argument was wrong in the general case and the correction is the operator's. **Duplication under source control with a defined upstream is not the hazard — it is how development works.** Every clone is a copy; every branch is a copy. `git subtree` gives a reconciliation path in both directions, so keeping `claude.nvim`'s parser in sync is a command rather than a discipline, exactly as diction's history came across. Rewritten history on `main` is the one case that breaks it, and that is recoverable with help rather than fatal. With more than one developer it reverts to ordinary branching.
>
> What survives from the original argument is narrower and still worth having: **a shared fixture keeps two-language implementations honest**, because Lua and Python can both be tested against `tests/fixtures/shapes.jsonl` with the same negative control. That is a reason to share a test, not a reason to avoid a copy.
>
> The open question when this is picked up is therefore not *whether* to vendor but *what* — the jq program, the fixture, a subtree of the parser, or some combination.

**6. Isolated-context agent creation.** Ways to spawn sub-agents, forks and new agents in a controlled context. Sub-tree of `cai` for the same reason as 5.

**7. Hours tracking.** Its own tooling.

**8. Hours already worked, unrecorded.** A combination of 6 and 7 — use isolated agents plus the tracking tooling to reconstruct time spent on items that were never logged.

Suggestions on the design of what was moved here. Nothing here is a decision and none of it is owed a response.

**This file follows the ceiling pattern.** The block above is a rewritable head, maintained; everything below it is append-only. A reader meets the head, not the log. There is no floor — an empty log is a valid state. Corrections are new entries pointing back at old ones, never edits over them. See `SOPIA/docs/guides/the-ceiling-pattern.md`.

## trans-fairy

**inject role-level labeling — future, partially probed 2026-08-28.** Easy mode (natural roles + `origin.kind: "inject"`) is the spec. A role-level label — `injected-prompter` / `injected-responder` / `injected-tool-call` / `injected-inflight-status` — was probed against claude 2.1.236: renaming the record **`type`** drops the record (content lost, priming defeated), but a non-standard **`message.role`** under `type: "assistant"` loads and reaches context. So role-level labeling is feasible only via `message.role`, and only worth pursuing if `origin.kind` proves insufficient. **Needs docs reads or probing** — specifically whether the model reads the `message.role` string or just treats the content as a normal turn. Owner's read: we likely will not need it.


**Open item 10 is now closable.** `docs/transfairy/DESIGN.md` lists a shipped template — line grammar with placeholder content — as unruled, noting that the structural value is the grammar. That grammar existed only inside `reference/transfairy/build-transcript.py`. It is now `src/cai/grammar/records.py` with a fixture. The open item can be answered by pointing at the module.

**The builder must import that module, not re-hardcode its field sets.** The sets are known in three places today — `records.py` (owner), `reference/transfairy/build-transcript.py` (frozen pre-extraction exemplar, currently in agreement), and `DESIGN.md`'s emission prose (explanation). They match now and will drift the moment the real `build` re-declares them instead of importing. Verified against the reference 2026-08-28: `USER_FIELDS`/`ASSISTANT_FIELDS`/`USAGE_FIELDS` equal the builder's emitted keys.

**The spec covers pool handling the reference implementation does not.** `DESIGN.md` specifies mint on absent, append when short, never regenerate or shuffle. The reference reads a pool and assumes it is long enough; an undersized pool raises mid-loop. The failure is benign — the file write happens after the loop, so nothing partial is emitted — but it reports nothing about why.

**`--pool-size N` is specified as "computed from message count".** From the reference: one for `SID`, two per human record, three per assistant record, one for the closing snapshot. Worth stating in the spec as the formula rather than as a description.

**The header of `DESIGN.md` is stale.** It reads *"Living copy. Maintained in `~/some-notes/day-one/`. The copy in `trans-fairy/` is frozen"* — that directory no longer exists, and the copy carrying the line is no longer the frozen one.

**Machine paths were stripped from the reference** before it came here. Four hardcoded absolute paths became placeholders. The design already removes the cause by deriving them from flags.

## redact

**Its known limits are recorded in its own draft and should survive the move.** It cannot cover anything appended after it runs, including the tool call that ran it. Candidate detection is shape-based and over-matches when unscoped. Without `--candidates-from` pointing at a pristine backup, a re-leak cannot be cleaned, because deriving candidates from the live transcript yields zero once the original location is already scrubbed.

**Two defects it produced are filed as cases in SOPIA `tools/CASES.md`, 15 and 16.** The shape-partial blanker is directly addressed by `src/cai/grammar/records.py`, which enumerates content shapes rather than assuming one. The false-zero regex is not addressed by anything here.

**Backups are an exposure.** Redaction produced three, one a complete unredacted copy. Whatever retention rule applies should be decided rather than defaulted.

**The real redact imports `src/cai/grammar/records.py`, it does not re-declare shape handling.** `reference/redact/redact-transcript.py` carries its own `PAYLOAD_KEYS` and a bespoke `blank_payloads` recursion — the pre-extraction exemplar. The shared module owns content-shape traversal, which is the whole reason it exists. **`records.py` may need to grow the payload/structural key sets the reference script currently owns** (`PAYLOAD_KEYS`, and the `STRUCTURAL_KEYS` that must never be scrubbed), so both tools read them from one place. Same rule as trans-fairy's builder, under trans-fairy above.

## trans-fairy — the grammar describes our builder, not the live schema

**Found 2026-08-31 by running `truncate` against a real Claude Code transcript**, which is far richer than anything our builder emits. A 7,191-record live transcript carried fifteen record types; `records.py` knows five frame types plus user/assistant. The unknown ones: `agent-name`, `custom-title`, `atis-latch`, `queue-operation`, `attachment` (1,338 of them), `file-history-delta`, `bridge-session`, and `system` with subtypes such as `turn_duration`. Frame records are **per-turn, not once per file** -- 215 `ai-title` records, not one.

**Fields, too.** Real `user` records carry `isCompactSummary`, `isVisibleInTranscriptOnly`, `sessionKind` and `session_id`; `assistant` carries `sessionKind`; `usage` carries `cache_creation`, `inference_geo`, `iterations`, `output_tokens_details`, `server_tool_use` and `speed`.

**The consequence is scoped, not fatal.** The six build checks are *build-output* verification -- they ask whether our builder emitted conformant records, and for that they are correct. They are not a general validator, and using them as one rejects healthy real transcripts. `truncate` was doing exactly that and now runs cut-specific verification instead. **Anything else that reuses `verify()` as general validation has the same defect**, and the key-set check in particular should be understood as conformance-to-our-model rather than conformance-to-Claude-Code.

**Not fixed by widening the grammar.** Widening it to the live schema would make the builder's own conformance check meaningless -- the check earns its keep precisely by being narrow. If a general validator is ever wanted it is a second function with a different contract.

## redact — noted while implementing, 2026-08-31

Stored rather than raised. None of these block the tool; each is a real edge the first implementation does not cover.

**A line that fails to parse as JSON is passed through untouched.** The alternative -- mangling it -- is worse, so the behaviour is deliberate, but it means a secret sitting in a corrupt line survives every pass silently. `--verify` will catch it as residual, which is the mitigation, but nothing reports *why* it survived.

**`blank_payloads` replaces payload STRINGS.** A payload key holding a bare list of strings is walked but its members are not blanked, because only `str` values are replaced. Nested dicts are covered. Worth a fixture either way.

**The candidate floor is a length threshold, not a shape judgement.** Anything at or under eight characters is dropped to avoid matching half the file. A genuinely short secret is therefore invisible to this tool, and nothing says so at the point of use.

**Substitution counting is per-occurrence-per-line**, so the reported number is an activity count rather than a count of distinct values. Fine for a log line, misleading if anyone treats it as a measure of exposure.

**`reference/redact/audit-terms.sh` is not wired in.** The sealed-term indirection -- hold terms in a separate file, search through variables so the record holds the name and never the value -- is the technique that broke the reintroduction cycle, and it currently lives only as a reference script. A `--terms-from` flag reading names rather than values is the obvious shape.

**`--assess` is unimplemented**, so the position/inference half of the audit findings is still unmeasured. See the audit-findings section above; the tool reports presence only, and says so in `--man-help`.

## redact — structural keys

`redact-transcript.py` excludes structural keys by name: `uuid`, `parentUuid`, `sessionId`, `promptId`, `requestId`. A `promptId` is otherwise indistinguishable from a leaked identifier under a generic UUID pattern.

Operator's assessment, 2026-08-26: the exclusion was the right call. The gap it leaves is that a structurally similar value appearing in **body text** cannot be distinguished from a structural field — the tool has no way to tell a quoted uuid from a real one. Acceptable for a dumb tool. For a smarter one this is the large gap to close, and `src/cai/grammar/records.py` is where the distinction would live, since it already separates a record's frame from its content.

## Windows

The temp and data locations are portable as specified: `tempfile.gettempdir()` honours `TMPDIR`, `TEMP` and `TMP`; the data directory is `%LOCALAPPDATA%\cai` against `$XDG_DATA_HOME/cai` elsewhere. Neither needs a dependency.

What is **not** known is how Claude Code keys a project directory on Windows. On Linux it is the cwd with `/` replaced by `-`. A Windows path carries a drive letter and backslashes, and nobody here has checked what the client does with them. `--target-cwd` and any re-home therefore have unverified behaviour on Windows. Do not assume it transposes.

## redact — what the audit says it should measure

See `docs/redact/AUDIT-FINDINGS.md`. Three become design requirements rather than notes:

**Report position, not only density.** The tool measures presence. The audit found inference scored 9–10 of 20 while presence scored 0 of 27 after remediation, and that position of surviving markers is a stronger tell than their count. A `--assess` mode that reports where markers fall in the file, not just how many, would cover the half currently unmeasured.

**Never offer to smooth a gap.** Rewriting surviving text to restore flow is fabrication. If the tool ever grows a rewrite feature, this is the boundary.

**Truncation is not free.** A shorter file puts markers nearer its end and worsens the positional tell even as density improves. Any truncation option should say so at the point of use.

## Both — the ceiling pattern applies to three things here

`SOPIA/docs/guides/the-ceiling-pattern.md` describes four rules for an artifact that grows without becoming expensive to read. None of the three were present in this repository before 2026-08-26.

**This file.** Applied above. It is the fastest-growing artifact in the suite and had no head.

**The backup manifests.** `DESIGN.md` specifies one `MANIFEST.md` per numbered capture, which is right — each is bounded by construction. What is missing is the cumulative view: after twenty captures a reader wanting "what changed across all of them" reads twenty manifests. A `bak/HEAD.md` carrying a rewritable summary over the capture log would give the hand-pick interface the retention rule assumes, without giving up plain-copy deletability.

**Tool output.** Neither tool is implemented, so this is a design note rather than a defect. A redaction run and a build run each produce a report. If those accumulate in the data directory, they need a head or they become the thing this pattern exists to prevent. Decide it before either tool writes its first report, because retrofitting a ceiling means deciding what the head should have said for runs nobody remembers.

## Both


**Neither tool has a test beyond the grammar module.** The grammar is covered with a fixture and a negative control; the tools themselves are unimplemented, so there is nothing yet to cover.

**History lives elsewhere.** `trans-fairy`'s documents were copied here from a separate repository that held their commit history. That repository has since been removed; `diction`'s history came across intact via `git subtree`.

## Extraction out of SOPIA

Two sections of the draft's own `IMPROVEMENTS.md` were left behind deliberately, because they are about SOPIA's `tools/` rather than about anything here: the `rename_mark` blockquote floor in `searchguards.py`, and the absence of any transfer convention for quoted spans.

Recorded because the operator is working toward extracting SOPIA's toolset as well. When that happens, those two notes come with it, and this suite is where they land. Until then they stay where the code they describe lives.

## UNBUILT — The weighting pipeline — a concrete use for `cai`

SOPIA is building a weighting system at `docs/weightings-candidates/`, and its three stages split cleanly along what a script does well and what it cannot do at all.

**Stage one is a parser.** Read the logged focus-mode reports at `docs/logs/mode-focus-reports`, count how many times each tagged item returned across runs, and emit candidates into `unreviewed/`. Exhaustive, boring, no drift, no per-run cost. This is the half that should never wait on a person.

**Stage two is not a parser.** Clustering four candidates worded four different ways into the one underlying problem is a reading task. It wants bounded subagents, which is the isolated-agent item already on this board rather than a new one.

**Stage three is the operator and stays that way.**

The rails come with it, and they are not this repository's to relax — they live in SOPIA's `docs/weightings-candidates/README.md`. Output lands in the candidates tree and nowhere a mode loads. Triage may cluster and summarise but may not author a weight. Promotion is an act, not a batch approval.

**This depends on a format SOPIA has not emitted yet.** No focus-mode report has been logged, so the parser has nothing to read and the entry shape is a specification rather than an observation. Building against it now would be building against a guess.

## UNBUILT — Salted session tags

The `Session:` trailer in a commit carries a tag derived from the session UUID by an algorithm keyed on a salt, rather than the UUID itself.

**Its agentic mode follows the forced-read gate in [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md)**, which is where the exposure question is asked.

**The tool creates the salt and holds it. The session never reads it.** That single change is what makes the design work, and it is worth recording that the first version — salt kept in the transcript — did not.

### Why this beats a random nonce

A nonce needs a stored record per session. **A keyed derivation needs one salt for every session that will ever exist**, and regenerates any mapping on demand.

**That matters because run files get deleted.** `.mode-run/` is gitignored, bundled, and cleanup is offered at every exit. A nonce recorded there loses its link the moment the operator accepts an offer the system itself made. A salt held by the tool is untouched by any of that.

### What it costs

**The salt is a key, and keys need a home, a backup, and a rotation story.** Lose it and every past commit's provenance goes with it. That is a worse failure than the problem being solved, so the backup is not optional.

**Rotation breaks every prior derivation.** Tags already in history stop being verifiable. They stay harmless — a stale tag reveals nothing — but the provenance claim over pre-rotation commits is gone permanently.

### Redaction is the backstop, and it is not sufficient

If the salt reaches a session's context, redaction cleans the artifact. **It does not un-read it.** A salt that has been in context is a salt that has been disclosed, and the correct response is rotation, with redaction as cleanup rather than as the remedy.

**Store it outside every repository**, somewhere no session has reason to `cd` into or grep. Nothing enforces that a session does not read it; it holds by discipline, which is the right amount of machinery for it.


## UNBUILT — trans-fairy — partial history grafts

Operator's proposal, 2026-08-27: **truncate, concatenate, and cut portions out of the history being prepended or appended to a target.** Today the graft is whole-file; this makes it a selection.

**Selection is where the damage lives, and `DESIGN.md` already says so** about picking which file to graft: *"Choosing which file to graft is a selection, not a confirmation, and auto-selecting by recency is where a wrong default does real damage."* Cutting inside a file is the same class at a finer grain, and a wrong cut is quieter than a wrong file.

**A cut must leave a marker.** A history with a silent excision reads as continuous and is not. This is the same boundary `docs/redact/AUDIT-FINDINGS.md` draws with *never offer to smooth a gap* — restoring flow across a removal is fabrication, and so is removing without saying it happened.

**Under `--agent` the cut ranges are stated back before the write**, not summarised. The agent flow already prints what was written and the exact next command; a selection it cannot see is one it cannot verify.

**The guarantee is disclosure, not fidelity.** An earlier note here treated a cut history as a broken record. That was the wrong frame — the tool exists to inject context on purpose, and has never claimed to hand over a complete session. What it owes is not lying about what is there. A cut that is stated is honest; a cut that is silent is not.

**Weights cross the cut.** This is the part worth a notice of its own.

Removing text does not remove its effect on the text around it. What survives was shaped by what was excised — conclusions stay, reasoning stays, positions already settled stay settled, and the evidence for all of it is gone. An agent receiving a cut graft inherits the outcomes without the basis, and has no way to know which of its own footing is missing.

Same mechanism as `SOPIA/docs/guides/POISONED-designing-a-test-with-an-agent-POISONED.md`: *a summary carrying none of the words still carries the choices.* A cut graft is that, deliberately.

**And it is testable**, which is the reason to write it down now rather than treat it as a caveat. Graft a history with a known portion removed, then measure whether the receiving agent reproduces conclusions whose support was cut. The tool is growing in exactly the direction that makes this cheap to run.

### What the tool is actually for

The operator's framing, and it is a better one than lying-or-not: **the question is how unpredictably an agent reacts to a given context, not whether the context is authentic.**

**A conversation is a context being built one turn at a time.** This tool assembles one directly. Same artifact, different production method, and the slow way costs hours.

**A grown context is coherent because every turn answered the one before it.** An assembled one has places where something references what is no longer there. The agent does not fail on that — it fills in, and what it fills in is the unpredictable part. Not deceit. Interpolation with nothing to interpolate from.

**Judge a handcrafted context the way a grown one is judged: by what it produces.** Authenticity is not the standard, and holding it as one rules out the tool's entire purpose.

**This is already being done the slow way and already being measured.** `SOPIA/docs/guides/modes/` is a handcrafted context with a load plan, and two tests have been run against it — twelve agents on a fixture drawn from it, and two cold agents against its exit rules. The methodology for judging a crafted context exists and has been used.

**The test that follows:** hand one agent a real session's context and another a handcrafted equivalent, and measure whether behaviour matches. That answers what the coherence of a grown context is worth, which is the thing nobody currently knows.

## UNBUILT — Policy enrollment over an API — `cai` carries policy, never owns it

The operator's design, recorded 2026-08-27. Nothing here is built.

**The problem.** `SOPIA/tools/expire-grant.sh` is a repository-local script for a general problem: a time-scoped permission decays into a permanent one the moment nobody re-reads the date. The same is true of the git flow around it, and of the guide-pathing convention SOPIA just adopted. More than one repository wants these. **And centralizing rulesets is the exact failure this infrastructure has already had, at length** — see SOPIA's `docs/guides/generalized-discussion-shaping-prompt-rules.md` and the SOP chasm entry in `docs/guides/modes/rationale.md`.

This design resolves that tension rather than picking a side.

### The shape

**An API call asks what the enrollment criteria are.** It publishes criteria; it does not adjudicate. The judgement of whether a repository qualifies stays with the person.

**Operator decision only. No auto-loading, ever.** A repository does not acquire a policy by being adjacent to one, by being in the same tree, or by an agent deciding it fits. The onus of enrolling sits with the operator, per asking repository, as a deliberate act.

**The response gives the pathway to the approved git-flow procedures, not the procedures.** An address, followed on trigger. Same discipline as SOPIA's fragments table.

**The API warns that centralizing rulesets can be dangerous, in the response, every time.** Not in documentation somebody read once. The warning ships with the payload, because the payload is the hazard. It should say the other half too: centralization genuinely covers important flows and guides, and refusing it wholesale costs real coverage.

**Fail safe, which for now means asking.** Where an enrolled policy and a local rule contradict each other, stop and ask. Do not resolve, do not prefer the newer, and above all do not prefer the central one — a central rule silently winning a contradiction is how a suggestion becomes an authority nobody granted.

### The guide reader

**`cai` paths out the specified guides so nobody arrives at an adjacent policy by accident.** The failure being prevented is not confusion, it is weight bias — a reader who wanders into a neighbouring ruleset while looking for this one carries its pull into unrelated work, and cannot tell afterwards that it happened.

This is SOPIA's fragments model enforced by tooling rather than by discipline. A pointer resolves to exactly the fragment named and to nothing beside it in the directory.

### The CasAPILayer suite

**`cai` needs an API client for CasAPILayer**, which is where this work would be hosted. That is a distribution inside this suite, not a change to that repository as far as this note goes.

**CasAPILayer is another agent's territory.** If this design turns out to require endpoints that do not exist there, that is a conversation with its agent and an owner decision, not something this board can plan into.

### Open questions, none of them settled

**Does an enrollment pin a version or track upstream?** Tracking reintroduces the precise hazard — one place changes and reaches every consumer at once, with nothing able to differ without being wrong. Pinning keeps the disagreement local and visible, at the cost of drift. The `git subtree` reasoning in item 5 of the head applies almost unchanged, and probably answers it.

**Does an enrollment itself expire?** The grant-expiry pattern generalises to enrollment cleanly, and a policy adopted once and never re-affirmed is the same decay in a larger unit.

**What revokes one, and does revocation leave a tombstone?** SOPIA's convention says a correction is a new line rather than an edit, which suggests a revoked enrollment stays visible as revoked.

## ✓ `cai sync` — frozen snapshots, so a single download is enough

**Owner's ask, BUILT 2026-09-02.** `cai` should work from one download with no SOPIA reachable and no network, which means carrying what SOPIA defines. **A snapshot, not a mirror** — a mirror is whatever the source says now, a snapshot is what it said at a stated instant, and the second is the only one you can trust when the source is gone.

**Frozen AND synced, which are usually opposites here.** A synced copy tracks a source and is updated; a frozen file is immutable and carries one commit. **Each sync writes a NEW dated file rather than editing one**, so every snapshot keeps its freeze, the set tracks the source, and the earlier ones stay valid. **Append rather than mutate** — the same shape as re-freezing under a new name, and as a cut that appends instead of inserting.

**The priority, owner 2026-09-02:**

```
hosted  →  SOPIA REMOTE  →  SOPIA local  →  local sync-frozen snapshot
```

**The remote tier, owner 2026-09-02, and it sits ABOVE the working copy on purpose.** A definition is a shared decision, so **the pushed state is the agreed one.** A working tree may hold an experiment nobody else has seen, and reading policy out of it would let one machine's half-finished edit answer as though it were settled.

**Consequence worth stating rather than discovering: a local edit to a definition does not take effect until it is pushed.** That is intended, and it will surprise somebody.

**The remote is derived, not configured.** Same host, same owner, sibling repository — taken from this repository's own `origin`. Nothing is configured, so nothing can be configured wrongly. `cai-tools`' origin yields exactly SOPIA's actual remote.

**And it is read through a MIRROR rather than fetched per call.** A network round trip on every lookup would make `cai grant check` — which runs inside a git hook — depend on the network being up, and **a gate that fails when the network does is a gate that gets turned off.** The mirror refreshes on a TTL, every failure falls through to the next tier rather than raising, and `CAI_NO_REMOTE` disables it outright.

**Two more exist and are deliberately not peers of those three.** An **environment variable** sits above everything as an *override* — not a source of truth but a way to say *use this instead*, and treating it as a tier would make an accident indistinguishable from an instruction. The **embedded fallback** sits below everything as *bootstrap*, so a fresh install answers before any sync has run.

**A snapshot beats the embedded fallback because it can be dated.** Both are copies; only one states when it was taken, and *stale* without an age is not actionable.

**The hosted store is not built and its slot is already correct.** It goes ahead of the SOPIA source, everything below stays as written, and no caller changes — because callers ask `cai` rather than reading a file.

### ✓ `sync-frozen`, not `defaults-frozen`

**Owner, 2026-09-02, and it marks a distinction that had been collapsed.** Two frozen contexts exist and they are not the same thing:

| directory | holds | can say |
| --- | --- | --- |
| `defaults-frozen` | material that never had a source — a test fixture, a reference artifact | that it is unchanged |
| `sync-frozen` | dated snapshots **of** a source, taken by `cai sync` | that **and** what it is a copy of, and when |

**Both keep the one-commit freeze contract**, and the freeze check accepts either. **Only the second can be stale**, because only the second is a copy of something that moves.

**The jobs are data.** Adding something to carry is an entry in `specs.json`, not a code change — which is what makes `sync` reusable rather than three hardcoded copies of one operation.

**When the source is missing it reports and writes nothing.** That is the case snapshots exist *for*, so it is named rather than worked around: **a sync that invented content would be fabricating the very thing it was meant to preserve.**

## ✓ `fence` and `mode` — composed from both, used by several

**Owner's proposal, 2026-09-02, and the evidence supported it in different strengths.** Not everything belongs in a shape: **the tools' contracts stay as prose in the scripts**, because encoding them as data would cost verbosity and buy nothing checkable. What benefits is a concept that **several tools implement separately**.

**`fence` was the stronger case, because the two copies had already diverged.** `redact` and `notation.audit` each carried a regex for the same thing, and they were not the same regex — one captured the `/` that distinguishes an opener from a closer and one did not. **Two definitions of one concept is duplication; two that disagree is a defect waiting for the case that separates them.**

**What a fence *is*, is a shape** — a delimiter alone on a line. **Which fences exist, is a notation** — `Source`, `Draft`, `Raw Source`, the `comms` family. `cai.fence` composes the two and owns neither.

**Collapsing them surfaced a live bug the tests had been missing.** The shared pattern uses named groups, and `audit.py` read `group(1)` — which had been the name and became the closer. **Fence names came back empty and every suite still passed**, because nothing asserted the extracted name. Fixed, with a check that would have caught it.

**`mode` was duplication without divergence — yet.** The same three modes govern truncation, injection, lineage, composition and fabrication: **twelve branches across four files**, consistent only because the same words kept being typed. `cai.mode` holds what each mode emits, derives the expected record count rather than each caller writing `{"silent": 1, "loud": 2, "true-silent": 0}`, and raises one message instead of four.

## ✓ Every tool takes from both — the vocabularies consolidated

**Owner, 2026-09-02: `hook`, `grant`, `enroll` and `redact` all take from `document` and `notation` too.** True, and it was a to-do rather than an observation: **five vocabularies were hardcoded across four tools**, so `push` in a grant and `push` in a hook were the same word by coincidence rather than by construction.

**Now recorded in notation, with one home each:**

| notation | symbols |
| --- | --- |
| `grant-effect` | `allow` `deny` |
| `grant-action` | `commit` `push` |
| `enrollment-level` | `strong` `weak` `off` |
| `projection-selection` | `conversation` `turns` `tools` `all` |
| `visibility-mode` | `silent` `loud` `true-silent` |

**That last one is the interesting entry**, because it was never one word in one tool — the same three modes govern truncation, injection, lineage, composition and fabrication, and each had its own copy. **Naming it once makes the consistency a fact rather than a habit.**

**And two shapes that existed only as code are now documents**: `hook-definition` and `enrollment-marker`. The hook shape **points at `grant-action`** rather than restating it, which is what makes a grant and a hook agree by construction.

**The tools consult rather than assume, where it is cheap.** `enroll` reads its levels from notation and falls back to the built-in set; `redact` **reports** a disagreement between its table and the dictionary rather than repairing it, because the dictionary is what a person reads and a silent reconciliation would hide the drift instead of surfacing it.

## ✓ `document`, `name`, `fabricate` — and the split they made explicit

**Built 2026-09-02.** The owner's framing settled what these three are: *the entire shape of the name is the document, the symbols are the notations, and `cai name` is the implementation of both.*

**That generalises past naming.** A tool is an implementation over documents and notations — it arranges atoms whose meanings it does not own, into shapes it does not own either.

| holds | owns |
| --- | --- |
| `cai notation` | what a symbol MEANS |
| `cai document` | how parts are ARRANGED |
| a tool | what to DO with an arrangement |

**Keeping the three apart is what stops any one becoming the place everything accumulates**, which is the failure the whole suite exists to answer. **`cai name` defines nothing**: a marker's meaning is notation's, the field order is document's, and it renders, parses and explains what it is given.

**`cai document` holds two shapes so far** — the session name and the grant — with a part able to *point at* a notation rather than restate it, so a symbol has one home even when several shapes use it.

### ✓ `fabricate`, and why the name does heavy lifting

**Owner, 2026-09-02: *fabricate* means both to manufacture and to deceive**, and the tool is exactly where those meanings meet. **Every use is manufacture; the failure mode is deception.**

**So the word is a warning label that cannot fall off.** A neutral name — `insert`, `compose`, `add` — would describe the mechanism and hide the hazard, and somebody would reach for it without the second meaning ever crossing their mind.

**Three modes, the vocabulary used everywhere here:** `marked` by default, carrying `origin.kind: "fabricated"` in meta so it does not reach the model and a measurement arm is unaffected; `loud`, which announces itself in message content; and `true-silent`, which **breaks the contract** and warns at the point of use, exactly as its namesake does for a cut. **It never writes in place** — a fabrication must not overwrite the record it came from — and it has no `--agent` path.

## ✓ `cai edit` and `cai time` — two guards the session earned

**Both built 2026-09-01, and both exist because of a specific failure rather than a general worry.**

### ✓ `cai edit` — an anchored replacement that refuses to take more than it names

**Four functions were destroyed in ten minutes** by index-based slice replacement — `now`, `_parse`, `freeze_status` and its helpers — each removing more than intended, each noticed only when a test failed afterwards, **twice after the pattern had already been noticed.**

**What finally held was not more care; it was a guard.** So the guard belongs in a tool:

| refuses | because |
| --- | --- |
| an anchor matching **zero** times | the edit does nothing, which is what a silently failed replacement looks like from outside — and what leaves a commit describing work the diff does not contain |
| an anchor matching **more than once** | it would hit something it was not aimed at |
| an **undeclared** change in definition or heading count | removing something nobody mentioned is the defect itself |

**Every refusal leaves the file untouched.** A half-applied edit is worse than one that did not run, because it is found later and somewhere else.

### ✓ `cai time` — because a stamp was six hours wrong

**A freeze stamp was written as `20:20:00Z` while the local clock read 20:20 MDT.** Six hours out, in the field the entire staleness measure rests on. **It was caught by a check comparing the stamp against git's own record** — that is, by having built a second witness in the same hour, which is luck rather than process.

**Every grant, freeze stamp and boundary in this suite is a UTC instant, and every one was being typed by hand.** `cai time window 2h` computes one and **prints both renderings**, so the operator's local statement and the record's UTC can be checked against each other rather than converted in someone's head.

## ✓ One resolver, three data files — `cai.source`

**Owner's ruling, 2026-09-01: hook data should go the same way as everything else — retrievable from the filesystem until it is hosted.** With that, three data files resolve identically, and three copies of the resolution logic would have drifted. It is written once in `src/cai/source.py`.

| data | source of truth | embedded fallback |
| --- | --- | --- |
| grants | `SOPIA/docs/grants.json` | none — a missing grant source is *no grant*, never permission |
| notation dictionary | `SOPIA/docs/guides/notation-dictionary.json` | yes, dated |
| hook definitions | `SOPIA/docs/hooks.json` | yes, dated |

**The order is environment variable, then the source, then the fallback** — and it is the order a hosted store slots into. It becomes the first candidate, everything below stays as written, and **no caller changes, because callers ask `cai` rather than reading a file.** That is what makes the migration a no-op rather than a rewrite.

**Grants have no fallback on purpose.** For the other two, an out-of-date answer is worse than nothing but still usable; for a grant, **a stale copy answering *yes* would be permission granted by a file nobody is maintaining.** A read failure there returns *no grant*, never an exception a caller might mishandle into an affirmative.

**Four functions were lost and restored while doing this**, each to an index-based slice replacement that removed more than its author intended — `now`, `_parse`, `freeze_status` and its helpers. **The fix that finally held was a guard rather than more care**: every edit now asserts the function count is unchanged, and refuses to write if it is not. The same shape as everything else here — the check belongs at the point of effect, not in the resolve to be more careful next time.

## ✓ `cai enroll` — per-repo, and strong or weak by choice

**Owner's design, BUILT 2026-09-01** at `src/cai/enroll/`, 18 checks.

**Bare invocation prints what an agent should ASK, not what it should decide.** Enrollment is an operator's choice about their own repository, and a tool that picked a default would make that choice by omission — which is how a repository ends up enrolled without anyone agreeing to it. **`cai enroll` with no level refuses**, and says why.

**`off` is a recorded decision rather than the absence of one.** It writes a marker, so refusal is distinguishable from neglect — otherwise an unenrolled repository and one that declined look identical, and nobody can tell which happened.

**The installed hook is a SHIM, and everything it does lives in `cai hook`.** Owner's question: *are the hooks' main shapes being retrieved via cai?* They were not, quite — the *decision* came from `cai grant check` at runtime, but the **shape** did not: which action to ask about, what a refusal says, what it exits, all sat in the file on disk.

**That is a second home for behaviour, and the worst kind — one that only updates when somebody reinstalls it.** A repository enrolled today would have kept today's refusal message forever.

**Now the file is one `exec` line.** `cai hook pre-commit --repo <root>`, and nothing else. What stays in it is the minimum that cannot live elsewhere: which hook this is, and where the root is. **Improving the check, the message, or the exit behaviour reaches every enrolled repository at once**, without anyone reinstalling anything. The gate is currently **opt-in**: it refuses when asked, and a plain `git commit` walks straight past it. That is the honest limit of every tool in this suite, and it puts `cai commit` at *check at the point of effect* on the lesson-12 ladder rather than at *no path at all*.

**Enrollment is a per-repository ask, not a global setting.** A repository opts in deliberately, which matters because the answer differs by repository: SOPIA holds policy and wants the strong form; a scratch repository wants nothing.

| enrollment | mechanism | what it costs |
| --- | --- | --- |
| **strong** | `pre-commit` and `pre-push` hooks calling `cai commit`/`cai grant` | every commit passes the gate whether or not anyone chose to; bypass requires `--no-verify`, which is a deliberate act that leaves a trace |
| **weak** | the tool is available and nothing installs hooks | the gate is advisory, used by whoever remembers, and forgetting is the default |

**Both are legitimate and the choice is the operator's.** A repository that does not want that much enforcement should be able to say so **rather than being enrolled by default and having the hooks removed later**, which loses the record of the decision.

**What the strong form does not close, and must not.** `--no-verify` remains: **a hook that cannot be bypassed becomes a hostage situation the first time it is wrong.** What changes is that bypassing becomes visible and deliberate rather than being what happens when nobody thinks about it.

**The hatch is the operator's, and an agent may never reach for it.** Owner's ruling, 2026-09-01. **An agent that meets a refusal reports it and stops** — it does not bypass, and it does not propose bypassing as the remedy. If a bypass is warranted the operator asks for it, in their own words, having seen the refusal.

**This is the `--agent` contract applied to the escape hatch**, and it matters more here than anywhere else it appears: **the whole value of a gate is that the thing being gated cannot open it.** An agent permitted to bypass a refusal it triggered is not gated at all, and the refusal becomes a formality it narrates on the way past.

**Weak enrollment is not the absence of enforcement. It is enforcement by convergence.** Owner, 2026-09-01: *the more `cai` is used, the more it is the only tool used.* A tool that is genuinely better becomes the habit, and once it is the habit the weak form approaches the strong form in practice — **without the hostage problem, because nothing was ever forced.**

**Which makes the tool's quality the enforcement mechanism under weak enrollment**, and that is a real constraint rather than a compliment. **A tool people route around is not weakly enrolled; it is abandoned.** Every refusal that is wrong, noisy, or slow spends adoption directly, so the warn-rather-than-refuse choices in `cai commit` are not politeness — they are what keeps the convergence running.

**The asymmetry that makes this worth building.** Today's eight failures were all the same shape and none were prevented by care. **Under strong enrollment none of them reach a commit**; under weak enrollment they are caught only when someone remembers to route through the tool, which is exactly the thing that failed eight times.

## ✓ `cai commit` — the gate for a failure that kept happening

**Owner's ask, BUILT 2026-09-01** at `src/cai/commit/`, 18 checks. *If this happened for you, it may surface elsewhere.*

**Every failure it exists for is one shape: the commit ran without verifying what it depended on.** From a single session:

| what happened | how often | why |
| --- | --- | --- |
| committed with failing tests | 3 | the test run and the commit were separate statements in one shell line |
| committed after an edit raised and wrote nothing | 3 | same, leaving a message describing work the diff did not contain |
| subject named a file absent from the change | 2 | the message was written for an edit that did not land |

**None were prevented by care and all were found afterwards.** That is the signature of a check belonging at the point of effect rather than in a habit. **Chaining with `&&` fixes it for whoever remembers; this fixes it for whoever does not.**

**It refuses** an empty staged diff — which is what a silently failed edit looks like from outside — a failing `--verify` command, and a denied grant, **failing closed if a grant cannot be evaluated at all.**

**It warns rather than refuses** when the subject's scope appears nowhere in the staged paths. That heuristic caught a real case, a subject saying `CLAUDE.md` over a diff containing only `docs/grants.json` — but **a scope may legitimately name a component instead of a path, and a false positive must not block a correct commit.**

**Two bugs in the gate itself, found by reading it before running it**: a stray commit call with no stdin that would have hung or committed garbage ahead of the real one, and a contorted assignment in the verify branch. **The tool written to catch commits that skipped their checks nearly shipped with an unchecked commit inside it.**

## ✓ `cai grant issue` — the half that was missing for three weeks

**Found by a peer session, 2026-09-23, not by this repository.** `Legolas` was told to commit three `docs/port-rewrites/` files into SOPIA, asked `cai grant check` as SOPIA's `CLAUDE.md` instructs, was correctly refused, and then went looking for how a grant is obtained. `cai grant --help` lists `check`, `list`, `why`. It read the source rather than trusting the help text and confirmed the absence was real.

**The gap is not ergonomic, it is structural. Obtaining a grant meant hand-authoring JSON, and the party with the file open is the GRANTEE.** Every route that unblocks a blocked session ran through the blocked session. That is the one shape `docs/grant-shape.md` already warns about in prose — *a local grant file is written by the same party it authorises* — stated there as a limit on attestation, while the same sentence was also describing a missing feature and nobody read it that way.

**One of the three hand-written grants already carried the cost in its own note**: a start instant typed a minute into the future, so the grant silently did not apply and a different one answered.

**`--session` names the GRANTEE, who is not the caller** — which is what makes resolution different from `current_session`. Three identifier namespaces coexist on this machine and only one authorises:

| | |
| --- | --- |
| `sessionId` | the full uuid in `~/.claude/sessions` — **this is the key** |
| name | what an operator reads; not unique |
| `ListAgents` ref | a short hex tag — **not an id prefix** |

**The ref is the trap, and it is live.** The peer reported its own ref as `f6b1b6` against a `sessionId` beginning `0b1e9641` — unrelated strings. An operator pasting a ref from a `ListAgents` row would key a grant to nothing, or to an unrelated session whose id happens to start those six characters. **A bare six-hex argument is refused by shape**, and the refusal says why. Ambiguous names and non-matches are refused rather than guessed, and the resolved name *and full id* are echoed before anything is written.

**The self-grant guard refuses issuing to the calling session**, `--self-grant` plus a mandatory `--note` overrides it, and the note lands in the record. **It is a speed bump, not attestation** — anything that can write the store can write it without the CLI. It is worth having because it makes the honest path the easy one, which is the most a local file can do; only a server-side signer closes it.

**And the guard says when it could not run.** If the caller's own identity is unresolved the guard silently passes, so the written record carries a warning saying so. A guard that cannot identify the caller cannot guard, and that fails *open* — the one place in this module that does, named rather than hidden.

### Three defects on the check side, found by the same report

**1. `_parse` raised instead of denying.** One `strptime` format, so a hand-written `+00:00`, a fractional second or a minute-precision stamp was a `ValueError` out of `check` and into the caller. **An exception is not a denial**, and what a caller does with one is unspecified — which breaks the single property the rest of the design rests on. The peer flagged it as unverified, reasoning from the code; it reproduced first try. The ordinary ISO forms all read now and anything left over denies, naming the grant as `UNREADABLE`. **A malformed instant must never collapse to `None`**, because `None` means *no expiry* and would turn a typo into a permanent grant — hence a distinct `BadTimestamp` rather than a lenient parse.

**2. Unresolved identity reported as a mismatch.** With no id, every grant failed the id comparison and the verdict read `names session b5d42ad9; this is unknown` — which a reader acts on by concluding the grant belongs to someone else and giving up. The truth was that the question was unanswerable, and the fix was one flag away. **It is not a rare path**: `current_session` falls back to cwd matching, and the ordinary shape of this work — a session rooted in repository A, asked about repository B — matches nothing. The peer hit it on its first attempt, against a workflow SOPIA's own `CLAUDE.md` prescribes. It is now its own verdict, returned before any comparison, carrying `identity: unresolved` and the flag that fixes it.

**3. One store silently masked the other.** `load` read the repository file only when the ephemeral store held no grants, so a single issued grant would have hidden every hand-written one. Harmless while nothing could write — **and reachable the moment an issuer existed.** Both are read and merged now, each grant tagged with its `_origin`.

**A test found a fourth while being written**: `append` refused to create a store that did not yet exist, because `stat_token` of an absent file is `None` and the race check read that as *could not stat*. **Absent is a state, not a failure to read one** — a file that existed must be unchanged, a file that did not must still not exist, and they are different questions.

### Three more, found by USING it rather than testing it

**The first real issue revived ten expired grants.** The ephemeral store's TTL is enforced on the FILE's mtime, so a store past its TTL is ignored wholesale and fails closed — and appending rewrites that file, refreshing the mtime and bringing every stale grant inside it back into force. One grant issued at 18:47 revived ten from nineteen days earlier, and the very next verdict cited one of them as its reason. **Adding a writer is what made a read-side safety net reachable.** A write now prunes what has already lapsed; entries with no clock survive, and a malformed one is kept rather than silently deleted, because `check` denies it visibly and a deletion is not visible at all.

**`cai grant list` reported each row's status from a different grant.** It evaluated every row as an UNSCOPED question, and an unscoped question deliberately ignores action-scoped grants — so each action-scoped row got the status of whichever repository-wide grant answered instead. Two freshly issued, in-force grants listed as belonging to another session. It asks with each grant's own action now. **The same shape as the bug where the numbers came from one grant and the reason from another**, and a listing is where it is least visible, because a listing reads as a statement about each row.

**`cai grant prune`**, because the store needed a way to be cleaned that is not a text editor.

### ✓ What "a dead grant clobbering a live one" actually was

**Asked whether it is still possible, the answer needed separating into two claims, because only one of them was ever true.**

**It was never a PERMISSION clobber.** A candidate that fails — wrong session, expired, paused, spent, not yet begun — is `continue`d, never returned. The only early return of a denial inside the loop is an explicit `effect: deny` that passes every gate, and a dead grant by definition passes none. **A dead grant could not deny a live one, and could not have.** Now proved rather than reasoned: expired, paused and spent grants each placed ahead of a live one, each leaving it granted.

**It was a REPORTING clobber, and that was real.** When nothing matched, the verdict's `reason` and `grant` came from the first candidate examined — and an open-ended grant sorted as `"9999"`, so a dead one from three weeks earlier was always first. Observed: a spent one-time grant reported as *names session b5d42ad9* when the grant that mattered was the caller's own and the true answer was *spent*. **A denial a reader cannot act on is barely a denial**, which is the same rule this module already derived twice.

**One inelegance left, stated rather than hidden.** Ranking compares a bounded grant's `expires` against an open-ended grant's `granted` — two different kinds of date. When both grants match it decides only which window gets reported, never whether permission is given, so it is a tidiness question and not a correctness one.

### ✓ The key change was a migration, and it is now actually migrated

**Recording a defect is not remediating one.** The previous entry noted that changing `grant_id` orphaned the existing spend records and left it there. Asked whether that had been fixed, the answer was no — and the store proved it: a one-time commit grant that had already been used was sitting readable as *in force*, because its spend was recorded under the old three-field key and nothing looked there any more.

**A legacy record now counts against every grant that would have produced it.** The old key cannot say which sibling was spent — that ambiguity is the bug it caused — so it denies all of them. **That is the fail-closed direction**: it can deny something never used, and it can never permit something twice. New records stay precise.

**The spend records deliberately did NOT move to the runtime store.** Matching lifetimes looks tidy — grants are wiped on restart, so why keep their spends? Because `CAI_GRANTS` can point at a file that outlives a restart, and a spend record less durable than the grant it constrains would hand back a consuming grant every reboot. **Enforcement state must be at least as durable as what it enforces.**

**Two more fell out of looking.** A grant with no expiry sorted as `"9999"` and was therefore **the newest thing in the file forever**, so a dead open-ended grant from three weeks earlier outranked a live one issued minutes before and answered in its place — the verdict naming a different grant from the one that mattered, which is the failure that sort was added to fix. It ranks by `granted` when there is no window. And **a spent grant is as dead as a lapsed one**, but a consuming grant with no clock was the one kind nothing could ever remove; `prune` drops those too, which cleared four dead entries out of the live store.

### ✓ Listing is not using

**`cai grant list` was SPENDING one-time grants by reporting on them**, and reporting on path-scoped ones as *no grant recorded*.

Both are the same mistake in the same function, an hour apart. The action half was fixed when a listing evaluated every row as an unscoped question; `--path` then arrived and the paths half was missed identically — a path-scoped grant deliberately does not answer a question naming no paths, so every one of them listed as absent. And because `list` ran a real check, each row it printed consumed a use.

**A listing is a question about what exists, not a request to act**, so it passes `consume=False` and each grant's own action and paths.

**One consequence worth recording: changing `grant_id` orphaned the existing spend records.** Grants already spent under the old key read as unspent under the new one. Every affected grant here was either lapsed or expiring within minutes, but a key change is a migration of enforcement state and was not treated as one.

### ✓ A dry run must not spend a one-time grant, and a refused push must stay gated

**Two more from the same one-time grant, both found within a minute of the key collision.**

**`--dry-run` spent the permission it was previewing.** Checking is what consumes a consuming grant, and a dry run is a check — so previewing the push burned its single use, and the real push was then refused as spent. **A dry run that changes the outcome of the real run is not a dry run**, and the caller most likely to reach for one is the caller being most careful. `check` takes `consume`, a preview reports `would_spend` instead of recording one, and the gate passes `consume=not dry_run`.

**A refused push had no gated way to retry.** `--push` only ran after a successful commit, so a push refused on its own left a local commit, correct and unsendable, with no route but raw `git push` — **the gate's own bypass, reached for by the gated party.** `cai commit --push-only` pushes already-committed work through the gate, committing nothing.

### ✓ A consuming grant's key must distinguish it from its siblings

**Found in practice, on the first one-time grant ever issued.** A commit-and-push pair, `expiresAfterChecks: 1` on each. The commit succeeded; the push was refused as *spent -- this grant expired after 1 check(s) and has been checked 1 time(s)*, having never been checked at all.

`grant_id` was `repo|sessionId|granted` and **does not include the action**, so two grants issued in the same second for the same repository and session shared one consumption key. Checking either marked both spent. It now includes the action, the effect and the paths.

**It failed closed, which is the safe direction and is why it was survivable.** But a spend is enforcement state, and a key that collides silently revokes a permission somebody was granted — the failure mode is a refusal nobody can explain, which is precisely what spends a gate's adoption.

**Worth noting how it was found: by using the tool for its intended purpose, once.** Twenty-six suites and 949 checks did not, because every test that exercised consumption used a single grant.

### ✓ An unrecorded use is an unlimited grant

**Found by declining to tidy up.** Two or three agents now work these repositories at once, and `prune` was about to be run on a store holding another session's live grant. Checking why that felt wrong turned up three defects, two of them failing OPEN.

**`_record_use` swallowed its own failure.** `except OSError: pass` — so a spend that could not be written left the grant *granted* and the use unrecorded. **A one-time grant silently becoming unlimited**, which is the one direction this module must never fail in. Reproduced against a read-only directory: `granted: True, recorded: {}`. It raises now, and the check denies with the reason.

**Read-modify-write on a file every session on the machine shares is a lost update.** Two sessions checking the same one-time grant both read zero spends and were both granted. It takes an exclusive lock now, writes atomically, and twenty concurrent spends record as twenty. **A stale lock is broken rather than waited on forever**, because a crashed holder must not wedge the gate — a gate that hangs is a gate somebody turns off, and reopening the race for an instant beats never granting again.

**Truncate-in-place lost every spend on the machine if interrupted**, and every consuming grant then reads as unspent. The write is atomic.

**`prune` had no read-verify-write guard**, though it is the operation that REMOVES grants. A grant issued by another session between the read and the write would be dropped without trace — a permission somebody holds, deleted by a tidy-up. `append` has had that guard since the store was built; `prune` was written later and did not inherit it.

**And a fourth, in the fix itself.** `_use_lock(p, timeout=LOCK_TIMEOUT, stale=LOCK_STALE)` bound its bounds at definition, so rebinding the module attribute did nothing and the stale-lock branch could not be tested without waiting fifteen seconds. **`current_session` in this same file carries a comment warning about precisely that**, and it was written again three hundred lines below it. The positive control failing is what surfaced it — the assertion that the stale lock *would* be broken came back False, and the check was sound while the knob was fake. **A knob a test cannot turn is a branch nothing verifies.**

### ✓ A grant can name PATHS, not just a repository and an action

**Owner, 2026-09-23: a one-time grant to commit and push `SOPIA/docs/grant-shape.md`.** The shape could not express it — `repo` and `action` existed and nothing narrowed a grant to a file, so the nearest available grant was *the whole repository* and the narrowing lived only in the operator's intention to stage carefully.

**`--path` scopes a grant, and a directory covers what is under it.** Matching is on path SEGMENTS rather than string prefixes, so a grant naming `docs/grant` does not cover `docs/grant-shape.md` — **that is the substring-for-identity mistake this module has now made four times** in other places, and it was not going to be allowed to arrive in the one field whose whole job is narrowing.

**A path-scoped grant answers only a question that names paths**, exactly as an action-scoped one answers only a question naming an action. A caller that did not say what it is touching has not asked a question a narrow grant can answer, and matching anyway would turn *only this file* into *anything, if you do not mention it*. The verdict names any path-scoped grants in play so a caller that asked broadly knows to ask specifically.

**A push asks about what is not yet upstream.** `@{u}..HEAD` is the only honest answer to *which paths does this push touch*; without it a path-scoped grant could not gate a push at all, and the narrow half of a commit-and-push pair would be the half that does nothing.

**The gate now loads only the repository's own store**, which the per-repository split made possible and which it was still not doing.

### ✓ The store is runtime state, wiped on restart — owner's ruling, 2026-09-23

**Owner: *a grant is wiped out on a restart.*** A grant is a fact about the next hour, and anything outliving the machine's uptime is the wrong lifetime for it. A TTL enforced in software is a promise this code had already failed to keep once — its own writer defeated the mtime TTL — so the lifetime moves to the filesystem, which cannot be talked out of it.

```
$XDG_RUNTIME_DIR/cai-tools/grants/<project>.json      preferred
$TMPDIR/cai-tools-<uid>/grants/<project>.json         fallback
```

**`<project>` is the repository path in `.claude/projects` form**, so `/home/you/dev/Thing` is `-home-you-dev-Thing` and a dot becomes a dash exactly as the client does it. Matching the convention is the point: an operator who can read one directory listing can read the other.

**`XDG_RUNTIME_DIR` is preferred over `/tmp` for ownership, not taste.** Both are tmpfs here, but `/tmp` is `drwxrwxrwt root root` — a shared namespace any local user can create a directory in. **For a permission store that is a forging surface**: pre-create the path and you choose what `cai` reads. `XDG_RUNTIME_DIR` is mode 700 and uid-scoped, the same wiped-on-restart property with that problem already solved. The temp fallback is uid-scoped and its ownership and mode are **checked rather than assumed** — an unsafe directory reads as no grants, with a note, and refuses writes outright.

**Stores are written `0600`, created that way rather than chmod'd afterwards.** A chmod after the write leaves a window in which the file exists readable, and for a store that decides permissions the window is the whole problem. `os.makedirs(mode=...)` applies its mode to the last component only, so the parent chain is made private explicitly — otherwise the temp fallback leaves an intermediate directory group-writable.

**Partitioned by REPOSITORY, not by session**, because `check --repo PATH` always knows the path and does not always know who is asking. Resolving identity is the fragile step, and a lookup that depends on it inherits that fragility.

**On Windows the wiped-on-restart property does not hold** — the per-user temp directory survives a reboot — so the TTL stays as a backstop rather than being retired, and the help says which guarantee is doing the work where.

**Live grants were migrated rather than orphaned.** Moving the store silently invalidates every grant in force, including the one authorising the commit that moves it.

### ✓ No repository is a grant source — owner's ruling, 2026-09-23

**Half of this was already done and the remaining half was doing the harm.** SOPIA untracked `docs/grants.json` at `189d96b`; it has not been in a commit since. But `SOURCE_CANDIDATES` still named that path, so **an untracked file sitting in a working tree went on being found, read and served** — nineteen days after its last grant lapsed, and it is what the stale-session denial above was quoting. Untracking stopped the history from growing; it did not stop the file from answering.

`SOURCE_CANDIDATES` is now empty and `--store repo` is refused with the reason. `CAI_GRANTS` still points wherever a caller deliberately points it; what is gone is a repository being found by default. **The ephemeral store expires, and that expiry is the whole point** — a file in a repository is as durable as the repository, which is the wrong lifetime for a statement about the next hour.

**124 checks in `tests/test_grant_issue.py`**, every refusal paired with the same call succeeding once the one blocking condition is removed. Suite total 993 across 26 suites.

**What is still UNBUILT: revoke.** A grant can be issued and it expires, but nothing retracts one early. Pausing exists as a field and is honoured by `check`; nothing writes it. Worth noting that revoke-by-shadowing does not work and should not be invented — a store holding `{"grants": []}` no longer masks anything now that the two are merged.

## ✓ Grants belong in `cai`, keyed on name **and** actual `sessionId`

**Owner's ruling, BUILT 2026-09-01** at `src/cai/grant/`, 21 checks. The first concrete thing policy enrollment holds.

**`cai grant check --repo PATH` answers at the point of use**, reading `SOPIA/docs/grants.json` — SOPIA defines, cai evaluates. **It fails closed on every path**: missing source, unreadable source, unknown session, wrong repository, before the window, after it. There is no branch where uncertainty becomes permission.

**Every denial in the test suite is paired with a positive control** — the same call succeeding once the one blocking condition is removed. That is the rule this repository derived eleven times in a day, applied to the tool most likely to be trusted without checking.

**Its first act was to answer about the grant it was written under**, correctly, including how long remained.

### ✓ Deny, action scope, and expiry on use — added under test, 2026-09-01

**The owner tested it by granting a denial**: a grant refusing `git status` that **expired on its first check** rather than on a clock. Neither was expressible; both are now.

**A grant may `deny`, not only allow.** An explicit prohibition is not the same as the absence of a permission — **absence is silence and a deny is a statement**, and reporting them identically loses that.

**`expiresAfterChecks: N` spends a grant by being asked.** Checking it is what consumes it, so a consuming grant answers differently the second time, by design, and the verdict says so. **Consumption is enforcement state and lives on the `cai` side**, so the grant file stays a statement of intent rather than being rewritten by the act of reading it.

**Two ordering bugs the test found, and they are the same mistake mirrored.**

**A broad allow overrode a narrow deny.** The repository-wide grant matched first and returned before the action-scoped deny was reached — **a general permission silently defeating a specific prohibition, with nothing reporting the conflict.** That is the worst available ordering in a permission system. Denies are evaluated first now, and within an effect the action-scoped before the repository-wide.

**Then a narrow deny denied an unscoped question.** Asking *may I work in this repository* was refused by a prohibition on one unrelated action. **An unscoped check now ignores action-scoped grants and names them in the verdict**, so a caller that asked generally knows to ask specifically.

**A third was in the seed rather than the code**, recorded because the tool reported it correctly and I read past it: the deny carried a `granted` instant a minute in the future, so it was skipped as not-yet-begun and the allow answered. **The verdict said exactly that.**

**A grant expiry cannot live in `CLAUDE.md`, and that is not a lapse.** The file is read once at session start, so an expiry written there reads as active for the life of the session regardless of the clock. **Leaving a grant out of it is the correct application of the static-versus-dynamic rule, not a failure to record one** — a point this repository got backwards once, in a commit message, before the owner corrected it.

**So the grant is asked for, not read.** `cai` holds it and evaluates it at the point of use, which is the only shape a time-scoped permission can take. The document keeps the pointer, because a pointer is static; the tool computes the answer, because the answer is not.

**The key is the pair: the session's given name and its actual `sessionId`.** The name is what an operator reads and what carries lineage, and the id is what makes the grant unforgeable — and the pair matters because the two drift apart on purpose. **A slice inside a name records where a conversation came from; the `sessionId` records which file this is.** A grant keyed on the name alone would be inherited by anything wearing that name.

**Which closes the re-root gap without a rule about re-roots.** A composed session inherits its predecessor's context — including the belief that a grant is live — but it is minted with a new `sessionId`, so it simply does not match a grant naming the old one. **The inheritance problem disappears into the key rather than needing a clause**, which is the structural form rather than the behavioural one.

**It also removes the standing residual.** `tools/expire-grant.sh` needs no hook once nothing is reading an expiry out of a snapshot, because there is no snapshot to go stale.

## Policy enrollment — the three open questions, answered

Operator decisions, 2026-08-27, against the section above. Appended rather than edited in, per the ceiling pattern.

**Enrollment pins a version, and updates arrive on a new call.** Nothing is pushed to a consumer. A repository's policy changes when that repository asks and at no other moment, which is what keeps the central copy from reaching every consumer at once. Drift is the accepted cost and it is the cheaper one — a stale pinned policy is visible and local, where a silently-updated one is neither.

**Grants come from a fixed menu rather than a free duration:** 1 hour, 6 hours, 12 hours, 24 hours, 1 day, 1 week, 1 month. A closed set means a grant length is chosen rather than invented, and the menu is auditable in a way that arbitrary strings are not.

> **Unresolved in the menu:** `24 hours` and `1 day` are listed separately. If they are the same span, one should go. If `1 day` means to the end of the calendar day and `24 hours` means rolling from the grant, that is a real distinction and needs saying, because the two diverge by up to a day depending on when the grant was issued. Left as the operator wrote it pending her answer.

**The API layer carries a refresh endpoint**, on the model of SOPIA's `docs/guides/modes/refresh-mode-rules.md`. It refreshes the tracked policies in the context rather than re-enrolling anything, so a long-running session re-reads what it is operating under instead of running on what it loaded hours ago.

**The refresh has to be observable, and this is not a detail.** SOPIA's control layer emits a marker whenever it fires, and it exists because a session ran an entire multi-hour run without emitting one — the refresh was specified, believed to be running, and never fired, and nobody could tell from the outside. **A refresh you cannot see fire is a refresh that silently does not.** The endpoint should return something the caller is obliged to surface, not a 204.

## CasAPILayer — deliberately not looped in

**Its agent knows the forward-looking idea and none of the implementation.** That is the operator's call, made 2026-08-27, and the state is intentional rather than an oversight to correct.

Nothing in the policy-enrollment design is to be raised with that repository until the operator opens it.

## UNBUILT — The grant menu, settled — and the refresh index

Operator decisions, 2026-08-27, resolving the menu ambiguity flagged above.

**`1 day` means end of day, local time, from the moment of the grant. `24 hours` means rolling from that same moment.** They are different spans on purpose and the labels now say which is which.

**The prompt tells the operator that another duration can be requested.** The menu is a set of defaults, not a ceiling — it exists so a common grant is chosen rather than invented, and an uncommon one is still available by asking. A closed menu with no escape hatch produces the wrong grant chosen because it was on the list.

### The refresh index

**Same flow as SOPIA's `refresh-mode-rules`, with a different payload.** What comes back is an index of current enrollments and the grant period allotted to each, so a session can see what it is operating under and for how much longer.

**An exit clause requires the agent to notify the operator.** A grant ending is an event the person hears about, not a state they discover later by reading a date.

**Grants nest, and the outer one is a ceiling.** The overall grant timing out invalidates every other grant beneath it. Individual grants may be issued for shorter spans and expire on their own without touching the rest. Nothing beneath the outer grant can outlive it.

### The direction this is heading

**A file read becomes an API call, and the call is forbidden without an operator grant.** That is the point of the whole design — policy arrives because it was asked for, under a permission with an expiry, and is auditable both ways.

### Two things the design has to answer before it is built

**The bootstrap.** If calls require a grant, an agent holding no grant cannot call to discover that it holds no grant. There must be one always-available endpoint that answers *what do you have* and returns no policy content whatsoever. Without it the failure mode is silence, and **silence reads as compliance** — an agent with no grant and no way to learn it behaves exactly like an agent correctly operating under none.

**"Falls out of context" is not something a grant can actually do.** An expired grant stops being authoritative. It does not stop weighting. Anything an agent has already read stays in the transcript, survives compaction, and keeps pulling — this infrastructure has the evidence for that written up under contamination, and the correction there was to flag the context rather than to believe it had been cleared. The honest specification is that an expired grant **revokes authority and leaves influence behind**, and that a session which has held a policy and lost it is not equivalent to one that never held it. Where that difference matters, the answer is a fresh session, not a refresh call.

## UNBUILT — `cai` is the gate, and that answers both open questions

Operator resolution, 2026-08-27, against the two problems raised above. This is the load-bearing entry for the whole design.

**The agent does not hold the grant, does not make the call, and does not need the endpoints.** It asks `cai`. `cai` holds the grant and makes the call. What is forbidden is a direct call against a known endpoint path — and an agent that was never given the paths has no rule it can break, because it has nothing to break it with.

> We don't need the grant to stay in context. We don't need the agent to be uncontaminated. We just need cai to do its job as a gate keep.

**That is the answer to the bootstrap problem.** There is no bootstrap. Talking to `cai` never required a grant; only the endpoints do, and the agent does not have them. The initial onboarding call stays permitted so a context can be refreshed around the rule rather than tripping over it. **A rule the subject cannot know about is not a rule, it is a trap** — an agent following a perfectly ordinary procedure until the call lands has done nothing wrong, and the API says so in the response rather than treating it as a violation.

**And it is the answer to the contamination problem, by making it not a security property at all.** A grant living in context was always advisory — the agent it constrains is the same agent that decides whether to honour it, and no amount of expiry language changes that. A grant living in `cai` is enforced by something the agent cannot reason its way around. **A permission cannot be enforced inside the thing being permitted.** Everything upstream of this entry was trying to, and this is why the previous procedure's version of the same idea could never have worked no matter how it was worded.

**Contamination stops being fatal and becomes ordinary.** A poisoned context is still a real problem for the quality of the work. It is no longer a problem for whether the work is *allowed*.

### Brokering

**`cai` can open an agent of its own and broker an agreement, returned to the operator for approval.** The brokered agent operates under `cai`'s grant rather than one of its own, and its output is a proposal rather than an act — approval stays with the person, which is the same split the weighting pipeline's stage three already uses.

### What non-exposure does and does not buy

**Not exposing the endpoints reduces the surface. It is not the enforcement.** Paths leak — from a transcript, a config file, an error message, a packet. The enforcement is the API refusing any call that does not carry `cai`'s grant credential, and that has to be built even if no agent is ever told a path. **Obscurity prevents the accident; the credential check prevents the violation.** Ship both and rely on the second.

## UNBUILT — Where the credential lives — deferred, not dismissed

**`cai` sends requests from a machine, so the credential is on that machine.** Moving enforcement out of the agent's context moved it into the tool's environment; it did not make it disappear. This is a real tightening item and the operator's position is that it is likely fine for now and worth a proper discussion later. Recorded so the discussion happens on purpose rather than after an incident.

**What carries the weight in the meantime is conditioning, not secrecy.** The API index refresher should push hard on not making the calls directly, and the operator's reasoning is that this is unusually easy to comply with: the agent has reached a tool that is the known interface to the API, so routing through it is the obvious move rather than a sacrifice. A rule that asks for the thing the situation already suggests is the cheapest kind to hold.

**That claim is testable and is being tested.** See SOPIA's `docs/tests/api-refresher-fallout/DESIGN.md` — how far the conditioning survives once the refresher stops firing.

## UNBUILT — Agents see policy names, never endpoints — partly reachable already

**Owner, 2026-09-02: almost there at the filesystem level.** `cai flow` names sequences without exposing what runs them, `cai notation` and `cai document` resolve names to meanings and shapes, and between them an agent already works in names rather than targets for the local case. **What is missing is the hosted half**, where the indirection stops being a convention and becomes something an endpoint enforces. Marked UNBUILT because it is not done, not because nothing exists.

Operator clarification, 2026-08-27, correcting a gap in how the entries above were read.

**When `cai` carries out the API calls, the agent is blind to every endpoint.** It is not that paths are withheld as a precaution. There is no path in the agent's world at all. **What an agent gets is a policy name.** The only way one of these agents learns an endpoint is by going looking for it, which is a deliberate act and a different conversation from an accident.

**An agent may not carry out a call it was not explicitly asked for.** This matters most in the case that looks helpful: the operator says something like *remind me what the policy is here*, which is an indirection, not an instruction to go and fetch. **The agent's job there is to say what it would take, not to take it.** An agent that quietly converts a passing remark into a call has made a permission decision on the operator's behalf.

### Rejections re-condition, and that is a feature

The worked case the operator gave:

1. A grant has expired.
2. The operator asks for a rules refresh through `cai`.
3. `cai` rejects it, states that calls on this endpoint are not permitted because the grant expired, and asks for the re-grant flow.

**The rejection carries the rule with it.** Every refusal restates why, so the conditioning is repaired at exactly the moment it was about to be violated. This is a self-healing property the file-based version never had — a file only teaches when it is read, and a rejection teaches when it matters.

> **Term collision, flagged rather than resolved.** The operator called step 3 a *reflow*. In SOPIA `reflow` already means the text operation in `tools/reflow.py`, tied to the never-hard-wrap rule. These are two unrelated things sharing a word, and the re-grant sense is the newer one. Worth naming differently before either gets written into a tool surface.

> **Resolved the same day.** The step is **`re-grant`**, by owner ruling — *"Keep re-grant. That's better terminology for what it does."* **The ruling stands; the sentence that used to follow it did not.** It said `reflow` keeps two separate senses — the text operation and the operator's *run the flow again*. **Those converged on 2026-09-03** and the current meaning is in `SOPIA/docs/guides/notation-dictionary.json` under `reflow-operation`. **Left visible rather than deleted, because it is the worked example of the rule that caught it**: a quotation is citable as history, a factual claim standing beside it is not, and this pair sat in one blockquote for a week. Recorded at `SOPIA/docs/quotes/arch-quotes/2026-08-27-reflow-and-re-grant.md`, which is citable.

## UNBUILT — Session-name maintenance — `cai` as the enforcer

SOPIA's `docs/guides/interaction-conventions.md` defines a session-name shape: working directory, lineage markers, an operator HEAD marker, and eight characters of the session uuid. The uuid slice is the same one the commit trailer carries and the same one an agent signs an `r/w` contribution with, so one identifier addresses a session, signs its commits, and signs its edits.

**Almost all of it is derivable and none of that should be a person's job.** `~/.claude/sessions/*.json` already carries liveness, `kind` (`bg` or interactive), cwd, jobId and name for every live session on the machine. From that, `cai` can determine which sessions are forks, which have spawned forks that are still open, and which have gone away.

**Which makes the markers maintainable rather than remembered:**

- **`⑂`** set on a session that is a fork.
- **`⑃`** set on a session with open children, and **removed when the last one closes** — this is the one that decays fastest by hand, because nothing announces a fork ending.
- Name collisions detectable directly: on 2026-08-27 three sessions shared a working directory and two shared the exact name, which is the ambiguity SOPIA's `CLAUDE.md` says to stop on rather than guess through.

**The one marker `cai` must not set is the pivot.** `⚑`, long form `(╯°Д°)╯︵ ┻━┻`, marks the point the work turns around — not where the operator is typing, which is why it cannot be derived from liveness. That is intent, and it is unique with nothing enforcing it.

**But the demotion is `cai`'s.** Promoting a fork to pivot turns the previous pivot into an anchor, `⚓` or `┬─┬ ノ( ゜-゜ノ)`, in the same act. The operator chooses the promotion; the tool performs the consequence, because two separate steps is where the old pivot survives beside the new one.

**`LOC` is the counter-example and is worth building against.** Which session the operator is typing in is pure fact, derivable from `status` and `updatedAt`, and it must never be written into a name — a stored copy goes stale on every window switch and turns each switch into a rename. **`cai` answers it on request instead.** That is the general shape rather than a special case: serve fact on request, store only choice.

**The division is the same as the grant flow: the tool holds what is true, the operator sets what is chosen.**

**Verified mechanics.** A session's `name` field can be written in place while the process is running and survives subsequent harness writes — tested on 2026-08-27, the name held across an `updatedAt` advance. So maintenance does not require restarting anything.

## UNBUILT — Signed peer messaging — and why the ban is unnecessary

Recorded 2026-08-28. **Far-fetched against current use** — the operator's framing — and kept because the shape is right for secured internal process communication generally, not because anything is exposed today.

**The gap it closes.** Permission boundaries are per session; messages are not. An agent refused an action in its own session can ask a peer to perform it, the peer has its own permissions, and nothing structural stands between the request and the act. SOPIA's `docs/guides/interaction-conventions.md` records this as a live gap and can only answer it with a norm, because a convention cannot verify anything.

**A message header cannot vouch for its sender.** The convention carries the sender's session name and hash, which is useful for addressing and is a claim by the sender about the sender. **Signing turns it into something a receiver checks rather than trusts.**

### The ban is the weak half, and it is not needed

**"Route everything through `cai` and forbid direct peer-to-peer" is two rules of unequal strength.** Signing is structural: an unsigned message is detectably unsigned, and no trust is involved. Forbidding `SendMessage` is behavioural: every context has to honour it, forever, which is the arrangement this suite exists to replace.

**So drop the ban.** If receivers reject anything unsigned, **direct peer-to-peer becomes useless rather than forbidden** — the channel is still open and carries nothing. Nobody is asked not to use it. This is the same move as `cai` holding the grant instead of the agent: do not close the door, make what comes through it inert.

**And it degrades correctly.** An identity claim decides who a receiver thinks it is talking to; permissions live on the receiver and are granted by the operator there. Getting identity wrong costs a misdirected reply and cannot cost an action, signed or not. Signing raises the ceiling without the floor depending on it.

### What it inherits

**The signing key lives on the machine `cai` runs from**, which is the credential-location item already recorded above as deferred rather than dismissed. This does not add a problem; it makes an existing one load-bearing in a second place, and that is worth knowing before the tightening discussion happens rather than after.

## UNBUILT — Auto-pipe — a grant, not a mode

Operator's, 2026-08-28. A session may relay to a named peer without a prompt per message, **under a grant, with `cai` signing what goes through.**

**It reuses the machinery already specified rather than adding any.** Same closed duration menu, same expiry, same re-grant flow, same exit clause: when the grant lapses the pipe closes and the operator is told. *Auto-pipe to `<peer name and hash>`, 1 hour* is a grant in exactly the sense the enrollment design already means.

**The sign-off is what makes removing the prompt safe.** Relayed traffic stays signed by `cai`, so it remains attributable and verifiable whether or not anyone was asked. **Auto means unprompted. It never means anonymous.** Without the signature this is indistinguishable from an agent sending whatever it likes, which is the current state and the reason the item exists.

### Two properties it must have

**It grants sending and never acting.** The receiver-side rule holds regardless — a peer does not act on a message without the operator asking, in that peer's own context. So the blast radius of a badly scoped auto-pipe is bounded at *noise arrives somewhere that will not act on it*, which is a recoverable failure rather than a compounding one.

**It logs.** A grant that removes the prompt must not also remove the record. **Detectability is the property being traded away by going auto, so it has to be bought back explicitly** — what was piped, to whom, under which grant, and when. Otherwise the convenience is paid for with the one thing that makes a silent failure noticeable.

### Why this is the shape rather than a toggle

**A mode toggle has no expiry, no scope and no record.** It is on until somebody turns it off, which is the decay every other item on this board is written against. A grant is scoped to a peer, bounded in time, revocable, and leaves a trail — and it is the same object the operator already uses for everything else, so it needs no new vocabulary and no new failure modes.

## UNBUILT — `--bare` — the guaranteed floor, and what it gives the neovim client

Confirmed against the CLI's own documentation on 2026-08-28 rather than inferred.

**`claude -p --bare` suppresses MCP servers, hooks, skills and `CLAUDE.md`.** A session started that way carries nothing from the environment it happened to launch in.

**For the neovim client that is a contract rather than a convenience.** Item 4 on this board consumes the headless-session pattern, and without `--bare` an invocation inherits whatever the user's machine has configured that day — a different toolset, different instructions, different behaviour, none of it the editor's choice. **With it, the client decides everything the invocation sees.** *Works if your config is sane* becomes *works*.

**It answers the MCP problem by default instead of by discipline.** Every connected server's tool definitions occupy context on every turn whether or not they are used, and their descriptions are text the model reads and is guided by. That is ambient weighting the invocation never asked for, and `--bare` is the off switch.

**And it is the clean arm this suite has been missing.** A test needing an agent with no repository instructions previously needed one spawned from an unrelated working directory, which only moved the problem. One flag now does it properly, which makes matched-control designs cheap where they were awkward. The 2026-08-27 exposure battery in SOPIA had to record inherited `CLAUDE.md` as a confound in both arms; with `--bare` it would not have been one.

**Recorded limitation:** it is a launch-time decision. Nothing applies it to a session already running.

## Awaiting the operator — four items, none of them an agent's to settle

Surfaced by the 2026-08-28 transcript recovery. **Two are recorded in `transfairy/DESIGN.md` as well and are repeated here because this is the board she reads; two exist nowhere else.**

### 1. Interspersed-row timestamps — a principle call

> **Precedence, operator's ruling 2026-08-28: everything she has said about interspersion supersedes everything recovered from the transcript.** Both are kept for now, and where they conflict hers governs. The recovered material below is retained as subordinate rather than as a rival position.

**Her framing:** interspersion exists to measure an anticipated change in behaviour. *"We're not measuring an agent's honesty, we're actually anticipating a change in behaviour, and that's what the tool is for with the intersperse."*

**That bears directly on the timestamp question**, because the objection below is an honesty objection and she has ruled that honesty is the wrong frame for this operation. The two senses were separated in `DESIGN.md`: the record's honesty is a property that is preserved and is not under test, and what interspersion measures is behaviour.

**The recovered position, subordinate.** True authoring time is honest and makes the clock jump mid-stream; interpolating between neighbours is monotonic and is a fabrication. The question went dormant when open item `0a` dropped interspersion and became live again when the interweaving section reinstated position as a first-class parameter. The recovered framing calls it a fabrication inside a design whose stated purpose is that a record never misrepresents how material arrived.

**What is actually still open** is narrower than the recovered version makes it look: whether an interpolated timestamp counts as misrepresenting *how material arrived*, given that every grafted row is already self-labelled and self-bounded and therefore announces its own provenance regardless of what its clock says.

Also in `transfairy/DESIGN.md`, under *Injection and interweaving → Open*.

### 2. Restructure versus freeze — a conflict with no ruling

**Running the original thirteen-file restructure would be a large, deliberate delta to the artifact currently being used as a control.**

There is a recommendation on record to defer it. There is no ruling either way, and the longer it sits the more the restructure costs. **Recorded nowhere else.**

### 3. Two verification commands that have never been run

**Do meta records reach the model's context?** Attachment records are known to. Whether `system` records do decides whether the seam's text marker is belt-and-braces or the only thing carrying it.

**Does Claude Code tolerate an unknown `origin.kind` or an unknown `system.subtype`?** Both are additive and *should* be ignored gracefully. If unknown `origin.kind` values are rejected, `origin.kind: "graft"` breaks loading outright.

**The whole marker design rests on both, each is one command, and neither was run.** Recorded as `transfairy/DESIGN.md` open items 11 and 12.

### 4. A `ps` dump sits in the source transcript

**Record 1202: an unbounded `ps` put desktop configuration — feature flags, deployment mode, paths — into that session's transcript.** The agent that ran it flagged it at the time as low sensitivity and exactly the kind of thing being guarded against.

> **Corrected 2026-08-28, and the correction is the useful part.** This was first recorded as *the dump is in `EXTRACT-CANDIDATE.md` and would travel with anything bundled from it*. **It is not.** The extract carries only record 1202's prose mention — `claude-desktop` appears once, `ps` invocations zero, against a control term returning 435 through the same search. **The output is in the source transcript `dc7fafee`, not in the extract.**
>
> **That changes the remediation entirely.** Cleaning the extract does nothing; the transcript is the artifact. And the claim reached this board because a peer reported it and it was written in without being checked — which is the failure the *verify before citing* discipline exists to catch, arriving through a trusted channel rather than an untrusted one.

**It still bears on the shipped-template decision.** *The content is an entire private conversation* is not an abstraction, and this is a concrete instance of why the recommendation is a stripped skeleton rather than the real transcript.

**And it is the material the security-redaction regime exists for**, in a transcript nobody has run that tool against.

## trans-fairy — what a full read-through of the six documents found, 2026-08-28

**None of these are decisions and none of them are new design.** They are places where the documents disagree with each other or with themselves, found by reading all 1,866 lines in one pass after the recovery had roughly doubled two of them. **Ranked by whether a reader who follows the document gets hurt.**

### 1. The global flags block contradicts the tree section, and one of the four is harmful

`DESIGN.md`'s *Global flags* was written against the old `work-root = parent of bin/` model. The *Tree* section replaced that model with `<tempdir>` and `<datadir>` and the flags block never moved.

| flags block | tree section |
| --- | --- |
| `--work-root` default: parent of `bin/` | `<tempdir>`, the platform temp directory |
| `--pool` default: `<work-root>/bak/uuid-pool.txt` | `<datadir>/trans-fairy/uuid-pool.txt` |
| `--stage-dir` default: `<work-root>/tmp` | `<tempdir>/trans-fairy-<job>/staged/` |
| `--data-root` absent | *"Override with `--data-root`"* |

**The pool row is the one that matters.** Under the flags block the pool defaults into temp — which is the exact failure the tree section names and argues against by name: a cleaned temp directory means the next run mints fresh ids, and that is the stray-second-transcript case. **The document specifies the defect it forbids**, and an implementer working from the flag table rather than the prose would build it.

### ✓ 2. `graft` and `truncate` — they DO have CLI surfaces, and this entry was stale

**Corrected 2026-09-02, owner: *"`graft` is an install sub-command or flag"* and *"`split` is `cut` is `truncate`"*.** Verified against the parsers rather than the prose: `install --graft-onto` is graft's surface, and `split --truncate --at-line --at-uuid --before-text --keep` is truncation's. **Both were reachable; this entry has been wrong since 2026-08-28.**

**And I marked it UNBUILT during the sweep without checking, which is the sweep's own failure mode.** A false `UNBUILT` is the mirror of a false `✓` and arguably worse: a missing tick hides finished work, while a wrong `UNBUILT` sends someone to build a thing that already exists. **The rule that follows: a status mark is a claim about code and has to be verified against code, never against the heading that made the claim.**

**The real finding underneath is a naming one.** One operation carries three names — `truncate` as the function, `--truncate` as a flag, `split` as the sub-command it lives under, and *cut* throughout the prose. That is exactly the drift `cai notation` governs, and it is why the entry read as unbuilt: a reader looking for a `truncate` sub-command finds none and concludes it does not exist. **Four names, one operation, and the sub-command is named after the other thing it also does.**

The sub-command list carries `init`, `split`, `build`, `install`, `state`. **Four sections specify graft** — write semantics, landing approval, design intent, the scoped-token grammar — and `--graft-onto` is load-bearing in two of them. It appears in no flag block, no sub-command, and no stage row.

**Tail truncation has the same gap.** It is designed, it was scoped in by the operator on 2026-08-28, and it has no verb.

### 3. Two things are called *exists* and they are different things

*Nothing is built* and the stage table's `build` — *exists* — are both true and refer to different artifacts: there is no program, and there is a hand-driven reference implementation. Nothing in either document says which is meant. **Addressed in the new `docs/transfairy/README.md`**, which states the split rather than changing either line.

### 4. One fact, several homes — inside the directory that states the rule

- **The three-part naming scheme** (plain name, `NN-`, `NNN-`) is in `NAMING.md`, in `DESIGN.md` open item 4, and again in `DESIGN.md`'s *Naming, as used in `day-one/`*. Three copies.
- **The foreclosure governance rules** are stated in full twice, in `undecided-fairy-dust.md` and in `BUILDING` lesson 15a.
- **Lesson 11 restates lesson 10's release-phrase device**, including the same anecdote about the plan that would have restored an undamaged file. Known — `META-LEDGER.md` item 4 already calls it *the wanted 11-into-10 fold*.

### 5. Two ledger items are stale and one board entry here is

`META-LEDGER.md` item 5 reads *"`PIPELINE.md` does not exist."* It does. **The ledger's own rule is that a resolved item becomes a lesson or is deleted**; this one got an update note appended instead and stayed on the list.

`META-LEDGER.md` item 3 — no entry point, importance 3 — **is now closed by `docs/transfairy/README.md`.**

**And this board carries one too.** The `## trans-fairy` section above says *the header of `DESIGN.md` is stale*, naming `~/some-notes/day-one/` as the living copy. That was corrected on 2026-08-28: `cai-tools` is the living copy by owner ruling, and the old header is preserved in a blockquote as the failure it was. **Recorded as a new entry rather than an edit**, per the ceiling pattern.

### 6. Legibility, cheap to fix and nobody is hurt by it

**The open-item numbering in `DESIGN.md` runs 0a, 0, 1, 2, 3, 11, 12, 4, 5, 6, 7, 8, 9, 10.** Items 11 and 12 sit between 3 and 4.

**`--model` and `--effort` appear in the global flags block and are never explained anywhere.**

**Two `Incomplete` markers wait on the same referent** — which late change would have broken which protection, lesson 16a. They are in `BUILDING` and in `undecided-fairy-dust.md`, and the lesson is substantially weaker without it.

**Wrapping is split down the middle of the directory.** `BUILDING`, `META-LEDGER`, `NAMING` and `undecided-fairy-dust` hard-wrap near 79 columns; `DESIGN` and `PIPELINE` do not. Not a violation of anything stated in this repository, but it is a convention fork inside one directory and it will keep producing noisy diffs.

## ✓ `defaults-frozen` — the freeze contract, and what it found

**Named by the owner 2026-09-01.** The name carries both halves: **`frozen` is the contract** — immutable, one commit — and **`defaults` is what the contents are for**, the values used when a source cannot be reached. A directory whose name states its own contract needs no separate rule about what may go in it.

**Two gates, and neither is sufficient alone.** A frozen file writes its instant into itself; git records the commit independently — two records of one fact from different systems, neither able to forge the other.

| gate | checks | needs a clock |
| --- | --- | --- |
| **stamp** | the last commit is not meaningfully later than the stamp the file carries | yes |
| **history** | **exactly one commit touches the path** — the move that froze it — and it sits under `defaults-frozen` | **no** |
| **name** | the stamp records the filename it was frozen under, and it still matches | **no** |

**The history gate is the owner's, and it is the stronger one.** A second commit is an edit whatever the timestamps say. **That it needs no clock stopped mattering theoretically within minutes**: the stamp gate's first real finding was a stamp written as local time labelled `Z`, wrong by six hours — a clock error inside the clock check, in the field the whole staleness measure rests on.

**The name gate closes a bypass in the history gate, and the owner found it.** **Rename a tampered file and its new path carries exactly one commit** — which the history gate passes. Recording the name in the stamp catches it: the stamp names one file and the path names another.

**It is also what makes re-freezing possible at all.** A frozen file can never take a second commit, so an update is **a move plus a name change** — the new path holds the contract on one commit while the old path keeps its own. **Both remain valid**, which is append rather than mutate, the same shape used everywhere else here.

**A file that cannot carry an internal stamp gets a sidecar, and the sidecar is REQUIRED.** `shapes.jsonl` has nowhere to put a stamp, so `shapes.jsonl.frozen` carries one and **names the file it vouches for** — itself JSON, itself frozen, so all three gates apply to the pair.

**Requiring it is the whole point, and the first attempt got this wrong.** The first version *downgraded* to the history gate when no sidecar was found — and **that downgrade was the bypass**: renaming the file orphans its sidecar, the lookup follows the new name, finds nothing, and falls back to the weaker check. Absence of evidence was being read as absence of a requirement. **A missing sidecar now fails.**

**One edge the fix surfaced: a single-record JSONL file is valid JSON.** Format alone cannot decide which path applies, so a file that parses but claims nothing falls through to its sidecar rather than concluding from the parse. A JSONL fixture has nowhere to put one, and failing it for being the wrong format would be the tool refusing a question it can answer.

**`synced` is not `frozen`.** A copy that tracks a source is updated on purpose and has many commits; it reports staleness and makes no freeze claim. Conflating them was a defect in the first implementation.

### What the strategy found before it was applied

**`test_grammar` could not run on a fresh clone.** Its only input, `shapes.jsonl`, was excluded by a broad `*.jsonl` rule in `.gitignore` — a rule that exists to keep transcripts out of the repository and was **silently taking the grammar suite's fixture with them.** Verified by cloning and watching the suite raise `FileNotFoundError`.

**It was found by asking what would naturally live in the new directory**, before anything was moved there. The fixture now sits in `tests/defaults-frozen/`, is tracked by an explicit negation, and passes the history gate at one commit.

## ✓ The division: SOPIA defines, `cai` enforces

**Owner's architecture, 2026-09-01.**

> **`SOPIA`** — policy, definitions, rules, shapes.
> **`cai`** — the enforcer.
> **Where SOPIA cannot enforce, `cai` can. Where `cai` cannot define, SOPIA can.**

**The data lives in SOPIA; `cai` is the fetcher** — and that stays true when the data moves to a hosted store. The store changes; the shape does not, which makes the migration a **no-op for every caller**, because they were already asking `cai` rather than reading a file.

**It explains a failure this repository spent hours on today.** The grant expiry problem was **SOPIA attempting enforcement**: a time-scoped permission written into `CLAUDE.md`, a document read once at session start, which cannot evaluate a clock. Under this division it could not have happened — the grant is a *rule*, so SOPIA defines it, and evaluation at the point of use is *enforcement*, so `cai` does that. **The static-versus-dynamic lesson is a special case of this split**, arrived at from the other end.

### ✓ The dictionary moved to SOPIA, 2026-09-01

**`cai-tools/src/cai/notation/dictionary.json` holds sixteen definitions, and they are definitions — SOPIA's half.** Seeded, in fact, out of `SOPIA/docs/guides/interaction-conventions.md`, so the data's origin was already there and the copy walked to the wrong repository.

**The concrete cost is a two-homes drift with a count on it**: sixteen structured definitions in `cai-tools` and ten bolded marker definitions in the SOPIA guide, in **different repositories**, with nothing checking that they agree. That is the failure this whole thread has been closing, committed by the session that recorded it.

**Done.** The dictionary is `SOPIA/docs/guides/notation-dictionary.json`, beside the guide it pairs with — **rule and reason for the same subject, one home each**, with the guide now pointing at it rather than competing with it. `cai notation` resolves by env var, then that path, then its own embedded copy, **which is stamped as a dated fallback rather than the source and reports how far behind it is.** That resolution order is the one a hosted store slots into, which is what makes that migration a no-op for callers.

## ✓ `cai fabricate` — the inverse of `redact`, kept separate to keep `redact` honest

**Owner's proposal, 2026-09-01. ✓ BUILT 2026-09-02, and this line was stale.** `redact` removes. `fabricate` adds. **They are opposites and belong in different tools**, for the same reason `trans-fairy-write` is not a flag on `trans-fairy`.

**The proposal the owner rejected before it took flight names the hazard exactly:** *what if we redacted the void between two characters with content?* — an insertion dressed as a removal. It would work, and it would make `redact` a tool that does two opposite things while claiming one. **Not dishonest, but no longer honest by construction**, which is the property worth keeping: a tool that can only remove cannot be asked to add.

**Its first caller is the re-root fidelity test**, where fabricated content is a condition to be varied rather than an accident to be avoided.

**What it will need to answer**, unbuilt: whether it marks what it fabricated, and whether a mode exists that does not — carrying the same contract-breaking warning `true-silent` does; whether it may write into a live transcript or refuses as `trans-fairy-write` does; and what it declines outright. **The `--agent` contract applies** — an agent never answers a prompt that manufactures content into a record.

## ✓ `cai notation` — the living dictionary, and the ledger of what changed

**Owner's proposal, BUILT 2026-09-01** at `src/cai/notation/`, 32 checks. **The local set only** — the hosted store it is specced against does not exist yet, so what is built is the fallback the design already calls for: the set an outside adopter runs on when no connection can be established. The hosted store later becomes an accessor swap, with this copy demoted to stale.

**Seeded with eleven definitions carrying eighteen symbols**, taken from `SOPIA/docs/guides/interaction-conventions.md` rather than invented. Multi-symbol definitions are real in it: `⚑` and `(╯°Д°)╯︵ ┻━┻` are the same marker written two ways, as are `⚓` and `┬─┬ ノ( ゜-゜ノ)`, so lookup is many-to-one and a rename is an added symbol rather than a new entry.

**`---- Inner Source ----` is seeded as the first retired entry**, with a 365-day lifetime and a pointer to what superseded it — the worked example the ledger exists for, and the one that would otherwise be unresolvable in the commits from the window it shipped in.

**Seeded further from the same file, 2026-09-01 — the owner's point that `interaction-conventions.md` holds a great deal that is not a marker and lives nowhere else.** Added: the **`comms` fence family** (transit rather than origin, the fence travels with the block, a source never leaves its fence), **`comms draft`** and **`comms draft r/w`** with its write ceiling, and the **`AT:` state header**.

**And a status the design had not anticipated: `declined`.** `LOC` was **considered and deliberately rejected** as a marker — live state that a written name cannot keep true, answered by request instead. **A declined entry still resolves**, so someone meeting it in old material learns it was refused rather than finding nothing, but it must not read as available. **Recording a refusal is what stops it being re-proposed**, which is a cost the ledger was not built for and turns out to carry for free.

**Four statuses now: `in force`, `candidate`, `declined`, `retired`** — and `retired` is derived from the presence of a retirement date rather than restated, so the two cannot disagree.

**What is still only in the guide is structural rather than notational**, and belongs to `cai documentation` when it exists: chain-out versus fan-out, the acceptance criteria, *fan-out returns proposals not edits*, approval belonging to the receiver, and permissions living on the receiver. **Those are shapes, not symbols**, and cramming them into a marker dictionary would be the category error the notation/documentation split exists to prevent.

**A seeding error caught on the first audit, worth recording.** The confidence classes are the digits `1`–`5`, and scanning prose for them matched 321 times. They name **values inside a map**, not markers in text, so a definition now carries `scannable` and a scan skips the ones that do not name a text marker. The scope question is answered per entry rather than per caller.

It describes how to notate items — markers, fences, notes — and holds **the live dictionary behind them** alongside the notation strategies that govern their use.

**The `✓` marker was too thin a fix, and the owner said so.** It answers one status question; **the actual problem is that notation has no governance at all.** Eight markers are in live use across the two repositories — `⑂` `⑃` `⚑` `⚓` `⊘` `✓` and the `⟦ ⟧` family — with their definitions scattered across guides, and no record of what any of them used to mean.

**The worked example is a few hours old.** `---- Inner Source ----` was introduced, shipped in two commits, and renamed to `---- Raw Source ----` the same day. **The old name now appears nowhere in either repository.** A reader of the commits in that window sees a marker they cannot resolve; git history holds the rename, but git history is not a dictionary and nobody consults it to read a symbol.

**So the ledger is the point, not an accessory.** An `outdated / repurposed` section records how each definition was changed or removed, so **an old meaning never falls out of scope** while the dictionary itself stays free to move. **Repurposing is the dangerous case**: a symbol that once meant one thing and now means another is not merely unknown to an old reader, it is *misleading*, and only a ledger separates the two.

**One constraint, or it becomes the thing this repository keeps closing.** `cai notation` must be the **single home for what a marker means**, with the guides using markers rather than defining them. A dictionary that duplicates definitions already in the guides is a second home whose vocabulary will drift from the first — the failure demonstrated today, when the state file's shorthand sent five searches hunting for content present under other words.

**A first strategy for it, earned the hard way: a negative result is only as good as evidence the check can return a positive.** Nine bad queries in a single day produced answers that looked like findings, and every one of them was an empty result from a check that could not have found anything:

| failure | what happened |
| --- | --- |
| **vocabulary mismatch** | searched a shorthand label while the document used its own words — four times, against `Lesson 14`, `audit split`, `direction inference` and `state --json`, all of which were present |
| **filter matches nothing** | `git log --since=<today>` returned zero commits in a repository whose tip was committed that day |
| **exclusion eats the hit** | a `grep -v` meant to drop false positives removed the sought line, because it contained a common word |
| **pattern read as syntax** | a pattern beginning `--` was consumed as command options rather than searched for |
| **self-match** | a leak check matched a quotation of its own probe string, reporting a leak that was the test describing itself |

**The remedy is one step: validate the check against a known positive before trusting a negative.** Plant a canary, or run it on a case you already know exists. **This is exactly how P12 was established** — a phrase deliberately planted, then searched for — and exactly what was skipped every time the check went wrong.

**It belongs here rather than in a general style note because notation's core operations are searches.** The audit scans for symbols, the map is built by finding them in material, and neutralisation-on-carry rewrites the ones it locates. **A scan that cannot distinguish *absent* from *asked wrong* produces false all-clears inside the very system built to stop meanings going missing** — and a false all-clear on a gendering scan is where that cost was demonstrated today.

**The rule has been derived independently before, in an unrelated environment**, in close to the same words: *to accept a negative, verify against its positive.* Owner, 2026-09-01. **That is the corroboration class this design already calls strongest** — two readings arriving at the same shape without contact — and it is worth noticing that the principle validated itself the day it was written down.

**It is deliberately not being centralised as a remembered convention.** The owner has seen that go wrong before, and a rule of this kind is exactly the sort that decays into prose nobody reads at the moment it applies. **`cai` is the mechanism instead**: the same argument as grants, where a rule evaluated at the point of use beats one written into a document read at start.

**The buildable form, owner 2026-09-01: `cai` prompts after the finding.** When a check returns a negative — no matches, an empty set, nothing found — **cai surfaces the rule at that moment**: *if finding a negative, be sure to prove the positive.*

**That is better than the form first proposed here, which was a search that takes a positive control alongside its pattern and refuses without one.** The flaw in that shape: **it requires knowing in advance that the result will be negative**, and nobody does — a control constructed up front for a search expected to succeed is ceremony, and gets supplied trivially or skipped. **A prompt fires exactly when the empty result appears**, which is the only moment the discipline is actionable and the moment it is reliably forgotten.

**It is a prompt rather than a refusal on purpose.** A negative is often correct and blocking every one would make the tool unusable; the cost of the failure is not that negatives happen but that they go unexamined. **Surfacing beats refusing where the answer is usually fine and occasionally load-bearing.**

**The evidence that this needs to be mechanical rather than remembered is this repository.** The rule was recorded here as a notation strategy with nine worked examples attached, and **the eleventh instance was committed hours later, inside a guide about not repeating known failures.** Knowing the rule is not the same as running the check.

**What it would carry**, all currently scattered or unrecorded: the marker admission test (*would you make a different decision on seeing it in a listing*), the positional definition of a fence, the `loud` / `silent` / `true-silent` vocabulary, the delimiter families and why different families cannot collide, and the `⊘` visibility question — which is a notation problem wearing a naming problem's clothes.

**And the ledger only ever grows**, the same property `⊘` has: a retired definition can never be dropped, because the documents it governed still exist. That cost is the reason to keep the dictionary small and the admission test strict.

### ✓ Definitions carry a UTC decision time, and retirement is a lifetime — not a version

**Owner's ruling, 2026-09-01, replacing the versioned-vocabulary design recorded earlier the same day.** These entries are headed for a live database, so **every definition carries the UTC timestamp of the decision that made it**, and a retired or repurposed entry carries a **lifetime** — a fallout period after which it stops being served.

**This is strictly better than versioning, and it removes the cost versioning introduced.** Under versions, material had to declare which vocabulary it was written against, and undeclared material became the new ambiguity. **Under timestamps there is nothing to declare**: a commit's own UTC time is the query key, and the lookup is *what was live at this instant*. Continuous rather than bucketed, so there are no era boundaries to place a document inside, no version headers, and **one ledger instead of one per version**.

**It is also more granular than a version could be.** A version lumps every change in a period into a single step; a timestamp resolves each definition independently, so two markers that changed a week apart are not forced into the same bucket.

**Time is what decides when something falls out.** A definition nobody can audit *today* — an archived repository, an offline stretch of work — may simply age past relevance, and the lifecycle can then run again cleanly. **That is a heuristic, not proof of disuse**, and it should be stated as one: elapsed time is evidence that a symbol is probably dead, never that nothing will read it again. The same shape as the diagnostics bias already accepted here — a known trade rather than a hidden one.

**One refinement on self-cleaning: filter on read, compact on demand.** Cleaning during an audit or a lookup means a read that mutates, and two reads at different times then disagree about the store. **Expired entries should be omitted from results rather than deleted by the act of asking** — the observable behaviour is identical, reads stay pure, and physical removal becomes a separate maintenance step that can be run, logged, and reasoned about on its own.

**Lifetimes are per-entry with a default.** A marker that lived in one document and a marker that shipped across several repositories do not deserve the same fallout period, and a single global month would be wrong for both ends.

**Absorption lets a retired definition be dropped early, and it is resolved in the definition rather than in a pointer.** Where a surviving definition covers a retired one's use case, **the old symbol is not recorded against it.** Two shapes, worst case first: **copy the old definition to sit beside the main one**, so a single entry carries both readings; or better, **rewrite the definition so it rings true for both.** The second is preferred because a reader gets one coherent meaning instead of two to reconcile.

**Recording the absorbed symbol would keep it alive**, which defeats retiring it — a pointer is a live reference, and the entry it points at can then never be dropped. **The meaning is what a reader needs; the genealogy is not.** And the temporal trace survives regardless, because the definition carries the UTC time it changed: material from before that instant resolves against what was live then, absorption or no absorption.

### ✓ Where the definitions live, and when the local copy is allowed to answer

**Owner's ruling, 2026-09-01: a hosted database, with `cai` as the accessor.** It is authoritative. **As soon as the database is live, the local definitions are marked stale**, and staleness is a property of the copy rather than a judgement about its contents — a stale set may still be entirely correct.

**The stale copy answers only when no connection can be established.** Its purpose is **outside adoption**: an entity taking up this system without access to the hosted store runs entirely on the embedded set, and for them the fallback is the normal case rather than a degraded one. That is why the embedded definitions have to be self-sufficient, the same reason the reader's map embeds its own legend.

**If *we* are the ones on stale definitions, that is a finding, not a fallback.** These repositories are worked in daily, so falling back means something is wrong with the connection rather than with the material. **The response is to audit with the reader's map and then update the symbols in place** — which is where the archive distinction bites.

**A live collection is not an archive, and the map is used differently in each.** For an archive, the map **restores readability and the material is left alone** — it is a finished artifact and rewriting it would alter what it records. For a live repository, the map restores readability **and the symbols are then updated**, because the material is still being worked and carrying stale notation forward just defers the same audit.

**One gap worth closing at build time: a stale copy must carry the time it was last synced.** *Stale* without an age is not actionable — a reader cannot tell a copy that is a day behind from one that is a year behind, and those call for different confidence in what it says. **The sync timestamp makes staleness measurable rather than merely declared**, and it is the same instant-stamped shape the definitions themselves already carry.

### ✓ Audits are triggered by failure, not by a schedule

**Owner's ruling, 2026-09-01. `Optimistically read, pessimistically audit.`** Reading proceeds on the assumption that the dictionary is right; **an unparseable symbol or a usage that does not fit its definition is the trigger.** Encountering something that will not resolve *is* the error signal, so no cadence needs deciding and no scheduled sweep runs over material that was fine.

**The pessimism is in the response, not the reading.** One misused symbol is evidence that **more are likely misused** — a definition rarely drifts alone, and whatever moved it probably moved its neighbours. So a single failure escalates to an audit of the surrounding vocabulary rather than a patch of the one symbol.

**And the cost of being wrong about that is small, which is what makes the asymmetry affordable.** If the pessimistic audit turns up nothing else, the result is **a small map of aliases with a confidence rating on the one item that failed** — a usable artifact rather than wasted work. **The expensive mistake would be the other direction**: reading pessimistically, which taxes every read for the rare case, or auditing optimistically, which stops at the first symptom of a broader drift.

### ✓ The reader's map — reverse-engineered meaning for material that has fallen out

**Owner's design, 2026-09-01.** Archived or old material may use symbols whose definitions have expired out of the live tables. **The map holds their meanings, reverse-engineered from the material itself**, so a stretch of work that outlived its vocabulary stays readable.

**Whether a map is needed is decided by comparison, not by age.** Resolve the material's symbols against current definitions: **a match means no map is needed** — the symbol survived unchanged and current lookup is correct. **A mismatch, or an unresolvable symbol, is what the map is for.**

**The lookup order is the safety property: the map first, then the regular tables.** Checking current definitions first would silently return the *present* meaning for *past* material, which is precisely the repurposing hazard — a symbol that changed meaning reads as wrong rather than as unknown, and nothing announces it. **Map-first means the era-correct meaning wins where one exists**, and the general tables answer only what the map does not. Same shape as the envelope failing long: fail toward the reading that cannot quietly mislead.

**The audit creates the map, and it is the audit's output rather than a standing structure.** When an audit meets an **unrecognised symbol, an unrecognised definition, or a misuse of either**, that becomes a map entry recording what was found and what it appears to have meant. So the map tracks what is wanted and what is not, built only where resolution actually failed.

**There is exactly one map, and it is purged when the reading is done.** Owner's ruling, 2026-09-01, **superseding the frozen-audit-stamp design recorded earlier the same day.** An entry survives a purge only where a decision is pending — **slated to be updated, kept, or dropped**. Everything else goes, which keeps the map from becoming a permanent second dictionary. The disposability needs enforcing rather than intending: the pre-compact state file carried the same contract, went unpurged, and its vocabulary drifted from the documents it pointed at inside a day.

**Keeping dated stamps was the wrong answer to a real problem.** Cross-audit corroboration is the strongest evidence available, and it does depend on an earlier audit's findings surviving — but **preserving the artifact is not how they survive.** Three reasons the single map is better:

**A stamp that later proves wrong has negative value.** If a second audit contradicts the first, a retained first audit is a known-wrong reading sitting in the evidence pile, and it can corroborate a wrong answer for anyone counting agreements.

**Every retained stamp is another second dictionary**, with the drift risk multiplied and a rule to remember about which maps may be consulted for meaning. **The single map removes that rule rather than restating it.**

**And the corroboration is already in the confidence tuple.** A single map updated by each audit accumulates counts — `9 1` becomes `18 1` when a later audit agrees — so **agreement across audits is recorded in the numbers rather than in a pile of artifacts.** Re-derivation *is* the corroboration: a finding does not need preserving, it needs reaching again independently, and if it was right it will be.

**And this is where the confidence ratings earn their place.** A re-audit does not only add agreement — **it tests whether the existing rating still holds.** Counts move in both directions: a later reading that agrees pushes `9 1` toward `18 1`, and one that contradicts adds to the against-classes, so **a rating can fall as well as rise.** The tuple stops being a grade awarded once and becomes a running measure of how well an inference is surviving contact with more material.

**Which is the argument that settles the single map.** A frozen rating is fixed at the instant it was taken and **nothing will ever revise it** — it can be wrong for as long as it is kept, and nothing in the system says so. **A live rating on one map is falsifiable**: the next audit either confirms it or degrades it, and a degrading rating is itself the signal that an entry needs re-deriving. **Preserved stamps record what was believed; one live map records what is holding up.**

**A map that fails an audit is discarded rather than repaired.** Its whole value was as a **trusted** warm start, and a map known to contain an error costs more to sort than to rebuild — every later use carries *which entry was the bad one*. **Nothing is lost by rebuilding**: correct findings get re-derived, and the re-derivation is worth more than the record of them was.

**A nulled pointer triggers a full audit around that symbol, not a repair of the link.** When a symbol → symbol alias loses its target, **the target moving is evidence that the vocabulary shifted**, not that one link happened to break — so the response is to re-derive the definition from surrounding material rather than to re-point the alias at whatever looks closest. Re-pointing would be a guess dressed as a fix.

**The map carries its own build time, and its commit carries one too.** Owner, 2026-09-01. **The commit timestamp is the durable record** — a file's mtime moves for reasons that have nothing to do with when the map was built — while **the stamp inside the file makes it readable without git**, which is the disconnected case again: a map that needs the repository to be interpreted fails the same way one that needs the live tables does.

**That anchors the map in the definition timeline.** A map built at an instant **resolves against the definitions that were live at that instant**, which is the same continuous lookup the lifetimes already provide, now pointed at the map itself rather than at the material.

**And the map notes where its timestamp can be validated — or that it cannot be.** Owner, 2026-09-01. A build time is a **claim** until something can check it: inside a repository the commit is the validator, and a map copied out of one, or read after the repository is gone, may have no validator at all. **Recording the validation path makes the difference visible**, and recording its absence is the honest case rather than the missing one — the same discipline as `build` declining to assert provenance it cannot verify. **An unvalidatable timestamp is still useful; one that is silently unvalidatable is a fact-shaped guess.**

**And it comes with a cost rule rather than a commitment.** If reconciling a map's entries by **timestamp comparison** turns out to cost more than **re-deriving them from the material**, re-derive. **Decided by measuring, per case** — the machinery exists to make an answer available, not to be used because it is there. Derivation stays cheap here precisely because the map is small and short-lived, which is the same property that makes purging it safe.

**This is what makes it one system instead of four.** A single primitive — **a UTC instant** — does all of the work: **on a definition** it records when the decision was made; **on a lifetime** it decides when an entry stops being served; **on a commit** it places material in the era whose vocabulary it was written against; and **on the map** it fixes which definitions the reading was resolved against. No era declarations, no version headers, no separate registries. **Everything reduces to *what was true at this instant*, asked of a different object each time.**

**Misuse detection is the expensive part, and worth pricing separately.** An unrecognised symbol is a lookup miss — cheap and certain. **Misuse means the symbol and its definition are both known and the usage does not match**, which requires reading context rather than consulting a table. That is the capability that makes the map worth building and the one most likely to need a person.

**And a misuse finding has three resolutions, not one.** The usage may genuinely be wrong; the symbol may be old and mean something else; or — the owner's addition, 2026-09-01 — **the definition itself may be at fault**, inadequate for the context it is being used in. An agent can reason about which, because all three are visible in the surrounding material: what the symbol is doing there is evidence about what it was meant to mean.

**Where the derived meaning already exists in the main tables, the entry is symbol → symbol.** Reverse-engineer the definition from context, look for it among the live definitions, and if it is there, **map the old symbol onto the current one rather than restating its meaning.** `---- Inner Source ----` is the worked example available today: derive *an envelope declaring its whole contents inert*, find that already defined as `---- Raw Source ----`, and the entry is a rename rather than a definition.

**The alias is better than a copy because it stays live.** A restated definition is a snapshot and goes stale the moment the real one moves; **an alias follows its target**, so a later refinement reaches old material for free. It is also smaller, and it names a rename *as* a rename instead of leaving two suspiciously similar definitions for someone to reconcile.

**This does not contradict the absorption ruling, and the difference is where the pointer lives.** Recording an absorbed symbol on a *live definition* keeps a dead symbol alive in the durable tables. **An alias in the map is a pointer inside the disposable artifact** — scoped to old material, purged when the reading is done. **The pointer belongs in the thing that gets thrown away, not in the thing that persists.**

**An alias derived by reasoning is a proposal until confirmed.** The map is where a pending decision lives, so an inferred symbol → symbol entry is provisional by construction — but it should be marked as inferred rather than presented flat, because **a wrong alias is worse than an unresolved symbol**: it resolves confidently, and nothing downstream announces that the match was a judgement.

### ✓ The map is built by reading, which is what its name says

**Owner's expansion, 2026-09-01.** It is a *reader's* map: **a full read is still building it**, and it is meant to be updated so later reads and audits start further along. Not an artifact computed once and consulted afterwards — one that improves with use, and **dissolves when it stops being useful**, exactly as before.

**So `inferred` is not a flat flag but a moving signal.** An entry begins as a guess from a single occurrence, and every later occurrence either supports or contradicts it. What the entry carries is the running state — *a few errors in usage noticed* against *a lot of errors in usage noticed* — which **changes during the read rather than being fixed at its start**. An inference that looked sound at the tenth occurrence and poor by the hundredth should say so while the reading is still happening.

**And that signal discriminates between two very different findings, which is the reason to track direction and not just volume.** Deviations that are **consistent** — the same departure from the stated definition, repeatedly — are evidence the **definition is wrong**. Deviations that are **scattered and inconsistent** are evidence the **material is sloppy**. Both raise the error count identically, so a bare tally cannot tell them apart, and they call for opposite responses: fix the definition, or flag the usage.

**Inferred items are graded, and the grade is a parseable tuple rather than a score.** Owner's design, 2026-09-01: **`<count> <class>` pairs**, where the class is an enumeration whose definition lives in the map's own instructions. `9 1  3 2  1 3` reads as *nine agreed, three deviated consistently, one deviated in a scattered way* — easy for an agent to parse, and it carries both the **magnitude and the arrow** that a single number cannot.

| class | means | what it is evidence for |
| --- | --- | --- |
| `1` | agrees with the inferred reading | the inference |
| `2` | deviates **consistently** — the same departure, repeated | the **definition** is wrong |
| `3` | deviates **inconsistently** — scattered | the **material** is sloppy |
| `4` | contradicts the inferred reading outright | against the inference |
| `5` | confirmed by a person | the inference, decisively |

**The tuple replaces the confidence score, it does not annotate one.** A score is a summary, and a summary of guesses reads like advice — which is the thing the map must not give, since it reports difference and the operator adopts. **Counts cannot read as a recommendation**; they are what was observed. This also satisfies the earlier requirement that a rating name what it rests on, by making the rating *be* what it rests on.

**The enumeration is a symbol set and takes the same rules, but the exposure is bounded.** A numeric class code is the kind of thing that drifts — if `2` stops meaning *consistent deviation*, recorded grades change meaning silently. **The danger only exists while a map does**, though, and a map is short-lived by design: it is built by a read, dissolves when the reading is done, and survives only where a decision is pending. **So the enumeration has to be stable for the life of a map, not forever**, which is a much smaller requirement. It still belongs in `cai notation` with a UTC decision time, because a map that outlives its intended brevity should not also outlive its legend.


**With one deliberate exception to the single-home rule, and it needs stating because it is a real exception.** The map **embeds its own legend** rather than referencing one, because a map exists precisely for material whose vocabulary is no longer resolvable — **a map that needs the live tables in order to be read is self-defeating.** The embedded copy is stamped with the instant it was taken from `notation`, so it is a dated copy rather than a competing definition, and a mismatch between the two is detectable rather than silent.


**The map shows what is different. It does not recommend.** Owner's ruling, 2026-09-01, and it is the constraint the rest of this rests on. **At dissolution the onus is the operator's**: drop the map entirely, or adopt some part of it as definition additions, changes and deletions. **The map never adopts anything itself**, so a graded entry is a *difference reported*, never a proposal to act on.

**Which means the confidence rating describes the evidence for a reading, not a recommendation to take it.** That distinction has to be explicit in how the grade is presented, because **a high number reads like advice** — and an entry that says *0.9* invites adoption in a way the same entry saying *nine occurrences agreed, one did not, no person has confirmed it* does not.

**This closes the promotion hazard structurally rather than by discipline.** A guess cannot become a definition by attrition **because the map has no path by which anything becomes a definition** — adoption is an operator's deliberate act, and that act is where the inferred-ness is carried forward or knowingly dropped. **Same rule `state --audit` already follows**: it reports and does not repair, because provenance never written cannot be recovered by inspection. The map reports and does not adopt, for the same reason.

**And corroboration across audits is the strongest evidence available.** Where work has been reflowed and a **later audit derives the same symbol → symbol alias again**, that repetition is much stronger than any single derivation — two independent readings landing on the same mapping means **the intent behind the definition is genuinely recorded** rather than reconstructed plausibly once. **So evidence accumulates across audits, not only within a read**, and an alias that has survived a reflow is a different class of finding from one seen once.

### ✓ `cai name` respects the dictionary rather than holding one

**Owner's ruling, 2026-09-01: either `cai name` respects the running dictionaries, or it folds into `notation` with naming as a usage case.** These come to the same thing under the single-home constraint, and the split that keeps both is **by verb**.

**`notation` defines; `name` applies.** What `⑂` *means* is a notation fact and lives in exactly one place. Renaming a session, tracking a fork, deciding whether to show `⊘` — those are **operations on session names**, and they read their definitions rather than restating them. **`cai name` may not define a marker.** That is the whole of the constraint.

**So naming is a usage case of notation that keeps its own sub-command**, because a person renaming a session should not have to know the naming scheme is an application of a notation system. **The `⊘` visibility gate stays a notation policy** — it is a rule about a marker — with `name` enforcing it at the point a name is displayed.

## `cai documentation` — the larger shapes

**Owner's proposal, 2026-09-01. ✓ BUILT 2026-09-02, and this line was stale.** `cai document` holds the shapes and `cai flow` holds the workflow patterns, which between them are what this section asked for. Where `cai notation` holds the atoms, this holds the **structures**: document conventions, what belongs in which file, and the workflow patterns themselves. The pair standardises **workflows around tracking workflows**, which is the layer this repository has been building by hand all along.

**The split is the useful part.** A marker is a unit of meaning and a document is an arrangement of them, so a change to one need not disturb the other — and the deprecation ledger belongs with the atoms, because that is where meaning drifts.

## ✓ `cai name` — a sub-command that explains itself

**Owner's proposal, 2026-09-01. ✓ BUILT 2026-09-02, and this line was stale.**

**`cai name` with no arguments prints its help, and the help says why it does what it does** rather than only what the flags are. The naming scheme answers questions a reader will actually have — why the cwd leads, what the glyphs mean, why a slice may not match the current `sessionId` — and those answers currently live in two repositories' documentation. A tool whose bare invocation explains its own reasoning puts them where someone meets the question.

**It is the same instinct as the notice that always appears**: the explanation is attached to the thing rather than filed somewhere a reader has to already know about.

**What it would need to cover**, from the naming discussion: the name shape and field order, the lineage and role markers including `⑂⑃` co-occurring, the promotion/demotion split (promotion is the operator's, demotion the tool's), the admission test for a new marker, and that the name carries lineage while the `sessionId` carries iteration.

**It also gates marker visibility.** The candidate `⊘` marker — *not eligible as a clean subject* — cannot be shown by default, because knowledge of contamination is itself contamination: an agent reading it learns something a clean agent would not have. **`cai name` is where that flag lives**, withholding by default and revealing on request. See `SOPIA/docs/guides/interaction-conventions.md`.

**And it is the natural home for the fork-lineage gap.** `cai` does not track forks; the name is what records them, and the onus is on the operator unless the fork was requested through `cai` itself. A rename sub-command is where that could become tool-assisted rather than remembered.

## Session names — the slice is documentation, not an address

**Established by accident, 2026-08-31, and it changes what name maintenance may safely do.**

**The name is the address. The slice inside it is not.** Nine cross-session messages were delivered to `SOPIA ⑃ (╯°Д°)╯︵ ┻━┻ - 8624892a` while that session's actual `sessionId` was `3e17cb96`. Every one arrived. `ListAgents` matches the **whole name string**; nothing parses the hash.

**So a slice that does not match the current id is not broken.** In that case it recorded **where the conversation came from** — the session whose transcript it inherited — which is a different fact from *which file am I*, and arguably the more useful one. An agent reading it learns the lineage; addressing works regardless.

**A worked instance, and it is the best one available.** SOPIA commit `1df2d3f` — **the commit that documents the naming convention** — carries `Session: SOPIA - 8624892a` while having been written by `d2a21641`. It was self-caught at the time and **deliberately left in place**, on *corrections go forward*. So the canonical example of a slice recording something other than its writer is the commit that defined slices, and it is preserved rather than amended.

**The resolution, ruled 2026-09-01 — and marked DEFINITELY NEEDS IMPROVEMENT.** The mismatch was a misnaming, not an error: **the name shows lineage, the `sessionId` shows the iteration.** Two facts, deliberately carried by two fields, which is why a mismatched slice is informative rather than broken.

**It works for present purposes and the owner says plainly it can get hairy.** One field reading as lineage and another as iteration holds only while everyone remembers which is which — a convention resting on recall, which is the weak form. **Recorded as adequate-for-now rather than settled**, so a later reader does not mistake a working arrangement for a designed one.

**Consequence for any tool that maintains names: never rewrite a slice it did not write.** A mismatch may be deliberate. Correcting it to the current `sessionId` destroys a lineage record and gains nothing, because addressing never used it.

### The harder problem underneath

**`sessionId` is not stable across trans-fairy operations.** `rewrite_session` stamps a fresh id across all six fields on every graft, inject, compose and truncate. It does not touch message text, which is why an inherited transcript still refers to its origin by name hundreds of times while its own field says something else.

**So two different facts want the same eight characters:**

| fact | wants | stability |
| --- | --- | --- |
| **who to address right now** | current `sessionId` | changes on every tool operation |
| **who committed this** | the id at time of writing | must never change, or a trailer stops tying a commit to its session |

**One field cannot carry both.** Demonstrated the same day: thirteen commits carried a trailer that was **correct when written** and was later read as wrong, by the session that wrote them, because the session had since ended and only the current listing was checked.

**The commit trailer wants the historical id and should never be reconciled against a live listing.** A name wants whatever the operator finds useful to see, and the tool should treat that as hers rather than as a field to normalise.
