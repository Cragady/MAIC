# Security policy: the critical tier

This document is the policy. The code enforces it; a change to it goes through the change pipeline below. Decided by Micaiah, 2026-10-03 ([decisions.md](decisions.md)).

## The rule

For the critical areas, the shipped default is the most open setting ever allowed. Settings, project layers, agents, profiles, protocols, origins and every convenience or freedom policy (`insecure` levels, relaxed trust, `tools_without_reasoning = "insecure"` and the like) can only add restrictions here, never loosen them. Loosening a critical default is a reviewed code change, never a setting. Freedom lives in the tiers below.

## Critical areas

1. **Secret exfiltration:** keys, tokens, environment, `~/.ssh` and the like leaving through fetch, writes, model output or messages.
2. **Writes to protected places:** the harness, the tripwire, settings, the trust store, and anything outside the workspace.
3. **Privilege loosening:** project layers, agents, peers or origins widening mode, trust, timeouts or policies.
4. **Approval bypass:** anything that would let an action that must be asked run unasked (unanswered approvals, unattended turns, timeouts, self-approval by the party that caused the action).

The rest of the ranked impact areas (execution by composition, network egress, peer-originated actions, persistence, destructive operations, policy evasion) sit in the tier below, where Micaiah's levels from `refuse` to `insecure` apply.

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
