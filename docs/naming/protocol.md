# Naming protocol (proposal)

Status: **proposed** (Micaiah, 2026-10-03). Once accepted, this file is the protocol and changes go through [ledger.md](ledger.md).

Names come in role groups. Each group has a base role, the names that belong to it, the one in active use, and why. A name may be picked as active again after it was retired; nothing here bans a name. The history of every change is in the ledger.

## Role groups

### Reader (base version)

* **Names:** TheMadMaid, MadMaid.
* **Active:** MadMaid (auto-read, reviews diffs); TheMadMaid is the first of the line, kept for general work.
* **Why:** TheMadMaid was the first DeepSeek maid; MadMaid came next as the reviewer. Close names on purpose: same group, same lineage.

### Writer (base version)

* **Names:** BlindWriter, BlindMaid.
* **Active:** BlindWriter (edit mode in her own worktree).
* **Why:** she writes without running anything, so she works blind and "sees" only through the reader's review. BlindMaid is the household form of the same role.

### Overseer (driver, enforcer)

* **Names:** ComfyMaid, HeadMaid, and other heavy-landing household ranks that make the same point (candidates to be proposed).
* **Active:** ComfyMaid (Claude, in Claude Code).
* **Why:** ComfyMaid ties back to comfymaid-review and ComfyUI, where this began; HeadMaid says the rank outright. The group also names the old observation that management has the easiest job.

## Rules

1. **Distinct across groups, alike within.** Names in one group may resemble each other; names in different groups must be distinct at a glance.
2. **The active name is the one in use** in rosters, liaison sends (`--as`), voices and file names.
3. **Every change is a ledger entry:** the role, `old` -> `new`, the reasoning when there is one, and the time as a bare Unix timestamp (seconds since the epoch, an integer; no date formatting or time zones; on Windows too, the value is the Unix timestamp).
4. **Retired names may come back.** A user who wants retired names banned on their side can say so in settings (for example `naming.ban_retired = true`), using the ledger as provenance; the base protocol doesn't ban.
5. **Users keep their own protocols** beside the base ones, adopted the way contributors' protocols are; this protocol is the base, theirs may extend or override it in their own area.
