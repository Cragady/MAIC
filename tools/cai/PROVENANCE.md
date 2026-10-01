# Provenance

This directory is cai-tools, Micaiah's own code, moved into MAIC whole. How to run it and what it does with a MAIC session: [docs/cai.md](../../docs/cai.md).

* **Source**: `~/dev2/cai-tools` (origin `git@github.com:CascadeRefiningInc/cai-tools.git`), commit `1437a54ef3e98ddfca2daa7c50869959f692c324` (`git -C ~/dev2/cai-tools rev-parse HEAD`, committed 2026-09-23).
* **Copied**: 2026-10-01, every tracked file except `diction/`, in the repository's own layout (`src/cai/`, `tests/`, `docs/`, `reference/`, `bin/run-all.sh`, `README.md`, `IMPROVEMENTS.md`, `pyproject.toml`, `.gitignore`, `.cai-enrollment`), so the tests find the package where they always did. Not copied: the untracked `uv.lock`, `build/`, `.venv/` and `src/cai_tools.egg-info/`.
* **diction** is not here. It is MAIC's own, in the top-level `diction/` (moved there with new engines by a separate change). The dispatcher's `diction` entry is unchanged; `bin/cai` puts MAIC's top-level directory on `sys.path` so `diction.cli:main` resolves to that package, and until it has a `cli.py`, `cai diction` prints cai's existing not-installed message (exit 4). The one file of cai-tools' `diction/` that cai itself owns, the `reflow-lines` snapshot `cai sync` wrote (`sync-frozen/reflow-lines-20260903T071213Z.py`), is carried unchanged into MAIC's `diction/sync-frozen/`, where the sync job and its test look.

## Added

| file | what |
| :--- | :--- |
| `bin/cai` | The wrapper: puts `src/` (a checkout) or `share/maic/tools/cai/src` (installed) and MAIC's top-level directory on `sys.path`, calls `cai.dispatch.main` with argv untouched, returns its exit code. Installed as `bin/cai`, with `bin/maic-cai` a link to it. |
| `run_tests.py` | ctest `cai_tools`: every `tests/test_*.py` as written, from this directory, with a dist-info generated from `pyproject.toml` on the path (the entry-point registry an installed copy has, which `test_surfaces` reads), MAIC's state and runtime directories and cai's temp and grant stores in a temp dir, and `CAI_NO_REMOTE` as the default. Without `MAIC_NETWORK_TESTS=1` it skips `test_remote.py` and sets `GIT_ALLOW_PROTOCOL=file` for every suite (below, Tests). |
| `src/cai/grammar/maic.py` | MAIC sessions as a transcript format: detection by content, where sessions live, forks (`resumed_from` and its record count, by path then by id), the projection onto the record grammar, and the record shapes this suite writes into a session. |
| `src/cai/transfairy/maic.py` | trans-fairy's cut, compose, graft and inject on a MAIC session, producing a MAIC session. |
| `tests/test_maic.py` | The MAIC tests: every record type detected and projected, forks, read, fabricate, trans-fairy, the backup rule, `restore`, `list-backups`, liveness. |
| `PROVENANCE.md` | This file. |

## Changed

Every change keeps the existing behaviour on Claude Code transcripts and claude.ai exports; a MAIC session takes a branch of its own, chosen by `grammar/maic.is_maic` on the records.

| file | change | why |
| :--- | :--- | :--- |
| `src/cai/dispatch.py` | With no installed distribution, the `cai.tools` registry is read from `pyproject.toml`; then `diction` is added as `diction.cli:main` when that module exists. | MAIC runs the package from its directory without installing it; the registry stays in the one file it was written in. The `diction` lookup is rule 5 above. |
| `src/cai/reflow/__init__.py` | The same fallback for the `cai.reflow.operations` registry. | Same reason. |
| `src/cai/remote.py` | `sibling_url()` with no `repo` derives SOPIA's URL from cai-tools' recorded origin (`ORIGIN`) instead of asking git; given a `repo`, it asks git as before. | The repository the package sits in is now MAIC's, whose origin has another owner; deriving from it would point at a SOPIA that does not exist. The URL is the same one cai derived. |
| `src/cai/sync/runner.py`, `src/cai/sync/specs.json` | `REPO` is MAIC's root (five levels up, was three); the `reflow-lines` job reads `tools/cai/src/cai/reflow/lines.py` (was `src/cai/reflow/lines.py`). | "This repository's root" is MAIC's, and that is where diction, the job's destination, lives. The source file is the same file. |
| `src/cai/read/reader.py`, `src/cai/read/cli.py` | A MAIC session is projected before reading; a MAIC fork is read with its parent's records first. | read on a MAIC session. |
| `src/cai/redact/redactor.py` | `structural_values` takes extra keys, and for a MAIC session keeps `tool_call_id`, `id` and `child` values; `load_records` projects a MAIC session for `--project`. | redact on a MAIC session without breaking its threading. |
| `src/cai/fabricate/maker.py`, `src/cai/fabricate/cli.py` | `insert_maic`: each turn as a `msg` plus its `user` or `assistant` record, marked with cai's own fields (`origin.kind: fabricated`, `fabricated`, `fabricatedFor`) or cai's loud text; the default timestamp is MAIC's local form for a MAIC session; `uuids` lists only records that have one. | fabricate into a MAIC session. The help text is unchanged. |
| `src/cai/transfairy/stages.py`, `src/cai/transfairy/paths.py` | MAIC branches in `install`, `graft_install`, `inject_install`, `compose`, `truncate`, `audit_ledger` and `rebuild_ledger`; `Tree.projects_override` records whether `--claude-projects` was given. | trans-fairy on a MAIC session: a MAIC session out, into MAIC's home for the target cwd unless a projects dir was named. The true-silent warning is cai's text. |
| `src/cai/grant/store.py` | With the default session glob, running MAIC sessions are callers and grantees beside Claude Code's live sessions. | MAIC keeps no registry; the `start` record's host and pid say which sessions are running. |
| `src/cai/transfairywrite/writer.py`, `src/cai/transfairywrite/cli.py` | Before any write, a copy under `<state>/sessions/.backups/<id>/<UTC>.jsonl` (0600, directory 0700), its path on stderr; the write through a temp file and a rename; a `rewritten` record on a MAIC session; MAIC's checks and liveness for a MAIC session; `list-backups ID` and `restore ID [--backup TS] [--ignore-live] [-n]`; a MAIC section in `--man-help`. | MAIC's backup rule (docs/cai.md). The JSON report, the arguments and `--help` are unchanged. |

## Tests

None of cai's 26 test suites was changed. `tests/test_maic.py` was added. All 27 pass under `run_tests.py` (ctest `cai_tools`).

Three suites reach the network, by lifting `CAI_NO_REMOTE` so that `source.remote_payload` calls `remote.read`, which clones or fetches SOPIA over ssh into `~/.local/state/cai/mirror/SOPIA.git`: `test_remote.py` (all of its remote checks), `test_sync.py` (after it pops `CAI_NO_REMOTE`: "without it, SOPIA answers", `cli.main([])`, `cli.main(["list"])`) and `test_notation.py` (after it pops it: `cli.main([])`, `lookup`, `audit`). No other suite reaches the network; every remote lookup goes through the same `CAI_NO_REMOTE` check. MAIC's gate keeps the network off unless `MAIC_NETWORK_TESTS=1`: the runner skips `test_remote.py` and gives every suite `GIT_ALLOW_PROTOCOL=file`, so git refuses the transport before connecting and the other two take cai's unreachable-remote path. 26 suites run and pass that way ([docs/testing.md](../../docs/testing.md#the-network)).

## --help

Compared on 2026-10-01 against the uv-installed `~/.local/bin/cai` (an editable install of the same commit), with temp state directories: `cai --help` and `cai TOOL --help` for all 17 tools (`trans-fairy`, `redact`, `trans-fairy-write`, `notation`, `grant`, `commit`, `enroll`, `hook`, `edit`, `time`, `document`, `name`, `fabricate`, `sync`, `flow`, `read`, `reflow`) and `cai diction --help`: output (stdout and stderr together) and exit code identical for every one. The same comparison over `-h` and `--man-help` (57 invocations in all) differs in one place only: `trans-fairy-write --man-help` gains its MAIC SESSIONS section and the two new subcommands in its synopsis.

## Dependencies

None outside the standard library, as in cai-tools (its `pyproject.toml` declares none). diction's Whisper and CUDA wheels stay with diction.

## MAIC sessions: what maps and what does not

| cai concept | in a MAIC session |
| :--- | :--- |
| `uuid` / `parentUuid` chain | None. The projection mints positional ids and chains them in file order; `trans-fairy split --at-uuid` is refused with a message naming `--at-line` and `--before-text`; `fabricate` reports an empty `uuids` list. |
| `sessionId` | The file name. A MAIC record carries none, so `trans-fairy-write` reports `sessionId: null` and skips the id agreement check; trans-fairy mints MAIC ids (`<time>-cai-<pid>`). |
| lineage on the last message (`previousSessionId`, `originSessionId`, `includedSessionIds`) | On the `cai` marker record that announces the operation; `state --audit` and `--ledger` read it there. |
| `*-boundary` system records | `cai` records with the same `subtype`; MAIC ignores the type on load. |
| a notice the model reads (loud modes) | MAIC's own `inject`, `msg` and `context` records, as `maic sessions inject` writes. |
| the client's session registry (`~/.claude/sessions`) | The last `start` record's host and pid; a session is live while that process runs on this host. |
| `isSidechain` | None: a MAIC subagent has its own file, named by the parent's `tool` record (`child`), and is not followed. |
| `isCompactSummary` | The `compact` record (its `summary`, or the stage when there is none). |
| one record per message turn | `fabricate --at` and its `inserted` count are in records; a MAIC turn is two (`msg` and its transcript record). |
| `msg` records (what the model saw) | Meta records in the projection; `read` shows what was said (`user`, `assistant`, `tool`, `context`, `compact`). Provider `raw` blocks and thinking are not shown, as cai drops thinking everywhere. |
| grafting into a Claude Code transcript | A MAIC session grafted onto a Claude Code base is converted and goes through cai's build checks, which refuse adjacent same-role turns; MAIC sessions have them around notices, compactions and tool results. Graft onto a MAIC base, or compose. |
| a Claude Code transcript inside a MAIC compose or graft | Converted to text turns: tool traffic in fences, thinking dropped (its signature only verifies in its own session). |
| `state`, `--audit`, `--ledger` | Read the projects directory only; `--claude-projects` points them at a MAIC home. |
| in-place writes other than `trans-fairy-write` | `redact` with `--backup` and `reflow --replace` keep cai's in-place behaviour and their required `--backup`; MAIC's extra copy is `trans-fairy-write`'s alone. |
| `notation`, `enroll`, `hook`, `edit`, `time`, `document`, `name`, `sync`, `flow` | Read no transcript; unchanged. |
