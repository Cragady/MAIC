# The tool-call queue (design draft)

Micaiah, 2026-10-03. Status: draft; nothing built. Today's approvals are this queue's smallest setting (one call, the session hangs, `approvals_timeout` bounds the hang), so it is built around that behaviour, not beside it.

## Settings

* `approvals_queue = "hang"` stays the default for now; `"queue"` turns on the rest of this document once it is fleshed out.
* Each part is an option: the interrupt window, staleness, review prompts, queue depth, which tools may queue, `max_retries`, and the default accept mode.

## Timeouts

| Setting | Bounds |
| :- | :- |
| `approvals_proposal_timeout` | the hang: the single blocking proposal (there is only ever one, since it holds the line between owner and agent). This is today's `approvals_timeout`, renamed; the old name stays readable for a while with a warning naming the new one. |
| `approvals_queue_timeout` | waiting in the queue; past it the call is `stale`. None in `manual`: a call waits until decided. |
| `approvals_lifetime_timeout` | a call's whole life (window, queue and all); past it the call announces itself stale and recommends a drop, but can still run after one confirmation round once accepted. Applies in `manual` too. |
| `harness_answer_timeout` | maid demands the smart harness's answer on a queued call within a window close to an average model or tool call's latency; unanswered in time, the call is dropped. Configurable; the default is strongly recommended. |

Named as timeouts (not `max_age`) because each ends in an action, not a silent expiry.

## Stale calls and the harness

The harness, managing the queue, takes a stale call by an acceptance level (strict to permissive): `drop` (always), `ask` (the owner decides), `judge` (accepted only if the harness judges it safe), `accept`. It automates what the owner would do, with its guardrails on top; the critical tier caps every level.

## Hangs

An agent may request a `Hang` with a timeout: the session stops and waits, as today. A hang has the highest priority: the harness handles hangs one at a time, in order, and none can be skipped over. Each demands its own decision (accept, reject or drop); a hang is never dropped as part of a group. A reject or drop does not stop the same request being queued again, or accepted, later.

## Non-hang calls: the interrupt window

A call that needs the owner interrupts for a window of `0` to `t` milliseconds (configurable resolution) before it goes to the queue. During the window the owner may:

| Action | Effect |
| :- | :- |
| accept | into the queue |
| reject | early rejection; never touches the queue |
| drop | dropped by hand |
| stale | marked stale by hand |
| queue skip | runs ahead of the queue; the queue is kept (with the smart harness on, it still gives final approval) |
| queue jump | runs ahead and drops the entire queue (the newest command is taken to supersede it); the dropped calls are `dropped+stale` |

When the window closes untouched, the default accept mode decides (configured; by default, accept into the queue).

## The queue

One item at a time, in order, like Node's single-threaded event loop; each outcome reaches the agent as an event at its next step: `pending`, `accepted` (with its result), `rejected` (with the reason), `stale`, `dropped`, `dropped+stale`. Dropped calls are reviewed, not lost: the agent is told which ones in one prompt and decides for each whether to queue it again or record it as dropped. A call that needs a pending call's result waits behind it, and the agent is told.

## The smart harness as queue manager

With `harness = "smart"`, the harness manages the queue: it orders hangs first, judges each call (by its reasoning and the call itself, see decisions.md, "Working fact, falsifiable"), and gives final approval on queue skips.

## Modes, unpacked

The modes become how much of the queue clears itself:

* `manual`: means what it means today: every call is decided by the owner. With the queue, a call accepted in the window moves into the queue and its final approve or reject happens there, instead of bypassing the queue or being rejected or dropped up front; nothing clears itself.
* `auto-read`: as manual, but internal reads and approved read-only shell commands are allowed without the queue.
* `edit`: as auto-read, plus writes in the workspace.
* `auto`: least restrictive; the harness brakes only on blacklisted ground, or when the smart harness judges a call unsafe even in auto.

The critical tier ([security-policy.md](../security-policy.md)) applies in every mode. A `read-only` mode may come later and join the modes offered in the global settings. An administrator can enforce system-wide settings that a user can't override, for example removing `auto` from the modes a user may choose, even in that user's own global settings.

## Retries and identity

* Every tool call passes through maid, so each call instance gets an identity hash automatically: a timestamp, a brief title and a short description, hashed together. Nothing elaborate; it can be recreated from the transcript.
* Agents need not see the hashes: they can live in a ledger beside the transcript, if a ledger is needed at all.
* `max_retries` counts re-queues of one instance. Beyond the hash, the same title and description on the same machine show a call simply being tried again; past the limit, the agent is told so (for example: "this call ran 5 times with no different result; trying again won't help") and may give new reasoning to justify another try.
* Logs show the hash and title instead of arguments: brief, readable, secret at a glance.
