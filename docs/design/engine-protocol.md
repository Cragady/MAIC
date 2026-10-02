# Engine protocol

Design for the one protocol between MAIC's engine and every interface: MAIC's own TUI, maic.nvim as the interface (roadmap item 1), the local daemon that owns sessions and background work (item 4), side threads (item 3), and the web app and phone through the relay (items 6 and 7). Design first, then build; nothing here is code yet.

Micaiah's decisions frame it and are not reopened: the engine speaks JSON-RPC over stdio for nvim (`maic --rpc`), carrying the same messages as maic-server's API (item 1); a local daemon owns sessions so a background session outlives its interface, and the TUI, maic.nvim, the web app and a paired phone are all its clients (item 4); the workstation is the only place sessions live and the relay stores nothing; remote history loads lazily, the last few exchanges first and expensive items on tap; true parallelism for background sessions and tasks. The rules of [harness.md](../harness.md) and [remote.md](../remote.md) hold throughout: `Origin::Remote` is always asked, the tripwire is never reset from a client, trust changes remotely only with a step-up proof.

opencode's split (`opencode serve`, `opencode attach URL`, one REST API plus one event stream per instance) is the model where it fits; where MAIC differs it says why.

## Decisions at a glance

| Question | Decision |
| :--- | :--- |
| Encoding | JSON-RPC 2.0, one message per line (JSON lines), on every transport. No msgpack on this channel. |
| Transports | in-process (the TUI, first), stdio (`maic --rpc`), a Unix socket (the daemon), the relay tunnel (a new frame kind), and the LAN listener (two half-duplex HTTP streams, as the relay does) |
| Origin | stamped by the transport, never sent by a client; a turn's origin only ever moves from local to remote |
| Events | one notification method, `event`, with a per-session `seq`; a bounded in-memory ring per session for `after=N` resume |
| History | read from the session JSONL by record; items addressed `<file id>#<line>`; tool outputs, attached files and images collapsed over a per-client size and expanded on request |
| Tool output | streamed as `tool_output` chunks with byte offsets; a tee only: the model's capped result is unchanged and the child is never slowed by a client |
| Multiple clients | no input lock; every input names its client; the first valid answer to an approval wins |
| Remote limits | an allow-list per method and per `:` command; remote answers to approvals are yes, no or trip; no unlock, no trust, no settings writes, no unsandboxed shell |
| Versioning | an integer protocol version in `hello`, capability strings for additions, unknown fields and event types ignored |

## 1. Transports and encoding

### JSON lines, not msgpack

One encoding everywhere: JSON-RPC 2.0, UTF-8, one message per line. JSON escapes every newline inside a string, so a raw `\n` byte always ends a message and framing is a line reader.

* **It is already decided for stdio** (item 1), and maic-server, the web client and the session files are JSON. One codec means one shape of every message in tests, in the audit, and in a transcript of a protocol session.
* **nvim reads it without help.** maic.nvim starts the engine with `vim.fn.jobstart(cmd, {on_stdout = ...})`, splits on newlines, `vim.json.decode`s each line and writes with `vim.fn.chansend`. Nothing to install.
* **Not nvim's `rpc = true` job channel, on purpose.** A msgpack-rpc job channel dispatches every request from the engine into nvim's own API: to show an event the engine would call `nvim_exec_lua`, which ties the engine to nvim's API and hands it arbitrary code execution in the user's editor on a channel that exists only to render a conversation. That power belongs to the host connection, which is vetted separately (an ancestor's socket, owned by the user; [harness.md](../harness.md) layer 25). On a JSON channel the engine has no handle into nvim at all.
* **msgpack's gains do not apply.** The traffic is text deltas and small objects; the rare binary (an image) is base64 at 4/3 size. Supporting both would double every schema test for nothing.

MAIC's own msgpack codec (`cli/src/msgpack.cpp`) stays where it is, for the host connection.

**Size.** A message is at most 1 MiB, the relay's frame cap; anything bigger is chunked by design (tool output, history pages, expansion, images). An engine that receives a longer line answers `too_large` and closes the connection. Streaming events aim at 16 KiB or less.

### The transports

| Transport | Who uses it | Who may connect | Origin |
| :--- | :--- | :--- | :--- |
| in-process | the TUI linked with the engine, first step of the migration | the process itself | local |
| stdio | `maic --rpc`, spawned by maic.nvim (item 1) | the parent that holds the pipes | local |
| Unix socket | the daemon: `$XDG_RUNTIME_DIR/maic/engine.sock` | a peer whose `SO_PEERCRED` uid is the daemon's own | local |
| relay tunnel | a paired phone away from home | a paired key, then a token (an account later) | remote |
| LAN listener | a browser or phone on the LAN | TLS off loopback, then a token (an account later) | remote |

**In-process** passes the same JSON values (`nlohmann::json`) to `Engine::call` and to a client sink, with no text encoding. The schema is the same, so the protocol tests run against this transport first.

**stdio** is one engine process per nvim interface: it owns the sessions it creates and ends when stdin closes (a running turn is interrupted first, as quitting the TUI does). Diagnostics go to stderr, never stdout.

**The Unix socket** lives in a 0700 directory, the socket 0600. Every accepted connection is checked with `SO_PEERCRED` and closed when the uid differs. Without `XDG_RUNTIME_DIR` the daemon uses `<state>/run/` (0700), never `/tmp`. See [section 7](#7-security) for what the sandbox must hide first.

**The relay tunnel** keeps everything in [remote.md](../remote.md#the-wire): the pairing, the hellos, the triple Diffie-Hellman, the counters. It gains frame kind `6 Rpc` on stream 0, from either side, whose payload is exactly one JSON-RPC message. Kinds 1 to 5 stay while the web client moves over, then go (section 8).

**The LAN listener** cannot rely on a full-duplex request either, so it uses the relay's pattern without the relay: `GET /rpc` opens a chunked stream of lines down (its first line names the connection), `POST /rpc/{conn}` carries one or more lines up. Both carry `Authorization: Bearer`; no token in a URL. No WebSocket library joins the build.

### Envelope

```json
→ {"jsonrpc":"2.0","id":7,"method":"session.send","params":{"session":"20261001-091500-tui-4121","text":"run the tests"}}
← {"jsonrpc":"2.0","id":7,"result":{"started":true,"seq":812}}
← {"jsonrpc":"2.0","method":"event","params":{"session":"20261001-091500-tui-4121","seq":813,"type":"turn_start","turn":4}}
```

Commands are requests with an `id`. Session events are notifications with method `event`; engine-wide notifications use method `index` (section 2). Requests are answered in any order; a client may have up to 64 in flight.

## 2. The object model

| Object | Id | What it is |
| :--- | :--- | :--- |
| engine | `epoch`: random, per start | one process that owns sessions: `maic --rpc`, the TUI's in-process engine, or the daemon (`maic daemon`) |
| client | `c1`, `c2`, ... per engine | one connection; has a name (for remote: the token's or pairing's name, never self-declared), an origin, and per-client view settings |
| session | the transcript id, `20261001-091500-tui-4121` | one conversation with one main agent, one JSONL file; the id MAIC already uses everywhere |
| side thread | a session id | a session with `parent` and `side` (`btw` or `aside`); `btw` starts as a fork pointer at the parent's current record |
| task | the child's session id | a `task` subagent: a session of kind `sub` with `parent`, foreground (the parent waits) or background (it does not) |
| item | `<file id>#<line>`, or `~<seq>` while in flight | one displayable record: a user turn, a reply, a tool call with its result, a notice |
| approval, question | `a12`, `q3`, per engine | something waiting for a person |

**Session states.** The lifecycle and the activity are separate fields:

| `state` | Meaning | Agent in memory |
| :--- | :--- | :--- |
| `live` | at least one client has it in focus | yes |
| `background` | loaded, no client has it in focus; it keeps working | yes |
| `parked` | ended for now; in the index, resumes where it was, queued messages kept | no |
| `stopped` | ended; not in the index; an ordinary transcript (`maic -r`, `session.resume`) | no |

| `activity` (live and background only) | Meaning |
| :--- | :--- |
| `idle` | nothing running |
| `working` | a turn is running |
| `waiting` | an approval or question is pending; `waiting` in the entry says which |
| `slot` | ready to call the model but waiting its turn at a single-slot local server |

`live` and `background` are derived from focus: `:bg` takes this client's focus away, and the session is background only when no other client has it in focus. `unseen` marks a background session whose turn ended since a client last had it in focus (the switcher's "finished").

**The session index** is the daemon's list of live, background and parked sessions, kept in `<state>/engine/index.json` (0600, written through a temporary file and a rename) so parked sessions survive a restart. At daemon shutdown live and background sessions become parked, asking first when a turn is running. One entry:

```json
{"id":"20261001-091500-tui-4121","title":"fix the relay keepalive","workspace":"~/dev2/MAIC",
 "kind":"main","parent":null,"state":"background","activity":"waiting","model":"llamacpp/qwen3.5-9b","mode":"edit",
 "agent":"build","last_activity":"2026-10-01T09:42:10+02:00","unseen":false,"queued":0,
 "waiting":{"kind":"approval","id":"a12","session":"20261001-094001-sub-4121","tool":"run_shell","summary":"ctest --test-dir build"}}
```

Side threads and tasks are entries with `parent` set (`kind` `side` or `sub`), so a switcher lists them under their parent. `waiting` on a parent entry also covers its children, so a client watching only the top level still sees that something in the tree is asking. A remote client's entries omit the transcript path; accounts (item 6) add `owner` and filter by it.

Engine-wide notifications: `{"method":"index","params":{"entry":{...}}}` on any change to an entry, `{"method":"index","params":{"removed":"ID"}}`, and `{"method":"engine","params":{"tripped":true,"reason":"..."}}`. They replace state rather than append to it, so a client that missed one re-reads with `index.get`; they carry no `seq`.

## 3. Commands and events

### Commands

`S` is a session id; every session command also accepts a side thread or a task id. The Remote column is the default; section 7 has the rules behind it.

| Method | Params | Result | Remote |
| :--- | :--- | :--- | :--- |
| `hello` | `protocol`, `client {name, version}`, `capabilities`, `view {collapse_over}`, `auth` (remote only) | `protocol`, `engine {version, epoch}`, `client`, `origin`, `capabilities`, `limits` | yes |
| `engine.status` | | what `GET /api/status` returns today | yes |
| `engine.trip` | `reason` | `{tripped: true}`; interrupts every turn | yes |
| `index.get` / `index.subscribe` / `index.unsubscribe` | | entries / `{}` | yes |
| `session.list` | `all`, `query`, `limit` | transcripts as `maic sessions` lists them, stopped ones included | yes |
| `session.create` | `workspace`, `model`, `mode`, `agent`, `title`, `focus`, `leave` | the entry | yes, inside `server.workspaces` |
| `session.fork` | `session`, `at` (a record line; default the end), `focus`, `leave` | the new entry | yes |
| `session.side` | `session`, `kind` (`btw`, `aside`), `text`, `focus` | the side thread's entry; the text is sent in it | yes |
| `session.merge_draft` | `side` | `{note}` drafted by `small_model` | yes |
| `session.merge` | `side`, `how` (`summary`, `all`, `pick`), `note` or `turns` | the note's item | yes |
| `session.focus` | `session`, `leave` | the entry (this is `:switch`) | yes |
| `session.background` | `session` | the entry | yes |
| `session.park` / `session.stop` | `session`, `interrupt` | the entry; `busy` when a turn runs and `interrupt` is not true | yes |
| `session.resume` | `session` (id or unique prefix; a path only locally), `focus` | the entry | yes |
| `session.attach` | `session`, `exchanges` (default 3) | a snapshot, and events from its `seq` on (section 5) | yes |
| `session.subscribe` | `session`, `load`, `after` | `{}` then replay; `resync` when `after` left the ring | yes |
| `session.unsubscribe` | `session` | `{}` | yes |
| `session.history` | `session`, `before` (an item), `exchanges` or `limit` | `{items, more}` | yes |
| `session.expand` | `session`, `item`, `offset`, `length` (at most 256 KiB) | `{text, offset, size, done}` or `{mime, data, ...}` | yes |
| `session.send` | `session`, `text`, `now` | `{started}` or `{queued}`, with `seq` | yes |
| `session.image` | `session`, `name`, `mime`, `data` (base64), `part`, `parts` | `{pending: [names]}` | yes |
| `session.interrupt` | `session` | `{running, interrupting}` | yes |
| `session.answer` | `session`, `approval`, `choice` (`yes`, `no`, `always`, `trip`), `feedback` | `{choice}`; `already_answered` for a late one | yes, without `always` |
| `session.reply` | `session`, `question`, `text` | `{}` | yes |
| `session.set` | `session`, any of `mode`, `model`, `think`, `harness`, `agent`, `title`, `confirm` | the entry; `confirm_required` with the text to show for auto under a dumb harness | partly (section 7) |
| `session.command` | `session`, `line` (`":ban add foo"`) | `{lines, ok}`; events as the command causes them | per command (section 7) |
| `session.shell` | `session`, `command` | `{exit_code}`; output as `tool_output` events | no: it runs unsandboxed, as the user |

`leave` is `{session, as}` with `as` one of `bg`, `park`, `stop`, or `default` (a working session goes to the background, an idle one is parked). The "ask instead" setting is the client's: it asks, then sends an explicit `as`. An implicit `default` never parks or stops a session another client has in focus.

`session.command` exists so every `:` command the engine owns has one path, typed into any client; `session.set` is the same handlers for the ones a widget changes. Commands that are only about the view never leave the client (section 8).

### Events

Every event carries `session`, `seq` and `type`; inputs and answers also carry `by {client, name, origin}`.

| `type` | Fields |
| :--- | :--- |
| `turn_start` | `turn`, `origin` |
| `turn_end` | `turn`, `interrupted`, `seconds`, `tool_calls` (today's `done` and the TUI's footer) |
| `user` | `item`, `text`, `queued`, `images`, `by` |
| `text` | `item`, `delta`, `thinking` |
| `reply_end` | `item`, `ref` (the record it became) |
| `tool_start` | `item`, `call`, `tool`, `summary`, `path` |
| `tool_output` | `item`, `offset`, `data`, or `skipped` (a byte count) |
| `tool_end` | `item`, `ref`, `ok`, `size`, `head`, `collapsed` |
| `file_written` | `path`, `tool` (nvim's `:checktime`, `MaicFileWritten`) |
| `approval` | `id`, `item`, `thread {session, agent, title}`, `tool`, `summary`, `reason`, `origin`, `always_covers`, `preview`, `path`, `proposed_size` |
| `approval_answered` | `id`, `choice`, `by` |
| `question` | `id`, `thread`, `text`, `options` |
| `question_answered` | `id`, `by` |
| `notice` | `text`, `level` (`info`, `warn`, `error`) |
| `todo` | `items [{text, done}]` |
| `state` | `state`, `activity`, `waiting`, `by` |
| `settings` | whichever of `mode`, `model`, `remote_model`, `think`, `harness`, `agent` changed, `by` |
| `usage` | `input`, `output`, `calls`, `context`, `last_input`, `budget` (after every model call) |
| `title` | `text`, `source` (`auto`, `rename`) |
| `compaction` | `stage`, `bytes_before`, `bytes_after`, `ref` |
| `task_start` | `task` (the child's session id), `agent`, `model`, `model_reason`, `background`, `prompt_head` |
| `task_end` | `task`, `ok`, `steps`, `tokens`, `answer_size`, `ref` |
| `side` | `thread` (its id), `kind`, `event` (`opened`, `merged`), `ref` |
| `error` | `text`, `code` (a provider or transport failure in the turn) |

A child task's own text and tool events are in the child's session; a client that wants them subscribes to it. Its approvals and questions are raised on the top-level session's stream with `thread` naming the child, because that is where a person is looking, and they are answered there.

Several approvals can be pending in one tree at once (background children in parallel), so approvals are a set keyed by id, not today's single `pending` slot.

```json
← {"jsonrpc":"2.0","method":"event","params":{"session":"20261001-091500-tui-4121","seq":840,"type":"approval","id":"a12",
   "item":"~838","thread":{"session":"20261001-094001-sub-4121","agent":"explore","title":"find the flaky test"},
   "tool":"run_shell","summary":"ctest --test-dir build -R relay","reason":"runs a program","origin":"local","always_covers":"ctest","preview":""}}
→ {"jsonrpc":"2.0","id":31,"method":"session.answer","params":{"session":"20261001-091500-tui-4121","approval":"a12","choice":"yes"}}
```

## 4. Streaming tool output

Today `run_sandboxed` collects a command's output and returns it when the command ends, so a long build shows nothing until it finishes; only the user's own `!cmd` streams (`run_user_shell` in `cli/src/tui.cpp`).

**The change in core.** `run_sandboxed` and `run_sandboxed_argv` gain an output callback, the shape `run_user_shell` already has; `AgentEvents` gains `on_tool_output(std::string_view chunk)`. `run_shell` streams its interleaved output; a script tool streams stderr (its stdout is its result). Other tools emit nothing.

**What the model gets does not change.** The stream is a tee in front of the existing path: `absorb` keeps the head and a rolling tail in bounded memory, `trim_output` gives the model 24 KiB of head and 8 KiB of tail with the omitted count, the `msg` record holds that, the `tool` record keeps its 64 KB cap, and the timeout and cancel are as before. Nothing streamed ever reaches the model.

**Events.** The engine coalesces chunks into a `tool_output` event every 100 ms or 16 KiB, whichever comes first. `offset` is the byte offset in the whole output, so a client can tell exactly what it holds and where a gap is.

**Back-pressure stops at the engine.** The engine reads the child's pipes at full speed whatever the clients do; a slow phone never slows or blocks a build. Each connection has an outbound queue measured in bytes:

* Above 2 MiB queued, `tool_output` data for that connection is replaced by `{"skipped": N}` for the same `seq`, and consecutive queued `text` deltas of one item are merged into one event carrying the last `seq`. Nothing else is dropped or merged.
* Above 8 MiB the connection is closed with `too_slow`; the client reconnects and resumes or resyncs (section 5).
* Remote connections also get a per-session ceiling of 64 KiB/s of `tool_output` (the relay itself allows 4 MiB/s per pairing), so a runaway command cannot crowd approvals and replies off a phone link.

Output beyond what the record keeps is display only and is not stored, as it is in a terminal's scrollback today. After `tool_end`, `session.expand` serves the recorded result.

opencode does this by re-sending the whole tool part with the last 30,000 characters of output as metadata on every update. Offsets and deltas cost less on a phone link and say exactly what was missed.

## 5. History and lazy loading

### Items and records

History is read from the session JSONL ([sessions.md](../sessions.md)), not from the event ring. The displayable records (`user`, `assistant`, `tool`, `context`, `compact`, `title`, `undo`, `workspace`, the notes of `inject` and `graft`) become items; `msg`, `usage`, `start` and the other bookkeeping records do not. An item's id is `<file id>#<line>`, the line counted as `--fork-at` counts it. A fork's history starts with its parent's first N records, and those items keep the parent's file id, so an id always names one line of one file and never moves. An exchange is a `user` record and everything up to the next one.

The engine builds a line-offset index of a session file when it loads it (one sequential scan) and extends it as records are appended, so any page is a seek and a short read. `reset` and `clear` records cut what the model sees, never what history shows, the same as the transcript today.

Items being written have the provisional id `~<seq>` (the `seq` of the event that opened them) until `reply_end` or `tool_end` gives the `ref` they became.

### Snapshot, pages, expansion

```json
→ {"jsonrpc":"2.0","id":3,"method":"session.attach","params":{"session":"20261001-091500-tui-4121","exchanges":3}}
← {"jsonrpc":"2.0","id":3,"result":{"entry":{...},"load":"q7c2","seq":812,"more_before":true,
   "items":[{"id":"20261001-091500-tui-4121#57","kind":"user","text":"run the tests","time":"2026-10-01T09:41:02+02:00"},
            {"id":"20261001-091500-tui-4121#61","kind":"tool","tool":"run_shell","summary":"ctest --test-dir build","ok":true,
             "size":48213,"head":"Test project ~/dev2/MAIC/build\n    Start  1: harness","collapsed":true},
            {"id":"20261001-091500-tui-4121#62","kind":"assistant","text":"All 42 passed."}],
   "inflight":null,"pending":[],"todo":[],"usage":{...}}}
```

* **`session.attach`** returns the entry, the last `exchanges` exchanges, `inflight` (the reply so far, or the running tool with the last 8 KiB of its output), every pending approval and question in the tree, the todo list and usage, and subscribes the client from the snapshot's `seq` in the same step, so no event falls between the two.
* **`session.history {before, exchanges}`** pages backwards from an item. A client may drop pages it scrolled away from and fetch them again.
* **Collapsing.** Tool results, attached files (`context`) and images larger than the client's `collapse_over` arrive as `size`, `head` (the first three lines, at most 200 bytes) and `collapsed: true`. Defaults: 2 KiB for a remote client, 64 KiB for a local one; maic.nvim may ask for 0 (never collapse) and fold with nvim's folds. User turns and replies are never collapsed: the displayable conversation is what must always be there.
* **`session.expand {item, offset, length}`** returns up to 256 KiB of the item's text per call, with `done`; for a tool, the `tool` record's `result`; an image comes back as `mime` and base64 `data` in parts.

### Resume after a reconnect

Each loaded session keeps a ring of its last 10,000 events or 8 MiB, whichever is smaller, in memory only. `seq` counts from 0 per load of a session, and `load` (returned by `attach`) names that load; `epoch` names the engine.

1. Reconnect and `hello`. A different `epoch` means the engine restarted: `attach` again.
2. Same epoch: `session.subscribe {session, load, after: last_seq}` replays what was missed and continues.
3. A different `load` (the session was parked and resumed) or an `after` older than the ring answers `resync`: `attach` again.

This replaces maic-server's unbounded per-session `events` vector and its replay of the folded transcript as events on resume: history comes from the file, and the ring only bridges a dropped connection.

## 6. Several clients on one session

The TUI at the desk and the phone on the sofa can have the same session open. One ordered event stream per session is what keeps their views consistent: every client sees the same events in the same `seq` order, and everything per client (focus, collapse sizes, drafts, scroll position) stays in the client.

* **Input** has no lock and no owner. A `session.send` starts a turn when the session is idle and is queued into the mailbox when it is working, exactly as typing mid-turn does today; arrival order at the engine decides. Each `user` event names its client, so every view shows who typed what. Drafts stay in their client.
* **Interrupt** may come from any client with the session open, local or remote: stopping only adds restriction.
* **Approvals and questions** go to every client with the session (or its tree) open. The first valid answer wins under the session's lock; the rest get `approval_answered` with `by` and close their prompt, and a late answer gets `already_answered`. A remote client may answer an approval raised in a turn started locally (approving from the phone while away from the desk is the point); its answer is recorded with its origin, and it cannot answer `always` (section 7).
* **Settings and lifecycle** (`session.set`, park, stop) act for everyone and are announced with `by`, so the TUI says "parked from phone" rather than going quiet.
* **Origin is per turn and only rises.** A turn started by a local client runs as local; once a message from a remote client is delivered into it, the rest of that turn runs as `Origin::Remote`, because what the model does next is shaped by remote input. It never goes back down within a turn. Tasks inherit their parent's current origin.

## 7. Security

### The local socket, and what the sandbox must hide first

The socket is protected by its directory (0700), its mode (0600) and the `SO_PEERCRED` uid check. Any process running as Micaiah outside the sandbox can still connect, which is the same standing as her nvim's own socket and her shell.

**A prerequisite, found while writing this design.** The sandbox (`bwrap_args`, `core/src/sandbox.cpp`) binds `/` read-only and hides `/tmp` and the secret directories, but leaves `$XDG_RUNTIME_DIR` visible and passes the environment through. A Unix socket reached by a path is not confined by `--unshare-all` (that takes the network namespace, not the filesystem), and connecting to one needs no write access to the mount. So a sandboxed command can most likely reach every socket of the user's there today: the host nvim's socket (whose API runs arbitrary code outside the sandbox) and the session D-Bus (which can ask `systemd --user` to start a transient unit, past the textual `systemctl` trip pattern). This is from reading the code, not from a run, and should be confirmed with a test. A daemon socket in the same directory would let a sandboxed command answer its own approvals. So before the daemon lands: a `--tmpfs` over `$XDG_RUNTIME_DIR` (and over `<state>/run/`), the environment cleared to a fixed list (no `NVIM`, `DBUS_SESSION_BUS_ADDRESS` or `MAIC_*`), and escape tests for both in `harness_test`. It is the first step of the build order.

No tool exposes the engine to a model, and none will: agents never call engine methods.

### Origin, and who can send what

The transport stamps every command's origin: in-process, stdio and the Unix socket are `local`; the relay tunnel and the LAN listener are `remote`. A client cannot send an `origin` field, and there is no command that sets one. Every remote command passes the token check (an account's access token once item 6 lands) and writes an audit line in the existing `audit.log`: time, source (`relay/<phone>` or the address), the token's or user's name, the method, the session and the result code. Events are not audited.

**Approvals from a remote client follow the harness.** Rule 5 says a remote-origin action is asked every time. Today a session's `always` answers are checked after the remote downgrade (`Agent::authorise`, `core/src/agent.cpp`, the `always_allowed_` check), so an `always` given once lets later remote-origin calls of that kind through unasked. This design closes that: `always` entries never cover a remote-origin call, whoever gave them, and a remote client's choices are yes, no and trip (`limits.always: false` in its `hello` result).

**Cross-session content is data.** What arrives from another session (a background task's answer, a merged side thread, an injected or grafted note) enters the receiving conversation as a note labelled with its source session, the way `inject` and `graft` mark theirs, never as a typed user turn, an approval or a command. A child's approvals go to people, never to its parent agent.

### What a remote client can never do

| Never | How it is enforced |
| :--- | :--- |
| reset or unlock the tripwire | there is no such method at all; the session-scoped `:unlock` is a local-only command; `engine.trip` is allowed, since it only adds restriction |
| grant or change trust | no trust method over the protocol; the step-up route `POST /api/trust` stays the one remote path and refuses until accounts register a verifier |
| change global settings | no method writes a settings file; `session.set` changes one session |
| loosen a session | `harness = "dumb"`, `:allow`, removing a `:forbid` term, `:rule`, `:system`, `:prefill`, `:instructions off` are local only; the dumb-plus-auto confirmation is local only |
| run unsandboxed | `session.shell` (`!cmd`) and `:lua` are local only |
| move or touch files outside the harness | `:cd` (already refused for `Origin::Remote`), `:init`, `:undo`, `:export`, `:image FILE` are local only; a remote picture arrives through `session.image` |
| reach the machine | `:up`, `:down`, `:gpu`, `:ctx`, `:ctx2`, `:trust`, `:untrust`, `:lazylock`, `:open`, `:path`, `:artifacts`, `:settings` are local only |
| reach outside the workspaces | `session.create` and `session.resume` stay inside `server.workspaces` and use only directories already trusted here; `tripwire = "isolated"` refuses remote work, as today |

The rule is an allow-list: each engine method and each engine `:` command carries a remote flag, and anything not marked is refused for remote with `forbidden_remote`. Marked for remote: `:mode` (tightening freely, loosening up to `edit`), `:model` (presets; a model off the machine is labelled as today), `:think`, `:compact`, `:rename`, `:todo`, `:tools`, `:status`, adding a `:forbid` term, lowering `:budget`, and `:trip`.

## 8. How today's pieces migrate

**Who does what today.** `Agent` runs one turn (`submit`) and calls `AgentEvents`. Around it, the TUI does work that belongs to the engine: it owns the `SessionLog`, re-submits messages that were queued after the turn's last model call, titles the session with `small_model`, times the turn for the footer, runs `!cmd` and passes its output in as context, confirms auto under a dumb harness, and parses every `:` command, the session ones included. maic-server repeats a smaller copy of the same (`Session`, `Events`, `start_turn` in `server/src/server.cpp`).

**The engine** is a core class (`core/include/maic/engine.hpp`) that takes all of that over once: the session table, each session's `Agent` and `SessionLog`, the event ring, pending approvals and questions, the turn loop with queued re-submission and titles, the engine `:` commands, the index, and the dispatcher from method to handler. Clients keep the view: rendering, the editor, folds, themes, registers, `:stash`, `:copy`, `:h`, `:e`, `:set markdown|mouse|enter_sends`, `:q`.

1. **The TUI becomes a client of an in-process engine**: the same JSON values through `Engine::call` and a sink, no socket, no change visible to Micaiah. The protocol tests run here.
2. **`maic --rpc`**: the same engine behind stdio. maic.nvim's interface mode (item 1) is its first client.
3. **The daemon** (`maic daemon start|stop|status`): the same engine behind the Unix socket, its PID kept with its start time as MAIC keeps its services' (MAIC owns the PID, not systemd). The TUI and maic.nvim attach to it when it answers and run in-process when it does not.
4. **maic-server becomes an adapter, then goes.** Its HTTP routes are rewritten as calls into the daemon's engine (`POST .../messages` is `session.send` plus a subscription written out as server-sent events in today's shapes), so the web client keeps working; the LAN listener and the relay home link move into the daemon; the tunnel gains kind 6; the web client moves to the protocol; then the HTTP routes, tunnel kinds 1 to 5 and the separate `maic-server` binary are removed, leaving `/api/pair` and `/api/trust` until accounts replace them.

**The host connection stays separate.** maic.nvim's host connection is MAIC driving nvim: MAIC connects to `$NVIM` as a msgpack-rpc client to `:drop` files, show diffs, fire `User` autocmds, follow the colorscheme and offer the `diagnostics` tool. The engine protocol is the other direction: nvim driving MAIC. In interface mode both exist: maic.nvim starts `maic --rpc` as its job, and the engine, being nvim's child, finds `$NVIM` and passes the ancestor check as it does today. Two channels, two directions, two codecs, each vetted on its own terms. When the plugin renders from protocol events it fires the `Maic*` autocmds itself and says so with the `autocmds` capability, and the engine then does not fire them over the host connection. A daemon is not nvim's descendant, so its sessions get no host connection (open question 8).

## 9. Versioning and capabilities

```json
→ {"jsonrpc":"2.0","id":1,"method":"hello","params":{"protocol":1,"client":{"name":"maic.nvim","version":"0.1"},
   "capabilities":["tool_output","collapse","side_threads","tasks","autocmds"],"view":{"collapse_over":0}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocol":1,"engine":{"version":"dev","epoch":"k3f9w2"},"client":"c1","origin":"local",
   "capabilities":["tool_output","collapse","side_threads","tasks","index","images"],"limits":{"always":true,"max_message":1048576}}}
```

* `protocol` is one integer. The engine answers with the version it will speak, or `unsupported_protocol` with the range it supports. The major version changes only when a message changes meaning or a field is removed.
* Additions (a method, an event type, a field) do not change it. A capability string announces a feature on either side; the engine sends a client only the event types its capabilities cover (no `tool_output` events to a client without `tool_output`).
* Clients ignore unknown event types and unknown fields, as MAIC ignores unknown record types in a session file. The engine answers an unknown method with JSON-RPC's `-32601`.
* Nothing but `hello` is accepted before `hello`.

Errors use JSON-RPC's codes for protocol faults and `-32000` for the rest, with a machine `code` and a sentence:

```json
{"jsonrpc":"2.0","id":9,"error":{"code":-32000,"message":"`:allow` is not available to a remote client","data":{"code":"forbidden_remote"}}}
```

| `data.code` | When |
| :--- | :--- |
| `not_found` | no such session, item, approval or question |
| `busy` | park or stop while a turn runs without `interrupt` |
| `already_answered` | another client answered first |
| `forbidden_remote` | outside the remote allow-list |
| `confirm_required` | auto under a dumb harness; `data.text` is what to show |
| `resync` | `after` left the ring, or the session was reloaded |
| `too_large`, `too_slow` | the size caps of section 1, the queue cap of section 4 |
| `tripped` | the harness is tripped |
| `not_owner` | accounts: another user's session |
| `unsupported_protocol` | no common version |

## 10. Build order

Each step is one PR with its tests and is useful on its own.

| # | Step | Roadmap |
| :--- | :--- | :--- |
| 1 | The sandbox hides `$XDG_RUNTIME_DIR` and clears the environment; escape tests for the nvim socket and D-Bus | prerequisite |
| 2 | `always` never covers a remote-origin call; a turn's origin rises when a remote message is delivered into it | prerequisite |
| 3 | Streaming tool output in core (`on_output` in the sandbox, `on_tool_output`); the TUI shows a running command's output live | 1, 4 |
| 4 | `Engine` in core with the session table, ring, dispatcher and schema; maic-server's `Session` and `Events` move into it; protocol tests in-process | 1, 4 |
| 5 | The TUI as an in-process client: the turn loop, titles, `!cmd` and the engine `:` commands move into the engine | 1 |
| 6 | `maic --rpc`: JSON lines on stdio, `hello`, versioning, error codes | 1 |
| 7 | maic.nvim's interface mode on `--rpc`: conversation and input buffers, approval and question floats, folds over collapsed items, the `maic` and `maic-input` filetypes | 1 |
| 8 | History: the line-offset index, `attach`, `history`, `expand`, collapsing | 4 |
| 9 | Several sessions in one engine: create, fork, focus, background, park, stop, resume, the index file, `:new`, `:switch`, `:fork`, `:bg`, `:park`, `:stop` and the switcher in the TUI and maic.nvim | 4 |
| 10 | The daemon: `maic daemon`, the socket with the uid check, the TUI and maic.nvim attaching | 4 |
| 11 | Background tasks: `task` with `background = true`, `task_start` and `task_end`, answers as labelled notes through the mailbox, `task_result`, parallel approvals | 4 |
| 12 | Side threads: `session.side`, `:btw`, `:aside`, `merge_draft` and `merge` | 3 |
| 13 | maic-server as an adapter inside the daemon; tunnel kind 6 and the LAN `/rpc` streams; the web client on the protocol with lazy history | 4, 7 |
| 14 | The adapter, tunnel kinds 1 to 5 and the `maic-server` binary removed | 4, 7 |
| 15 | Accounts: `owner` on sessions and entries, access tokens in `hello`, `not_owner` | 6 |

Steps 1 and 2 are small and stand alone; 3 helps the TUI the day it lands; 4 to 7 deliver item 1; 8 to 11 and 13 deliver item 4; 12 is item 3 and needs 9.

## Open questions for Micaiah

1. **JSON lines only, or msgpack too for nvim?** Recommendation: JSON lines only. nvim decodes it natively, and a msgpack job channel would give the engine nvim's whole API, which belongs to the vetted host connection.
2. **Does a remote client get `always`?** Recommendation: no. Remote answers are yes, no and trip, and `always` never covers a remote-origin call (build step 2), so rule 5 holds as written. The alternative, an `always` from the phone that covers only local-origin calls, is workable but easy to misread on a small screen.
3. **May a remote client answer an approval raised in a local turn?** Recommendation: yes, recorded with its origin; answering from the phone while away from the desk is the use case.
4. **Does a remote message delivered mid-turn make the rest of the turn remote?** Recommendation: yes, for that turn only. It costs extra prompts in a mixed turn and keeps rule 5 honest.
5. **Park or stop from one client while another has the session in focus?** Recommendation: allowed, announced with who did it; the implicit `default` leaving verb never parks or stops a session another client has in focus.
6. **Should the TUI start the daemon itself?** Recommendation: not at first. The TUI attaches when the daemon answers and runs in-process otherwise; turn on starting it automatically (`daemon = "auto"`) once it has run for a while. Background sessions that outlive the terminal need the daemon, so `:bg` without one says so.
7. **Keep a spool of a command's full output for expanding later?** Recommendation: no. Expansion serves the recorded result (up to 64 KB); anything beyond was display only, as in a terminal. Revisit if a real case needs the middle of a huge log.
8. **A host for daemon sessions?** The `diagnostics` tool needs an nvim, and a daemon is no nvim's child. Recommendation: later, as a client capability (`host.diagnostics`): an attached nvim answers diagnostics requests over the protocol, each judged as a read, with no Lua execution offered.
9. **Remote mode changes.** Recommendation: a remote client may tighten freely and loosen up to `edit`; `auto` only from a local client, because auto loosens what local turns may do unasked.
10. **The ring's size.** Recommendation: 10,000 events or 8 MiB per loaded session, in memory only; a reconnect after longer than that resyncs from the file, which is cheap with lazy history.
11. **Typing presence ("the phone is typing").** Recommendation: no; every input names its client, which is enough.
12. **The `maic-server` name.** Recommendation: keep `maic server start` working as `maic daemon start` with the listener on until the adapter is removed, then retire the separate binary; `maic-relay` stays its own binary.
