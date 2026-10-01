# SIDE-TAKEAWAYS

Material that surfaced while drafting [`TAKEAWAY.md`](TAKEAWAY.md). None of it is the main topic — the main topic is the credential leak — but each item was exposed by the drafting and is heavy enough to keep.

**Weights** are relative and assigned by the drafting agent. §1 is the ceiling: it was raised by the operator, it changes a definition the repository already depends on, and everything below is scored against it.

---

## §1 — Authorship is conferred by the operator's grant, not by chronology

**Weight: 10 (ceiling).**

`tools/CASES.md` case 14 records that a mark-rename substitution has no concept of a quoted span, and that it would have rewritten the operator's verbatim words inside the in-force SOP — missing only because her quote used a plain hyphen where the mark used an em-dash. *"The protection was punctuation, not a rule."*

Drafting this document hit the gap that case cannot express. The operator's words were going into a blockquote **with five corrections she had approved**. On disk that is indistinguishable from the accident case 14 describes: a mutated quoted span, with nothing recording that it was sanctioned.

Her resolution, and the reason this sits at the ceiling:

> The definition of the source is contextual, and it naturally changes until it lands in history. Otherwise every keystroke could be considered source.

Which splits into two rules that were previously one:

- **Asking for a hard quote of something past** → that text is the source as it stands, errors included. Wanting to fix them does not license it. This is *do not correct backward*.
- **Designating a cleaned version as the source** → the corrections are **authoring, not mutation**. It became authored at the moment the operator granted it, because becoming the written source was the purpose of the grant.

So the protected unit is not "a quoted span" but "a span that has been authored, by someone with the authority to author it." A span-level guard cannot infer that; the authority lives outside the text.

**Her closing instruction on this, recorded because it is the operable half:** when an authorship flow seems vague, it may be prudent to ask the operator how they want it authored. Asking is cheap. Guessing produces either an unmarked mutation or a needless `[sic]`, and both misrepresent her.

## §2 — Verification that cannot fail correctly is worse than no verification

**Weight: 8.**

Cleaning the transcript, I twice reported it clean when it was not:

1. A blanking routine matched `toolUseResult` as a **string** and silently skipped it where it was an **object** with `stdout`. It reported success over a payload it had never touched.
2. A verification `grep -E '\w+:<keyname>'` returned **0** against a file that contained `<keyname>`. In the source the character before the colon is `+`, not a word character, so `\w+:` could never match. Zero read as proof.

Both are the family `tools/CASES.md` already tracks — cases 2, 8, 11 and 13 are all false zeros from a pattern that could not match its target. The operator caught both; the instrument caught neither.

The lesson is narrower than "test your tools": **a checker whose failure mode is silence will be trusted exactly when it is wrong.** Give it a positive control — assert it finds something you know is there — or it is decoration. This is the same reasoning behind testing every search guard twice.

## §3 — Remediation is its own leak vector

**Weight: 7.**

Two distinct ways the cleanup reintroduced what it removed:

- **Inline invocation.** A redaction script run through a heredoc has its own source recorded as a tool argument in the transcript it is cleaning. Written the natural way — enumerating the secrets as literal patterns — every pass appended a fresh copy of the list. The counter could not converge. Fix: keep the script on disk, invoke by path, derive candidates from a file at runtime.
- **Fixtures built from live data.** Writing a smoke test for the tool, I used a real value instead of inventing one, putting a just-scrubbed item straight back. A fixture has a known answer *by construction*; taking its inputs from production discards that property and imports the risk.

## §4 — The gating worked, and it worked twice

**Weight: 6 — and the weight is not settled. Neither the operator nor the drafting agent is confident in it.** Recorded rather than resolved, in the same spirit as `tools/CASES.md` case 13: no confident diagnosis, so no guess dressed as one.

The operator withheld write permission and released paths one at a time. Twice I read an approving message as a green light and moved to write; both times the gate, not my judgment, was what stopped it.

Worth recording because it is evidence about where the control belongs. An agent mid-task reads ambiguous approval optimistically. The cheap fix is not better agent judgment — it is that permission is granted explicitly, for one path, and that "I understood the spec" is never treated as "I was told to start."

**Why the weight is doubted.** Three reasons, none of them decisive:

- It is an agent writing approvingly about a control that constrained the agent. That is structurally close to flattery of the operator's process, and it reads well whether or not it is doing any work.
- The evidence is two instances, both the drafting agent's own errors, from one session. Thin support for a standing conclusion.
- It may be redundant. `TAKEAWAY.md`'s *why the operator judges* section already places the authority with her; this restates it from the permissions angle without adding a mechanism.

**Against cutting it:** it is the only item naming *where* the control sits rather than *that* it should sit there, and it is evidence-backed rather than asserted. If it is cut, that sentence belongs in §1 rather than lost.

---

Lighter material is in [`IMPROVEMENTS.md`](IMPROVEMENTS.md).
