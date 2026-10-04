# Asynchronous work as lanes of one event loop (design note)

Micaiah, 2026-10-03. Status: the full lanes design is backlog (build only once the tool-call queue, [tool-call-queue.md](tool-call-queue.md), is proven). **In use first, the simple version: an item marked async runs as a subagent** (see "Sessions that would rather stay synchronous" below).

## The decision

Asynchronous work gets one shared model, not a second queue. Two independent queues invite ordering ambiguity (which wins when an approval needs an async result), deadlock (a decision waiting on work that waits on a decision) and priority inversion (a hang stuck behind a slow build). One event loop with typed items avoids all three by construction.

## Lanes

* **The decision lane:** the tool-call queue. One item at a time, at the owner's pace; hangs first and never skipped.
* **Execution lanes:** concurrent, each with its own cap. They mostly exist already; this names them and gives them one vocabulary.

## What maid has today, mapped onto lanes

| Lane | Today | Its cap today |
| :- | :- | :- |
| Session turns | each session's lane: inputs queue first in, first out behind a running response (`response.create`, status `queued`) | one running response per session |
| Background tasks | `task` subagents and background sessions | `max_tasks`, with project exceptions |
| Provider calls | requests per provider account | `max_concurrent` per model; shared holds after a 429; the circuit breaker |
| Services | `maid up` / `down` and the services' readiness waits | one start per service |
| Liaison sends | a turn handed to a daemon session, waited on until it completes | the session's own lane |
| Approvals | today's blocking approval: the decision lane at depth one | one at a time (the hang) |

## The shared vocabulary

Every item, in any lane:

* **Outcomes:** `pending`, `running`, `done`, `failed`, `stale`, `dropped` (the decision lane adds `accepted`, `rejected`, `dropped+stale`).
* **Identity:** the same short hash as tool calls (a timestamp, a brief title, a short description), and `max_retries` per instance.
* **Time:** a lifetime timeout and a time-in-lane timeout, as in the queue design; each ends in an action, never a silent expiry.
* **Cancellation:** one way to cancel any item, with its outcome reported.
* **Dependencies:** an item may wait on another's result, in any lane; the agent is told what it waits on, and nothing blocks blindly.

## What the agent sees

One compact view of pending work across lanes (counts, the items that need it, the ones that finished since its last step), not a stream of every event. Context is the scarce resource here.

## Sessions that would rather stay synchronous

A session that doesn't want async behaviour at all can get most of the benefit anyway: long or parallel work goes to background subagents through thin wrappers, which keeps token costs down. Each subagent returns a brief description (what it did, the outcome, where its details are), and the full report stays in its own transcript, read only when needed. That is also the answer to the flood of events: the parent sees one short line per subagent instead of every event, and the detail is a transcript away. It matches how forks merge back with a summary rather than their raw history (roadmap item 3), and how Claude's own agents report today (a few lines back, the transcript for the rest).

## Backgrounded calls with a watcher (idea, Micaiah)

A long-running tool call can itself go to the background, with a minimal-context subagent adopting it to report back. Linux has no call to reparent a process, and none is needed: maid already owns every tool process as its child and captures its output to a file with an index, so "backgrounding" is maid detaching the call from the turn, and "adopting" is handing the watcher a handle (the call's title, its output file, a `wait` tool), while maid keeps the process. Where useful: a `pidfd` (`pidfd_open`) to wait on or poll a process that is not the waiter's own child, and the daemon marked as a child subreaper (`PR_SET_CHILD_SUBREAPER`) so orphaned grandchildren return to maid, never to init. The watcher's context is the command, one line of why, and "summarize when it ends, or at milestones"; its capability set is read-only (it may watch, never signal or kill; cancelling goes through maid). The parent gets one line when it is done. The owner's kill keychord bypasses the watcher entirely: it goes straight to maid, which owns the process and stops it (and its process group); the watcher is then told the call was killed by the owner and reports that, nothing more.

## Costs to plan for

* **Nondeterminism:** testing needs a controllable clock and scheduler; the policy testing's two layers (a base set and a case corpus) extend to scheduling.
* **Contention:** builds, fetches and model calls compete for the same machine and accounts, which is why every execution lane has a cap.
* **Audit:** every outcome is recorded with its hash and title (brief, readable, secret at a glance).
