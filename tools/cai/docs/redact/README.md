# Claude Code transcript redaction

> **Read [`TAKEAWAY.md`](TAKEAWAY.md) first.** This tool exists because an agent leaked credentials into a session transcript. The takeaway carries the rule that should have prevented it. This file documents only the cleanup, and cleanup is not the control that matters.
>
> Material surfaced while drafting that is important but off-topic is in [`SIDE-TAKEAWAYS.md`](SIDE-TAKEAWAYS.md) — most relevantly §1, on when a quoted span may be edited and by whose authority. Lighter notes, including candidate `tools/CASES.md` rows, are in [`IMPROVEMENTS.md`](IMPROVEMENTS.md).

**Implemented 2026-08-31.** `cai redact <transcript> --backup PATH [--verify]` scrubs secrets out of an on-disk Claude Code session transcript after an agent has dumped credentials into it, and verifies the result against a pristine backup. It shares the record grammar with `trans-fairy` (`src/cai/grammar/records.py`), which owns the payload and structural key sets. Full flags: `cai redact --man-help`. Tests: `tests/test_redact.py`, including positive controls on both defects this tool produced.

## The problem

Claude Code persists every session to newline-delimited JSON at:

```
~/.claude/projects/<slugified-cwd>/<session-uuid>.jsonl
```

Every tool call and every tool result is recorded verbatim, including `stdout`. So a single careless `cat` of a config file that happens to hold OAuth material, device identifiers, or an org UUID writes that content permanently to disk, where `--resume` and `--continue` will replay it back into context on every future run of that session.

The conversation itself is append-only from the agent's side — it cannot unsend a message. The `.jsonl` file, however, is an ordinary file, and *that* is the durable artifact worth fixing.

## Scope of what this fixes, and what it does not

| Surface | Fixed? |
| --- | --- |
| On-disk `.jsonl` (what `--resume` replays) | Yes |
| Terminal scrollback | No — clear the pane yourself |
| The agent's live context in the current session | No — persists until compaction |
| Any backup copy you made | No — delete it deliberately |

## The other regime, and why this tool does not serve it

Transferred from `transfairy/DESIGN.md`, 2026-08-28, where it was worked out against a different problem. **Two operations share the word *redaction* and their requirements are opposites.**

**Security redaction wants to be visible.** An explained gap closes the question; an unexplained one invites reconstruction. Markers, declared regimes and a stated authority are the mechanism, and this tool is built for it end to end.

**Experimental redaction must be invisible as topic.** The moment it announces that a subject was removed, it leaks the variable — *something was removed here about a topic* tells the reader there was a topic. **A marker, which is this tool's core mechanism, is disqualifying there.**

**So the rule is two artifacts, not one.** A security-redacted copy for sharing, and a separately produced copy for experimental use. **One file cannot satisfy both, and the attempt fails silently at whichever is being watched less.**

**And for the experimental case, redaction is the wrong operation anyway.** A redaction cannot be proven complete — keyword removal will not catch oblique references, structural echoes, or the model's own paraphrases, and **one miss voids a run silently**, which is the worst failure shape available. A truncation point is a single index, so nothing past it provably survives. That operation lives in `transfairy`.

**The same collision appears from the grafting side.** A graft that reads as seamless is a bug, because a grafted row must announce itself to the subject. A redaction that announces itself leaks the variable. **Graft announces to the subject; experimental redaction announces only to the record** — and a tool that holds both invariants at once has to know which one it is doing.

## The trap: self-reintroduction

This is the part that is not obvious, and it cost four passes to work through the first time.

A redaction script invoked through a heredoc has its **own source text recorded as a tool-call argument in the same transcript it is cleaning.** If that source enumerates the secrets as literal patterns — which is the natural way to write it — then every cleanup pass appends a fresh, complete copy of the secret list to the file. The verification pass does the same thing. The count never reaches zero, and each attempt to check makes it worse.

Two mitigations, both used here:

1. **Keep the script on disk and invoke it by path.** The transcript then records only `redact-transcript.py <args>`, not the pattern list.
2. **Derive the candidate list at verification time from the backup**, rather than hardcoding it. The verifier's own source text contains only generic shape regexes (`[0-9a-f]{8}-[0-9a-f]{4}-...`), which are not themselves sensitive.

If you must run it inline anyway, plan on a follow-up pass that blanks the tooling lines, identified by a neutral marker word rather than by the secrets.

## Preserve structural identifiers

Do **not** blanket-scrub every UUID. Claude Code uses these as threading fields, and removing them breaks resume:

- `uuid`, `parentUuid` — message chain
- `sessionId` — session identity
- `promptId` — groups tool calls under the user turn that triggered them
- `requestId` — API correlation

A `promptId` will look exactly like a leaked credential to a naive UUID regex. Check which JSON key holds a match before deciding it is a secret.

## Nested result shapes

`toolUseResult` is sometimes a plain string and sometimes an object with `stdout` / `stderr`. A blanker that only handles the string form silently misses the object form. Recurse and match on key name across both.

## Usage

```sh
# 1. Redact. Backs up first; refuses to run without a backup target.
./redact-transcript.py \
    ~/.claude/projects/<slug>/<session>.jsonl \
    --backup ~/scratch/ \
    --blank-lines 47,51,61

# 2. Verify against the backup. Exits non-zero if anything survived.
./redact-transcript.py \
    ~/.claude/projects/<slug>/<session>.jsonl \
    --backup ~/scratch/<session>.jsonl.bak \
    --verify
```

`--blank-lines` replaces whole tool results with a notice, for lines that are nothing but a credential dump. Everything else is handled by value-level substitution across all lines, so surrounding conversation survives.

### Cleaning a re-leak

If later tooling reintroduced a value you already scrubbed, candidates must come from the **pristine backup**, not the live file — the live file no longer holds that value at its original location, so the default pass finds nothing and silently no-ops with `0 candidate value(s)`:

```sh
./redact-transcript.py <transcript> \
    --backup <new-backup-path> \
    --candidates-from <pristine-backup> \
    --only-lines 47,51,61
```

Never point `--backup` at an existing backup with `--force`; that overwrites the pristine copy with the already-redacted file and destroys the only reference the verifier has.

## Scope: relate everything to the credential

Redact what the leaked secret *points to*, not everything that looks identifier-shaped. Useful test — does this value help someone locate, decrypt, or use the exposed credential?

- **In scope:** the ciphertext itself; the key name and file path that locate it; the account and org identifiers naming what it authenticates; the device salt bound to its encryption.
- **Out of scope:** crash-reporter ids, process handles, feature-flag names, and build or product versions *when the ciphertext they relate to is already gone*. A version matters only as "which encryption scheme protects the blob" — once the blob is removed, it is orphaned metadata.

Over-redaction has a real cost: it shreds the surrounding conversation and makes the transcript useless for resume.

## Operational notes

- Run it while the session is idle. The app appends continuously; anything written after your pass is not covered, including the tool call that ran the pass.
- The line numbers you pass to `--blank-lines` shift as new lines are appended. Re-survey immediately before blanking.
- Verify JSON validity after every write. A corrupted line makes the whole session unloadable, and the failure mode is silent until next resume.
- The backup is a complete unredacted copy. It is the recovery path if you over-redact, and it is also a second exposure. Delete it once satisfied.

## Related

Prompted by a Claude Desktop capabilities question, where investigating local state meant reading app config that turned out to hold credential material. The general lesson: prefer targeted greps and key-listing over whole-file dumps when poking at application config, because the dump is what lands in the transcript.
