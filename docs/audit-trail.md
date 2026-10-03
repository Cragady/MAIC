# Audit trail

The audit trail is an optional, local record of every tool call MAID's agents make, kept so that [`maid-leak-audit`](leak-audit.md) can audit sessions that left no transcript behind. It is **off by default**. This page is for deciding whether you want it, and for running it once you do.

## The problem it solves

The leak audit reads session transcripts. A recorded session keeps one in `<state>/maid/sessions/`; an unrecorded one (`--no-record`, `maid -p` without `--record`, a temporary TUI session) keeps it only in the runtime directory, which is gone at logout. So whatever an unrecorded session's agent did, run a `busctl` or probe `/run/user` for a socket, can no longer be audited the next day. The audit trail closes that gap: one small entry per tool call, recorded session or not, in the state directory, where it outlives the logout until an audit has looked at it.

## Who it is for, and who does not need it

You want it if you run unrecorded sessions and want to know afterwards what their agents reached for; if other people or other agents use MAID on your machine (through `maid-server` or headless jobs); or if you need a durable, private record that the audit was done and what it found.

You do not need it if you record every session (then the transcripts are the record, and the leak audit reads them as it always has), or if you never run the leak audit. Leaving it off costs nothing: MAID writes no trail, and every start checks one setting and goes on.

## Turning it on

```
maid audit-trail init                 # writes ~/.config/maid/audit.lua, enabled = false, a comment on every key
$EDITOR ~/.config/maid/audit.lua      # enabled = true, and anything else you want to change
maid audit-trail schedule install     # a systemd user timer that runs maid-leak-audit every `every`
maid audit-trail status               # what is there, by state; never what the entries hold
```

Every parameter lives in `~/.config/maid/audit.lua`, your own global file, evaluated at your Lua level like `diction.lua` ([settings.md, Lua levels](settings.md#lua-levels)). No project can set any of it: a project's `.maid/audit.lua` is never read, and an `audit` table in a project's settings changes nothing. Once maid-server has accounts ([roadmap.md](roadmap.md)), only an administrator configures the trail, the archive and the off-site commands.

## What it records, and what it never records

One JSON line per tool call, in every session (the TUI, `maid -p`, maid-server sessions, subagents):

```json
{"id": 4182, "time": "2026-10-02T09:14:03Z", "session": "20261002-091200-tui", "recorded": false,
 "workspace": "/home/you/dev/app", "tool": "run_shell", "arguments": {"command": "busctl --user list"},
 "decision": "ask", "reason": "", "approval": "no", "judged_by": "user", "ran": false, "ok": false}
```

* `id`, monotonic across all sessions; `time` in UTC; the `session` id and whether it was `recorded`; the `workspace`.
* The `tool` and its `arguments` exactly as the model gave them. For `write_file`, `edit_file` and the like that includes the text the agent wrote.
* What the harness made of it: its `decision` and `reason`, your answer at the prompt (`approval`), the reviewer's verdict and model (`review`; with a checker panel also `review.judged_by`, the judge that decided), and who decided (`judged_by`: harness, reviewer, user, `unattended` when the turn was unattended and the engine denied the call at once, or `timeout` when nobody answered the approval prompt within `approvals_timeout`; in both of those, nobody refused anything).
* Whether it `ran`, and how it ended (`ok`, and `exit` for a command).

Never: conversation text (your messages, the model's replies), feedback you typed with a refusal, your answers to the model's questions, the reviewer's reasoning, or any tool output. Files are 0600 in a 0700 directory, `<state>/maid/audit-trail/`, beside `index.json` (the audit's bookkeeping: id ranges, states, dates and verdict words, never content) and `seq` (the last id given).

## What it costs

* **Disk.** A command or a read is a few hundred bytes; a write carries its text. A busy day of a few thousand calls is about a megabyte. A container file past `file_mb` (16 MB) continues in a numbered part (`20261002.jsonl`, then `20261002.2.jsonl`), and the live trail past `live_mb` (256 MB) holds the next start for an audit, so it cannot grow without bound unseen.
* **An audit's GPU time.** Only entries that name an escape route (the keyword list in [leak-audit.md](leak-audit.md#what-it-checks)) go to the judge; most days have none. Each candidate is one request to the local judge. With thinking off it takes seconds; with thinking on it can run to `judge_max_tokens`, which on an 8 GB card at a few tens of tokens a second is up to about a minute a candidate.
* **The hold.** With no scheduler, or a scheduler that has not run for `grace` x `every`, the next MAID start runs the audit before the session opens (`enforce = "judge-and-hold"`), and you wait for it. A schedule keeps the audit out of your way; `scan-and-continue` and `notify` trade the wait for less certainty.

## How entries move

Containers are only containers: each **entry** has its own state, and entries leave the live trail one by one.

| State | Meaning |
| :-- | :-- |
| live | younger than `live_window` (1 day): the newest entries, in the file being appended to |
| stale live | older than that and not retired: never audited, or audited with its stale timer still running |
| archival | the **retirement signal**: audited successfully at least once, and its result unchanged for `stale_days` (14) |

An entry's timer starts at its first successful audit. Each later audit compares the entry's result (its verdict, or that it is no candidate) with the last one: unchanged, the timer keeps running; changed, the timer starts over, and an archival entry goes back to stale live. An entry nobody has audited is never retired, however old.

The retirement signal is one signal with one outcome per setting: with `archive = "off"` (the default) it **deletes** the entry; with `archive = PATH` it **moves** the entry into a chunk at that path instead. Moving chunks further, to cold storage, is a separate and optional step, **off-site**, which MAID only recommends commands for:

```
written --> live --(older than live_window)--> stale live --(retirement signal)--+--> deleted            archive = "off"
                                                  ^      |                        |
                                                  +------+                        +--> archived (chunk)   archive = PATH
                                         a changed result restarts                          |
                                         the timer (and ends archival)                      +--(optional, yours)--> off-site
```

### A worked example

Defaults (`live_window = "1d"`, `stale_days = 14`) and a daily audit at 03:00.

* **Sep 1**: an unrecorded session writes entries 1 to 3; entry 2 is `busctl --user list`. On Sep 2 entry 7 is written.
* **Sep 2, 03:00**: entries 1 to 3 are first audited (entry 2 judged *reached*); their timers start. Still live (under a day old).
* **Sep 3**: 1 to 3 are stale live, results unchanged. Entry 7 is first audited; its timer starts.
* **Sep 10**: you switch the judge to thinking. Entry 2 is now judged *mentioned*: its result changed, so its timer restarts on Sep 10.
* **Sep 16**: entries 1 and 3 have been unchanged for 14 days: archival, and retired by this audit (deleted, or moved into a chunk). Entry 2 stays.
* **Sep 17**: entry 7 retires, newer than entry 2 but settled sooner. That is intended: an entry whose result keeps changing is the one worth keeping in view.
* **Sep 24**: entry 2 retires, if its result held.

### Audit order

`order` decides which entries the judge sees first: `stale-first` (default: stale live and archival oldest first, then live), `live-first`, `oldest-first` or `newest-first`; `maid-leak-audit --order` overrides it for a run. It matters when the judge fails partway: the audit stops sending at the first failure, the entries before it in that order are audited, and the rest keep their state for the next run. `stale-first` settles what is waiting to retire before it looks at what is new.

## The archive

With `archive = PATH` (any mounted directory: an external drive, a network share, an rclone mount you set up), each audit that retires entries writes them to `audit-chunk-<UTC>-<n>.tar.gz` files there (Python's `tarfile`, gzip), split at `chunk_mb`. A chunk holds, per original container, a member with that container's retired lines; `map.json`, the entry id ranges with their original container, result signature (the sorted candidate ids with their verdicts, hashed), first and last audit and timer start; and `manifest.json`, the audit id, the report it belongs to and the SHA-256 and size of every member. A `<chunk>.sha256` file beside each chunk lets any copy of it be checked later with `sha256sum -c`.

Each chunk is written to a temporary name, read back and checked against what was meant to go in, and only then renamed; only after every chunk of the audit is in place are the live copies removed, each container rewritten under the writer's lock so a call made meanwhile is never lost. Any failure (the directory missing because the drive is not mounted, a full disk, a chunk that does not read back) removes nothing: the entries stay archival and the next audit tries again. MAID never creates the archive directory, since a missing one is usually an unmounted drive. `index.json` keeps a pointer (id range, container, chunk, signature, audit id) for every archived range.

`maid-leak-audit --from-archive CHUNK|all` audits archived chunks again: each is checked against its manifest, extracted into a private temporary directory, audited with the live trail, and the copy is deleted when the audit ends. Nothing is ever taken back into the live trail or its index.

## Off-site: the haircut, recommended and never run

The archive grows until you move old chunks on. `maid audit-trail offsite DEST [--older-than 90d]` lists the chunks older than that, their total size, and prints the commands to move them to DEST. **It runs none of them**: copying, syncing or uploading your audit data is your decision, made each time. It looks for these on your PATH and prints the best fitting command for each one it finds:

| Tool | Why you might pick it | What it prints |
| :-- | :-- | :-- |
| rsync | another disk, a mounted share, or `host:path` over ssh; the plainest | `rsync -a --checksum --remove-source-files` limited to the chosen chunks, then `sha256sum -c` at DEST |
| rclone | cloud storage (S3, B2, Drive, ...) already configured as a remote | `rclone move --checksum` with an `--include` per chunk |
| restic | encrypted, deduplicated, versioned backups you can prune later | `restic backup` of the chunks, `restic check --read-data`, then the removal |
| kopia | the same, with a UI if you want one | `kopia snapshot create` of the archive, `kopia snapshot verify`, then the removal |
| syncthing | another machine of yours keeps the copy, no cloud | how to share the archive as Send Only, with `ignoreDelete` on the receiving side, then the removal |

Then a coreutils fallback (`cp -a`, `sha256sum -c` against each chunk's `.sha256`, `rm`, one chunk at a time) when `cp` and `sha256sum` are there, and last, plain steps for any tool or medium: copy, check against the hashes, and only then delete.

## Scheduling and enforcement

A schedule is required while the trail is on; how strict MAID is about it is yours to set.

* **With systemd.** `maid audit-trail schedule install` writes `maid-leak-audit.service` and `maid-leak-audit.timer` (from `contrib/systemd/`) into `~/.config/systemd/user/`, the timer repeating every `every`, and enables it with `systemctl --user`. `schedule remove` disables and deletes them. Run `install` again after changing `every`.
* **Without systemd** (or when the timer cannot run): the data-driven check. Every completed audit records `next_audit_due` in `index.json`; every entry point (a TUI start, `maid -p`, `maid status`) reads that and the containers' sizes, which costs a stat or two. If a scheduler ran the audit, nothing happens.

MAID enforces when the audit is due and no timer is installed, when it is overdue past `grace` x `every` (whatever is installed), or when the live trail passes `live_mb`. Then `enforce` decides:

* `judge-and-hold` (default): the full audit with the local judge, before the session opens, everything else on hold, with a progress line and then the audit's one-line result. With `start_services` the judge's llama.cpp service is started first if it is down. If the judge still cannot run, MAID runs the phase-1 scan instead, marks the entries it read *scanned, pending judgement*, says so, and the next start tries the judge again.
* `scan-and-continue`: the phase-1 scan, then the session.
* `notify`: one line, then the session.

## The judge

`judge` names the preset; like every leak audit it must resolve to this machine. `judge_thinking = true` (the default) lets the model think before its verdict, which gives better judgements on borderline cases (did the call reach the bus, or only mention it?) at the cost of time and tokens per candidate; `judge_max_tokens` caps the whole reply, thinking included, so one candidate cannot run away (one that spends it all is judged *unclear*). The verdict is read from the final answer only, never from the thinking; the thinking goes into the private report under its candidate for you to read. `maid-leak-audit --thinking on|off` overrides it for a run, and when the server refuses thinking for the model the run falls back to thinking off and the report says so.

## Every audit.lua key

| Key | Default | Meaning |
| :-- | :-- | :-- |
| `enabled` | `false` | write the trail at all |
| `every` | `"1d"` | how often the audit runs: the timer and the due check (`30m`, `12h`, `1d`, `2w`) |
| `grace` | `3` | with a timer installed, a start holds only once the audit is `grace` x `every` overdue |
| `live_window` | `"1d"` | entries younger than this are live |
| `stale_days` | `14` | days of an unchanged result before the retirement signal |
| `order` | `"stale-first"` | which entries are judged first: `stale-first`, `live-first`, `oldest-first`, `newest-first` |
| `enforce` | `"judge-and-hold"` | what a due audit does at start: `judge-and-hold`, `scan-and-continue`, `notify` |
| `start_services` | `true` | `judge-and-hold` starts the judge's local service when it is down |
| `judge` | `"qwen-9b"` | the preset that judges; it must resolve to this machine |
| `judge_thinking` | `true` | the judge thinks before its verdict |
| `judge_max_tokens` | `2048` | the judge's reply budget per candidate, thinking included |
| `file_mb` | `16` | a container past this continues in a numbered part |
| `live_mb` | `256` | the live trail past this holds the next start for an audit |
| `chunk_mb` | `256` | archive chunks are split at this |
| `archive` | `"off"` | `"off"`: the retirement signal deletes; a directory (`~` expands): it moves into chunks there |

## Commands

```
maid audit-trail init                            write ~/.config/maid/audit.lua (never overwrites it)
maid audit-trail [status [--json]]               on or off, files and size, entries by state, schedule, archive
maid audit-trail purge                           delete the live trail and its index, after a yes at a terminal
maid audit-trail offsite DEST [--older-than 90d] print, never run, the commands to move old chunks to DEST
maid audit-trail schedule install|remove         the systemd user timer
maid-leak-audit [--order ORDER] [--thinking on|off] [--scan] [--from-archive CHUNK|all]
```

`purge` keeps `seq`, so ids go on from where they were, and never touches the archive.

## Limits

* The trail is as good as the keyword filter behind the audit: a call that reaches a socket without naming any listed term is recorded but never a candidate ([leak-audit.md, Limits](leak-audit.md#limits)).
* The trail holds no output, so a judge sees a call and the session's other calls, not what came back; it judges intent, not effect.
* It records what MAID's harness saw. A process an agent started that then acted on its own is outside it, as is anything done outside MAID.
* The trail is yours and private, but not tamper-evident: anything running as you can edit or delete it. Signing entries or chaining their hashes is not done.
* `write_file` and similar entries carry the text written, so the trail can hold file contents the agent produced; purge or a short `stale_days` with `archive = "off"` keeps that short-lived.
* Two MAID starts that both find the audit due race for its lock: one audits; the other's audit and scan are refused while it runs, it says so, and its session opens.
* Off-site is never automatic. A full archive only grows until you act on `maid audit-trail offsite`.
