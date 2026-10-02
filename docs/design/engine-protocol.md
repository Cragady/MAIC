# Engine protocol

Design for the one protocol between MAIC's engine and every interface: MAIC's own TUI, maic.nvim as the interface (roadmap item 1), the local daemon that owns sessions and background work (item 4), side threads (item 3), and the web app and phone through the relay (items 6 and 7). Design first, then build; nothing here is code yet.

Micaiah's decisions frame it and are not reopened: the engine speaks JSON-RPC over stdio for nvim (`maic --rpc`), carrying the same messages as maic-server's API (item 1); a local daemon owns sessions so a background session outlives its interface, and the TUI, maic.nvim, the web app and a paired phone are all its clients (item 4); the workstation is the only place sessions live and the relay stores nothing; remote history loads lazily, the last few exchanges first and expensive items on tap; true parallelism for background sessions and tasks. The rules of [harness.md](../harness.md) and [remote.md](../remote.md) hold throughout: `Origin::Remote` is always asked, the tripwire is never reset from a client, trust changes remotely only with a step-up proof.

opencode's split (`opencode serve`, `opencode attach URL`, one REST API plus one event stream per instance) is the model where it fits; where MAIC differs it says why.

Every specification this protocol follows (JSON-RPC 2.0, OpenAI's API description, OpenRPC, JSON Schema, OpenAPI, server-sent events, JSON Lines), with its version and MAIC's known deviations, is listed in [standards.md](../standards.md).

## Decisions at a glance

| Question | Decision |
| :--- | :--- |
| Encoding | JSON-RPC 2.0, one message per line (JSON lines), on every transport. No msgpack on this channel. |
| Transports | in-process (the TUI, first), stdio (`maic --rpc`), a Unix socket (the daemon), the relay tunnel (a new frame kind), and the LAN listener (two half-duplex HTTP streams, as the relay does) |
| Origin | stamped by the transport, never sent by a client; a turn's origin only ever moves from local to remote |
| Names | OpenAI's, exactly, wherever OpenAI's API description defines the concept; MAIC's own under `maic.` and in a `maic` object (section 10) |
| Events | one notification method, `maic.event`, carrying OpenAI's Responses stream events (and MAIC's) with `sequence_number` and `stream_id`, in a fixed order (section 10); a bounded in-memory ring per session for `starting_after=N` resume |
| History | read from the session JSONL by record; items addressed `<file id>#<line>`; tool outputs, attached files and images collapsed over a per-client size and expanded on request |
| Tool output | streamed as `response.shell_call_output_content.delta` (and `maic.tool.output.delta` for script tools) with byte offsets; a tee only: the model's capped result is unchanged and the child is never slowed by a client |
| Multiple clients | no input lock; every input names its client; the first valid answer to an approval wins |
| Remote limits | an allow-list per method and per `:` command; remote answers to approvals are yes, no or trip; no unlock, no trust, no settings writes, no unsandboxed shell |
| Versioning | an integer protocol version in `maic.hello`, capability strings for additions, unknown fields and event types ignored |
| Filtering | a client names the deltas it does not want in `maic.hello`'s `exclude`; numbers stay the session's, and the first event after filtered ones carries `maic.filtered_from` (section 9) |
| Lifecycle | OpenAI's Responses and Conversations flow: sessions as conversations, background responses resumed with `starting_after`, `previous_response_id` lineage, `cancelResponse`, stable item ids; MAIC's own beside it (section 12) |
| Steering | OpenAI's `response.steer` for a message mid-turn, and `maic.steer` on a running response: `steer`, `drop`, `further`, `interrupt`, `keep`, `halt`; bans can trigger them; configured in `steering` (section 11) |
| Harness path | `harness = "auto"`, `"smart"`, `"dumb"` or `"external"`, per agent and per session, the fixed rules always the floor (section 13) |
| Security tiers | `open`, `guarded` (default), `airtight`: validation, ordering, steering rights, step-up, audit and release checks ([protocol-security.md](protocol-security.md)) |
| Schemas | OpenRPC 1.3 for methods, JSON Schema 2020-12 for payloads, OpenAPI 3.2 for the HTTP side, `ordering.json` for the state machines, a conformance checker on every build (section 15) |

## 1. Transports and encoding

### JSON lines, not msgpack

One encoding everywhere: JSON-RPC 2.0, UTF-8, one message per line. JSON escapes every newline inside a string, so a raw `\n` byte always ends a message and framing is a line reader.

* **It is already decided for stdio** (item 1), and maic-server, the web client and the session files are JSON. One codec means one shape of every message in tests, in the audit, and in a transcript of a protocol session.
* **nvim reads it without help.** maic.nvim starts the engine with `vim.fn.jobstart(cmd, {on_stdout = ...})`, splits on newlines, `vim.json.decode`s each line and writes with `vim.fn.chansend`. Nothing to install.
* **Not nvim's `rpc = true` job channel, on purpose.** A msgpack-rpc job channel dispatches every request from the engine into nvim's own API: to show an event the engine would call `nvim_exec_lua`, which ties the engine to nvim's API and hands it arbitrary code execution in the user's editor on a channel that exists only to render a conversation. That power belongs to the host connection, which is vetted separately (an ancestor's socket, owned by the user; [harness.md](../harness.md) layer 25). On a JSON channel the engine has no handle into nvim at all.
* **msgpack's gains do not apply.** The traffic is text deltas and small objects; the rare binary (an image) is base64 at 4/3 size. Supporting both would double every schema test for nothing.

MAIC's own msgpack codec (`cli/src/msgpack.cpp`) stays where it is, for the host connection.

**Size.** A message is at most 1 MiB, the relay's frame cap; anything bigger is chunked by design (tool output, history pages, expansion, images). An engine that receives a longer line answers `maic_too_large` and closes the connection. Streaming events aim at 16 KiB or less.

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
→ {"jsonrpc":"2.0","id":7,"method":"response.create","params":{"stream_id":"20261001-091500-tui-4121","conversation":"20261001-091500-tui-4121","input":"run the tests"}}
← {"jsonrpc":"2.0","id":7,"result":{"id":"20261001-091500-tui-4121.r9","object":"response","created_at":1790000000,"status":"in_progress","background":true,"access_programs":null,"error":null,"incomplete_details":null,"instructions":null,"model":"llamacpp/qwen3.5-9b","tools":[],"output":[],"parallel_tool_calls":false,"metadata":{},"tool_choice":"auto","temperature":null,"top_p":null,"previous_response_id":null,"conversation":{"id":"20261001-091500-tui-4121"},"maic":{"turn":4,"origin":"local","sequence_number":812}}}
← {"jsonrpc":"2.0","method":"maic.event","params":{"type":"maic.input.added","sequence_number":813,"stream_id":"20261001-091500-tui-4121",
   "item":{"id":"~813","type":"message","role":"user","content":[{"type":"input_text","text":"run the tests"}]},"queued":false,"by":{"client":"c1","name":"maic.nvim","origin":"local"}}}
← {"jsonrpc":"2.0","method":"maic.event","params":{"type":"response.created","sequence_number":814,"stream_id":"20261001-091500-tui-4121",
   "response":{"id":"20261001-091500-tui-4121.r9","object":"response","created_at":1790000000,"status":"in_progress","background":true,"access_programs":null,"error":null,"incomplete_details":null,"instructions":null,"model":"llamacpp/qwen3.5-9b","tools":[],"output":[],"parallel_tool_calls":false,"metadata":{},"tool_choice":"auto","temperature":null,"top_p":null,"previous_response_id":null,"conversation":{"id":"20261001-091500-tui-4121"},"maic":{"turn":4,"origin":"local"}}}}
```

Commands are requests with an `id`. Session events are notifications with method `maic.event`; engine-wide notifications use `maic.index` and `maic.engine` (section 2). Requests are answered in any order; a client may have up to 64 in flight.

## 2. The object model

| Object | Id | What it is |
| :--- | :--- | :--- |
| engine | `instance`: random, per start | one process that owns sessions: `maic --rpc`, the TUI's in-process engine, or the daemon (`maic daemon`) |
| client | `c1`, `c2`, ... per engine | one connection; has a name (for remote: the token's or pairing's name, never self-declared), an origin, and per-client view settings |
| session | the transcript id, `20261001-091500-tui-4121` | one conversation with one main agent, one JSONL file; the id MAIC already uses everywhere. On the wire it is OpenAI's `conversation` and its stream's `stream_id` |
| side thread | a session id | a session with `parent` and `side` (`btw` or `aside`); `btw` starts as a fork pointer at the parent's current record |
| task | the child's session id | a `task` subagent: a session of kind `sub` with `parent`, foreground (the parent waits) or background (it does not) |
| item | `<file id>#<line>`, or `~<n>` while in flight | one displayable record: a user turn, a reply, a tool call with its result, a notice |
| response | `<session id>.r<k>` | one run of the agent loop, OpenAI's `response` object (section 10); a turn is the chain of responses from one message to its answer |
| approval, question | `a12`, `q3`, per engine | something waiting for a person |

**Session states.** The lifecycle and the activity are separate fields:

| `state` | Meaning | Agent in memory |
| :--- | :--- | :--- |
| `live` | at least one client has it in focus | yes |
| `background` | loaded, no client has it in focus; it keeps working | yes |
| `parked` | ended for now; in the index, resumes where it was, queued messages kept | no |
| `stopped` | ended; not in the index; an ordinary transcript (`maic -r`, `maic.session.resume`) | no |

| `activity` (live and background only) | Meaning |
| :--- | :--- |
| `idle` | nothing running |
| `working` | a turn is running |
| `waiting` | an approval or question is pending; `waiting` in the entry says which |
| `slot` | ready to call the model but waiting its turn at a single-slot local server (`response.queued`) |

`live` and `background` are derived from focus: `:bg` takes this client's focus away, and the session is background only when no other client has it in focus. `unseen` marks a background session whose turn ended since a client last had it in focus (the switcher's "finished").

**The session index** is the daemon's list of live, background and parked sessions, kept in `<state>/engine/index.json` (0600, written through a temporary file and a rename) so parked sessions survive a restart. At daemon shutdown live and background sessions become parked, asking first when a turn is running. One entry:

```json
{"id":"20261001-091500-tui-4121","title":"fix the relay keepalive","workspace":"~/dev2/MAIC",
 "kind":"main","parent":null,"state":"background","activity":"waiting","model":"llamacpp/qwen3.5-9b","mode":"edit",
 "agent":"build","harness":"auto","judge":"maic","tier":"guarded","last_activity":"2026-10-01T09:42:10+02:00","unseen":false,"queued":0,
 "waiting":{"kind":"approval","id":"a12","session":"20261001-094001-sub-4121","tool":"run_shell","summary":"ctest --test-dir build"}}
```

Side threads and tasks are entries with `parent` set (`kind` `side` or `sub`), so a switcher lists them under their parent. `waiting` on a parent entry also covers its children, so a client watching only the top level still sees that something in the tree is asking. A remote client's entries omit the transcript path; accounts (item 6) add `owner` and filter by it.

Engine-wide notifications: `{"method":"maic.index","params":{"entry":{...}}}` on any change to an entry, `{"method":"maic.index","params":{"removed":"ID"}}`, and `{"method":"maic.engine","params":{"tripped":true,"reason":"..."}}`. They replace state rather than append to it, so a client that missed one re-reads with `maic.index.get`; they carry no `sequence_number`.

## 3. Commands and events

Names follow section 10: OpenAI's exactly where OpenAI defines the concept, `maic.` for MAIC's own. A session is an OpenAI conversation (`conversation_id`), and its event stream is a lane (`stream_id`); both are the session id. Every method that takes a session also accepts a side thread's or a task's id.

### Commands

The Remote column is the default; section 7 has the rules behind it.

| Method | Params | Result | Remote |
| :--- | :--- | :--- | :--- |
| `maic.hello` | `protocol`, `client {name, version}`, `capabilities`, `view {collapse_over}`, `auth` (remote only) | `protocol`, `engine {version, instance}`, `client`, `origin`, `capabilities`, `limits`, `tier`, `path` | yes |
| `maic.engine.status` | | what `GET /api/status` returns today | yes |
| `maic.engine.trip` | `reason` | `{tripped: true}`; cancels every response | yes |
| `maic.index.get` / `maic.index.subscribe` / `maic.index.unsubscribe` | | entries / `{}` | yes |
| `maic.session.list` | `all`, `query`, `limit` | transcripts as `maic sessions` lists them, stopped ones included | yes |
| `createConversation` | `metadata {title}`, `maic {workspace, model, mode, agent, focus, leave}` | the `conversation` object, `maic.entry` | yes, inside `server.workspaces` |
| `getConversation` | `conversation_id` | the `conversation`, `maic.entry` | yes |
| `updateConversation` | `conversation_id`, `metadata {title}` | the `conversation` (`:rename`) | yes |
| `listConversationItems` | `conversation_id`, `after`, `limit`, `order`, `include`, `maic {exchanges}` | OpenAI's item list (`data`, `first_id`, `last_id`, `has_more`) | yes |
| `getConversationItem` | `conversation_id`, `item_id`, `include` | the item | yes |
| `maic.item.expand` | `session`, `item_id`, `offset`, `length` (at most 256 KiB) | `{text, offset, size, done}` or `{mime, data, ...}` | yes |
| `response.create` | the `CreateResponse` fields MAIC reads: `stream_id`, `conversation`, `input` (`input_text`, `input_image` with a `file_id`), `previous_response_id`, `instructions` (local only), `tool_choice`, `max_tool_calls`, `truncation`, `include` | the `response` (`queued` or `in_progress`) and `maic.sequence_number`; queued FIFO behind a running response on the same lane | yes |
| `response.steer` | `previous_response_id`, `input` | `{}`; `response.steer.accepted` follows on the stream (section 11) | yes |
| `getResponse` | `response_id`, `stream`, `starting_after`, `include` | the `response`, or with `stream` its events after `starting_after` | yes |
| `cancelResponse` | `response_id` | the `response`, `status: "cancelled"` | yes |
| `listInputItems` | `response_id`, `after`, `limit`, `order`, `include` | the response's input items | yes |
| `Compactconversation` | as the pinned spec defines (`previous_response_id`, `input`, ...) | the compacted output; `:compact` is this | yes |
| `Getinputtokencounts` | as the pinned spec defines (`conversation`, `input`, ...) | the input token count, from MAIC's estimate | yes |
| `maic.steer` | `session`, `response_id`, `action`, `note`, `trim`, `at`, `tool`, `step_up` | `{steer {id, previous_response_id}}` (section 11) | per tier and setting (section 11) |
| `maic.session.fork` | `session`, `at` (a record line; default the end), `focus`, `leave` | the new entry | yes |
| `maic.session.side` | `session`, `kind` (`btw`, `aside`), `text`, `focus` | the side thread's entry; the text is sent in it | yes |
| `maic.session.merge_draft` | `side` | `{note}` drafted by `small_model` | yes |
| `maic.session.merge` | `side`, `how` (`summary`, `all`, `pick`), `note` or `turns` | the note's item | yes |
| `maic.session.focus` | `session`, `leave` | the entry (this is `:switch`) | yes |
| `maic.session.background` | `session` | the entry | yes |
| `maic.session.park` / `maic.session.stop` | `session`, `interrupt` | the entry; `maic_busy` when a response runs and `interrupt` is not true | yes |
| `maic.session.resume` | `session` (id or unique prefix; a path only locally), `focus`, `leave` | the entry | yes |
| `maic.session.attach` | `session`, `exchanges` (default 3) | a snapshot, and events from its `sequence_number` on (section 5) | yes |
| `maic.session.subscribe` | `session`, `epoch`, `starting_after` | `{}` then replay; `maic_resync` when `starting_after` left the ring or the epoch changed | yes |
| `maic.session.unsubscribe` | `session` | `{}` | yes |
| `maic.session.image` | `session`, `name`, `mime`, `data` (base64), `part`, `parts` | `{file_id}` once every part is in, for an `input_image` | yes |
| `maic.approval.answer` | `session`, `approval`, `choice` (`yes`, `no`, `always`, `trip`), `feedback` | `{choice}`; `maic_already_answered` for a late one | yes, without `always` |
| `maic.question.reply` | `session`, `question`, `text` | `{}` | yes |
| `maic.session.set` | `session`, any of `mode`, `model`, `think`, `harness`, `agent`, `tier`, `confirm` | the entry; `maic_confirm_required` with the text to show for auto under a dumb harness | partly (section 7) |
| `maic.session.command` | `session`, `line` (`":ban add foo"`) | `{lines, ok}`; events as the command causes them | per command (section 7) |
| `maic.session.shell` | `session`, `command` | `{exit_code}`; output as `maic.tool.output.delta` events | no: it runs unsandboxed, as the user |

`deleteResponse`, `deleteConversation` and `deleteConversationItem` are not offered (`-32601`): a session file is append-only.

`leave` is `{session, as}` with `as` one of `bg`, `park`, `stop`, or `default` (a working session goes to the background, an idle one is parked). The "ask instead" setting is the client's: it asks, then sends an explicit `as`. An implicit `default` never parks or stops a session another client has in focus. A call that moves focus without `leave` only lets go of the session left (`bg`): nothing ends a session unless a client asked for it.

`maic.session.command` exists so every `:` command the engine owns has one path, typed into any client; `maic.session.set` is the same handlers for the ones a widget changes. Commands that are only about the view never leave the client (section 8).

### Events

Every event is one notification, `maic.event`, whose params are the event object exactly as the Responses WebSocket would send it: `type`, `sequence_number`, and `stream_id` (the session id) on every one. MAIC's own events have the same three fields; inputs, answers and steers also carry `by {client, name, origin}`.

**OpenAI's events**, in OpenAI's shapes:

| `type` | When |
| :--- | :--- |
| `response.created`, `response.in_progress` | a response starts; `response.maic {turn, origin}` |
| `response.queued` | it waits for a single-slot local server (the session's `slot` activity) |
| `response.output_item.added`, `response.output_item.done` | every output item: `message`, `reasoning`, `function_call`, `function_call_output`, `shell_call`, `shell_call_output`, `compaction`; on `done`, `item.maic {ref, status, judged_by}` |
| `response.content_part.added`, `response.content_part.done` | each `output_text` part of a `message` |
| `response.output_text.delta`, `response.output_text.done` | the reply's text (`logprobs: []`) |
| `response.reasoning_text.delta`, `response.reasoning_text.done` | thinking (today's `text` with `thinking`) |
| `response.function_call_arguments.delta`, `response.function_call_arguments.done` | a tool call's arguments as the model writes them; the harness judges the call only after `.done` |
| `response.shell_call_command.added`, `.delta`, `.done` | a `run_shell` command |
| `response.shell_call_output_content.delta`, `.done` | its output while it runs (`delta {stdout, stderr}`, `maic {offset, skipped}`; section 4) |
| `response.compaction.compacting` | a compaction inside a response |
| `response.steer.accepted`, `response.steer.failed` | a steer is queued, or refused with OpenAI's steering error codes |
| `response.completed`, `response.incomplete`, `response.failed` | a response ends; `incomplete_details.reason: "steered"` when a steer ended it; `response.maic {turn, final, ended_by}` |
| `error` | `code`, `message`, `param`: a failure outside a response, and a halt's canned message (`code: "maic_halted"`) |

**MAIC's events**:

| `type` | Fields |
| :--- | :--- |
| `maic.response.cancelled` | `response` (`status: "cancelled"`); OpenAI defines no stream event for a cancelled response, so MAIC adds one |
| `maic.turn.paused` | `turn`, `steer` (the `interrupt` that paused it) |
| `maic.input.added` | `item` (an input `message`, role `user`), `queued`, `by` |
| `maic.review.started`, `maic.review.delta`, `maic.review.verdict`, `maic.review.cancelled` | `item_id` and the fields of the addendum; `node` when a trusted node reviews; `by` on `cancelled` |
| `maic.approval.requested` | `id`, `item_id`, `thread {session, agent, title}`, `tool`, `summary`, `reason`, `origin`, `always_covers`, `preview`, `path`, `proposed_size` |
| `maic.approval.answered` | `id`, `choice` (`yes`, `no`, `always`, `trip`, or `withdrawn` by a steer), `by` |
| `maic.question.asked` | `id`, `thread`, `text`, `options` |
| `maic.question.answered` | `id`, `by`, `withdrawn` |
| `maic.steer.applied` | section 11 |
| `maic.tool.output.delta` | `item_id`, `offset`, `data` or `skipped`: a script tool's stderr, or `!cmd`, while it runs |
| `maic.file.written` | `path`, `tool` (nvim's `:checktime`, `MaicFileWritten`) |
| `maic.notice` | `text`, `level` (`info`, `warn`, `error`) |
| `maic.todo.updated` | `items [{text, done}]` |
| `maic.session.state` | `state`, `activity`, `waiting`, `by` |
| `maic.session.settings` | whichever of `mode`, `model`, `remote_model`, `think`, `harness`, `judge`, `agent`, `tier` changed, `by` |
| `maic.session.title` | `text`, `source` (`auto`, `rename`) |
| `maic.session.compacted` | `bytes_before`, `bytes_after`, `ref`: a compaction between responses |
| `maic.usage.updated` | `usage` (OpenAI's shape) for the last model call, `calls`, `context`, `last_input`, `budget` |
| `maic.task.created` | `task` (the child's session id), `agent`, `model`, `model_reason`, `background`, `prompt_head` |
| `maic.task.completed`, `maic.task.failed` | `task`, `steps`, `tokens`, `answer_size`, `ref` |
| `maic.side.opened`, `maic.side.merged` | `thread` (its id), `kind`, `ref` |

A child task's own events are in the child's session; a client that wants them subscribes to it. Its approvals and questions are raised on the top-level session's stream with `thread` naming the child, because that is where a person is looking, and they are answered there.

Several approvals can be pending in one tree at once (background children in parallel), so approvals are a set keyed by id, not today's single `pending` slot.

```json
← {"jsonrpc":"2.0","method":"maic.event","params":{"type":"maic.approval.requested","sequence_number":840,"stream_id":"20261001-091500-tui-4121","id":"a12",
   "item_id":"~838","thread":{"session":"20261001-094001-sub-4121","agent":"explore","title":"find the flaky test"},
   "tool":"run_shell","summary":"ctest --test-dir build -R relay","reason":"runs a program","origin":"local","always_covers":"ctest","preview":""}}
→ {"jsonrpc":"2.0","id":31,"method":"maic.approval.answer","params":{"session":"20261001-091500-tui-4121","approval":"a12","choice":"yes"}}
```

## 4. Streaming tool output

Today `run_sandboxed` collects a command's output and returns it when the command ends, so a long build shows nothing until it finishes; only the user's own `!cmd` streams (`run_user_shell` in `cli/src/tui.cpp`).

**The change in core.** `run_sandboxed` and `run_sandboxed_argv` gain an output callback, the shape `run_user_shell` already has; `AgentEvents` gains `on_tool_output(std::string_view chunk)`. `run_shell` streams its interleaved output; a script tool streams stderr (its stdout is its result). Other tools emit nothing.

**What the model gets does not change.** The stream is a tee in front of the existing path: `absorb` keeps the head and a rolling tail in bounded memory, `trim_output` gives the model 24 KiB of head and 8 KiB of tail with the omitted count, the `msg` record holds that, the `tool` record keeps its 64 KB cap, and the timeout and cancel are as before. Nothing streamed ever reaches the model.

**Events.** The engine coalesces chunks into a `response.shell_call_output_content.delta` (for `run_shell`; the interleaved output is its `stdout`) or a `maic.tool.output.delta` (a script tool's stderr) every 100 ms or 16 KiB, whichever comes first. `offset` (`maic.offset` on OpenAI's event) is the byte offset in the whole output, so a client can tell exactly what it holds and where a gap is.

**Back-pressure stops at the engine.** The engine reads the child's pipes at full speed whatever the clients do; a slow phone never slows or blocks a build. Each connection has an outbound queue measured in bytes:

* Above 2 MiB queued, tool output data for that connection is replaced by a skip count (`maic.skipped`, with an empty `delta`) for the same `sequence_number`, and consecutive queued `response.output_text.delta` or `response.reasoning_text.delta` events of one item are merged into one event carrying the last `sequence_number` and `maic.merged_from`, the first one it covers, so a client sees no unexplained gap (section 10). Nothing else is dropped or merged.
* Above 8 MiB the connection is closed with `maic_too_slow`; the client reconnects and resumes or resyncs (section 5).
* Remote connections also get a per-session ceiling of 64 KiB/s of tool output (the relay itself allows 4 MiB/s per pairing), so a runaway command cannot crowd approvals and replies off a phone link.

Output beyond what the record keeps is display only and is not stored, as it is in a terminal's scrollback today. After the output item's `response.output_item.done`, `maic.item.expand` serves the recorded result.

**As built (step 4, 2026-10-01).** In core, `run_sandboxed` and `run_sandboxed_argv` take `OutputTaps`, whose `on_output(stream, bytes, offset)` gets each stream (`Stdout`, `Stderr`; `run_sandboxed`'s interleaved output is all `Stdout`) in chunks every 100 ms or 16 KiB, a chunk never ending inside a UTF-8 character, from a delivery thread of its own; the reading loop never waits on it. More than 2 MiB outstanding drops newer chunks, counted in `SandboxResult::dropped_chunks` and `dropped_bytes`, and what is still queued 200 ms after the program ends is dropped the same way, so every call is made before the run returns. `AgentEvents::on_tool_output(call_id, stream, chunk, offset)` (a no-op by default) carries `run_shell`'s output, a script tool's stderr and a Lua tool's `maic.shell` output (one stream across its commands); a subagent's arrive as `<agent>:<call id>`. The TUI keeps the last 12 lines under the call and the result replaces them; headless prints nothing new. maic-server sends `{"type": "response.shell_call_output_content.delta", "item_id", "delta": {"stdout", "stderr"}, "maic": {"offset", "skipped"?}}` for `run_shell` and `{"type": "maic.tool.output.delta", "item_id", "offset", "data" or "skipped"}` otherwise, through a per-session bucket of 64 KiB/s: a chunk over it is not sent, and a run of them becomes one skip event before the next data or the result. Until step 5 numbers items, `item_id` is the call id and the server's own `seq` stands for `sequence_number`. The 2 MiB and 8 MiB per-connection limits above arrive with the engine's connections (step 5). Since step 5 the engine sends these events, with item ids, `output_index` and `sequence_number`, and the per-session bucket is per remote connection (section 16, as built).

opencode does this by re-sending the whole tool part with the last 30,000 characters of output as metadata on every update. Offsets and deltas cost less on a phone link and say exactly what was missed.

## 5. History and lazy loading

### Items and records

History is read from the session JSONL ([sessions.md](../sessions.md)), not from the event ring. The displayable records (`user`, `assistant`, `tool`, `context`, `compact`, `title`, `undo`, `workspace`, the notes of `inject` and `graft`) become items; `msg`, `usage`, `start` and the other bookkeeping records do not. An item's id is `<file id>#<line>`, the line counted as `--fork-at` counts it. A fork's history starts with its parent's first N records, and those items keep the parent's file id, so an id always names one line of one file and never moves. An exchange is a `user` record and everything up to the next one.

The engine builds a line-offset index of a session file when it loads it (one sequential scan) and extends it as records are appended, so any page is a seek and a short read. `reset` and `clear` records cut what the model sees, never what history shows, the same as the transcript today.

Items being written have the provisional id `~<n>` (the `sequence_number` of the `response.output_item.added` that opened them) until their `response.output_item.done` gives the `ref` they became.

### Snapshot, pages, expansion

```json
→ {"jsonrpc":"2.0","id":3,"method":"maic.session.attach","params":{"session":"20261001-091500-tui-4121","exchanges":3}}
← {"jsonrpc":"2.0","id":3,"result":{"entry":{"id":"20261001-091500-tui-4121","title":"fix the relay keepalive","workspace":"~/dev2/MAIC",
            "kind":"main","parent":null,"state":"live","activity":"idle","model":"llamacpp/qwen3.5-9b","mode":"edit","agent":"build","harness":"auto",
            "judge":"maic","tier":"guarded","last_activity":"2026-10-01T09:42:10+02:00","unseen":false,"queued":0,"waiting":null},
   "epoch":"q7c2m0x4ya1b8nde","sequence_number":812,"more_before":true,
   "items":[{"id":"20261001-091500-tui-4121#57","type":"message","role":"user","status":"completed","content":[{"type":"input_text","text":"run the tests"}],
             "maic":{"time":"2026-10-01T09:41:02+02:00"}},
            {"id":"20261001-091500-tui-4121#61","type":"shell_call_output","call_id":"c3","status":"completed","output":[],"max_output_length":null,
             "maic":{"summary":"ctest --test-dir build","ok":true,"size":48213,"head":"Test project ~/dev2/MAIC/build\n    Start  1: harness","collapsed":true}},
            {"id":"20261001-091500-tui-4121#62","type":"message","role":"assistant","status":"completed","content":[{"type":"output_text","text":"All 42 passed.","annotations":[],"logprobs":[]}]}],
   "inflight":null,"pending":[],"todo":[],
   "usage":{"usage":{"input_tokens":9120,"input_tokens_details":{"cached_tokens":0,"cache_write_tokens":0},"output_tokens":38,"output_tokens_details":{"reasoning_tokens":0},"total_tokens":9158},
            "calls":12,"context":32768,"last_input":9120}}}
```

* **`maic.session.attach`** returns the entry, the last `exchanges` exchanges, `inflight` (the reply so far, or the running tool with the last 8 KiB of its output), every pending approval and question in the tree, the todo list and usage, and subscribes the client from the snapshot's `sequence_number` in the same step, so no event falls between the two.
* **`listConversationItems {conversation_id, after, order: "desc", maic: {exchanges}}`** pages backwards from an item. A client may drop pages it scrolled away from and fetch them again.
* **Collapsing.** Tool results, attached files (`context`) and images larger than the client's `collapse_over` arrive with their content left out and `maic {size, head, collapsed: true}` (`head` the first three lines, at most 200 bytes). Defaults: 2 KiB for a remote client, 64 KiB for a local one; maic.nvim may ask for 0 (never collapse) and fold with nvim's folds. User turns and replies are never collapsed: the displayable conversation is what must always be there.
* **`maic.item.expand {item_id, offset, length}`** returns up to 256 KiB of the item's text per call, with `done`; for a tool, the `tool` record's `result`; an image comes back as `mime` and base64 `data` in parts.

### Resume after a reconnect

Each loaded session keeps a ring of its last 10,000 events or 8 MiB, whichever is smaller, in memory only. `sequence_number` counts within the session's epoch (section 17): a load that the last one closed cleanly goes on from where it stopped, and `epoch` (returned by `attach` and `subscribe`) names the number space. `engine.instance` in the hello names the running engine.

1. Reconnect and `maic.hello`. A different `engine.instance` means the engine restarted: the ring is new, but the numbers are not.
2. `maic.session.subscribe {session, epoch, starting_after: last_sequence_number}` replays what was missed and continues. After a restart this works when the client held everything up to the last load's close: the new ring starts at the next number.
3. A different `epoch` (the history was reset, forked or rewritten, or the last load never closed) or a `starting_after` older than the ring answers `maic_resync`: `attach` again.

This replaces maic-server's unbounded per-session `events` vector and its replay of the folded transcript as events on resume: history comes from the file, and the ring only bridges a dropped connection.

## 6. Several clients on one session

The TUI at the desk and the phone on the sofa can have the same session open. One ordered event stream per session is what keeps their views consistent: every client sees the same events in the same `sequence_number` order, and everything per client (focus, collapse sizes, drafts, scroll position) stays in the client.

* **Input** has no lock and no owner. A `response.create` starts a turn when the session is idle; while it works, `response.steer` delivers into the running response at its next boundary, exactly as typing mid-turn does today (and `response.create` on the lane queues a new turn behind it); arrival order at the engine decides. Each `user` item names its client, so every view shows who typed what. Drafts stay in their client.
* **Interrupt** (`cancelResponse`) may come from any client with the session open, local or remote: stopping only adds restriction.
* **Approvals and questions** go to every client with the session (or its tree) open. The first valid answer wins under the session's lock; the rest get `maic.approval.answered` with `by` and close their prompt, and a late answer gets `maic_already_answered`. A remote client may answer an approval raised in a turn started locally (approving from the phone while away from the desk is the point); its answer is recorded with its origin, and it cannot answer `always` (section 7).
* **Settings and lifecycle** (`maic.session.set`, park, stop) act for everyone and are announced with `by`, so the TUI says "parked from phone" rather than going quiet.
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
| reset or unlock the tripwire | there is no such method at all; the session-scoped `:unlock` is a local-only command; `maic.engine.trip` is allowed, since it only adds restriction |
| grant or change trust | no trust method over the protocol; the step-up route `POST /api/trust` stays the one remote path and refuses until accounts register a verifier |
| change global settings | no method writes a settings file; `maic.session.set` changes one session |
| loosen a session | `harness = "dumb"`, `:allow`, removing a `:forbid` term, `:rule`, `:system`, `:prefill`, `:instructions off` are local only; the dumb-plus-auto confirmation is local only |
| run unsandboxed | `maic.session.shell` (`!cmd`) and `:lua` are local only |
| move or touch files outside the harness | `:cd` (already refused for `Origin::Remote`), `:init`, `:undo`, `:export`, `:image FILE` are local only; a remote picture arrives through `maic.session.image` |
| reach the machine | `:up`, `:down`, `:gpu`, `:ctx`, `:ctx2`, `:trust`, `:untrust`, `:lazylock`, `:open`, `:path`, `:artifacts`, `:settings` are local only |
| reach outside the workspaces | `createConversation` and `maic.session.resume` stay inside `server.workspaces` and use only directories already trusted here; `tripwire = "isolated"` refuses remote work, as today |

The rule is an allow-list: each engine method and each engine `:` command carries a remote flag, and anything not marked is refused for remote with `maic_forbidden_remote`. Marked for remote: `:mode` (tightening freely, loosening up to `edit`), `:model` (presets; a model off the machine is labelled as today), `:think`, `:compact`, `:rename`, `:todo`, `:tools`, `:status`, adding a `:forbid` term, lowering `:budget`, and `:trip`.

## 8. How today's pieces migrate

**Who does what today.** `Agent` runs one turn (`submit`) and calls `AgentEvents`. Around it, the TUI does work that belongs to the engine: it owns the `SessionLog`, re-submits messages that were queued after the turn's last model call, titles the session with `small_model`, times the turn for the footer, runs `!cmd` and passes its output in as context, confirms auto under a dumb harness, and parses every `:` command, the session ones included. maic-server repeats a smaller copy of the same (`Session`, `Events`, `start_turn` in `server/src/server.cpp`).

**The engine** is a core class (`core/include/maic/engine.hpp`) that takes all of that over once: the session table, each session's `Agent` and `SessionLog`, the event ring, pending approvals and questions, the turn loop with queued re-submission and titles, the engine `:` commands, the index, and the dispatcher from method to handler. Clients keep the view: rendering, the editor, folds, themes, registers, `:stash`, `:copy`, `:h`, `:e`, `:set markdown|mouse|enter_sends`, `:q`.

1. **The TUI becomes a client of an in-process engine**: the same JSON values through `Engine::call` and a sink, no socket, no change visible to Micaiah. The protocol tests run here.
2. **`maic --rpc`**: the same engine behind stdio. maic.nvim's interface mode (item 1) is its first client.
3. **The daemon** (`maic daemon start|stop|status`): the same engine behind the Unix socket, its PID kept with its start time as MAIC keeps its services' (MAIC owns the PID, not systemd). The TUI and maic.nvim attach to it when it answers and run in-process when it does not.
4. **maic-server becomes an adapter, then goes.** Its HTTP routes are rewritten as calls into the daemon's engine (`POST .../messages` is `response.create` plus a subscription written out as server-sent events in today's shapes), so the web client keeps working; the LAN listener and the relay home link move into the daemon; the tunnel gains kind 6; the web client moves to the protocol; then the HTTP routes, tunnel kinds 1 to 5 and the separate `maic-server` binary are removed, leaving `/api/pair` and `/api/trust` until accounts replace them.

**The host connection stays separate.** maic.nvim's host connection is MAIC driving nvim: MAIC connects to `$NVIM` as a msgpack-rpc client to `:drop` files, show diffs, fire `User` autocmds, follow the colorscheme and offer the `diagnostics` tool. The engine protocol is the other direction: nvim driving MAIC. In interface mode both exist: maic.nvim starts `maic --rpc` as its job, and the engine, being nvim's child, finds `$NVIM` and passes the ancestor check as it does today. Two channels, two directions, two codecs, each vetted on its own terms. When the plugin renders from protocol events it fires the `Maic*` autocmds itself and says so with the `autocmds` capability, and the engine then does not fire them over the host connection. A daemon is not nvim's descendant, so its sessions get no host connection (open question 8).

## 9. Versioning and capabilities

```json
→ {"jsonrpc":"2.0","id":1,"method":"maic.hello","params":{"protocol":1,"client":{"name":"maic.nvim","version":"0.1"},
   "capabilities":["tool_output","collapse","side_threads","tasks","autocmds"],"exclude":["response.output_text.delta"],"view":{"collapse_over":0}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocol":1,"engine":{"version":"dev","instance":"k3f9w2"},"client":"c1","origin":"local",
   "capabilities":["tool_output","collapse","side_threads","tasks","index","images"],"exclude":["response.output_text.delta"],"limits":{"always":true,"max_message":1048576},"tier":"guarded","path":{"via":"stdio"}}}
```

* `protocol` is one integer, MAIC's own: it versions MAIC's extensions and envelope, while OpenAI's shapes follow the pinned description (section 15). The engine answers with the version it will speak, or `maic_unsupported_protocol` with the range it supports. The major version changes only when a message changes meaning or a field is removed.
* Additions (a method, an event type, a field) do not change it. A capability string announces a feature on either side; the engine sends a client only the event types its capabilities cover (no `response.shell_call_output_content.delta` or `maic.tool.output.delta` to a client without the `tool_output` capability).
* **Filtering.** A client may also name, in `exclude`, event types it does not want, from `ordering.json`'s `filter.filterable`: the deltas, since each item's done event carries what its deltas did (a client that excludes `response.output_text.delta` reads the text from `response.output_text.done`). Any other known type is refused with `maic_not_filterable`; a type the engine does not know is ignored, so a client can name tomorrow's deltas today. The hello's answer lists everything the connection will not be sent, from both. `sequence_number` stays the session's, so `starting_after` means the same to every connection whatever it filters, and a client that changes its filter or hands off to another device resumes from a number it holds. The first event sent after filtered ones carries `maic.filtered_from`, the first number filtered out, as a merged delta carries `maic.merged_from`: the client sees "these were mine to skip" rather than a loss. A connection that excludes nothing never gets it. A second `maic.hello` replaces the filter.
* Clients ignore unknown event types and unknown fields, as MAIC ignores unknown record types in a session file. The engine answers an unknown method with JSON-RPC's `-32601`.
* Nothing but `maic.hello` is accepted before `maic.hello`.

Errors use JSON-RPC's codes for protocol faults and `-32000` for the rest; `data` is OpenAI's error object, its `code` OpenAI's where OpenAI defines the condition and `maic_`-prefixed otherwise (section 10):

```json
{"jsonrpc":"2.0","id":9,"error":{"code":-32000,"message":"`:allow` is not available to a remote client","data":{"type":"invalid_request_error","code":"maic_forbidden_remote","message":"`:allow` is not available to a remote client","param":null}}}
```

| `data.code` | When |
| :--- | :--- |
| `response_not_found`, `response_not_active`, `response_already_completed`, `too_many_pending_steers`, `invalid_input`, `steering_not_supported` | OpenAI's steering codes, as OpenAI defines them (section 11) |
| `maic_not_found` | no such session, item, approval or question |
| `maic_busy` | park or stop while a response runs without `interrupt` |
| `maic_already_answered` | another client answered first |
| `maic_forbidden_remote` | outside the remote allow-list |
| `maic_confirm_required` | auto under a dumb harness; `data.message` is what to show |
| `maic_resync` | `starting_after` left the ring, or the session's epoch changed (section 17) |
| `maic_too_large`, `maic_too_slow` | the size caps of section 1, the queue cap of section 4 |
| `maic_tripped` | the harness is tripped |
| `maic_not_owner` | accounts: another user's session |
| `maic_unsupported_protocol` | no common version |
| `maic_not_filterable` | `maic.hello`'s `exclude` names a known event type that is not a delta |
| `maic_steer_disabled` | the action is not in `steering.actions` or not allowed for this client |
| `maic_step_up_required`, `maic_step_up_failed` | the tier asks a remote client for a fresh step-up code, or the code was refused |
| `maic_no_external_harness` | `harness = "external"` where no external agent judges |
| `maic_schema` | a message failed its schema at `reject` (`param` holds the JSON pointer) |
| `maic_protocol_violation` | the engine withheld an event that broke its own ordering or schema; the response fails with it |
| `maic_tier_unavailable` | the session's tier cannot run here (`airtight` on an unstamped build, `open` over a remote transport) |

## 10. Names: OpenAI's, extended under `maic`

Micaiah's rule (2026-10-02): OpenAI's API description, [openai/openai-openapi](https://github.com/openai/openai-openapi) (MIT; pinned at commit `de3a025c40f84b99d1401ee1c5fe69fbf8de789b`, OpenAPI 3.1.0, API version 2.3.0), is the gold standard for LLM communication and processes. Where it defines a concept MAIC has, MAIC uses its names, field names, value enums and shapes unchanged; what is MAIC's alone is an extension. "We can extend, but we shouldn't extinguish." [standards.md](../standards.md) makes it the first principle for every standard MAIC follows.

### The convention

1. **OpenAI's names, exactly**: event types, objects (`response`, `conversation`, items, content parts, `usage`, errors), fields, enum values, request parameters, REST operations by their `operationId` (`getResponse`, `cancelResponse`, `listConversationItems`), and the Responses WebSocket's client events by their `type` (`response.create`, `response.steer`).
2. **MAIC's own event types and methods start with `maic.`** (`maic.review.started`, `maic.steer`, `maic.session.park`).
3. **MAIC's own fields on an OpenAI-shaped object go in one `maic` object on it** (`"maic": {"judged_by": "rules"}`), never beside OpenAI's fields.
4. **A closed OpenAI enum is never extended.** A MAIC value goes in the `maic` object beside OpenAI's (`"status": "incomplete", "maic": {"status": "denied"}`). An open string or an extensible enum (error codes, `ResponseSteerErrorCode`) takes MAIC values with the prefix `maic_` (`maic_forbidden_remote`), including a value MAIC mints from an upstream's: llama.cpp's numeric error code 500 becomes `"maic_llamacpp_500"`, with the number and its source kept in `error.maic.upstream` ([standards.md](../standards.md#adapter-normalizations)).
5. **An extension never redefines or shadows an OpenAI name**, and an OpenAI-only client that ignores every `maic.*` event and every `maic` object still follows a session: responses created, items added, text streamed, tools run, responses completed, steered, failed.

MAIC's own: the harness's review stream, approvals, questions, steering actions beyond `response.steer`, `judged_by`, trust and tiers, a session's lifecycle beyond a response's `status`, and the session index.

### The channel is the Responses WebSocket's counterpart

OpenAI's Responses WebSocket mode is the nearest thing to MAIC's channel: one connection, client events `response.create` (with `stream_id`, a lane whose requests run FIFO) and `response.steer`, and server events that are the `ResponseStreamEvent`s plus `stream_id` and the steering events. MAIC's JSON-RPC channel is shaped after it: one notification, `maic.event`, carries one server event exactly as the WebSocket would send it, with `stream_id` set to the session id (a session is a lane); the WebSocket's client events are methods of the same names; REST operations are methods named by `operationId`. JSON-RPC 2.0 stays the envelope (ids, answers, errors), and an error's `data` holds OpenAI's error object, `{type, code, message, param}`.

### Objects

| OpenAI | MAIC |
| :--- | :--- |
| `conversation` (`{id, object, metadata, created_at}`) | a session: `id` the session id, `metadata.title` its title, `maic.entry` the index entry |
| `response` (`status`: `queued`, `in_progress`, `completed`, `failed`, `cancelled`, `incomplete`) | one run of the agent loop: the model, the tools it calls and their results, until it answers, is steered or stops. `id` is `<session id>.r<k>`; `background` is always `true`, because the engine owns every run whatever the clients do; `previous_response_id` links it to the response it continues; `maic {turn, origin, final, ended_by}` |
| none | a turn: the chain of responses from one message to its answer, linked by `previous_response_id`, numbered by `maic.turn`, closed by the terminal event that carries `maic.final: true` |
| output items `message` (`output_text`), `reasoning` (`reasoning_text`), `function_call`, `function_call_output`, `shell_call`, `shell_call_output`, `compaction` | the reply, thinking, a tool call and its result (`run_shell` as a shell call, every other tool as a function call), a compaction |
| input items: `message`, role `user`, with `input_text` and `input_image` parts | what a person typed or attached |
| item ids | `<file id>#<line>`, or `~<n>` (the `sequence_number` of its `added` event) until the record exists |
| `usage` (`input_tokens`, `input_tokens_details`, `output_tokens`, `output_tokens_details`, `total_tokens`) | on every finished response; per-call figures and the budget in `maic.usage.updated` |

The session file keeps its own records ([sessions.md](../sessions.md)); the engine maps records to items at the protocol boundary, so no transcript changes shape.

### The order

OpenAI's description gives each event's shape but not the sequence: OpenAPI 3.1 gives `text/event-stream` a `schema` for one event. The order below is that missing half, borrowed from how the Responses API streams and held to by the checker (section 15). Anthropic's Messages stream has the same shape (`message_start`, `content_block_start` with `index`, `content_block_delta`, `content_block_stop`, `message_stop`), and MAIC's provider clients read both already.

1. **Every event carries `sequence_number`**, one number space per epoch of a session (section 17): 0 when the epoch starts, one more per event, going on across loads while the epoch holds. A response's events are therefore a contiguous run of the session's numbers, and `starting_after` means the same on `getResponse` and on `maic.session.subscribe` (open question 23). A merged delta (section 4) carries `maic.merged_from`, the first number it covers. `maic.index` and `maic.engine` notifications replace state and carry no number.
2. **Every lifecycle has an opening event and exactly one terminal event.** A response: `response.created`, then `response.queued` while it waits for a slot, then `response.in_progress`, then one of `response.completed`, `response.incomplete`, `response.failed` or `maic.response.cancelled`. Items, parts, reviews, approvals, questions and tasks likewise.
3. **Items are announced before their deltas and closed after them**: `response.output_item.added`, then for a message `response.content_part.added`, the `response.output_text.delta`s, `response.output_text.done`, `response.content_part.done`, then `response.output_item.done`. Nothing is sent for a closed item or part.
4. **A response ends after its items.** Its terminal event comes after every item it opened is done; an abrupt end (a halt, the tripwire, a provider failure) first closes open items with `status: "incomplete"` (and `maic.status: "discarded"` when a halt threw them away).

Items are addressed by `item_id` and `output_index` (0-based within the response, in `added` order, never reused); content parts by `content_index`.

### The state machines

Derived from the messages of sections 2 to 6 and 11. `-` is "not yet"; a terminal state takes no further event.

**Session** (`maic.session.state`; one machine per load of a session):

| From | Event | To |
| :--- | :--- | :--- |
| - | `maic.session.state` `live` or `background` (attach, create, resume) | `live`, `background`; the load's header (`epoch`, `protocol`, section 17) |
| `live` | `maic.session.state` `background` | `background` |
| `background` | `maic.session.state` `live` | `live` |
| `live`, `background` | `maic.session.state` `parked` | `parked`, terminal for this load |
| `live`, `background` | `maic.session.state` `stopped` | `stopped`, terminal |

`activity` moves `idle` to `working` at a turn's first `response.created`, to `slot` at `response.queued`, to `waiting` at `maic.approval.requested`, `maic.question.asked` or `maic.turn.paused`, back to `working` when the last of those closes, and to `idle` at the terminal event with `maic.final: true`. Parking or stopping with a response running first cancels it.

**Response** (at most one open per session; a response with a `previous_response_id` follows that response's terminal event):

| From | Event | To |
| :--- | :--- | :--- |
| - | `response.created` | `created` |
| `created` | `response.queued` | `queued` |
| `created`, `queued` | `response.in_progress` | `in_progress` |
| `in_progress` | `response.completed` | `completed`, terminal |
| `in_progress` | `response.incomplete` | `incomplete`, terminal |
| `created`, `queued`, `in_progress` | `response.failed` | `failed`, terminal |
| `created`, `queued`, `in_progress` | `maic.response.cancelled` | `cancelled`, terminal |

**Turn** (MAIC's; `maic.turn` is the previous turn plus one): opens at a `response.created` with a new `maic.turn`; each later response of the turn names the previous one in `previous_response_id`; `maic.turn.paused` may follow a `maic.response.cancelled` with `final: false`, and the next response of the turn resumes it; the turn closes at the terminal event carrying `maic.final: true`.

**Item** (only inside an open response):

| From | Event | To |
| :--- | :--- | :--- |
| - | `response.output_item.added` | `open` |
| `open` | the item's own deltas: `response.content_part.added`, `response.output_text.delta` / `.done`, `response.content_part.done` (a `message`); `response.reasoning_text.delta` / `.done` (a `reasoning`); `response.function_call_arguments.delta` / `.done` (a `function_call`); `response.shell_call_command.*` (a `shell_call`); `response.shell_call_output_content.*` or `maic.tool.output.delta` (an output item) | `open` |
| `open` | `response.output_item.done` | `done`, terminal |

**Action** (a tool call: from its call item's `done` to its output item's `done`):

| From | Event | To | Only when |
| :--- | :--- | :--- | :--- |
| `proposed` | `maic.review.started` | `reviewing` | the round-trip path (`smart`) and an action the reviewer reads |
| `reviewing` | `maic.review.delta` | `reviewing` | the client subscribed to review text |
| `reviewing` | `maic.review.verdict` `allow` | `allowed` | |
| `reviewing` | `maic.review.verdict` `ask`, then `maic.approval.requested` | `asking` | a failed reviewer answers `ask` |
| `reviewing` | `maic.review.verdict` `deny` | `denied` | |
| `reviewing` | `maic.review.cancelled` | `withdrawn` | a steer or a halt |
| `proposed` | `maic.approval.requested` | `asking` | the rules ask |
| `proposed` | none (the rules allow, or deny) | `allowed`, `denied` | |
| `asking` | `maic.approval.answered` `yes` or `always` | `allowed` | |
| `asking` | `maic.approval.answered` `no` or `trip` | `denied` | |
| `asking` | `maic.approval.answered` `withdrawn` | `withdrawn` | a steer |
| `allowed`, or `proposed` on the external path | `response.output_item.added` (the output item) | `running` | on the external path the other agent already ran it, and no `maic.review.*` or `maic.approval.*` may appear for it |
| `running` | output deltas | `running` | |
| `running` | `response.output_item.done` | `done`, terminal | a cancelled run ends `status: "incomplete"` |
| `denied`, `withdrawn` | the output item's `added` and `done`, its output saying why | `done`, terminal | `maic.status` `denied` or `withdrawn` |

`maic.judged_by` on the output item must agree with the path taken: `maic` after a `maic.review.verdict`, `rules` when no review occurred on MAIC's side, the external agent's name on the external path.

**Review, approval, question.** A review is `maic.review.started`, any `maic.review.delta`, then `maic.review.verdict` or `maic.review.cancelled`. An approval is requested, then answered once. A question is asked, then answered once (`withdrawn: true` when a steer took its place). A late answer is a request error (`maic_already_answered`), never an event.

**Steer.** `response.steer.accepted` and `maic.steer.applied` are legal only while a response of the turn is open or the turn is paused, and come before every event they cause.

**Gaps.** A client expects the next event's number (or its `maic.filtered_from`, else its `maic.merged_from`) to be one more than the last it holds. A larger number is a gap: it resubscribes with `starting_after` set to the last number it holds, and the engine replays from the ring or answers `maic_resync`. A smaller or repeated number within a load is a protocol violation.

### Renames to make when the protocol is built

Names MAIC's code or this design's first draft use today, each replaced by the name above when its step lands:

| Today | Becomes |
| :--- | :--- |
| maic-server's `GET /api/sessions/{id}/events?after=N` | `starting_after`, on `maic.session.subscribe` and `getResponse` |
| the server's session field `seq` | `sequence_number` |
| server events `text` (with `thinking`) | `response.output_text.delta`, `response.reasoning_text.delta` |
| `tool_call`, `tool_result` | the `function_call` or `shell_call` item and its output item (`response.output_item.added` / `.done`) |
| `user` (with `queued`) | `maic.input.added`; delivering into a running response is `response.steer` |
| `approval`, `approval_answered` | `maic.approval.requested`, `maic.approval.answered` |
| `notice`, `mode` | `maic.notice`, `maic.session.settings` |
| `error {text}` | `error {code, message, param}` |
| `done` | `response.completed` |
| `POST .../messages`, `.../interrupt`, `.../approvals/{id}`, `.../mode` | `response.create` or `response.steer`, `cancelResponse`, `maic.approval.answer`, `maic.session.set`, through the adapter until the routes go (step 17) |
| the first draft's `hello`, `session.*`, `event`, `turn_start`, `tool_output`, ... | the methods and events of section 3 |
| the addendum's `review.*`, and `tool.started` / `tool.finished` with `judged_by` | `maic.review.*`, and the output item's `added` / `done` with `maic.judged_by` |

The capability string `tool_output` keeps its name: capabilities are MAIC's own handshake vocabulary.

## 11. Steering

OpenAI defines steering, and MAIC adopts it as written: `response.steer {previous_response_id, input}` queues user input for a running response; `response.steer.accepted {steer {id, previous_response_id}}` says the engine owns it; the response then finishes at a safe boundary with `response.incomplete` and `incomplete_details.reason: "steered"`, and a successor `response.created` (its `previous_response_id` the steered response) carries the input and is the commit point; input that cannot be committed comes back in `response.steer.failed` with one of OpenAI's codes (`response_not_found`, `response_not_active`, `response_already_completed`, `too_many_pending_steers`, `invalid_input`, `steering_not_supported`, `successor_creation_failed`). In MAIC that is a message typed while a turn runs: it lands at the next boundary, after the current model call and its tools, as the mailbox does today.

Micaiah's six actions extend it, through `maic.steer`, for when waiting for the boundary is not what she wants:

| Action | Generation | The partial reply | A pending review or approval | A running tool | The model is told | The response, then the turn |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `steer` | stopped now | kept | withdrawn | cancelled, or waited for (`tool`) | the note: "The user redirected you: NOTE. Continue from where you stopped, following it." | `response.incomplete` (`steered`), then a successor carrying the note: the turn continues |
| `drop` | stopped now | kept, or trimmed (`trim`, `at`) | withdrawn | cancelled, or waited for | "The user dropped the topic you had started; it was removed from your reply. Leave it and do not return to it." and the note, if any | as `steer` |
| `further` | continues to the next boundary | kept | left alone; the note waits until it resolves | waited for | "The user asks you to go deeper on what you were just saying." and the note, if any | as `steer`, at the boundary |
| `interrupt` | stopped now | kept | withdrawn | cancelled | nothing yet: the next steer or message says it | `maic.response.cancelled` (`final: false`), then `maic.turn.paused` |
| `keep` | stopped now | kept as the answer | withdrawn | cancelled | nothing: the partial reply stands as the answer | `response.completed`, `maic {ended_by: "keep", final: true}` |
| `halt` | stopped now | discarded | withdrawn | cancelled, output discarded | the canned halt message | items closed `incomplete` (`maic.status: "discarded"`), `error` with `code: "maic_halted"` and the message, then `maic.response.cancelled` (`final: true`) |

`steer`, `drop` and `further` are OpenAI steers with MAIC's text as their input, so they also emit `response.steer.accepted`, and an OpenAI-only client sees exactly a steered response and its successor. `interrupt`, `keep` and `halt` stop rather than redirect, and show to such a client as a cancelled or completed response. `cancelResponse` (Ctrl-C, `:MaicInterrupt`) stays what it is in OpenAI's API: the response ends `cancelled` with its partial output kept, and the turn ends with it.

**Kept** works as a ban's cut does ([bans.md](../bans.md#what-the-model-sees)): the partial reply's item closes `incomplete`, and the steered response's output is the assistant's turn so far. **Trimmed** removes the tail of the partial reply from the start of what was dropped: `trim` is `none`, `sentence`, `paragraph` (the default, `steering.drop_trim`) or `all`, and `at`, a byte offset chosen in the client, overrides it; `response.output_text.done` then carries the trimmed text and `maic.trimmed` the range removed, so a client replaces what the deltas built. **Withdrawn** means the proposed action does not run: a review in flight is cancelled (`maic.review.cancelled`), an open approval closes (`maic.approval.answered`, `choice: "withdrawn"`), and the output item says "not run: the user redirected", so the model may propose it again under the new direction. An action proposed under the old direction never runs after a redirect. **A running tool** is cancelled as `cancelResponse` cancels one (process group killed, the output so far recorded), or, for `steer` and `drop` with `tool: "wait"` (or `steering.on_running_tool = "wait"`), finishes first and the steer applies at the next boundary; `further` always waits, and `interrupt`, `keep` and `halt` always cancel.

```json
→ {"jsonrpc":"2.0","id":52,"method":"maic.steer","params":{"session":"20261001-091500-tui-4121","response_id":"20261001-091500-tui-4121.r9","action":"drop","note":"leave the CI config alone"}}
← {"jsonrpc":"2.0","id":52,"result":{"steer":{"id":"st7","previous_response_id":"20261001-091500-tui-4121.r9"}}}
← {"jsonrpc":"2.0","method":"maic.event","params":{"type":"response.steer.accepted","sequence_number":905,"stream_id":"20261001-091500-tui-4121",
   "steer":{"id":"st7","previous_response_id":"20261001-091500-tui-4121.r9"}}}
← {"jsonrpc":"2.0","method":"maic.event","params":{"type":"maic.steer.applied","sequence_number":906,"stream_id":"20261001-091500-tui-4121","steer":"st7",
   "action":"drop","trigger":"client","by":{"client":"c2","name":"phone","origin":"remote"},
   "trimmed":{"item_id":"~880","from":1412,"to":2210},"withdrawn":["a14"],"cancelled_tool":null}}
```

Then the trimmed `response.output_text.done`, the item and part closes, `response.incomplete` with `incomplete_details.reason: "steered"`, and the successor `response.created`. `maic.steer.applied` carries the steer's id, `action`, `trigger` (`client` or `ban`), `by` (for a ban: `{client: "engine", name: "bans"}` with the turn's origin), `ban` (which entry fired, `{list, index}`, never the matched text), `trimmed`, `withdrawn` (review and approval ids), `cancelled_tool` and, for a waiting steer, `waits_for`.

**Rules.**

* `response_id` must name the running response (or, for a paused turn, its last). A steer for one that has ended answers OpenAI's `response_already_completed` or `response_not_active`, and is never applied to the next turn.
* Steers apply one at a time under the session's lock, in arrival order; after a `keep` or `halt` the rest answer `response_not_active`. A steer and an approval answer race under the same lock: if the answer won, the tool is running and the steer meets a running tool; if the steer won, the late answer gets `maic_already_answered`.
* **A note is input.** A remote client's `response.steer`, or `maic.steer` with text, raises the turn's origin to remote (section 6), as a remote message delivered mid-turn does. `interrupt`, `keep` and `halt` carry no text and change no origin.
* **A paused turn** has its session `waiting` (`waiting.kind: "steer"`). `steer`, `drop`, `further`, `response.steer` or `response.create` on its lane resume it with a successor response; `keep`, `halt`, `cancelResponse`, park and stop end it. A pause has no timeout (open question 14).
* **Tasks.** Steering a parent while a foreground `task` runs treats the child as a running tool; a child is steered directly by its own session id.
* **`cancelResponse` needs no step-up** and is open to every client with the session in every tier, because stopping only adds restriction.
* **The transcript** gets a `steer` record (action, by, trigger, note, trimmed range, withdrawn ids), so a resumed or forked session shows what the model was told. A halt's discarded text is recorded only as a byte count.

### Steers from bans

A string or regex ban ([bans.md](../bans.md)) can name a steer action for its hits, instead of the cut, tell and re-ask it does today:

```lua
bans = {
  patterns = {
    "as an ai( language model)?",                                                  -- cut, tell, re-ask: as today
    { "kubernetes|helm chart", steer = "drop", note = "This project has no cluster; leave deployment out." },
    { "curl [^|]*[|] *(ba)?sh", steer = "halt" },
  },
}
```

On a hit the filter cuts before the match exactly as cut mode does, so the match never reaches a screen, then applies the entry's action with `trigger: "ban"` in `maic.steer.applied`: `drop` trims to before the match at least (further when `drop_trim` says so) and continues with the note; `steer` appends the note and continues; `interrupt` pauses for a person; `keep` ends the turn with the clean part; `halt` discards and sends the canned message. `further` is refused for a ban when settings load (it would go deeper into the banned topic). `retries` still bounds a turn: an entry with a steer that fires again after `retries` hits escalates to `halt`. Token bans never produce text, so they trigger nothing.

### Settings

In the settings files like everything else (`settings.lua`, any layer unless marked), and per agent under `agents.NAME.steering`:

```lua
steering = {
  actions = { "steer", "drop", "further", "interrupt", "keep", "halt" },
  halt_message = "The user halted this turn. What was in progress was discarded; do not continue it. Wait for the next message.",
  drop_trim = "paragraph",
  on_running_tool = "cancel",
  clients = { ["local"] = "all", remote = "all" },
  ban_actions = { "steer", "drop", "interrupt", "keep", "halt" },
}
agents = { explore = { steering = { actions = { "interrupt", "keep", "halt" } } } }
```

| Key | Default | Meaning | Where it may be set |
| :--- | :--- | :--- | :--- |
| `actions` | all six | the actions this session accepts; any other answers `maic_steer_disabled`. `response.steer` (a message mid-turn) and `cancelResponse` are not among them and are never disabled | any trusted layer; a project layer and an agent only remove actions |
| `halt_message` | the text above | what `halt` puts in the model's context and shows every client as the turn's error | any trusted layer (it is an instruction to the model, as `MAIC.md` is) |
| `drop_trim` | `"paragraph"` | the default trim of `drop`: `none`, `sentence`, `paragraph`, `all` | any trusted layer |
| `on_running_tool` | `"cancel"` | what `steer` and `drop` do to a running tool when the request does not say: `cancel` or `wait` | any trusted layer |
| `clients` | local `all`, remote `all` | which actions local and remote clients may send (`"all"`, `"none"` or a list); the tier may narrow it further and adds step-up (section 14) | global file only |
| `ban_actions` | all but `further` | which actions a ban entry may name | any trusted layer, narrowing |
| ban triggers | none | `steer` and `note` on a `bans.strings` or `bans.patterns` entry | wherever bans are set ([bans.md](../bans.md)) |

Per agent, `agents.NAME.steering` takes the same keys; an agent defined in a project file can only narrow, and `clients` there is ignored with a warning naming the file. `:steering` shows what is in force and where each value came from; `:steer ACTION [NOTE]` sends one from the TUI (its keys are chosen with the TUI work, step 7).

## 12. Following the Responses and Conversations lifecycle

OpenAI's description covers the whole flow, not only one call: `POST /responses` (`createResponse`, with `background`, `previous_response_id`, `conversation`, `stream`, `stream_options`, `store`, `instructions`, `tool_choice`, `parallel_tool_calls`, `max_tool_calls`, `truncation`, `include`), `GET /responses/{response_id}` (`getResponse`, with `stream` and `starting_after`: resuming a stream from a sequence number) and its `DELETE`, `/responses/{response_id}/cancel`, `/responses/{response_id}/input_items`, `/responses/compact`, `/responses/input_tokens`, and `/conversations` with `/conversations/{conversation_id}/items`. MAIC follows that flow, and every row is either OpenAI's as written or a `maic` extension beside it:

| OpenAI | MAIC | Status | Why |
| :--- | :--- | :--- | :--- |
| `response.create` (the WebSocket's form of `createResponse`) | `response.create` on the channel | adopted | the channel is the WebSocket's counterpart (section 10) |
| `background: true` | every response | adopted | the engine owns every run; clients come and go, as with OpenAI's background responses |
| `getResponse` with `stream=true&starting_after=N` | `getResponse`, for one response | adopted | the same need: leave a running stream and come back to it |
| none | `maic.session.subscribe {starting_after}`, the session's whole stream; `maic.session.attach` | extension | a session runs many responses and MAIC-only events between them; the parameter keeps OpenAI's name and meaning |
| `cancelResponse` | `cancelResponse` (Ctrl-C) | adopted | the response ends `cancelled`, its partial output kept |
| none | the `halt`, `interrupt` and `keep` steers | extension | stopping that throws the work away, pauses, or accepts it, beside OpenAI's one cancel |
| `deleteResponse`, `deleteConversation`, `deleteConversationItem` | not offered | adopted names, not offered | a session file is append-only; `maic.session.stop` ends a session and its transcript stays |
| `previous_response_id` | links each response of a turn to the one before, and a fork's first response to where it forked | adopted | lineage by pointer, as MAIC's forks already are |
| `stream_id` lanes | one lane per session | adopted | requests on a lane run FIFO, as a session's queued messages do |
| `conversation`, `createConversation`, `getConversation`, `updateConversation` | sessions | adopted | a durable container of items with stable ids |
| none | `live`, `background`, `parked`, `stopped`, the index, focus, side threads, forks | extension | a session's life beyond its items |
| `listConversationItems`, `getConversationItem` | history pages and single items | adopted | |
| none | `maic.exchanges` paging, collapsing and `maic.item.expand` ranges | extension | lazy history for a phone, by exchange and by byte range |
| item ids | `<file id>#<line>` | adopted | ids are opaque strings; MAIC's name one line of one file and never move |
| `listInputItems` | a response's input items | adopted | |
| `Compactconversation`, `response.compaction.compacting`, the `compaction` item | `:compact` and automatic compaction | adopted | `encrypted_content`, opaque by contract, holds MAIC's reference to its `compact` record; the readable summary and byte counts are in `maic`. Micaiah's flow (prune old tool results first, summarise only when that is not enough) is unchanged |
| `Getinputtokencounts` | the context estimate MAIC makes before each call | adopted | |
| `parallel_tool_calls`, `max_tool_calls` | honoured as limits; MAIC runs one call at a time per agent, so a response reports `parallel_tool_calls: false` | adopted | each action is judged and, when asked, approved on its own; parallel work runs as background tasks, each with its own harness |
| `tool_choice` | `auto`, `none` and a named tool, within the agent's tool list | adopted | |
| `truncation` | `auto` lets compaction run; `disabled` refuses an input that does not fit | adopted | |
| `include` | the values MAIC has data for | adopted | `reasoning.encrypted_content` never applies: MAIC's reasoning is plain text |
| `instructions` | the operator prompt for this response | adopted | local clients only (section 7) |
| `store` | `true`, or `false` in a `--no-record` session | adopted | |
| `stream_options` | read; `include_obfuscation` has nothing to do on a local channel | adopted | |
| `usage` | on every finished response | adopted | |
| none | `maic.usage.updated`: per-call figures, context and budget | extension | |
| `response.steer` and its events | a message typed mid-turn | adopted | section 11 |
| none | the six steering actions, reviews, approvals, questions, `judged_by`, tiers, trust | extension | OpenAI's API has no harness judging actions on the user's machine and no person stopping a turn but by cancel |

## 13. Harness paths

Micaiah's three pipelines, chosen per agent and per session, with the rules of [harness-authority.md](../harness-authority.md) as the constraints:

| `harness` | Path | Who judges an action | Streams | Valid for |
| :--- | :--- | :--- | :--- | :--- |
| `dumb` | the basic path | MAIC's fixed rules alone | one: the reply | any model |
| `smart` | the local round trip | MAIC's reviewer model, over the fixed rules | two: the reply, and each action's review | everything MAIC's own loop drives: API and local models, L1, L2 |
| `external` | full remote, no round trip | the external agent's own harness, over MAIC's fixed rules | one: what the agent said and did | an L3 agent (its own tools, opted in per agent definition) whose own harness is smart |
| `auto` | per harness-authority.md | | | `smart` for API and local models and L2; `external` for an L3 agent with a smart harness; `smart` for an L3 agent in a bypass mode |

Set in `harness` (settings), `agents.NAME.harness`, and per session with `maic.session.set {harness}`, `:harness` and `--harness`; the session wins over the agent, the agent over the settings layers. The entry and `maic.session.settings` carry `harness` (the choice) and `judge` (the result: `maic`, `rules` or the agent's name, the same values as `judged_by`).

Constraints:

1. **The fixed rules are the floor on every path**: forbidden terms, trip patterns, trust, the sandbox, self-protection. No value turns them off.
2. **One judge per action.** `smart` and `external` never both judge one action: on `external` MAIC sends no review, and on L2 the external agent's own prompts and classifier are off for MAIC's tools.
3. **`external` needs an external harness.** For an API or local model, L1 or L2 it answers `maic_no_external_harness`: there nobody else judges, so it would mean nobody does.
4. **An L3 agent in a bypass mode** keeps `smart` under `auto`; an explicit `external` for it asks for the same confirmation as dumb plus auto (`maic_confirm_required`).
5. **Loosening is local** (section 7): `smart` to `dumb` or `external`, and anything to `dumb`. A remote client may only move toward `smart`. `dumb` with auto mode asks once, as today.
6. **The tier caps the choice** (section 14): `airtight` refuses `dumb`.

## 14. Security tiers

The protocol and steering run at one of three tiers, `open`, `guarded` (the default) and `airtight`, from no checking at all (a private local sandbox, experiments) to everything validated, ordered, stepped-up and audited. The tiers, how they are set and shown, and how they map onto the rendezvous relay and a trusted node are in [protocol-security.md](protocol-security.md). What no tier changes: transport security, the remote allow-list of section 7, `Origin::Remote` always asked, no tripwire reset from a client, and the harness's fixed rules.

## 15. Schemas, the ordering machine and conformance

### The files

```
protocol/
  openai/                     the pinned subset of openai/openai-openapi: every OpenAI schema MAIC emits, reads or accepts
  maic.openrpc.json           every method, and the notifications `maic.event`, `maic.index`, `maic.engine` (OpenRPC 1.3)
  schemas/                    JSON Schema 2020-12 for MAIC's own messages and the `maic` objects;
                              event.schema.json is the union over `type`: OpenAI's event schemas by $ref into openai/,
                              then the `maic.*` events, as ResponseStreamEvent is OpenAI's union
  ordering.json               the state machines of section 10 as data: states, events, transitions, terminal states, rules
  maic-server.openapi.yaml    the HTTP side while it exists (OpenAPI 3.2): `itemSchema` for text/event-stream
                              and application/jsonl points at event.schema.json
```

A top-level `protocol/`, installed under `share/maic/protocol/`, because the engine (C++), maic.nvim (Lua), the web client and the tests all read it. OpenRPC is to JSON-RPC 2.0 what OpenAPI is to HTTP; its `components.schemas` are JSON Schema, so the payload schemas are written once and referenced from both the OpenRPC and the OpenAPI document. The notifications are listed as methods with no `result`, which is how OpenRPC describes a notification. OpenAPI 3.2's `itemSchema` describes each item of a sequential media type (`text/event-stream`, `application/jsonl`), the formal version of what OpenAI's 3.1 document says with a plain `schema`.

**OpenAI's schemas are not copied by hand.** `protocol/openai/` is the subset of [openai/openai-openapi](https://github.com/openai/openai-openapi) MAIC needs, extracted by a script from the file at commit `de3a025c40f84b99d1401ee1c5fe69fbf8de789b` (OpenAPI 3.1.0, API 2.3.0, MIT), with a README naming the commit, the date and the extraction command: the Responses objects, items, content parts, the stream and steering events, the conversation and list objects, the error objects, and `CreateChatCompletionStreamResponse` with what it references (what `core/src/openai.cpp` reads from llama.cpp). MAIC's schemas reference it and never restate it, so an OpenAI shape can only change by bumping the pin, in a change of its own. The change that adds it adds its MIT row to [cleanroom.md](../cleanroom.md). No network is used: the gate reads the pinned copy.

### One source, checked

The schemas are the source of every message's shape; the C++ keeps building `nlohmann::json` values as today, with no generated structs. CMake embeds the files into the binary (a generated source holding their text), so runtime validation needs no file lookup. `ordering.json` is the source of the machines: the engine's runtime enforcement and the checker both load it, and the tables in section 10 are its design. `protocol_schema_test` keeps the sides in step:

* every method in the dispatcher is in `maic.openrpc.json` and the reverse, and every method not starting with `maic.` is an `operationId` or a WebSocket client event `type` of the pinned spec;
* every event type the engine can emit is in the union and the reverse, and every one not starting with `maic.` is an event of the pinned spec;
* no MAIC schema defines a name the pinned spec defines, and every OpenAI-shaped object MAIC sends has no field beyond OpenAI's but `maic` (rules 3 to 5 of section 10);
* every state and event in `ordering.json` names a real event type; every JSON example in this design and in the user docs validates; the schemas use only the keywords the validator supports.

**The validator** is MAIC's own: the checker that script tool arguments already pass through ([tools.md](../tools.md)) grown to the keywords the schemas, OpenAI's included, use (`type` with type arrays, `properties`, `required`, `additionalProperties`, `enum`, `const`, `items`, `anyOf`, `oneOf`, `allOf`, `$ref` and `$defs`, `minimum`, `maximum`, `minLength`, `maxLength`, `minItems`; `discriminator` and the `x-` keys read as annotations), with no new dependency (open question 18). When Python's `jsonschema` package is importable, the conformance test also validates the recorded streams with it, and skips that half otherwise, as the TUI test does without pyte.

### The checker

`maic protocol check [FILE...]` and ctest `protocol_conformance` read recorded streams, one message per line as `{"dir": "in" | "out", "conn": "c1", "msg": {...}}`, and check: every message against its schema; every session's events against `ordering.json`; sequence numbers (0 at each load, one more each time, `maic.merged_from`, no repeats, `maic_resync` honoured); every request answered once; a steer's events before their effects; `maic.judged_by` against the path. The first violation per stream is reported with its `sequence_number`, the rule's id and the rule's text, and the exit status is 1.

It runs on every build (`scripts/check.sh`), over:

* **The test suite's own streams.** Every in-process protocol test records its exchange into `build/protocol-streams/`, and the checker runs over all of them after the tests.
* **Mutated streams.** Each recorded stream is mutated (an event dropped, duplicated, swapped or renumbered, a stream truncated, a `type` changed, a required field removed, a field added beside OpenAI's outside `maic`, a steer placed after a terminal event); every mutation that breaks a rule must be rejected with that rule's id, and every one that breaks none must pass.
* **Driven streams.** A randomised driver runs the in-process engine against `FakeServer`: messages, `response.steer` and steers of every action at random points, approvals answered, raced and withdrawn, ban hits with steer triggers, cancellations, a slow consumer. Every stream it produces must pass. A fixed seed in the gate keeps it deterministic ([testing.md](../testing.md)); `scripts/check-asan.sh` runs longer seeds, beside ctest `fuzz`.
* **An OpenAI-only view.** Every stream above, with every `maic.*` event and `maic` object removed, must still pass OpenAI's own schemas and the response and item machines: rule 5, tested.
* **The provider side.** `FakeServer`'s OpenAI-compatible chunks and the client's parsing are validated against the pinned `CreateChatCompletionStreamResponse`; fields llama.cpp adds (`timings`, `reasoning_content`) are allowed, and what OpenAI defines must be shaped as defined.
* **Recorded real streams**, when a session ran at `airtight` and kept its stream ([protocol-security.md](protocol-security.md)).

## 17. Event identity: epochs, self-describing streams and lineage

The numbers in section 5 order a session's events and show gaps. Three more things make the stream durable, readable without this code, and able to show where each event came from. Micaiah set the shape; the prior art is noted in [standards.md](../standards.md#model-apis-and-protocol-prior-art).

### The epoch

An **epoch** is one incarnation of a session's history. It is a random id, carried on every load's first event (`maic.session.state`) and written to the transcript as an `epoch` record. `sequence_number` belongs to the epoch, not the load: a load that the one before it closed cleanly (a `stream` record with `closed: true` and the number it stopped at) continues the same epoch from that number, so a client that held everything resumes across a restart. A new epoch starts, from 0, naming the one it replaces (`previous_epoch {epoch, reason}`) or the parent it forked from (`forked_from {session, epoch, records}`), when the history is fresh, was reset, was forked, was rewritten, or the last load did not close. `maic.session.subscribe` takes the epoch back; another epoch is `maic_resync`. The engine process has its own `engine.instance` in the hello, random per start, which tells a reconnecting client the ring is new without saying the numbers are.

### Self-describing streams

A transcript and a protocol recording each carry the shape of every kind of line they hold, so a reader needs no source and no git history, even for a protocol since renamed, obfuscated or deleted. This is Avro's object-container approach rather than a central registry.

* A **skeleton** is a value with every name kept and every value emptied (`null` stays, a boolean becomes `false`, a number `0`, a string `""`, an array holds its elements' distinct skeletons in canonical order). A skeleton is hashed over its [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785) (JCS) serialization, in OCI digest form (`sha256:...`).
* The first time a file holds a record or event of a given type and shape, a **skeleton line** precedes it: `{of, hash, canonical, skeleton}`, and `must_understand: true` for a type a reader must not skip (section below). A protocol recording opens with a `{dir: "header", protocol, canonical}` line, the protocol hash being the digest of `protocol/` (names and types, prose dropped) in the same form; `maic protocol hash` prints it.
* The reader checks each skeleton hashes to its own `hash` (`skeleton.hash`), that every event matches a skeleton declared before it (`skeleton.undeclared`), and reads an event or record whose type this build does not know by its skeleton alone, leaving it untouched. `maic protocol check --blind` uses only the file's own skeletons and the universal rules (numbers, `stream_id`), never the built-in schemas, to prove a file reads itself.
* The serialization rule is itself part of the protocol (named in every header and skeleton line), so changing it is a protocol change like any other, tracked where the event list is.

### must_understand

A type whose skeleton carries `must_understand: true` may not be skipped: a reader that does not know it refuses the file or stream rather than rebuild a conversation, or follow a stream, without it. The transcript marks the record types whose loss would change the conversation or the stream's identity (`msg`, `compact`, `reset`, `clear`, `undo`, `resumed_from`, `epoch`); everything else an old reader may pass over. This is SOAP's `mustUnderstand` applied to self-description, and it is the floor under the future "null means not understood" answer (below).

### Lineage

Where OpenAI's own fields do not already say what caused an event (`previous_response_id`, `item_id`), `maic.cause` on the event carries the `sequence_number` of the event that caused it: a response's `response.created` names the `maic.input.added` it answers. A fork's `forked_from` names its parent session, that parent's epoch and the record count it holds. No event carries a per-event hash: the epoch and the skeletons give identity and readability, and anything needing tamper evidence (the audit trail) adds its own chain rather than burdening every event.

### Future: the dumb answer

Not built; recorded here so the shape is fixed. When a client meets a skeleton it does not know, it may answer with a value of that skeleton filled with `null`. A `null` in a field whose schema does not allow `null` means "not understood" (OpenAI's genuinely nullable fields keep their meaning), so the receiver knows the sender did not understand and answers as policy dictates; every type's not-understood answer must be the fail-closed, no-op one, and a type with no safe empty answer is `must_understand` and refused instead. An unknown skeleton on either side is the signal that the client needs updating.

When the **server** is the older side (needs accounts, [roadmap.md](../roadmap.md) item 6): an administrator may approve a skeleton hash on that server ahead of time, scoped and expiring, behind a step-up. A newer client's first-use skeleton line is then accepted when it hashes to an approved value; the server validates the shape, stores and forwards it, and answers `null` where it would have to understand it, but never acts on it. The server refuses an approved skeleton whose name collides with a built-in type, one marked `must_understand`, or one in a security-relevant area (approvals, trust, auth, steering), whatever the admin approved, so a newer client keeps talking to an old server without the upload becoming a way past the server's own checks.

## 16. Build order

Each step is one PR with its tests and is useful on its own. The names, `sequence_number`, the schemas, the ordering machine and the checker arrive with the engine itself (step 5), so no step ever ships an unchecked message.

| # | Step | Roadmap |
| :--- | :--- | :--- |
| 1 | The sandbox hides `$XDG_RUNTIME_DIR` and clears the environment; escape tests for the nvim socket and D-Bus | prerequisite |
| 2 | `always` never covers a remote-origin call; a turn's origin rises when a remote message is delivered into it | prerequisite |
| 3 | OpenAI's description pinned as `protocol/openai/`; `FakeServer`'s chunks and the OpenAI-compatible client validated against it, offline (section 15) | prerequisite |
| 4 | Streaming tool output in core (`on_output` in the sandbox, `on_tool_output`); the TUI shows a running command's output live. **Built 2026-10-01** (section 4, as built) | 1, 4 |
| 5 | `Engine` in core with the session table, ring, dispatcher and schema; maic-server's `Session` and `Events` move into it; protocol tests in-process; `protocol/` with the OpenRPC document, the JSON Schemas and `ordering.json`, the validator, runtime validation and ordering checks at the `guarded` defaults, `maic protocol check`, ctest `protocol_conformance` over recorded, mutated and driven streams. **Built 2026-10-02** (as built, below) | 1, 4 |
| 6 | The TUI as an in-process client: the turn loop, titles, `!cmd` and the engine `:` commands move into the engine. **Built 2026-10-02** (as built, below) | 1 |
| 7 | Steering: `response.steer` and `maic.steer` with its six actions, `response.steer.accepted` and `.failed`, `maic.steer.applied`, `maic.turn.paused`, `maic.response.cancelled`, the `steer` record, the `steering` settings and per-agent overrides, ban triggers, `:steer` and `:steering`, the TUI's keys; the driver gains steers. **Built 2026-10-02** (as built, below) | 1 |
| 8 | `maic --rpc`: JSON lines on stdio, `maic.hello`, versioning, error codes. **Built 2026-10-02** (as built, below) | 1 |
| 9 | maic.nvim's interface mode on `--rpc`: conversation and input buffers, approval and question floats, folds over collapsed items, the `maic` and `maic-input` filetypes, steering keys . **Built 2026-10-02** (as built, below) | 1 |
| - | Event identity (section 17): epochs and durable numbers, self-describing transcripts and recordings, `maic.cause`, `maic protocol hash` and `--blind`. **Built 2026-10-02** (as built, below) | 1 |
| 10 | Harness paths: `harness = "auto"` and `"external"` with L3 agents, `judge` on the entry, the constraints of section 13 | 2 |
| 11 | History: the line-offset index, `attach`, `history`, `expand`, collapsing. **Built 2026-10-03** (as built, below) | 4 |
| 12 | Several sessions in one engine: create, fork, focus, background, park, stop, resume, the index file, `:new`, `:switch`, `:fork`, `:bg`, `:park`, `:stop` and the switcher in the TUI and maic.nvim. **Built 2026-10-03** (as built, below) | 4 |
| 13 | The daemon: `maic daemon`, the socket with the uid check, the TUI and maic.nvim attaching; the tiers `open` and `guarded` with their enrollment (global default, per directory, per agent) and the tier on the entry and in the status strip. **Built 2026-10-03** (as built, below) | 4 |
| 14 | Background tasks: `task` with `background = true`, `maic.task.created` and `maic.task.completed`, answers as labelled notes through the mailbox, `task_result`, parallel approvals | 4 |
| 15 | Side threads: `maic.session.side`, `:btw`, `:aside`, `merge_draft` and `merge` | 3 |
| 16 | maic-server as an adapter inside the daemon; tunnel kind 6 and the LAN `/rpc` streams; the web client on the protocol with lazy history; `maic-server.openapi.yaml` (OpenAPI 3.2) checked by the conformance test | 4, 7 |
| 17 | The adapter, tunnel kinds 1 to 5 and the `maic-server` binary removed | 4, 7 |
| 18 | Accounts: `owner` on sessions and entries, access tokens in `maic.hello`, `maic_not_owner`; step-up for remote steering; the `airtight` tier with the conformance stamp and recorded streams | 6 |
| 19 | The relay's tier presets and, when wanted, the trusted node role ([protocol-security.md](protocol-security.md)) | 7 |

**As built (step 5, 2026-10-02).** `protocol/` holds `maic.openrpc.json` (OpenRPC 1.3.2: the 18 methods step 5 answers and the three notifications, each method marked `x-maic-remote`, `instructions` marked local only), `schemas/maic.schema.json` and `schemas/event.schema.json` (JSON Schema 2020-12; the union is the 30 event types the engine sends, OpenAI's by `$ref` into `openai/subset.json`, which gained the roots `ConversationResource`, `ConversationItem` and `CreateResponse`) and `ordering.json` (the session, response, item, content part, action, approval and question machines as data, and the ids and texts of the rules that are not one machine: sequence numbers, turns, schemas, requests answered). CMake builds the files into `maic_core` (`cmake/embed_protocol.cmake`); `maic::protocol` bundles them into one document, validates with MAIC's validator (which gained `schema_undeclared` for rule 3: a field beside OpenAI's outside `maic`), and checks order with `StreamChecker` and recorded exchanges with `Conformance`. `Engine` (`core/include/maic/engine.hpp`) owns the sessions, their Agents and transcripts, a ring per session (10,000 events or 8 MiB), approvals and questions as sets, the index, and the dispatcher; every event it sends passes its schema and the machines at `guarded` (a fault goes to `<state>/engine/protocol.log` and, once per kind, to local clients as `maic.engine {notice, level}`). Connections are queues the engine never waits on: past 2 MiB tool output becomes skip counts and text deltas merge (`maic.merged_from`), past 8 MiB the connection closes with `maic_too_slow`, and a remote connection gets 64 KiB/s of tool output per session. maic-server is an adapter: each HTTP request is one remote engine connection named by its token, and its server-sent events are the engine's events. Departures, each to be closed by the step named:

* **`response.create` on a busy lane is delivered into the running response** at its next boundary (the mailbox, as typing mid-turn does), `maic.input.added` with `queued: true`; the result is the running response with `maic.queued`. Step 7 splits this into `response.steer` and FIFO queueing (closed: step 7, as built). maic-server's old mid-turn message also aborted the model call in progress (`deliver_now`); it now waits for the boundary.
* **Item ids stay `~<n>`** on `response.output_item.done`; the `<file id>#<line>` refs come with step 11's line index. `maic.judged_by` and `maic.status` are not set yet (no review events before step 10; `AgentEvents` does not say a call was denied).
* **No argument or command deltas**: the agent gets each tool call whole, so a call is `response.output_item.added`, `response.function_call_arguments.done` (or `response.shell_call_command.added` and `.done`), `response.output_item.done`. The call's output item opens at its first output or its result.
* **A response's `tools` is `[]`**, not the agent's tool list on every response event; `previous_response_id` is null (one response per turn until steering; closed by step 7); `instructions`, `tool_choice`, `max_tool_calls`, `truncation` and `include` are accepted and not applied yet.
* **A subagent's calls** show on the parent's stream as `maic.notice` lines, its output as `maic.tool.output.delta` with `call` under the task's output item, and its approvals with `thread {agent}` and no `call_id` (its own stream and session id come with step 14).
* **Every loaded session is `live`**: focus, background and park come with step 12; activity changes are `maic.session.state` events too. The engine writes the index file when given one and lists what it held as parked after a restart; `maic.session.resume` loads one. maic-server keeps no index file (the daemon's, step 13).
* **Additions to the shapes above**: `call_id` on `maic.approval.requested` and `.answered` (the action machine's key), `output_index` and `call` on `maic.tool.output.delta`, `{load, sequence_number, activity, replay_from}` from `maic.session.subscribe` (not `{}`), `questions` beside `pending` in a snapshot, `remote_model`, `created`, `turns`, `response` and (local only) `transcript` on an entry, `maic.sequence_number` and `maic.queued` on `response.create`'s result, and the error code `maic_hello_required`.
* **Capabilities** are exchanged and echoed but filter nothing yet: a filtered event would read as a gap in `sequence_number` (section 10), so filtering waits for a way to say "skipped by capability". (Built since: see filtering, as built, below.)
* **Tiers**: `open` and `guarded`; `airtight` refuses to start until the conformance stamp exists (step 18).
* **History**: `maic.session.attach` answers no items (`more_before: false`) until step 11; maic-server's `GET /api/sessions/{id}` folds `entries` from the transcript file. Its `sequence_number` (was `seq`) is the last event's number, not the next; `?after=N` (from N on) is read as `starting_after=N-1`; the transcript path is no longer shown to a remote client; a remote `always` is refused (the web client's Always button is gone) and loosening to `auto` from a remote client answers `maic_step_up_required`.
* **The checker** runs over the test suite's own recorded, mutated and driven streams (`protocol_conformance`, `protocol_check`); the provider-side and Python `jsonschema` cross-checks of section 15 stay as step 3 left them, and the machines for reviews and steering join with steps 10 and 7.

**As built (step 6, 2026-10-02).** The TUI and `maic -p` each run an in-process `Engine` and hold one local connection to it (`via: "in-process"`). Each opens its session with `Engine::open_local`, a C++ call only an in-process host has: the host's setup callback configures the session's Agent from the command line (`configure_agent`, the mode, the nvim host, a restored history for `-r` or `--fork-at`, the transcript wherever `--no-record` puts it), because creating, forking and resuming over the protocol come with step 12. Then the client attaches (`maic.session.attach`), the TUI also follows the index (`maic.index.subscribe`), and from there everything goes through `Engine::call`: `response.create` for a message, `cancelResponse` for Ctrl-C, `maic.approval.answer`, `maic.question.reply`, and these, new with this step:

* **`maic.session.command {session, line}`** runs the `:` commands that act on a session (`core/src/session_commands.cpp`): `:mode`, `:harness`, `:model`, `:models`, `:think`, `:undo`, `:export`, `:rename`, `:budget`, `:compact`, `:clear`, `:trip`, `:status`, `:todo`, `:tools`, `:init`, `:cd`, `:ban`, `:sampling`, `:image`, `:forbid`, `:allow`, `:rule`, `:ctx`, `:ctx2`, `:prefill` (`:prefix`), `:system`, `:instructions`, `:session`, `:lua`, `:luafile` and `:trust imports --approve`, under the spellings the TUI has always taken. It answers `{ok, lines: [{text, level}], ask?, send?}`. A command that must ask first (auto under a dumb harness, each untrusted directory on `:cd`, an import from outside, `:init`'s move) answers `ask {id, title, lines, keys}`, and the answer is `maic.session.command {session, ask, key}`, from a local client only; `send` is text for the client to send as the person's next message (`:init`'s request for a MAIC.md). The remote allow-list of section 7 is enforced per command (`maic_forbidden_remote`). `maic.session.set` takes `confirm` (local only) and answers `maic_confirm_required` for auto under a dumb harness without it.
* **Titles**: after the first turn small_model titles the session, between the response's end and idle, as `maic.session.title {text, source: "auto"}`; `:rename` and `updateConversation` give `source: "rename"` with `by`. Only sessions an in-process host opened are titled (`LocalSession.titles`; `maic -p` turns it off), so maic-server's sessions stay untitled as before.
* **`!cmd`** is `maic.session.shell {session, command}` (local only): the user's shell, unsandboxed, in the session's workspace; its output streams as `maic.tool.output.delta` with its own `item_id` (`sh<n>`) and no `output_index`, since it is no response's item, so the item machine does not take it; it answers `{exit_code, item_id}` when the command ends, and what it printed reaches the model as context (into the running turn when there is one). `{session, interrupt: true}` stops it.
* **`maic.approval.proposed {session, approval}`** (local only) answers a waiting write's content, for the TUI's diff in nvim (the approval event carries only `proposed_size`); **`response.create` with `maic.now`** delivers into the running response at once, its model call abandoned (`:w now`), and with no input delivers what is already queued.

The turn loop was the engine's already; a cancel between two responses of a turn now stops it there (`cancelResponse` answers for the turn while no response is open) and keeps what was queued for the next message. A failed response's error is `failure_text`'s, the hints about a stopped service included. The TUI renders only from events: `maic.input.added` is the person's line (with `item.maic.images`, the pictures attached to it), the text and reasoning deltas stream into the reply, a call item's `added` is the tool line and nvim's `MaicToolCall`, output deltas feed the live tail, the output item's `done` replaces it with the result (and the kept output behind the fold), `maic.notice`, approvals and questions open and close their modals (`MaicApproval` fires on both), `maic.usage.updated`, `maic.session.settings` and the index entry drive the status strip, `maic.session.title` says the title, and each response's terminal event gives the footer (`▣ model · time · tool calls`) and `MaicTurnStart` / `MaicTurnEnd`. `maic -p` prints the same events in its old shapes. What stays in the TUI: the view, the editor, themes, registers, `:stash`, `:copy`, `:h`, `:e`, `:set`, `:q`, Lua mode, the nvim host, and the machine's commands (`:up`, `:down`, `:gpu`, `:trust` and `:untrust`, `:settings`, `:lazylock`, `:path`, `:open`, `:artifacts`, `:unlock`), which act on no session ([limits.md](../limits.md#the-engine-protocol)).

Additions to the shapes: `maic.notice` takes `kind` (`tool_call` with `tool` and `path`, `tool_result` with `ok` and `full_output`) for a subagent's calls and results until its own stream (step 14); `maic.usage.updated` also comes mid-response, after each model call that ends in tool calls, and carries `normalized`; `maic.session.settings` names `workspace` after `:cd`; an entry has `think` and `harness`; `CommandResult` in `maic.schema.json`. The engine's guarded checks run on every event of the TUI's and `maic -p`'s streams; `MAIC_PROTOCOL_RECORD=DIR` makes either record its connection (`tui-<pid>.jsonl`, `headless-<pid>.jsonl`) and check it as it goes (`protocol::Recorder`), and the pty suite runs every case that way and fails it on a violation or a line in `protocol.log` ([testing.md](../testing.md#the-pty-harness)). Capabilities still filter nothing.

**As built (step 7, 2026-10-02).** A turn is now one or more responses. Ids are `<session>.r<k>` from a counter per session (it starts at the transcript's turn count when a session is resumed), and each response after a turn's first names the one before in `previous_response_id`. `response.create` on a busy lane is OpenAI's FIFO: the input is announced (`maic.input.added`, `queued: true`) and the answer is a response with `status: "queued"` and the turn number it will run as; it runs when the turns before it end, a cancel included, and `cancelResponse` on it takes it off the lane (the turns behind it move up a number). A turn's first response opens at once, under the session's lock, so a client never holds a response id the engine does not know. Steering:

* **`response.steer {previous_response_id, input}`** answers `{steer {id, previous_response_id}}` and emits `response.steer.accepted` and the input as `maic.input.added`; the text goes to the agent's mailbox. At the next boundary (the top of the agent's next step, `AgentEvents::on_delivered`) the response ends `response.incomplete` with `incomplete_details.reason: "steered"` and `maic {final: false, ended_by}`, and a successor opens: its `response.created` is the commit point. Input that comes after the turn's last model call ends the response `response.completed` with `final: false` and continues in an automatic successor, OpenAI's "normal completion can also be followed by an automatic successor". The TUI and the web client send their mid-turn messages this way; `response.create` with `maic.now` still delivers at once (`:w now`) and now also says `response.steer.accepted`.
* **`maic.steer {session, response_id, action, note?, trim?, at?, tool?, step_up?}`** checks the action against `steering.actions` and the client side's list (`maic_steer_disabled`) and the target (`response_not_found`, `response_already_completed`, `response_not_active` for a queued response or a turn that a keep or halt is ending; `too_many_pending_steers` past 16 waiting, or for a second stop while one is being applied). `further`, and `steer` or `drop` with `tool: "wait"` while a tool runs, go to the mailbox and wait (`waits_for`: the waiting approval or question, the running call, or `"boundary"`). The others stop now: `maic.steer.applied` goes out at once with what will be trimmed and withdrawn and the call stopped, reply deltas stop showing, the cancel flag rises, and the worker finishes the steer in `AgentEvents::stopped`, which returns a `Redirect` (what of the partial reply stays in the history, then `Continue`, `Pause`, `Keep`, `Halt` or `End`): steer and drop post their text to the mailbox and continue through the same boundary; interrupt ends the response `maic.response.cancelled` (`final: false`), emits `maic.turn.paused {turn, steer, response_id}` and sets the session waiting (`kind: "steer"`); keep ends it `response.completed` (`ended_by: "keep"`); halt emits `error {code: "maic_halted"}` and ends it `maic.response.cancelled` (`ended_by: "halt"`), closing the reply and a stopped call's output `incomplete` with `maic.status: "discarded"`. Keep and halt return the steers still waiting as `response.steer.failed` (`response_not_active`, with their input) and take them out of the mailbox. A withdrawn approval answers `choice: "withdrawn"` and its call's result is "not run: the user redirected"; a withdrawn question answers `withdrawn: true`.
* **A paused turn** resumes with a successor of its last response when `response.steer`, `response.create`, or a steer, drop or further arrives; keep, halt or `cancelResponse` (by the last response's id) end it by opening a successor and ending it at once, so the turn closes on a final event as every turn does. The entry's `response` is the paused turn's last.
* **The model's text**: the section's sentences; a `steer` with no note says "Continue from where you stopped." (Ctrl-Q's resume). A note from a remote client raises the turn's origin like a remote message.
* **Bans**: an entry written as a table names its steer (`{ "pattern", steer = "drop", note = "..." }`; JSON `{"text", "steer", "note"}`), kept beside the entry through the layers (`Bans::string_steers`, `pattern_steers`); `BanFilter::hit_steer()` says which entry fired, and such an entry cuts in replace mode too. The agent hands the hit to `stopped`, the engine applies it with `trigger: "ban"`, `by {client: "engine", name: "bans"}` and `ban {list, index}`; past `retries` the action becomes halt. `steering.ban_actions` is checked when settings load (never further).
* **Settings**: `steering` as in section 11 (`SteeringSettings`, `read_steering`); a project layer only removes actions, `clients` is the global file's alone, and `from` remembers which file set each key for `:steering`. `agents.NAME.steering` is read and checked (it only narrows) and narrows nothing yet, since no session runs as an agent until step 14.
* **The transcript** gets a `steer` record (`steer`, `action`, `by`, `trigger`, `note`, `response`, `ban`, `trimmed`, `withdrawn`, and for a halt the discarded byte count only); `load_session` shows it as a notice line.
* **The TUI**: Ctrl-S sends `interrupt` while a turn runs (flow control is turned off with ISIG's other changes), the pause menu takes Ctrl-Q (resume), `s`, `d`, `f` (the input, if any, is their note), `k`, `h`, and Esc to type a message that resumes; `:steer ACTION [NOTE]` (a `maic.session.command` the engine answers itself) and `:steering`; a footer only at a turn's end; a trimmed reply is redrawn from `response.output_text.done`. `maic -p` ends a turn a ban's interrupt paused, having no one to resume it.
* **Protocol files**: `response.steer` and `maic.steer` in the OpenRPC document; `response.incomplete`, `error`, `response.steer.accepted`, `response.steer.failed`, `maic.steer.applied` and `maic.turn.paused` in the event union (37 types); the subset gained the roots `ResponseSteerEvent`, `ResponseSteerAcceptedEvent` and `ResponseSteerFailedEvent` (357 schemas), and the OpenAI-only view takes the two steering events as OpenAI's; `ordering.json` gained the `steer` machine (accepted, applied, failed, by the steer's id) and the rules `turn.paused` and `steer.in_turn`. The design's `maic.steer` example is checked like the others. `protocol_conformance` has a steering section (the lane, drop, interrupt and resume, keep, halt with a failed steer, further and a withdrawn approval, `maic_steer_disabled`), ban steers through an in-process host, mutations for the new rules, and a driver of fourteen turns that steers at random.

Not yet: `step_up` is accepted and ignored (step 18); a review in flight is stopped by the cancel without `maic.review.cancelled` (step 10); maic.nvim's steering keys come with its interface mode (step 9). The limits are in [limits.md](../limits.md#the-engine-protocol).

**As built (step 8, 2026-10-02).** `maic --rpc` (`cli/src/rpc.cpp`) runs one engine with its parent as the one local connection (`via: "stdio"`), JSON-RPC 2.0 one message per line on stdin and stdout. It starts as the TUI does, without its questions: trust is settled and never asked, the settings are the TUI's with the same flags (`--model`, `--mode`, `--harness`, ...), the nvim host is found from `$NVIM` unless bare, and a due audit holds first. It opens no session itself; the client creates or resumes one (`createConversation`, `maic.session.resume`), and each is set up as the TUI's own (`EngineOptions::setup`: `configure_agent`, confinement under `tripwire = "isolated"`, the instruction files, the nvim host). `-p`, `-i`, `-c`, `-r`, `--context`, `--image` and a prompt are refused beside `--rpc`.

* **The pipes are the protocol's alone.** It keeps its own copies of fd 0 and 1, then points fd 0 at `/dev/null` and fd 1 at stderr, so a stray print, a settings file's Lua or a child can neither read a request nor break the stream. Diagnostics go to stderr.
* **Requests run concurrently**, each on its own thread, at most 64 at once (the 65th line is read when one ends), so a long one (`maic.session.shell` answers when its command ends; a `:` command may restart a server) never holds up `cancelResponse` or a stop. Requests are answered in any order, as section 1 allows; a client that needs one done before the next waits for its answer. `maic.hello` alone runs in line, so whatever follows it finds the hello done. `maic.session.attach` and `maic.session.subscribe` hold the writer from their call to their answer, so the answer goes out before the replay it queued: the stream is joined at the answer, by the checker and by any client.
* **Faults**: a line that is not JSON is answered `-32700` with a null id and the connection stays; a line over 1 MiB is answered `maic_too_large` (null id: it was never read as a request) and the connection closes, exit 1; `maic_too_slow` from the engine also ends it, exit 1. Stdin closing, SIGINT, SIGTERM or SIGHUP end it as quitting the TUI does (running turns interrupted, sessions parked, the last events sent), exit 0.
* **A local client's name** is the hello's `client.name` (a remote client's is still its token's or pairing's, section 2), so `by` reads `maic.nvim` rather than the transport.
* **Transcripts** follow the TUI's rule: `record = false` writes them to the runtime directory. This holds for every engine's `createConversation`, maic-server's included.
* **The kind** of an `--rpc` transcript is `rpc`. `tripwire = "isolated"` scopes the lock to `$XDG_RUNTIME_DIR/maic/sessions/rpc-<pid>.tripped`, which `maic unlock` lists.
* **`MAIC_PROTOCOL_RECORD=DIR`** records the connection as `rpc-<pid>.jsonl`, checked as it goes.
* **Not built here**: capability filtering stayed as step 5 left it, awaiting a decision on how a filtered stream says what it skipped (built next: filtering, as built, below). Batch requests (a JSON array on one line) are answered `-32600`, as before.
* **Tests**: `cli_smoke.py` drives `maic --rpc` against the fake provider: the hello gate, a session created and replayed from 0, a turn, a `run_shell` approval answered over the pipe, `maic.engine.status` answered while `!sleep 2` runs, a resume with `starting_after`, `-32601`, the exit at EOF with the transcript kept, then `-32700`, `maic_unsupported_protocol`, `maic_too_large` and SIGTERM. The client's recording and the engine's both pass `maic protocol check`.

**As built (step 9, 2026-10-02).** maic.nvim's interface mode (`maic.nvim/lua/maic/ui.lua`) is the first client of `maic --rpc` outside the tests. `:Maic` opens it by default (`ui = "nvim"` in the plugin's defaults; `ui = "terminal"`, `:MaicTerminal`, or an argument only the TUI takes runs the TUI in a terminal as before), and `maic --ui nvim` or `ui = "nvim"` in MAIC's settings starts nvim with it from the shell. It says hello as `maic.nvim` with `tool_output` and `autocmds` and `view.collapse_over = 0`, creates or resumes one session and attaches. Rendering follows the TUI's step 6 mapping of events to lines, into a conversation buffer (filetype `maic`, markdown through treesitter) that only the plugin writes; tool output and reasoning are manual folds, closed once done when long enough; approvals, questions, the engine commands' `ask` and the pause menu are floats with one-key answers; the input (filetype `maic-input`) sends with `<CR>` or `<M-CR>`, `/cmd` as `maic.session.command`, `!cmd` as `maic.session.shell`, a message mid-turn as `response.steer`. Steering: `<C-c>` `cancelResponse` (or the shell's interrupt), `<C-s>` `maic.steer` `interrupt`, `<C-q>` and the menu the other actions, `:MaicSteer ACTION [NOTE]`; all are buffer keymaps under the never-overwrite rule and in the keymap check. The plugin fires the `Maic*` User autocmds and runs `:checktime` itself.

* **Call ids are not unique across a session** (a fake model, and some servers, reuse `call_1`): the plugin keys a tool's lines by the newest call with that id, and an output by its own item id.
* **`autocmds`** is sent as the design says, though the engine has nothing to stop: under `--rpc` nothing fires the autocmds over the host connection.
* **History**: the plugin renders `attach`'s items (messages, collapsed outputs folded), but the engine returns none until step 11, so a resumed session starts empty ([limits.md](../limits.md#nvim)).
* **Not built here**: the machine's `:` commands are still the TUI's only ([limits.md](../limits.md#the-engine-protocol)).
* **Tests**: `maic.nvim/tests/ui_test.lua`, run by `tests/cli_smoke.py` against the real `maic --rpc` and the fake (the engine's two recordings pass `maic protocol check`), and `maic --ui nvim` against a stand-in nvim; [nvim.md](../nvim.md#tests).

**As built (filtering, 2026-10-02).** `maic.hello` takes `exclude`, and its answer lists every type the connection is not sent: the excluded deltas and the types of a capability it did not announce (`ordering.json`'s `filter`: `filterable`, and `capabilities` with `tool_output` covering `response.shell_call_output_content.delta` and `maic.tool.output.delta`). The engine's `Client` keeps the set and, per session, the first number it filtered since the last event it sent; the next event sent carries it as `maic.filtered_from` (declared on the event envelope), and a delta merged into a queued one (section 4) clears it, since the merge's range covers the filtered numbers. Subscribing or unsubscribing forgets it, so a replay starts clean. The TUI and maic-server's adapter announce `tool_output`; `maic -p` does not, and is spared tool output it never printed.

* **The checker** learns each connection's filter from its hello answer (`Conformance`, `StreamChecker::filter`): an excluded type sent is `seq.filtered`, a `maic.filtered_from` on a connection that excludes nothing or not before the event's own first number is `seq.filtered`, and `seq.next` takes `maic.filtered_from` first, then `maic.merged_from`, then the number.
* **Tests**: `protocol_conformance` has a filtering section (the refusal of a lifecycle type, an unknown type ignored, two connections on one turn with a shell call where every filtered run is accounted for by the full connection's numbers, and a third connection that resumes unfiltered after a number the filtered one holds) and three mutations (a marker removed is `seq.next`; an excluded type sent and a marker on an unfiltered connection are `seq.filtered`). `cli_smoke.py` runs a `maic --rpc` client that excludes text deltas and reads the reply from `response.output_text.done`; both recordings pass `maic protocol check`.
* **Numbers and their identity** were per load here (`load` named the number space); section 17 replaced that with the session's epoch and numbers that survive a reload. The ring is trimmed from the front, never renumbered.

Steps 1 to 3 are small and stand alone; 4 helps the TUI the day it lands; 5 to 9 deliver item 1, with steering in from the start; 10 is item 2's choice of judge; 11 to 14 and 16 deliver item 4; 15 is item 3 and needs 12; 18 and 19 complete the tiers.

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
13. **Does Ctrl-C stay the stop?** Recommendation: yes. `cancelResponse` ends the turn as today, and the `interrupt` steer (pause and wait) gets a key of its own: a paused background session holds its slot with nobody looking at it.
14. **A paused turn's timeout.** Recommendation: none. A pause is shown as waiting, like an approval, and park or stop end it.
15. **`drop`'s default trim.** Recommendation: `paragraph`. `sentence` leaves most of a tangent in place and `all` throws away the good part before it; a client that lets you mark the spot sends `at`.
16. **The halt message, and what the model sees.** Recommendation: the text in section 11, and the model is told that something was discarded but not what, so a halted tangent is not re-read into the next turn.
17. **Remote steering under `guarded`.** Recommendation: all six actions. A note is the same as a message sent mid-turn and raises the turn's origin to remote; `airtight` adds the step-up check.
18. **Which JSON Schema validator?** Recommendation: MAIC's own, grown from the script tool checker to the keyword subset the schemas use, with a test pinning that subset; no new dependency. jsoncons (in vcpkg, full 2020-12) is the fallback if the subset grows past what is reasonable to keep by hand.
19. **Where the schema files live.** Recommendation: a top-level `protocol/`, installed with MAIC, since C++, Lua, the web client and the tests all read it.
20. **The `harness` values.** Recommendation: `dumb`, `smart`, `external` and `auto` (the default; superseded: the built-in default is `dumb`, see Decisions). `external`, not `remote`, because remote already means a remote client's origin.
21. **A ban's steer after `retries`.** Recommendation: escalate to `halt`; a model that keeps reaching a topic it was steered off three times is not going to stop on the fourth.
22. **The pinned OpenAI description: the whole file or a subset?** Recommendation: the subset MAIC references, extracted by a script named in its README, so a bump is a reviewable diff rather than 4.6 MB.
23. **`sequence_number` across a session.** OpenAI's sequence numbers belong to a response's stream; MAIC's stream is a session's, carrying many responses and MAIC's own events between them. Recommendation: one number space per session load, so each response's events are a contiguous run of it and `starting_after` means the same on `getResponse` and on `maic.session.subscribe`; OpenAI's schema says only "the sequence number for this event", and the OpenAI-only view in the checker holds MAIC to it.
24. **Operation names as methods.** Recommendation: the `operationId` exactly (`getResponse`, `Compactconversation`, `Getinputtokencounts`), even where OpenAI's spelling is uneven, since it is the one machine name the spec gives an operation and the schema test can check it; the WebSocket's client events keep their `type` (`response.create`, `response.steer`).

25. **How `interrupt` and `halt` look to an OpenAI-only client.** Recommendation: as a cancelled response (`getResponse` answers `status: "cancelled"`; `maic.response.cancelled` closes the stream for MAIC's clients), not as `incomplete` with `steered`, because OpenAI's `steered` promises an automatic successor and a pause or a halt has none.

Open questions about the tiers and the relay roles are in [protocol-security.md](protocol-security.md#open-questions-for-micaiah).

## Addendum: review streams and the judge of each action (2026-10-01)

Micaiah's two streaming paths, specified in [harness-authority.md](../harness-authority.md), are part of this protocol:
* `review.started` {session, item, action, reviewer_model} when MAIC sends a proposed action to its reviewer; `review.delta` {text} only if the reviewer streams its reasoning and the client subscribed to it; `review.verdict` {verdict: allow|ask|deny, reason, model, tokens}. These belong to the local round trip and appear between the reply's tool-call event and the action's output events.
* Every `tool.started` and `tool.finished` carries `judged_by`: `"maic"` (MAIC's harness reviewed it), `"rules"` (only the fixed rules applied, as in a dumb harness), or the external agent's name (`"claude-code"`) when that agent's own harness approved it on the remote path, where no `review.*` events occur.

## Decisions (Micaiah, 2026-10-02)

The open questions above are settled as recommended, with these refinements in her words or close to them:
* **1, encoding**: JSON lines for now. Keep room to add a raw-bytes fast path later, offered only on the `open` tier for a node she controls, where speed matters more than checking.
* **2, remote `always`**: no, as built in v0.3.1. The session index is what lets a remote client pick up a session that is not active, which covers the use case.
* **6, daemon auto-start**: not at first. When it comes, MAIC says so when the setup is a client only (no local daemon to start), and the repository ships baseline systemd user unit files that MAIC hands over or installs when asked; Windows gets its own equivalent.
* **7, full command output**: no spool; expanding serves the recorded result (24 KiB head and 8 KiB tail), and the middle of a huge log is gone once it scrolls past, as in a terminal. **Revised (2026-10-01): the full output is kept, clearly marked as display only.** When a command's output outgrows the model's cap, its whole stream (up to `full_output_max_mb`, 64 by default) is written beside the session as it arrives, `<id>.d/<call>.out`, with a chunk index `<call>.idx` (stream, offset, length, milliseconds) so it can be replayed as it ran; the `tool` record gains `full_output {path, bytes, sha256, delivered_to_model: false}`. Every reader labels it "full output, display only: the model saw the capped result", so it can be recovered and reflowed into a fork that gives the model the output it was reaching for, and is never mistaken for what the model received ([sessions.md](../sessions.md#full-output)). `maic.item.expand` keeps serving the recorded result; the kept output is a separate, labelled read.
* **9, remote mode changes**: tighten freely; loosen up to `edit` freely; loosen beyond `edit` (to `auto`) only after the step-up check.
* **13, keys**: Ctrl-C ends the turn (`cancelResponse`); the `interrupt` steer action, which pauses the turn and keeps it open with its partial reply, gets Ctrl-S (the terminal's own "stop output" key; MAIC turns flow control off so the terminal does not swallow it), and while paused Ctrl-Q resumes and a small menu offers steer, drop, further, keep and halt. In maic.nvim these are defaults in the defaults table, checked by the keymap check.
* **20, harness values**: `dumb`, `smart`, `external`, `auto`, chosen by a setting (global default, per agent, per session). The smart harness costs real performance on a local card, so with a node she controls doing the work, the local client runs `external`: the node's harness judges, and the client itself operates as dumb.
* **Defaults (2026-10-02)**: the built-in defaults are `harness = "dumb"` and `mode = "auto"`, so no flag is needed for either: the smart harness is too slow on a local card, and likely on anyone's own models. `auto` is therefore not the default harness; step 10 builds it as a value you choose. Auto mode from the settings starts only in a workspace whose project directories are all trusted fully, and at least one; elsewhere a session starts in manual and `:mode auto` (or `--mode auto`) still turns it on. A remote client's `createConversation` that asks for auto is refused with `maic_step_up_required`, the same check as `maic.session.set` (decision 9): creating a session in auto counts as loosening to it. A remote resume carries no mode; a transcript that was in auto resumes in manual for a remote client.
* **22, OpenAI's description**: keep the full pinned file in the repository for reference, and extract the subset the conformance checks use from it by script.
* All others (3 to 5, 8, 10 to 12, 14 to 19, 21, 23 to 25): as recommended.
* **Filtering** (after step 8): a client declares its filter in `maic.hello`, the checker and the gap rule honour it, resume stays session-global, and the first event after filtered ones carries `maic.filtered_from`, shaped like `maic.merged_from` (section 9). The other ways were set aside: numbering each connection's stream apart (resume and hand-off between devices get messy) and allowing gaps everywhere (real loss goes unseen). Integers stay the order and gap test (OpenAI's `sequence_number`); an id per stream incarnation was considered and is already there as `load` (section 10).


**As built (event identity, 2026-10-02).** Section 17. `maic/skeleton.hpp` gives `skeleton_of`, `canonical_json` (RFC 8785: UTF-16-ordered keys, ECMAScript numbers, minimal escapes), and `sha256_digest` in `sha256:` form; `protocol::protocol_hash()` is the digest of `protocol/` with prose (`description`, `title`, `summary`, `text`, `$comment`) dropped, and `maic protocol hash` prints it. The engine mints an epoch per fresh history (`stream_start` reads the transcript's `epoch` and `stream` records: a clean `closed` `stream` continues the epoch and its numbers, a tool-declared epoch is taken as it is, anything else is a new epoch naming its predecessor or fork); `maic.session.state` carries `epoch` and `protocol`, and a fresh epoch also `previous_epoch` or `forked_from`. `open_session` writes the `epoch` and opening `stream` records, `shutdown` the closing one. `maic.session.subscribe` and the `Snapshot` take `epoch`, not `load`; another epoch is `maic_resync`. `SessionLog::write` prepends a `skeleton` record (must_understand for `msg`, `compact`, `reset`, `clear`, `undo`, `resumed_from`, `epoch`) the first time it writes each type and shape; `walk_records` reads an unknown type by skipping it and refuses the file only when the skeleton says must_understand (`known_record_type`, `record_must_understand`). `Recorder` writes a `header` line and a `skeleton` line before the first event of each shape; `Conformance` and `StreamChecker` gained `blind`, reached by `maic protocol check --blind`, which checks only numbers, `stream_id` and the file's own skeletons, and otherwise read an unknown event type by its declared skeleton (`skeleton.hash`, `skeleton.undeclared`, `skeleton.must_understand`). `maic.cause` is on the event envelope and on `response.created` (the `maic.input.added` a turn answers). The engine's per-start id in the hello is now `engine.instance` (it was `epoch`). trans-fairy is unchanged by this step.

**As built (step 11, 2026-10-03).** Section 5. `TranscriptIndex` (`maic/session.hpp`) holds a session's displayable records (`user`, `assistant`, `tool`, `context`, `compact`, `title`, `undo`, `workspace`, `steer`) with the file and byte offset of each, its parents' first through `resumed_from`, ids `<file id>#<line>` counted as `walk_records` counts (skeleton lines are not records), and the entries where exchanges start; the engine's `Session` keeps one and refreshes it before every history request, reading only what was appended since and starting over when the transcript moved. Records become OpenAI's items: `user` and `assistant` messages, `tool` as `shell_call_output` (for `run_shell`, OpenAI's shell output) or `function_call_output` with `maic {summary, ok, full_output}`, its own id standing in for the call id a record does not keep; the others as `maic.notice` items with `kind` (`HistoryNotice`), since OpenAI has no item for them. `attach` answers the last `exchanges` exchanges (3 by default), `more_before`, and `inflight`: the open response's items still in progress (the reply so far, a running tool's output item with its last 8 KiB and its call), rebuilt from the ring. `listConversationItems` (OpenAI's, with `maic.exchanges`) pages from `after` newest first or, with `order: "asc"`, oldest first by `limit`; `getConversationItem` answers one item; `maic.item.expand` serves up to 256 KiB of an item's text from `offset`, cut on characters. Collapsing follows the hello's `view.collapse_over` (0 never collapses; 64 KiB local and 2 KiB remote by default), and a page keeps inside 768 KiB, collapsing a tool result or an attached file before it leaves a turn out. maic.nvim draws `attach`'s items and `inflight`, keeps a line at the top while older history is left, and loads the ten exchanges before it when the cursor reaches that line or on `:MaicOlder`. The hello's capabilities gain `collapse`. Not built: the `ref` a live item's `response.output_item.done` should give ([limits.md](../limits.md#the-engine-protocol)); maic-server's history (step 16); the TUI's resume still draws from `load_session`, since a `clear` hides earlier turns there and the protocol's history does not.

**As built (step 12, 2026-10-03).** Section 2. One engine holds several sessions, each with its own worker, so their turns run at once (a single-slot local server still serves one call at a time).

* **Focus.** A client has at most one session in focus (`Client::focus`); a session's `focused_by` makes it `live`, and with no client it is `background` and keeps working. `createConversation`, `maic.session.fork` and `maic.session.resume` take `focus` (default true) and `leave`; `maic.session.focus` is `:switch`, `maic.session.background` is `:bg`; a client that disconnects lets go of its focus. Each change is a `maic.session.state` on the session's stream and an index entry. A turn that ends with no client in focus sets `unseen`, cleared by the next focus.
* **Leaving.** `leave` is checked before anything changes. Without it the session left goes to the background. `default` parks an idle one, sends a busy one to the background (a response, a paused turn, a queued turn or a `!cmd`; the worker titling a session after its turn is not busy) and touches neither when another client has it in focus. An explicit `park` or `stop` interrupts a running turn: the client asked first.
* **Park and stop** (`unload`): `maic_busy` while busy unless `interrupt`; with it the turn is interrupted, waiting approvals answered no and a `!cmd` ended, and the worker joined. The stream ends with `parked` or `stopped`, the transcript gets a closed `stream` record (as at shutdown, so the epoch and its numbers go on at the next load), subscribers and focus are dropped and the session leaves memory. A parked entry keeps the turns that waited on the lane and what was in the mailbox as `queued_inputs` (with each one's origin), in the index file but never sent to a client; `queued` counts them, and `maic.session.resume` runs them in order. `stopped` takes the entry off the index (`maic.index` `removed`); a parked entry can be stopped without loading it. A parked session is resumed from the transcript its entry names, so a `--no-record` one in the runtime directory comes back too.
* **Forks** (`maic.session.fork`): a `SessionLog::Fork` pointer at the parent's first `at` records (default all), the parent's model, mode and session settings, the history restored from the pointer, turns counted from it; it starts as any session does (auto held where the workspace is not trusted, a remote client's workspace inside `server.workspaces`), and its first event names the parent's epoch (`forked_from`).
* **Ordering.** The session machine gained `restarts_on: "/epoch"`: a load's first event carries the stream's epoch, so a session parked and loaded again within one recording starts a new instance; a mutation that strips the epoch from that event is caught (`machine`).
* **The TUI** sets `EngineOptions::setup` as `maic --rpc` does, so a session it opens later is set up like the first, and follows the index (`maic.index.get`, then the notifications). `:new [DIR]`, `:switch [ID|TITLE]`, `:fork`, `:bg`, `:park [ID]`, `:stop [ID]`, with `--bg`, `--park` or `--stop`, and `session_leave` in settings (`default`, `ask`, `bg`, `park`, `stop`). The switcher lists a new session first, then the other sessions (loaded ones newest first, then parked) with what each is doing, its model and its workspace; on this session `:bg`, `:park` and `:stop` open it to pick where to go, and parking or stopping a working session is asked first. Switching unsubscribes from the session left, attaches the new one (its last ten exchanges, what is in flight, a waiting approval or question), moves the process to its workspace as `:cd` would, and drops events still queued for the session left. The top strip counts the other sessions, waiting and finished ones; `:q` with another still working says so once.
* **maic.nvim** keeps one engine per tab (`conn`) and a view per session (`ui`), each with a conversation buffer of its own under the tab's one input; events go to the view of their `stream_id`. `:MaicNew`, `:MaicSwitch`, `:MaicFork`, `:MaicBg`, `:MaicPark`, `:MaicStop` as in the TUI, the switcher through `vim.ui.select`, `session_leave` in setup. A background session's view stays subscribed and keeps writing its buffer; its approvals and questions wait for a switch to it, with one notification naming it. A parked or stopped session's view and buffer go once no window shows them. The winbar counts the other sessions.
* **Tests.** `protocol_conformance` has a sessions section: two sessions with overlapping turns (the second completes while the first's reply is held mid-stream), `unseen`, focus and leave, a fork naming its parent's epoch, the default leave parking an idle session, `maic_busy` and a park with interrupt keeping the queued message, resume running it, stop and stopping a parked entry, and focus surviving one client's disconnect but not the last; plus the epoch mutation. The pty suite: `:new` parking the idle session, the switcher resuming it with its history, `:fork`, and a working session sent to the background that keeps working while another turn runs, with `:q`'s warning. `ui_test.lua`: `:MaicNew`, `:MaicBg` through the switcher, `:MaicSwitch ID` back to the working session, `:MaicFork`, and the switcher resuming a parked one.


**As built (step 13, 2026-10-03).** Section 8's third step, and the tiers of [protocol-security.md](protocol-security.md). The user's guide is [daemon.md](../daemon.md).

* **`maic daemon run`** (`cli/src/daemon.cpp`) holds `engine.lock` (flock) in `$XDG_RUNTIME_DIR/maic/` (0700, refused when it is not the user's own directory; `<state>/run/` without a runtime directory), removes a socket left by one that did not end cleanly, listens on `engine.sock` (bound under umask 077, then 0600), checks each peer's `SO_PEERCRED` uid, and writes `engine.pid` with the process's start time. It unsets `NVIM`, works from `$HOME`, keeps the index in `<state>/engine/index.json`, titles its sessions, and sets each one up from the settings of its own workspace (`EngineOptions::settings_for`: the TUI's `tui_settings` with no flags). Each connection is `serve_lines`, the loop `maic --rpc` uses on stdio, now shared; when one ends the engine lets go of the client's focus as the `default` leaving verb does (`Engine::leave`: an idle session nobody else has in focus is parked, a working one keeps on in the background), then disconnects it. SIGTERM, SIGINT or SIGHUP stops accepting, removes the socket, shuts the engine down (every session parked, turns interrupted) and lets each connection send what was queued.
* **`maic daemon start`** runs `maic daemon run` detached (`setsid`, output to `<state>/engine/daemon.log`) and returns once the socket answers; **`stop`** asks first when a session is working (without a terminal it is refused unless `--yes`), sends SIGTERM to the PID it recorded (checked by start time) and waits; **`status`** (exit 3 when not running, `--json`) lists the index; **`unit`** prints, `unit install` writes and `unit remove` disables and removes `contrib/systemd/maic-daemon.service` (decision 6: MAIC never enables it). `daemon` is handed off before maic's own flags are read, as `audit-trail` is, so `--json` and `--yes` stay its own.
* **Attaching.** `daemon = "attach"` (default) or `"off"`. The TUI attaches when the daemon answers and `not_for_daemon` finds no flag only its own engine can honour: it creates (with `workspace`, and `mode` from `--mode`) or resumes (`-r`/`-c`, by path) over the protocol, applies `--model` as `:model`, and fills its welcome from `maic.session.describe` (new, local only: the instruction files, tools and tool notices); `:q` closes the connection, so a working session goes on and an idle one is parked. It shows `daemon` in the strip and says once when the daemon stops. `maic --rpc` with no agent flag carries stdin and stdout to the socket byte for byte (`bridge_to_daemon`), so maic.nvim needs no change but to name its workspace in `createConversation`. Otherwise both run their own engine, as before.
* **Tiers per session.** `protocol_tier` and `protocol_tiers` (global file only; a project file's are ignored with a warning), `agents.NAME.protocol_tier` (global only; parsed, applied with step 14), and `maic trust DIR --protocol TIER` (kept in `trust.json` under `protocol`, beside the directory entries so trust and untrust leave it; listed by `maic trust --list`). `resolve_protocol_tier` takes the nearest enrolled directory at or above the workspace (the recorded one first), else the default, and an agent's tier only at or above that floor. A session's tier is fixed at open into its `start` record (`tier`); a resumed one takes its recorded tier, raised to its directory's floor; a fork starts from its parent's. `airtight` is refused (`maic_tier_unavailable`) until step 18. The engine checks each session's events at its own tier and each request at the tier of the session it names (else the engine's default, `EngineOptions::tier`, which the hosts set from `protocol_tier`). `:tier [open|guarded]` runs in the engine before the remote allow-list: tightening from any client, loosening from a local one and never below the tier the session opened at, announced as `maic.session.settings {tier, by}`. The index entry carries `tier`; the TUI's strip shows `PROTOCOL OPEN` and `AIRTIGHT`; `:status` and `:harness` say the tier and where it came from.
* **Sandbox.** Without `XDG_RUNTIME_DIR`, `<state>/run/` is masked too, as the runtime directory already was, so a sandboxed command cannot reach the daemon's socket.
* **Tests.** `protocol_conformance` has a tiers section (the default, a directory's, airtight refused, `:tier` tightening from either client and loosening only locally and never below the opening tier, `:status`, a resumed session's recorded tier) and `Engine::leave` parking an idle session. `cli_smoke.py`'s `daemon_smoke`: status when stopped; start (socket 0600, directory 0700); a second start and a second run; `maic --rpc` over the socket with a turn; an idle session parked when its client closes; a working one finishing after its client closed and seen by the next; stop refusing to interrupt unasked; a daemon killed with SIGKILL and started again over its stale socket, its sessions parked; resume after the restart; stop removing the socket and PID file; the daemon's recorded connections passing `maic protocol check`; and the unit printed, installed (a fake systemctl shows it is never enabled) and removed. The pty suite: a TUI attached to a daemon quits mid-turn, the turn finishes in the daemon, and the next TUI's `:switch` shows it finished with its reply. The peer check for another user's connection has no test (it needs a second account).
