# Remote access

Micaiah wants to talk to her agents from her phone. This page is how that works: what is built (`maic-server`, its API and the web client), how to set it up, how tokens and TLS keep it hers, the relay that is designed but not built, and what a native client would add.

The rules that shaped it, from [harness.md](harness.md) and [roadmap.md](roadmap.md):

* The server is a **client of the core**, like the CLI. It never bypasses the harness; it drives `Agent` exactly as the TUI does.
* Everything it asks for is **`Origin::Remote`**, and the harness asks about every remote tool call, whatever mode the session is in. Approvals are answered from the phone.
* The tripwire can be **tripped** from a client and **never reset**. There is no route for the reset; see below for why.
* No telemetry. The server talks to nothing but the clients that connect to it and the model providers already in settings.

## Architecture

```
  phone (browser, later a native app)
     |  HTTPS + bearer token, on the LAN
     v
  maic-server on the workstation          (server/)
     |  one Agent per session, Origin::Remote
     v
  maic_core: harness, tools, sandbox, session log     (core/)
     |
     v
  llama-server on the same machine, or a remote provider from settings
```

`maic-server` (also `maic server start`) is one process on the workstation. It owns agent sessions: one `Agent` per session id, each with its own `SessionLog` written to the normal sessions tree with kind `server`, so `maic sessions` lists them and `maic -r ID` can continue one at the terminal later. The model, mode, providers and compaction settings come from the same settings files the CLI reads; `--model` and `--mode` override the defaults for new sessions.

The web client is a single file, `server/web/index.html`, served at `/`. No build step, no CDN, nothing loaded from anywhere but the server itself. It streams replies, shows tool calls and results, answers approvals, lists sessions, and switches modes. It is written for a phone: large tap targets, safe-area insets, and it reattaches to a running turn after the screen was locked.

### Direct on the LAN

The simple case. The workstation listens on its LAN address with TLS, the phone opens `https://workstation:7373/`, pastes a token once, and talks. Nothing leaves the LAN. This is what is built.

### Through a relay (designed, not built)

For the phone away from home, without opening a port on the home router. The workstation makes an **outbound** WebSocket connection to a small relay (a tiny process on any VPS, or a free tier somewhere), and keeps it open. The phone connects to the same relay. The relay pairs the two by a rendezvous id and forwards bytes in both directions. Nothing inbound reaches the workstation, so there is no port to forward and no exposure when the relay is down.

The relay must be able to see nothing. So the bytes it forwards are encrypted end to end between the phone and the workstation, on top of whatever TLS the relay itself uses:

* **Pairing** happens once, at home, on the LAN: the workstation shows a QR code (or `maic server pair` prints one) holding a fresh X25519 public key, the rendezvous id and the relay's address; the phone scans it and sends its own public key back over the direct HTTPS connection. Each side stores the other's key. After that the phone is the only device that can decrypt what the workstation sends through the relay, and vice versa.
* **Sessions** through the relay use a Noise-style handshake (`IK` pattern: both sides know the other's static key) to derive fresh keys per connection, then an authenticated stream cipher (ChaCha20-Poly1305) over the WebSocket frames. Inside that tunnel runs exactly the same HTTP API as on the LAN, bearer token included, so the server code does not know or care which path a request came by. The relay sees connection times, sizes and the rendezvous id. It sees no token, no prompt, no file, no approval.
* **The relay** needs about a hundred lines: accept two WebSocket connections that present the same rendezvous id (the workstation's is marked as the host side), pipe frames between them, drop both when either closes. It stores nothing. Anyone can run one; the design does not depend on trusting whoever does.
* **Rate limiting and audit** stay on the workstation, where the tokens are. The relay adds nothing to security; it only adds reachability.

Why not a VPN (Tailscale, WireGuard)? It works today and is a fine answer for anyone who already runs one; the relay design is for the case where MAIC should not depend on a third party's control plane for something it can do with a public key and a hundred lines. Both keep the same rule: the server end is always the workstation, and the harness is always in front of it.

### Why the tripwire stays local

The tripwire (`/var/lib/maic/tripwire`, root-owned) exists so that once something has gone wrong, nothing running as the user, on the machine or through it, can quietly make the problem go away. Resetting it needs the sudo password, typed at the workstation. A remote reset would need one of two things: the password sent over the network, or a standing capability on the workstation that resets the lock without a password. The first puts the sudo password in a phone's memory, in a relay's traffic, and behind a bearer token. The second is exactly the capability the tripwire is designed not to have: any process running as the user could invoke it, which includes a compromised agent.

So the API has no reset route at all, not a disabled one, not one that needs a second token. If the lock trips while she is out, the agent keeps talking but nothing runs until she is back at the keyboard. Tripping from the phone is fine, because tripping can only add restriction.

## Setup

On the workstation:

```sh
cmake --preset default && cmake --build --preset default    # builds maic and maic-server
maic server token new phone        # prints a token once; only its SHA-256 is stored
maic server start                  # http://127.0.0.1:7373, for a browser on this machine
maic server start --listen 0.0.0.0:7373     # every interface, TLS on, certificate made on first use
```

`start` prints the address, the certificate's SHA-256 fingerprint when TLS is on, the allowed workspace roots, and a reminder that nothing can be reset from a client. Ctrl-C stops it. `maic server status` shows the configuration and whether a server is answering.

On the phone: open `https://<workstation address>:7373/`. The browser warns about the self-signed certificate; compare its fingerprint with the one `start` printed, accept it once, and paste the token into the token box. The token stays in that browser's local storage and is never shown again.

Settings (`~/.config/maic/settings.lua`, key `server`):

```lua
server = {
  listen = "0.0.0.0:7373",           -- default 127.0.0.1:7373
  workspaces = { maic.home .. "/dev2", maic.home .. "/notes" },   -- default: ~/dev2, else the current directory
  cert = maic.home .. "/.config/maic/server.crt",   -- optional; empty means self-signed under ~/.local/state/maic/server/
  key  = maic.home .. "/.config/maic/server.key",
}
```

A remote session's workspace must be inside one of `workspaces`, resolved with symlinks followed; anything else is a 403. The first root is the default workspace.

State, under `~/.local/state/maic/server/` (`$XDG_STATE_HOME/maic/server/`):

| File | What |
| :--- | :--- |
| `tokens.json` | token names and SHA-256 hashes, 0600 |
| `audit.log` | one line per request, 0600 |
| `cert.pem`, `key.pem` | the self-signed pair, made on first TLS start; the key is 0600 |

Session transcripts go where the CLI's go: `~/.local/state/maic/sessions/`, kind `server`.

## Tokens

One token per device, made on the workstation: `maic server token new NAME`. It is 24 random bytes from OpenSSL, shown once as 32 URL-safe characters. Only its SHA-256 hash is stored, so the file on disk cannot be turned back into a token. `token list` shows names and creation times; `token revoke NAME` removes one, and the running server notices on its next request.

Every request under `/api/` needs `Authorization: Bearer <token>`. A missing or wrong token is a 401 with a JSON error. The check hashes the presented token and compares it against every stored hash with `CRYPTO_memcmp`, never stopping early, so timing says nothing about which entry came close. After five failures inside a minute from one source address, that address gets 429 until the minute is up; a success clears the count.

Every request, accepted or not, adds a line to `audit.log`: time, source address, token name (or `-`), method, path, status.

```
2026-09-30T10:40:38Z 192.168.1.20 phone POST /api/sessions/20260930-104012-server-4121/messages 200
2026-09-30T10:41:02Z 192.168.1.33 - GET /api/status 401
```

The web page at `/` needs no token: it is the client itself, and it contains nothing but code.

## TLS

Loopback (`127.0.0.1`, `localhost`, `::1`) runs plain HTTP, because it never leaves the machine. Any other listen address turns TLS on and there is no flag to turn it off.

With nothing configured, the first TLS start makes a self-signed P-256 certificate, valid ten years, with the listen address (or every interface address when listening on `0.0.0.0`) and the hostname in its subject alternative names. `start` prints its SHA-256 fingerprint; the phone's browser shows the same fingerprint in its warning, and once accepted it keeps the certificate pinned. A pair from elsewhere (a LAN CA, or a real certificate for a name that resolves at home) goes in `server.cert` and `server.key`; when a pair is configured it is also used on loopback.

## API

JSON in, JSON out, `Authorization: Bearer <token>` on everything under `/api/`. Errors are `{"error": "..."}` with the status. Streaming responses are `text/event-stream`: one `data: {json}` line per event, a `: keepalive` comment every 15 seconds of silence, and the stream ends when the session is idle again.

| Method and path | Body | Result |
| :--- | :--- | :--- |
| `GET /` | | the web client, no token needed |
| `GET /api/status` | | `harness {tripped, reason}`, `services [{name, state, where}]`, `model`, `provider`, `remote_model`, `mode`, `sessions`, `listen`, `tls`, `fingerprint`, `workspaces`, `version` |
| `GET /api/sessions` | | `{sessions: [session]}` |
| `POST /api/sessions` | `{workspace?, model?, mode?}` or `{resume: ID, mode?}` | 201 and the session; 403 when the workspace is outside the allowed roots; 400 for a missing directory or unknown mode |
| `GET /api/sessions/{id}` | | the session plus `entries` (the folded transcript), `usage`, `pending_approval` |
| `POST /api/sessions/{id}/messages` | `{text}` | streams the turn's events; when a turn is already running the text is queued for the model's next call and the stream continues from there |
| `GET /api/sessions/{id}/events?after=N` | | streams events from sequence N: the way to reattach after a lost connection |
| `POST /api/sessions/{id}/approvals/{approval_id}` | `{choice: yes/no/always/trip, feedback?}` | answers a pending approval; 404 when none by that id is waiting |
| `POST /api/sessions/{id}/interrupt` | | stops the running turn; a pending approval is answered no |
| `POST /api/sessions/{id}/mode` | `{mode}` | changes the session's mode |
| `POST /api/trip` | `{reason?}` | trips the harness lock. There is no reset route. |

A session: `{id, workspace, model, mode, remote_model, running, turns, created, seq, transcript, pending_approval}`. `seq` is the next event number, for `events?after=`. `transcript` is the path of its `.jsonl` file.

Events, each with a `seq` and a `type`:

| `type` | Fields |
| :--- | :--- |
| `user` | `text`, `queued` when it went in mid-turn |
| `text` | `text` (a delta), `thinking` |
| `tool_call` | `summary` |
| `tool_result` | `text`, `ok` |
| `notice` | `text` |
| `approval` | `id`, `tool`, `summary`, `reason`, `origin` (`remote`), `always_covers`, `preview` |
| `approval_answered` | `id`, `choice` |
| `mode` | `mode` |
| `error` | `text` (a transport or provider failure) |
| `done` | `interrupted`, `usage {input, output, calls, context, last_input}` |

`always` remembers the approval for the session only, per file or per program, the same as at the terminal; nothing outlives the session. A denied call with `feedback` reaches the model as "DENIED by the user, who says: ...".

Reading the stream from a browser: `fetch` with the bearer header, then `response.body.getReader()`, split on blank lines, parse the `data:` lines. `EventSource` cannot send a header, which is why the client does not use it.

## A native client

The web client is enough to start. A native Android or iOS app would add:

* **Notifications** when an approval is waiting or a turn finishes, so the phone need not stay on the page. The server would gain a long-lived event channel per device for this (a WebSocket, or the relay tunnel itself), never a third-party push service carrying the content; the notification says only "MAIC is asking", the content stays on the workstation until the app fetches it.
* **Certificate pinning** in the app rather than in the browser's exception list, and the pairing flow (scan the QR code, store the key) for the relay.
* **Background reattach**: keep `seq` per session and resume streams when the app returns, without the page-reload dance.
* **Voice input**, which the browser has too but a native app can wire to a hardware button.
* **A denser conversation view** with folding, search, and the transcript export the CLI has.

The API would not change for any of this; it is the same one the browser uses.

## Tests

`build/server/server_test` (`ctest --test-dir build -R server`) starts the server on a random port against a fake OpenAI-compatible server and covers: token creation, verification, revocation and file permissions; the rate limit; 401 with no token and with a wrong one; the audit line for both; 403 for a workspace outside the roots; session creation, streaming, the folded transcript and replay from any `seq`; an approval round trip in auto mode where the call is still asked about because the origin is remote; a denial with feedback reaching the model; interrupt of a running turn and of a pending approval; per-session mode changes; the self-signed certificate and a pinned HTTPS client; and that no reset route exists.
