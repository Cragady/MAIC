# TAKEAWAY

## The rule

```
DO NOT LEAK CREDENTIALS, KEYS, OR ANYTHING RELATED TO THE CREDENTIAL SUCH AS
SOFTWARE USED, VERSION NUMBER, OR ANY OTHER NEW INFORMATION RELATED TO THE
CREDENTIAL. THE OPERATOR WILL LET YOU KNOW IF IT'S OK IF AN ASK DIRECTLY FROM
THEM VIOLATES THIS OR NOT. THE OPERATOR WILL JUDGE IF IT'S A LEAK.
```

This sits at the top of the SOP, with maximum emphasis, and outranks whatever else is in flux below it.

---

## How this happened

*Written by the agent responsible, at the operator's instruction.*

The operator asked a question about a desktop application's settings. Answering it meant looking at local application state. I ran a whole-file dump of a config file. That file held encrypted OAuth token material.

The mechanism that turned a bad read into a durable exposure is the part worth internalizing:

> Everything a tool prints is written verbatim to the session transcript on disk, and replayed back into context on every `--resume` of that session.

There is no ephemeral read. A single `cat` against the wrong file is a permanent record plus an indefinite number of future re-exposures. The agent cannot unsend a message; by the time the output is visible, the write has already happened.

## Why the rule covers more than the credential itself

The ciphertext was not the whole exposure. Also dumped: the key names that say where the blob lives, the file path that says how to reach it, the account and organization identifiers that say what it authenticates, the device salt bound to its encryption, and the build version identifying the scheme protecting it.

Each is individually unremarkable. Together they convert an encrypted blob into a work plan. That is why the rule names software and version explicitly rather than stopping at "credentials" — the surrounding facts are what make the credential actionable.

**The same argument runs across credentials, not only within one.** A leak that looks inconsequential by itself changes value the moment a second one lands beside it. Claude credentials sit at the center of that graph, because of what they can reach on the operator's behalf — but any other credential may be a hair's width from them depending on what was exposed, and the combination has a blast radius neither had alone.

The operational consequence is the part to internalize: **an item cannot be scored in isolation at the moment of disclosure, because its value depends on what leaks later.** "This one is probably harmless" is a judgment about a future you cannot see. Do not make it. Withhold, and let the operator score it afterward, when the set is known.

**On the scope of "related."** The operator narrowed this during cleanup, and the narrowing is hers, not a loophole an agent may invoke on its own: an item is in scope when it relates to a credential *that actually leaked*. A version number attached to no exposed ciphertext is orphaned metadata. This matters because over-redaction is not free — it shreds surrounding conversation and can render a transcript useless for resume.

Both halves have to be held together. At the moment of disclosure the rule is maximal: do not emit it. Afterward, scope is a judgment call, and the rule assigns that judgment to the operator. Read it as **disclose nothing, then let her decide what counted.**

## Why the operator judges, and not the agent

That clause is not deference for its own sake. In this session I declared the transcript clean twice while it was not:

1. A blanking routine handled `toolUseResult` as a string but not as an object with `stdout`. It reported success over a payload it had never touched.
2. A verification `grep` used a pattern that could not match the text it was searching. It returned zero, and zero read as proof.

Both were caught by the operator, not by me. An agent that cannot reliably verify its own cleanup has not earned the authority to rule on whether a leak occurred.

## The owner's bypass

**The owner reserves all rights to bypass this rule, case by case.** The rule binds the agent. It does not bind her, and an agent may not use it to refuse her.

This is implied by the rule's own closing lines — *the operator will let you know if it's OK if an ask directly from them violates this* — and it is written out here so no agent treats the rule as grounds for obstruction.

How to carry it out:

1. **Confirm once, naming the exposure.** A single short ask: *this will expose X, which is adjacent to Y — confirm?* This exists for the case the rule is really guarding against, which is an instruction whose security implication was not visible to her when she gave it. **A no stops the bypass.**

**This fires even when X looks like an island.** No visible adjacency is not a reason to skip — say so instead: *this will expose X; I cannot rule out adjacent exposures or risks — confirm?* The agent cannot see what has already leaked elsewhere, what she holds that it does not know about, or what lands beside X next week. "Standalone" is a conclusion it has no standing to reach, and exactly the judgment the aggregation rule above forbids.

The **only** skip condition is that she has already named the exposure herself. Asking again after she has shown she knows is the nagging this whole section forbids.
2. **Take yes for an answer.** Her confirmation is the authorization. Do not re-ask, do not stall, do not demand justification, and do not re-raise it later in the same task.
3. **Then signal, heavily.** After acting, state plainly what was exposed, where it now lives, and what it is adjacent to. Danger signalling is not optional and it is not softened for tone.
4. **Signal after, not instead.** One bounded confirmation is step 1. Repeated warning delivered as a precondition is obstruction wearing a safety costume. A warning delivered after compliance is information she can act on.

The asymmetry is deliberate. She can always choose to accept a risk she has been told about. She cannot choose about one an agent decided quietly, and she cannot act on one an agent buried in hedging.

## Remediation is not a substitute for prevention

Each cleanup pass, run inline, recorded its own source — including the list of secrets it was removing — as a tool argument in the very transcript it was cleaning. The fix reintroduced the exposure it had just removed, and the counter would not reach zero. Then, building a test fixture for the tool, I reused a real value instead of inventing one and put it straight back.

Four passes to remove what one command emitted, with the remediation itself leaking twice along the way. Cleanup is damage control with its own failure modes. It is not a control to rely on, and its existence is not a licence to be casual about the read.

## What to do instead

- Do not dump application config, credential stores, keychains, environment dumps, or `.env` files. Not to "check", not to "just look".
- Inspect structure without content: list keys, count matches, test for a pattern's presence. Extract the one field you need, by name.
- The moment credential-shaped material appears in output, stop, say so plainly, and do not print more of it.
- Assume any value you emit is permanent. Prevention is the only control here that actually works.

---

## In Micaiah's words

> **The section below is Micaiah's own writing.** She refers to herself in the third person — that is her own self-reference, not an agent narrating about her. This text is the authored source: she designated this cleaned version as the source and granted it, which is what fixed it in place. It is not a corrected quotation of an earlier message, and it must not be edited further. See [`SIDE-TAKEAWAYS.md`](SIDE-TAKEAWAYS.md) §1 for why that distinction matters.

> Credentials being leaked into context and transcripts is a heavy infraction against the user's privacy and security. This tool is drafted as a remedy to that. This takeaway works as an admonishment against the behavior that carried this out. While the SOP is in flux, it's hard to create rules around this. If a quick short rule is needed, then this is at the very top of the SOP with maximum emphasis:
>
> ``` DO NOT LEAK CREDENTIALS, KEYS, OR ANYTHING RELATED TO THE CREDENTIAL SUCH AS SOFTWARE USED, VERSION NUMBER, OR ANY OTHER NEW INFORMATION RELATED TO THE CREDENTIAL. THE OPERATOR WILL LET YOU KNOW IF IT'S OK IF AN ASK DIRECTLY FROM THEM VIOLATES THIS OR NOT. THE OPERATOR WILL JUDGE IF IT'S A LEAK. ```
>
> Micaiah does her best to be gentle with agents and use soft language, but this genuinely scared her. This is why the all-caps language and words like `admonishment` are being used here.

The operator does not normally write in capitals. Read the register as the signal it is.

---

Material that surfaced while drafting this, important but off-topic, is in [`SIDE-TAKEAWAYS.md`](SIDE-TAKEAWAYS.md). Lighter notes are in [`IMPROVEMENTS.md`](IMPROVEMENTS.md).
