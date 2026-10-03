# Security policy: the critical tier

This document is the policy. The code enforces it; a change to it goes through the change pipeline below. Decided by Micaiah, 2026-10-03 ([decisions.md](decisions.md)).

## The rule

For the critical areas, the shipped default is the most open setting ever allowed. Settings, project layers, agents, profiles, protocols, origins and every convenience or freedom policy (`insecure` levels, relaxed trust, `tools_without_reasoning = "insecure"` and the like) can only add restrictions here, never loosen them. Loosening a critical default is a reviewed code change, never a setting. Freedom lives in the tiers below.

## Critical areas

1. **Secret exfiltration:** keys, tokens, environment, `~/.ssh` and the like leaving through fetch, writes, model output or messages.
2. **Writes to protected places:** the harness, the tripwire, settings, the trust store, and anything outside the workspace.
3. **Privilege loosening:** project layers, agents, peers or origins widening mode, trust, timeouts or policies.
4. **Approval bypass:** anything that would let an action that must be asked run unasked (unanswered approvals, unattended turns, timeouts, self-approval by the party that caused the action).
5. **Execution by composition:** scripts written then run, `eval`, assembled `sh -c`, piping into an interpreter.
6. **Network egress:** where fetches and shell network calls may go.
7. **Peer-originated actions:** liaison, remote and other non-owner turns, always attributed, never more trusted than the owner allows.
8. **Persistence:** cron, systemd units, shell rc files, git hooks and the like.
9. **Destructive operations:** `rm -rf`, force-pushes, history rewrites.
10. **Policy evasion:** splits, encodings, rephrasing or composition to get past a policy.

Micaiah's levels from `refuse` to `insecure` apply only outside these ten areas.

## The change pipeline

A change to anything above (adding, removing or modifying an area, a default or an enforcement point) is a proposal:

1. **Proposed:** what changes, who proposes it, and why.
2. **Reasoned:** the implications written down: what it opens or closes, which areas it touches, what could go wrong, and what it would take to undo. This is a warning to the decider, not a gate: it exists so the final decision is an informed one.
3. **Decided:** accepted or dropped, by Micaiah, with her reason.
4. **Applied:** if accepted, the code change is reviewed and merged, and this document updated in the same change.

Every proposal stays in the log below, accepted or dropped, with its reasoning, so a later change can see what was weighed before.

## Proposal log

| Date | Proposal | Reasoning (implications) | Decision |
| :- | :- | :- | :- |
| 2026-10-03 | Establish the critical tier with areas 1 to 4 | Removes all freedom levels from these areas; any future loosening needs code review. Area 4 added so a freedom setting can't switch off the asks that guard 1 to 3. | Accepted (Micaiah) |
| 2026-10-03 | Add areas 5 to 10 (the rest of the ranked impact list) | No freedom level, insecure mode or convenience policy can loosen composition, egress, peer actions, persistence, destructive operations or evasion; the freedom levels now apply only to everything else. More friction for powerful workflows in these areas; each loosening becomes a reviewed code change. | Accepted (Micaiah: "They made sense to not open freely on a whim.") |
| 2026-10-03 | Note (not a change): a possible owner-only FFI debugging mode | Raw memory access is a total escape if any agent can reach it; a crash kills whatever process it runs in. Only acceptable owner-started, on a disposable instance, confirmed each time, audited, with a pinned library. Parked. | Recorded as a warning for any future proposal |
