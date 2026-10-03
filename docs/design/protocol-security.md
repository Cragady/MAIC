# Protocol security tiers

The tiered security model for the [engine protocol](engine-protocol.md) and steering (its section 14), and how the tiers govern the rendezvous relay and a trusted node. Design first; nothing here is code yet.

Micaiah's decision (2026-10-02): a range from little or no safety, which is wanted in some cases (a private local sandbox, experiments), to fully air-tight, chosen by her per directory and per agent over a global default, the way trust tiers and Lua levels are. The schemas, the ordering machine and the conformance checker the tiers switch on are in [engine-protocol.md](engine-protocol.md#15-schemas-the-ordering-machine-and-conformance).

## What no tier changes

A tier decides how much the protocol checks, refuses and records. It never reaches the floor below it, which holds at `open` exactly as at `airtight`:

* TLS off loopback, the relay tunnel's end-to-end encryption, the `SO_PEERCRED` check on the socket, the token check on every remote command.
* The remote allow-list ([engine-protocol.md section 7](engine-protocol.md#7-security)): no unlock, no trust, no settings writes, no unsandboxed shell from a remote client.
* `Origin::Remote` asked every time; `always` never covers a remote-origin call.
* No tripwire reset from any client; `maid.engine.trip` and `cancelResponse` always open to every client, with no step-up, because stopping only adds restriction.
* The harness's fixed rules: forbidden terms, trip patterns, trust, the sandbox, self-protection ([harness-authority.md](../harness-authority.md)).

## The three tiers

* **`open`**: nothing past the floor is checked. Messages are parsed as JSON-RPC and handled; schema validation and ordering checks are off, and nothing extra is logged. For a private local sandbox, experiments, and measuring what the checks cost.
* **`guarded`** (the default): every check runs and logs what it finds, but only the floor refuses. A bug in a client or in the engine shows up in a log and a notice instead of breaking a session.
* **`airtight`**: every message is validated against its schema and every event against the ordering machine, and anything that fails is refused; remote steering needs a fresh step-up; everything is audited and each session's stream is kept for the checker; only a build that passed conformance runs it.

The names say where each sits: `open` and `airtight` are the two ends, and the middle is `guarded` rather than `standard` because `standard` already names a trust tier ([harness.md](../harness.md#directory-trust-and-restricted-settings-lua-built)), and both are set side by side in `maid trust`.

| | `open` | `guarded` | `airtight` |
| :--- | :--- | :--- | :--- |
| Schema validation of incoming messages | off | log: a line in `<state>/engine/protocol.log`, one notice per kind to local clients, the message handled as before | reject: `-32602` with `data.code: "maid_schema"` and the JSON pointer that failed in `param` |
| Schema validation of the engine's own events | off | log | reject: the event is withheld and the response fails with `maid_protocol_violation` (a bug, logged with the event) |
| Ordering enforcement | off | log: the engine checks each event against `ordering.json` before sending, and clients that implement the machine resync on a fault | reject: an illegal transition is withheld and the response fails with `maid_protocol_violation`; a client that sees a gap or an illegal event resyncs |
| Who may steer | every local client; remote only with `protocol_open_remote` | per `steering.clients` (default: every action, local and remote) | per `steering.clients`; remote only with step-up |
| Who may halt | every local client | as steer | as steer |
| Step-up for remote steering and halting | no | no | yes, for all six `maid.steer` actions; a plain `response.steer` is a message, as `response.create` is, and needs none; `cancelResponse` and `maid.engine.trip` never |
| Remote reach | none unless `protocol_open_remote = true` (global file only) | the allow-list of section 7 | the allow-list, with step-up also for `maid.session.set` and `maid.session.command` |
| Harness paths | all | all, with today's warnings and confirmations | `smart`, or `external` only with a smart harness on the other side; `dumb` is refused (`maid_tier_unavailable`) |
| Logging and audit | the harness's own records only | every remote command (section 7's audit line), every steer from any client, every validation and ordering fault | everything in `guarded`, every command from any client, every step-up attempt, every trusted node request, and the session's whole event stream kept in `<state>/engine/streams/<session>.<load>.jsonl` (0600, 14 days by default) for `maid protocol check` |
| Conformance before release | not required: any build runs `open` | the gate (`scripts/check.sh`, which `release.sh` already requires) runs ctest `protocol_conformance`, so every tagged release passed it; a dev build runs `guarded` with no further check | the binary must carry a conformance stamp: the hash of `protocol/` and the commit, written by `release.sh` after the gate passed. An unstamped build refuses `airtight` sessions with `maid_tier_unavailable` (tests set `MAID_TESTING=1`) |

### Step-up

A step-up is a fresh proof from the person, not the device: the TOTP code of [accounts.md](accounts.md#totp), checked through the same `set_step_up_verifier` hook `POST /api/trust` uses (`core/include/maid/trust.hpp`), with the same replay rule (a code at or below `last_step` is refused). A remote client sends it as `step_up {code}` in the request's params; one success is good for that client for `step_up_minutes` (5 by default, global file only). Until accounts register a verifier, every step-up fails, so an `airtight` session takes no remote steering at all, the same answer `/api/trust` gives today.

## Where the tier is set

* **The global default**: `protocol_tier = "guarded"` in the global settings file only. In a project's file it is ignored with a warning naming the file, as `trust_*`, `global_lua` and `lua_memory_mb` are.
* **Per directory, by the user**: `protocol_tiers = { ["~/scratch/sandbox"] = "open", ["~/dev2/MAID"] = "airtight" }` in the global file, or `maid trust PATH --protocol TIER` and `:trust --protocol TIER`, recorded with the directory's entry in `<state>/trust.json`. The recorded one comes first. The protocol tier, the trust tier and the Lua level are three separate axes of one enrollment.
* **Per agent, by the user**: `agents.NAME.protocol_tier`, read from the global file only; an agent defined in a project file cannot carry one.
* **Resolution**: the agent's tier when set, else the directory's, else the global default; but a tier enrolled explicitly for a directory is a floor, and an agent can never run looser than it. So an `open` experiment agent runs open in an unenrolled directory and `airtight` in `~/dev2/MAID` above.
* **In a session**: `:tier TIER` tightens for this session from any local client; loosening back down, at most to the resolved tier, is local only; a remote client can only tighten. The change is announced in `maid.session.settings` with `by`.

A session's tier is fixed into its `start` record, so a resumed session reports the tier it ran at.

**Built (step 13, 2026-10-02)** for `open` and `guarded`, as written above, with these details: the directory is the nearest enrolled one at or above the session's workspace; `maid trust DIR --protocol TIER` (`none` forgets it) keeps the tier under `protocol` in `trust.json`, apart from the trust entry; a resumed session takes its recorded tier raised to its directory's floor; `:tier` never loosens below the tier the session opened at; per-agent tiers apply to a background task's session (step 14); `airtight` is refused with `maid_tier_unavailable` (step 18). See [engine-protocol.md](engine-protocol.md), as built (step 13).

### How a session shows its tier

* `tier` in its index entry and in `maid.session.settings`; `tier` in the `maid.hello` result.
* The status strip: `open` in the warning colour, so an unchecked session is never mistaken for an ordinary one; `airtight` marked; `guarded` shown only in `:status`.
* `:status` and `:harness` name the tier and where it came from (global, directory, agent, session).
* A remote client's session view shows the tier and its path (below) beside the model.

## The relay and a trusted node

The streaming paths of [harness-authority.md](../harness-authority.md#the-two-streaming-paths) let the relay stay nearly free of memory and compute: the work is delegated to the ends through streaming. Micaiah may still want work done on a remote machine ("If I have a beefy server, maybe I want it there!"), so the remote side is a range of roles, from a thin relay with no responsibilities to a trusted node that runs everything.

| Role | What it does | What it sees | Where encryption ends |
| :--- | :--- | :--- | :--- |
| thin relay (the default) | pairs and forwards; nothing else | pairing ids, frame sizes, timing | at the ends: the client and the workstation |
| a node running some of the toolchain (`models`, `reviewer`, `conformance`) | serves what it was granted to the workstation, as a provider or a reviewer | what it is sent for that work | at the node, for that work |
| trusted node | runs the whole LLM toolchain itself, harness included: the engine, the models, the reviewer, the sessions it hosts | everything of the sessions it runs | at the node: it is the endpoint |

### Thin relay (the default)

`maid-relay` as [remote.md](../remote.md#through-a-relay) describes it: it pairs the two ends and forwards end-to-end encrypted frames, sees only pairing ids, sizes and timing, keeps no content, and does no review or validation. Its memory is the frames in flight and its CPU is copying them. The ends do the work: the workstation runs the agent and the harness, the reviewer runs locally or at a provider, and on the external path the other agent's harness judges. Because the relay holds no session key, it cannot review an action, validate a stream or keep anything: every byte it forwards is sealed under keys it does not have. Its tier covers what it can affect: pairing strictness, rate and size caps, idle expiry, how much it logs, and whether the ends may go peer to peer.

### Trusted node

A machine of Micaiah's that runs the whole LLM toolchain itself, harness included. A client using it talks to it directly: the node's engine runs the agent, its reviewer judges each action locally without calling out, its models answer, and the client needs no round trip to the workstation at all. A session on a node works on the node's files, under the node's own harness, trust, sandbox and tripwire, and every action names its judge (`maid.judged_by: "maid"` with the node's name).

**It keeps end-to-end encryption.** The node is the endpoint (at that point it is the API), so encryption runs from the client to the node: the client pairs with the node's own static key and the tunnel of [remote.md](../remote.md#the-wire) terminates there, and a browser reaches it over TLS under the node's own certificate authority. A relay between them still sees only sealed frames.

Less than the whole toolchain is the same role, granted narrower: `maid node grant NAME ROLE` with `models` (a provider over the tunnel), `reviewer` (the workstation's round trip reviews there, `maid.review.started` names the node), `conformance` (`maid protocol check` over kept streams), `index` (a read-only mirror of the session index), `sessions` (hosting sessions), or `all`; `maid node revoke` and `maid node list` beside it.

### Promoting a relay to a node

A relay can grow into a trusted node: once the machine becomes the certificate authority and TLS source, and its server takes on encrypting everything, it is an endpoint, and so a trusted node. That promotion is always a deliberate act and never happens silently:

1. **Micaiah re-pairs the machine in its new role**: `maid node pair` on the LAN, at the workstation's terminal, with a step-up code (once accounts exist), creating a node key kept in `<state>/server/nodes.json`, apart from the phones' `pairs.json`.
2. **Every client is told the endpoint changed**: a notice that this machine can now read everything sent to it, and the client pairs with the node's key before it talks to the node. Until then it keeps talking to the workstation through the machine as a relay.
3. **The session shows the role** (below).

The tunnel enforces it: a client completes the handshake only with a static key it paired with, and a relay has none, so a relay that tried to read traffic could not finish the triple Diffie-Hellman with any client. Nothing a relay or a node sends can grant itself a role, and a phone's pairing cannot be promoted. One machine can also run a relay and a node side by side, as two processes with two keys; the relay process still sees nothing.

A node that hosts sessions changes one earlier rule deliberately: each session lives on exactly one of her machines, the workstation or a node, and a relay still stores nothing (open question 5).

### How the tiers map onto both roles

| | `open` | `guarded` | `airtight` |
| :--- | :--- | :--- | :--- |
| Relay: pairing | today's: 8 digits, 2 minutes, 3 tries, on the LAN | today's | a 10-character code (the recovery-code alphabet), 1 minute, 3 tries, on the LAN, and a step-up code once accounts exist |
| Relay: rate and size caps | `--rate` 4 MiB/s, frames up to 1 MiB, `--max-pairs` 64 | the same | 1 MiB/s, frames up to 1 MiB, 8 pairs |
| Relay: idle expiry | 60 s | 60 s | 30 s (keepalives stay at 20 s) |
| Relay: logging | one line per attach and detach: id, side, byte counts, time | the same | attach and detach times only, the pairing id as a hash prefix, no byte counts: on someone else's box, less is safer |
| Relay: peer-to-peer upgrade | allowed | allowed, the relay the fallback | off: everything goes through the relay, so neither end learns the other's address |
| Node: pairing and promotion | the local command | the local command and a step-up code | the same, the node's key re-confirmed at the workstation every 30 days, and the node running a build with the conformance stamp at `airtight` itself |
| Node: grants | any | any | any but `index`: no copy of one machine's sessions rests on another |
| Node: validation of its traffic | off | log | reject |
| Node: audit | none | each node request in `audit.log` as `node/<name>` | the same, and the node keeps its streams as the workstation does |

**Who enforces what.** The relay's caps, idle expiry and logging are set where it runs (`maid-relay --tier open|guarded|airtight`, with `--rate`, `--idle` and `--max-pairs` still overriding), by whoever runs it. The workstation cannot trust a relay's claim about itself, so everything it can decide it decides from the session's own tier: pairing, the peer-to-peer upgrade, its own rate caps and audit. An `airtight` session refuses peer to peer whatever the relay allows. A node enforces its own tier on the sessions it runs, and a client connecting to it sees that tier in `maid.hello`.

### How a session shows its remote path

Each remote client's connection has a path, `{via, role, tier, node}`: `via` is `lan`, `relay`, `p2p` or `node`; `role` is `thin` or `endpoint`; `tier` is the relay's announced tier (labelled as announced) or the node's. It is in the client's `maid.hello` result and in the entry's `clients` list while that client is attached. The status strip shows it (`phone via relay (thin), guarded`; `on node beefy (endpoint), airtight`), `:harness` says where the reviewer runs (`reviewer: qwen-9b on node beefy`), and after a promotion every client shows the endpoint change until it has re-paired.

## Open questions for Micaiah

1. **Tier names.** Recommendation: `open`, `guarded`, `airtight`; `standard` would collide with the trust tier of the same name.
2. **Remote reach of `open`.** Recommendation: none unless `protocol_open_remote = true` in the global file, so an experiment never becomes reachable by accident.
3. **Agent tier against directory tier.** Recommendation: the agent's tier applies, but an explicitly enrolled directory is a floor it cannot go below, so marking a repository `airtight` cannot be undone by picking an agent.
4. **How long `airtight` keeps streams.** Recommendation: 14 days, 0600, under the state directory, with `stream_keep_days` in the global file; long enough to check a week's work, short enough not to become a second transcript store.
5. **Nodes that host sessions.** This revisits "the workstation is the only place sessions live". Recommendation: allow it at every tier, since a session on a node lives on that node and nowhere else; refuse only the `index` mirror at `airtight`, which copies one machine's sessions onto another.
6. **Peer to peer under `airtight`.** Recommendation: off; the relay path costs a little latency and keeps each end's address private.
7. **Step-up lifetime.** Recommendation: 5 minutes per client, so a burst of steering from the phone costs one code.
8. **The notice after a promotion.** Recommendation: shown in every client until it re-pairs, and the client keeps talking through the machine as a plain relay until then, so a promotion never interrupts work and never reads anything unannounced.

## Decisions (Micaiah, 2026-10-02)

All eight open questions are settled as recommended. On question 2: the `open` tier switches validation, ordering enforcement, steering restrictions and most logging off, which suits a private machine or an experiment; an `open` session therefore refuses remote clients unless `protocol_open_remote = true` is set in the global file, so an experiment never becomes reachable from off the machine by accident. A fast, loose link to a node she controls is the intended case for turning it on. Revised the same day: besides the setting, a remote client may join an `open` session through a deliberate multi-step flow: a warning naming what `open` turns off, then the step-up check, then an explicit yes to each warning, then a final warning before the join completes. The setting becomes `protocol_open_remote = "never" | "ceremony" | "always"` (default `ceremony`), global only; `always` is for a node she controls.

