# Sessions

A session is one JSONL file: one JSON object per line, appended and never rewritten. It holds both what the model saw (the messages, exactly as sent) and what you saw (the displayable transcript), so it can be resumed, forked, exported, listed and scrubbed with nothing but a text tool. Files are created `0600` in a `0700` directory under `~/.local/state/maic/sessions/`. `core/src/session.cpp` writes and reads them; the agent adds records through `SessionLog::write`, which stamps every record with `type` and `time` (local time, ISO 8601 with the offset).

The user-facing side (`-c`, `-r`, `--append`, `maic sessions`) is in [cli/README.md](../cli/README.md#sessions); this page is the format. cai's tools ([cai.md](cai.md)) read and build sessions too, and cai's `trans-fairy-write` is the one exception to never rewriting a session file: it copies the file to `sessions/.backups/<id>/` first and appends a `rewritten` record naming the copy.

## Records

Every line has `type` and `time`. The rest depends on the type.

| type | written by | fields | meaning |
| :--- | :--- | :--- | :--- |
| `start` | every open | `workspace`, `model`, `mode`, `host`, `pid`; for a subagent (kind `sub`) also `parent` (the session whose `task` call started it), `agent` (`profile` in records written before the rename) and `model_reason` | The session was started or resumed here. A file has one per open; `maic sessions` counts them and shows the first workspace as where it was started and the last as where it was last opened, and lists a `sub` under its parent. |
| `msg` | the agent | `role`, `content`, and for assistant turns `tool_calls` (`id`, `name`, `arguments`), for tool turns `tool_name`, `tool_call_id`, `is_error`; `raw_kind` and `raw` when the provider's own blocks are kept | One message as sent to or received from the model. Loading a session replays these in order. `raw` holds provider content that must go back unchanged: Anthropic thinking blocks with their signatures (`raw_kind` `anthropic`), and DeepSeek's `reasoning_content` (`raw_kind` `openai`), which its thinking models require on every earlier assistant turn while tools are in the request. |
| `user` | the agent | `text`, `provider`, `model`, `remote`, `mode`; `queued` when it was sent mid-turn | What you typed, for the transcript. Also where the model and mode in effect are recorded per turn. |
| `assistant` | the agent | `text` | The reply, for the transcript. |
| `tool` | the agent | `tool`, `arguments`, `result` (capped at 64 KB), `ok`; for `task` also `agent`, `child` (the subagent's transcript), `model`, `model_reason`, `steps` and `tokens` (older records: tool `delegate` with `profile`); a reviewed action has `review` with `verdict`, `reason`, `model` and `model_reason` | One tool call with the harness's outcome, for the transcript. The full result is in the `msg` record. |
| `usage` | the agent | `input`, `output`, `context` | Token counts the provider reported for one reply. |
| `model` | a subagent | `from`, `to` (preset names), `model`, `reason` (`usage limit`) | The subagent's model hit its usage limit and the rest of its job runs on `to`. |
| `context` | `--context` and `-C` | `text` | A file attached to the conversation, shown as a notice. |
| `title` | `:rename`, an auto-title, or an import | `text` | The name `maic sessions` shows. The last one wins. |
| `compact` | `:compact` or automatic compaction | `stage` (`prune`, `head`, `all`), `summary` and `messages`, or `bytes_before` and `bytes_after` | Context was freed. What the model sees from here on is re-dumped after it (see `reset`); the transcript keeps everything. |
| `reset` | compaction | `messages` | Forget the `msg` records so far; the ones that follow are the conversation now. |
| `clear` | `:clear` | | Forget messages and transcript; a new conversation starts in the same file. |
| `undo` | `:undo` | `path`, `summary` | A file the agent changed was restored. |
| `resumed_from` | `--no-append`, `--record` with `-c`/`-r`, `--fork-at` | `id`, `path`, `records` | The first record of a fork: this file's history starts as the first `records` lines of the named session. Nothing is copied. |
| `imported_from` | `maic sessions import` | `path`, `format`, `model`, `messages`, `skipped`, `malformed` | The session was built from another tool's transcript (see Import). |
| `inject` | `maic sessions inject` | `role` | The `msg` that follows is a note placed by the command, not something typed or generated (see Building sessions from sessions). |
| `graft` | `maic sessions graft` | `id`, `path`, `messages` | The records that follow the next note were copied from the named session. |
| `compose` | `maic sessions compose` | `id`, `path`, `from`, `records` | This file holds the named session's records from line `from` on, copied; what came before is not here. |

Loading follows `resumed_from` pointers first (by path, then by id if the parent was rehomed), then reads the file: `msg` records become the conversation, `user`, `assistant`, `tool`, `context` and `compact` become the transcript, `reset` and `clear` cut as described. A line that is not a JSON object is skipped. Unknown types are ignored, so an older MAIC can read a newer file.

`SessionLog::write` stamps `time` only when the record has none, so a record copied from another session keeps the moment it was written.

## Homes

The directory a session lives in is its home: `general/` by default, `projects/<encoded workspace path>/` for a project (a workspace with a `MAIC.md`, or `sessions_home = "project"` in settings), or any name under `sessions/`. The encoded path is the absolute workspace with `/` and spaces replaced by `-`, the layout Claude Code uses for its own transcripts. `maic sessions rehome ID [project|general|NAME]` moves a file; forks keep working because they find their parent by id when the path no longer exists.

Unrecorded sessions (`--no-record`, `maic -p` without `--record`) go to the runtime directory instead (`$XDG_RUNTIME_DIR/maic/sessions`, cleared at logout) and are never listed, though `maic -r PATH` can still open one.

## Forks

A fork is a new file whose first record is `resumed_from`. It names the parent and how many of its records count; the parent is never edited, so any number of forks can continue the same past.

* `maic -c --no-append`, `maic -r ID --no-append`, and `maic -p ... -r ID --record` fork at the end of the parent.
* `maic -r ID --fork-at N` (also with `-c`, and with `-p`) forks at record N: the new session loads the parent's first N lines and nothing after them. The parent's file itself is never modified, so N stays meaningful. To pick N, look at the file (`maic sessions path ID`, then number its lines); a `user` record is a natural cut point, since the assistant turn that follows it is then re-generated. `--fork-at` cannot be combined with `--append`.

`maic sessions` shows a fork as "resumed from ID (first N records)".

## Building sessions from sessions

Three commands make a new session out of existing ones. Each creates one file and modifies nothing: the sources are read, never rewritten, so an experiment that lands badly costs a file, not the thread. All three take `--home general|project|NAME` (`project` is the session's own workspace) and print the new id; `maic -r ID` opens the result like any session. Anything placed by a command is marked twice: for a reader of the file by its own record type, and in the conversation itself by a note the model reads as a system message and the transcript shows as a notice, never as a typed turn. The ideas come from cai's `install --inject`, `install --graft-onto` and `trans-fairy compose` ([cleanroom.md](cleanroom.md): behaviour, no code).

| command | what the new file holds |
| :--- | :--- |
| `maic sessions inject ID (--text T \| --file F\|-) [--at N] [--role user\|system]` | `resumed_from` ID at N (the whole file by default), an `inject` record, the note as a `msg` of that role, and a `context` notice `injected ROLE note (DATE): ...`. For handing a resumed session an understanding, a reminder or a repo summary without pretending someone typed it. |
| `maic sessions graft ID --onto TARGET [--at N]` | `resumed_from` TARGET at N (its end by default), a `graft` record, a note (`The next K messages were grafted from session ID on DATE: they took place separately and are not this conversation's own history.`), then ID's records copied: everything but its `start`, `title`, `imported_from` and system-prompt messages. A fork is copied as the conversation it holds (its parent's first records, then its own), not as a pointer. |
| `maic sessions compose ID --from N [--root FILE\|-]` | A `start` record with ID's workspace, model and mode; a `compose` record; the system prompt from ID's first N records; the root text, if any, as a user `msg` with a `context` notice `root (composed, not typed): ...`; a note (`This conversation continues session ID from its record N on ...; what came before the cut is not present. Do not infer what the missing context said.`); then ID's records from N on, copied with the same exclusions. A cheap way to resume a long session: the recent stretch with a short briefing in front of it, instead of the whole history or a compaction summary. |

`N` counts lines of the named file, as `--fork-at` does; `maic sessions path ID` finds the file. A `user` record is the natural cut for all three.

## Reading and summarising

* `maic sessions read ID [--range A-B] [--tools]` prints the conversation as plain text (`[user]`, `[assistant]`, `[notice]` blocks; `[tool]` and `[result]` lines with `--tools`), user turns `A` to `B` (`3`, `3-`, `-5`), for piping into another tool or another session. This is what cai's `redact --project` and `read` do (a projection of the transcript to what was said); it is read-only here, with no in-place path at all.
* `maic sessions state ID` is one screen: title, where and when, model, parent and subagent links, record count with first and last times, turns, replies, tool calls per tool with failures, the files written, edited, moved, copied, deleted or restored, token totals against `budget_tokens` in settings, the last context window, compactions by stage, clears and undos, and every fork and subagent of this session. The counts cover the whole history, parents included.
* `maic sessions time ID [--slowest N]` gives each turn's length (from the prompt to the last record before the next prompt), its tool calls, the total, and the slowest tool calls (each from the record before it to its result). Records are stamped to the second, so that is the resolution.
* `maic sessions name ID [--model M]` titles a finished session with the title model (`title_model` in settings, or `--model`), the same prompt and clean-up as the auto-title after a first turn, appended as a `title` record. A remote title model is refused for a session that ran on a local model, as in the TUI; a running session is refused (`:rename` inside it).

## Import

`maic sessions import FILE [--as claude-ai|claude-code|auto] [--home general|project|NAME] [--conversation UUID]` builds a session from another tool's transcript and prints the new session's id. The result is an ordinary session: it lists, resumes and exports like one written by the agent.

Two formats are recognised, by content unless `--as` says otherwise:

* **claude-ai**: a claude.ai data export, `conversations.json` or one conversation cut out of it. A full export holds every conversation, so `--conversation UUID` picks one; without it the command lists the uuids and names and stops. Messages are threaded by parent uuid and the longest branch is taken (a regenerated answer's abandoned sibling is left out). Pasted files with extracted text are inlined into the user's message as `[pasted file: NAME]`; files without text are counted. The conversation's name becomes the `title`. The workspace is the current directory, since an export records none.
* **claude-code**: a transcript from `~/.claude/projects/<encoded cwd>/<session>.jsonl`. `user` and `assistant` records become messages; an assistant turn is spread over several records there (thinking, text, one per tool call) and is merged back into one message. `tool_use` blocks become `tool_calls`, and the `tool_result` blocks in the following user record become tool messages and `tool` transcript records, paired by id. Sidechain (subagent) records and `isMeta` records (slash-command echoes) are skipped. The workspace is the transcript's `cwd`; the title is its `custom-title`, else `ai-title`, else `summary`; the model named in the records is kept in `imported_from` for reference. Tool names stay as the source had them (`Bash`, `Read`), which the agent understands on resume.

Thinking blocks are never carried over: their signatures only verify inside the session that produced them. Records of kinds MAIC has no use for are skipped and counted; lines that are not JSON objects are counted as malformed. Neither is fatal. The `imported_from` record carries the counts.

Where it goes: `general/` unless `--home` says `project` (under the source's workspace, the Claude Code layout) or a name.

## Redact

`maic sessions redact ID|FILE [--in-place | -o FILE]` writes a copy with credential material replaced by `[REDACTED:kind]` and prints how many values of each kind it replaced. The default output is `./<id>.redacted.jsonl` in the current directory, deliberately outside the sessions tree, so the copy is never listed or resumed by mistake; `-o FILE` puts it elsewhere; `--in-place` rewrites the session file itself (through a temporary file, so a crash leaves the original). An existing output file is never overwritten. The source is never modified except with `--in-place`.

Replacement happens inside JSON string values, never in the serialised line, so the copy is valid JSON and structure survives: `type`, `time`, ids, `tool_call_id`, tool and role names, and provider signatures are left alone. A line that is not JSON is redacted as plain text and counted as malformed. Two passes:

1. A field whose name says secret (`API_KEY`, `token`, `DB_PASSWORD`, `client_secret`, ...) has its whole value replaced with `[REDACTED:secret-field]`, whatever the value looks like.
2. Every other string is scanned for shapes, in this order, and the first pattern to match a stretch of text names it:

| kind | matches |
| :--- | :--- |
| `private-key` | `-----BEGIN ... PRIVATE KEY-----` through the matching `END` line |
| `url-password` | the password in `scheme://user:password@host` |
| `basic-auth` | the password in `-u user:password` / `--user user:password`; the token after `Basic` |
| `env-secret` | the value of `NAME=value` when NAME contains `KEY`, `TOKEN`, `SECRET`, `PASSWORD`, `PASSWD`, `PWD` or `CREDENTIAL(S)` as a word (`API_KEY`, `db_password`; not `monkey`) and the value is 8 characters or more |
| `cli-password` | the argument of `--password`, `--passwd`, `--token`, `--api-key`, `--secret`, `--access-key` |
| `json-secret` | the value of `"password": "..."`, `"api_key": "..."`, `"token": "..."` and similar inside text |
| `bearer` | the token after `Bearer` |
| `jwt` | three base64url parts starting `eyJ` |
| `aws-key` | `AKIA` / `ASIA` access key ids |
| `github-token` | `ghp_`, `gho_`, `ghu_`, `ghs_`, `ghr_`, `github_pat_` tokens |
| `slack-token` | `xoxb-`, `xoxp-`, `xoxa-`, ... tokens |
| `api-key` | `sk-` (OpenAI, Anthropic), `rk-`, Stripe `sk_live_`/`sk_test_`, Google `AIza`, GitLab `glpat-`, Hugging Face `hf_`, npm `npm_`, PyPI `pypi-` |
| `hex-32` | 32 or more uppercase hex digits (device ids, salts); lowercase git hashes are left alone |
| `numeric-handle` | 16 or more digits |
| `base64-blob` | 48 or more base64 characters |

The last three are the opaque shapes cai's `redact` removes. Two of cai's shapes are not applied: uuids and `1.2345.6` build versions, because both are ordinary in tool output here and neither is a credential on its own. Like cai, the report gives shape and count and never a value.

What it cannot do: cover anything appended after it runs, or judge what a reader could infer from what remains. Read the copy before sharing it.

## Export

`maic sessions export ID [FILE]` (and `:export` inside a session) writes the transcript as markdown: a title line, the session id, `## User` and `## Assistant` sections, tool calls and results in fenced blocks. It reads the `user`, `assistant` and `tool` records, so an imported or redacted session exports the same way.
