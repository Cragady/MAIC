# cai-tools in MAIC

cai-tools is Micaiah's own suite of transcript and repository tools. All of it lives in MAIC now, in [`tools/cai/`](../tools/cai/), with every tool's name, command line, exit codes, output and file formats kept exactly as they were. What MAIC adds is one more transcript format, the MAIC session, detected by its content, and one rule for `trans-fairy-write`: a backup before any write. Where the code came from and every line that changed: [tools/cai/PROVENANCE.md](../tools/cai/PROVENANCE.md).

## Running it

`cai` is a thin wrapper installed beside `maic`: it calls cai's dispatcher with the arguments untouched, stdin, stdout and stderr inherited, and the dispatcher's exit code returned. `maic cai TOOL ...` runs that same wrapper, so the two spellings are identical:

```
cai read SESSION.jsonl                 maic cai read SESSION.jsonl
cai trans-fairy --man-help             maic trans-fairy --man-help
cai trans-fairy-write T --from S ...   maic trans-fairy-write T --from S ...
```

`maic-cai` is a link to `cai`. `maic trans-fairy` and `maic trans-fairy-write` are short for `maic cai trans-fairy` and `maic cai trans-fairy-write`. `maic help cai` prints the dispatcher's listing, `maic help cai TOOL` a tool's own help, `maic help trans-fairy` and `maic help trans-fairy-write` theirs; `maic tools` lists every tool in both spellings. `scripts/release.sh` links `~/bin/cai` and `~/bin/maic-cai` to the release, like the other helpers.

The dispatcher's exit codes are cai's: `0` ok, `2` no such tool, `3` not implemented, `4` a known tool that is not installed.

**The old uv install is retired.** cai used to be installed from `~/dev2/cai-tools` with `uv tool install --editable .`, which put `cai` in `~/.local/bin`. MAIC's `cai` in `~/bin` comes first on PATH once a release links it. If the old one is still installed it can be reached as `~/.local/bin/cai` (or `uv tool run --from cai-tools cai`), and `uv tool uninstall cai-tools` removes it.

## The tools

Each in the README's words, or the tool's own first line where the README has none. Every one keeps its full reference behind `--help` or `--man-help`.

| tool | what it is | on a MAIC session |
| :--- | :--- | :--- |
| `cai` | list installed tools, and name known ones that are not | |
| `trans-fairy` | rebuild a chat export into a resumable transcript; move a conversation out of one body into another | `split --truncate` cuts a MAIC session (`--at-line` counts the file's lines, `--before-text` works as before; `--at-uuid` is refused, a MAIC record has no uuid) into a new MAIC session. `compose` with a MAIC root or suffix composes a MAIC session (a fork is taken as the conversation it holds; a Claude Code side comes over as text turns). `install` puts a MAIC session where MAIC would keep one for `--target-cwd` (`projects/<encoded path>/` under a `MAIC.md`, else `general/`), or in `--claude-projects` when that is named, never over an existing file; `--reflow` mints a MAIC id; the resume commands it prints are `maic -r`. `install --graft-onto ID` finds a MAIC base by id or unique prefix and writes a new session; `install --inject` marks one. Lineage and boundaries are `cai` records MAIC ignores on load; a loud notice is MAIC's own inject, msg and context triple. `state`, `state --audit` and `state --ledger` read the projects directory, which `--claude-projects` can point at a MAIC home. |
| `redact` | remove credential material from a transcript | The same shapes, backup and verify. MAIC's threading values (`tool_call_id`, a tool call's `id`, a subagent's `child`) are kept the way Claude Code's are; `--project` is `read`'s projection. |
| `trans-fairy-write` | overwrite an installed transcript, safely and deliberately | A MAIC session is a valid target and `--from`, checked with MAIC's shapes; it looks live while the `maic` process that last opened it runs, or by cai's five-minute rule. The backup rule below applies to every target. |
| `read` | project a transcript to what was said | The conversation, `--select tools`, the compaction slices and `--boundaries`, with `Ln` naming the session file's own lines. A fork is read as MAIC loads it: the parent's first records, then its own. |
| `fabricate` | a testing tool for building transcripts | Into a MAIC session each turn is written as MAIC writes one, a `msg` and its `user` or `assistant` record, with the same marks cai puts on a Claude Code record. |
| `reflow` | reshape data with any member of an open family, verifying content survived | `reflow context` reads a MAIC session through `read`'s projection. `reflow FILE` refuses a MAIC session as it refuses a Claude projects file (`--to` writes elsewhere); `-n` on a MAIC session says whether the result would still load. `--rewrite-session` is the operator's way through, below. |
| `notation` | the living dictionary of markers, and the ledger of what changed | Reads no transcript; unchanged. |
| `grant` | ask whether a time-scoped permission is in force, right now | A running MAIC session is a caller and a grantee like a live Claude Code session: its id, its title as the name, its workspace. MAIC keeps no session registry, so "running" is the `start` record's pid on this host. |
| `commit` | commit only when the preconditions hold | Finds its session through `grant`, so the same. |
| `enroll` | what this repository is held to, and how strongly | Reads no transcript; unchanged. |
| `hook` | what an installed git hook actually does | Reads no transcript; unchanged. |
| `edit` | an anchored replacement that refuses to take more than it names | Reads no transcript; unchanged. |
| `time` | stamps and windows, in UTC, computed rather than typed | Reads no transcript; unchanged. |
| `document` | the shapes, where notation holds the atoms and a tool does the work | Reads no transcript; unchanged. |
| `name` | session names: what they mean, and why they are shaped that way | Reads no transcript; unchanged. |
| `sync` | carry what SOPIA defines, so cai works from a single download | Unchanged; `reflow-lines` carries into MAIC's `diction/sync-frozen/`. |
| `flow` | what order, and what must hold between the steps | Reads no transcript; unchanged. |
| `diction` | speech to a maintained numbered list | Not in `tools/cai`: diction is MAIC's own, in the top-level `diction/`. The dispatcher's entry is cai's (`diction.cli:main`); the wrapper puts MAIC's top-level directory on Python's path so it resolves there, and until `diction/cli.py` exists `cai diction` prints cai's own not-installed message (exit 4). |

What a cai concept has no MAIC counterpart for is listed per tool in [PROVENANCE.md](../tools/cai/PROVENANCE.md#maic-sessions-what-maps-and-what-does-not).

## MAIC sessions

A MAIC session is told apart from a Claude Code transcript by its records, never by its path: MAIC's record types (docs/sessions.md) with their own fields, and none of Claude Code's (`uuid`, `parentUuid`, `message`, `isMeta`, `sessionId`) outside cai's own records. The shared layer is `tools/cai/src/cai/grammar/maic.py`: detection, where sessions live, forks (`resumed_from` with its record count, followed by path and then by id), and the projection onto the record grammar every cai reader already speaks, one record per line so line numbers stay the file's. Every MAIC record type maps to something: `user` and `assistant` to message records, `tool` to a call and its result (for reading, a result whose whole output MAIC kept is followed by the line `[full output, display only: the model saw the capped result] N bytes: maic sessions output ID CALL`; a resumable projection carries only what the model got, [sessions.md](sessions.md#full-output)), `context` to a user notice, `compact` to the compaction boundary, everything else to a meta record carrying its fields. The existing `maic sessions` subcommands (inject, graft, compose, read, state, time, name) are MAIC's own and unchanged.

## The backup rule

`trans-fairy-write` rewrites a session file in place and backs up first; the other commands that rewrite one, and which of them keep a copy, are listed in [sessions.md](sessions.md). Before any write, to a MAIC session or a Claude Code transcript, it copies the file to `~/.local/state/maic/sessions/.backups/<id>/<UTC time>.jsonl` (0600; `$XDG_STATE_HOME` moves it) and says where on stderr, beside the `--backup` cai always required. The new file lands through a temporary file in the same directory and a rename. A MAIC session then gets a `rewritten` record naming the copy and the command that wrote it; a Claude Code transcript stays byte for byte what `--from` gave. Nothing in the existing arguments or the JSON report changed. Two subcommands are new:

```
cai trans-fairy-write list-backups ID              the copies kept for a session
cai trans-fairy-write restore ID [--backup TS]     put the newest copy (or the one stamped TS) back
```

### `reflow --rewrite-session`

`cai reflow FILE` will not rewrite a transcript in place: a file under a Claude Code projects directory, and (added in MAIC, the same refusal applied to the new format) a MAIC session, in MAIC's sessions folder or recognised by its records wherever it is. `--to PATH` writes the result elsewhere and leaves the original alone. When the original really has to change, `--rewrite-session` does it, at a terminal only:

1. it says which file, what kind of transcript it is, and that it will be rewritten in place;
2. it shows whether the result would still load as that kind of transcript, with record and turn counts before and after (trans-fairy-write's own checks decide);
3. it recommends the dry run, printing the exact command (`... --rewrite-session --dry-run`), which reports the same verdict and writes nothing (exit 1 when the result would not load);
4. it asks for the exact phrase `yes, rewrite it`; anything else stops, and off a terminal it refuses outright, so no agent can pass it;
5. it refuses a transcript that looks live, copies the original to `.backups/<id>/` (where `list-backups` and `restore` find it), writes, and checks that what landed is byte for byte what was validated, putting the copy back if not;
6. for a MAIC session it appends a `rewritten` record, and when the result does not load it prints the `cai trans-fairy-write restore ID --backup TS` that undoes it. `restore` works on a session that no longer parses.

`ID` is a MAIC session id, a unique prefix, or any transcript's path. `restore` copies what is there first, so it can itself be undone, refuses a session that looks live (`--ignore-live` as for a write, `-n` for a dry run), and appends a `rewritten` record naming both copies. `maic sessions` never lists the `.backups` directory. `maic sessions redact --in-place` takes its copy the same way, in the same place, so `list-backups` and `restore` see it too. `redact` and `reflow --replace` keep the in-place modes cai gave them, each behind the `--backup` it requires.

## Permissions

These cai invocations write nothing and are on the default allow list, under both spellings of the wrapper (`run_shell:cai ...` and `run_shell:maic-cai ...`): `read*`, `time*`, `--help`, `trans-fairy --help`, `trans-fairy --man-help`, `trans-fairy state` and `trans-fairy state --audit`. `read --out` writes a new file, so `read* --o*` is a default ask entry. The read-only classifier counts the same commands as read-only under `cai`, `maic-cai`, `maic cai` and `maic trans-fairy`, plus any tool's `--help`, `-h` and `--man-help`, and only as one plain command (no redirection, chaining or substitution); the allow entries match only such a command too (docs/harness.md, the allow list). Nothing that writes is pre-approved: not `trans-fairy-write`, `fabricate`, `commit`, `hook`, `grant`, `enroll`, `redact`, `reflow`, `edit` or `sync`.

## Tests

ctest `cai_tools` runs `tools/cai/run_tests.py`: cai's own 26 suites as written, and `tests/test_maic.py` (every MAIC record type, read, fabricate, trans-fairy on a MAIC session, the backup rule, `restore`, `list-backups`, liveness). The classifier and the default entries are in `core/tests/harness_test.cpp` and `robustness_test.cpp`; `tests/cli_smoke.py` checks `cai --help` against `maic cai --help` and `cai read` against `maic cai read`, byte for byte.

cai needs nothing outside Python's standard library.
