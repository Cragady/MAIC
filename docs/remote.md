# Remote access

Micaiah wants to talk to her agents from her phone. This page is how that works: what is built (`maic-server`, its API and the web client, and `maic-relay` for the phone away from home), how to set it up, how tokens, TLS and the end-to-end tunnel keep it hers, and what a native client would add.

The rules that shaped it, from [harness.md](harness.md) and [roadmap.md](roadmap.md):

* The server is a **client of the core**, like the CLI. It never bypasses the harness; it drives `Agent` exactly as the TUI does.
* Everything it asks for is **`Origin::Remote`**, and the harness asks about every remote tool call, whatever mode the session is in. Approvals are answered from the phone.
* The tripwire can be **tripped** from a client and **never reset**. There is no route for the reset; see below for why.
* No telemetry. The server talks to nothing but the clients that connect to it and the model providers already in settings.

## Architecture

```
  phone (browser, later a native app)
     |  HTTPS + bearer token, on the LAN
     |  ...or, away from home, the same request end-to-end encrypted through
     |     maic-relay (relay/), which the workstation dialled out to
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

### Through a relay

For the phone away from home, without opening a port on the home router. The workstation makes an **outbound** connection to `maic-relay`, a second binary from the same code base, running anywhere with a reachable port: a VPS, a box at home behind a port forward, a friend's machine. It holds that connection open and reopens it with backoff whenever it drops. The phone connects to the same relay. The relay joins the two by a **pairing id** and copies frames between them, and nothing inbound ever reaches the workstation, so there is no port to forward and nothing exposed when the relay is down.

The relay can see nothing: every frame is encrypted end to end between the phone and the workstation, on top of the TLS the relay itself speaks. What it sees is the pairing id, the sizes of frames and when they passed. It keeps no content, has no store, and its log is one line per attach and detach with the id, the side, the byte counts and the time. Anyone can run one; the design does not depend on trusting whoever does, with one honest exception below.

**Why not a WebSocket?** `maic-relay` is plain HTTP/1.1 through cpp-httplib, which MAIC already links, rather than a WebSocket library added for this. cpp-httplib has no full-duplex request, so each side holds **two half-duplex streams**: a long `GET` whose chunked response carries frames down, and frames going up either as one long chunked `POST` (the workstation, which can stream a request body) or as one short `POST` per frame (the phone: browsers cannot stream a request body over HTTP/1.1, and the web client needs nothing more). It costs one extra connection per side and buys a relay with no second protocol in it.

#### What the relay does

Routes, the only ones it has besides `/` for the web client:

| Method and path | Who | What |
| :--- | :--- | :--- |
| `GET /r/ID/home`, `GET /r/ID/phone` | each side, once | the down stream: `application/octet-stream`, chunked, frames for that side as they arrive; a zero-length frame after 20 s of silence as a keepalive; 409 when that side is already connected, 503 at the pair cap |
| `POST /r/ID/home`, `POST /r/ID/phone` | each side | frames for the other side, as a long chunked body or one body per batch; 409 before that side's down stream is open, 410 once the pair is over |

`ID` is 8 to 64 URL-safe characters. The relay reassembles frames by their 4-byte length prefix so a frame split across posts or TCP segments arrives whole, and looks at nothing past that prefix. When either down stream ends, for any reason, the other is ended too and the pair is forgotten; the workstation reconnects within a second and waits for the next phone. A side that sends nothing for `--idle` seconds (60 by default; both ends send a keepalive every 20 s) is dropped, and its pair with it, which is how a phone that fell off the network frees the pairing. `--max-pairs` (64) caps the pairing ids held at once, `--rate` (4 MiB/s) caps the bytes per second per pairing id with a one-second burst, delaying what exceeds it, and a frame over 1 MiB ends the pair.

```sh
maic-relay --listen 0.0.0.0:7474 --cert /etc/maic-relay/fullchain.pem --key /etc/maic-relay/privkey.pem \
           --web /usr/local/share/maic/server/web/index.html --log /var/log/maic-relay.log
```

Loopback runs plain HTTP; any other address needs TLS and there is no flag to turn it off: `--cert` and `--key`, or without them a self-signed pair made under `--state` (default `~/.local/state/maic-relay`) whose fingerprint is printed. `--web` serves the MAIC web client at `/` so a phone can open the relay's address; without it `/` is a 404. The binary links only cpp-httplib and OpenSSL, not the core, so it copies to a VPS on its own.

**The one exception.** A browser runs the code it is served, and away from home the page comes from the relay. The tunnel keeps the relay from reading or forging anything, but a relay that served a modified page would have the phone's keys and token. So: run the relay yourself, or on a machine you would trust with the page, and use a certificate the phone can check. A native client ([below](#a-native-client)) carries its own code and removes this.

#### Pairing

Once per phone, on the LAN, so that the first exchange of keys never crosses the relay:

1. On the workstation, with `server.relay` set in settings and the server running: `maic server pair`. It prints an 8-digit code, valid two minutes, and a string of the form `maic://pair/<relay url>/<pairing id>/<code>`. The code's SHA-256 waits in `<state>/server/pairing.json` for the running server; three wrong guesses or the two minutes void it.
2. On the phone, on the LAN, in the web client it already uses with its token: Sessions, then **Pair with a relay**; paste the string, name the phone, tap Pair. The page makes an X25519 key pair in the browser and sends `POST /api/pair {code, name, public_key}` straight to the server over the LAN, bearer token included like every other request. The server checks the code, stores the phone's public key under its name in `<state>/server/pairs.json`, and answers with its own public key, the pairing id and the relay URL.
3. The page shows an **Open the relay** link. Following it opens the relay's address with the pairing bundle (the relay URL, the pairing id, the workstation's public key, the phone's own key pair and its token) in the URL fragment, which browsers never send to the server; the page served by the relay stores the bundle in that origin's local storage and removes the fragment from the address bar. The link is shown as text too for a browser that will not follow it; it holds the phone's key and token, so it is not for sending anywhere.

From then on the phone opens the relay's address and talks through it. `maic server pairs` lists the paired phones (name, key, when), `maic server unpair NAME` removes one and its next connection is refused; `maic server status` shows the relay link: connected since when, or when it last was and why it is not. A phone forgets a pairing with the **Forget this pairing** button on the relay page.

#### The wire

Frames on the relay: `len(4, big endian) | body`. The first body in each direction is the hello, `"MAIC1" | static key(32) | ephemeral key(32)`; every later body is `nonce(24) | ciphertext`, XChaCha20-Poly1305 with no associated data. A body of length zero is a keepalive and is dropped by the relay; the relay itself inserts one on an idle down stream.

Keys. Each side has a long-term X25519 key: the workstation's is made with its pairing id on first use and kept in `pairs.json`; the phone's is made in the browser at pairing and kept in its local storage. Each connection adds an ephemeral X25519 key per side. The phone sends its hello first; the workstation looks the static key up in `pairs.json` (re-read when the file changed) and drops the connection for a key it does not know, otherwise answers with its own hello. Both then compute three Diffie-Hellman results, ephemeral with ephemeral, phone ephemeral with workstation static, phone static with workstation ephemeral, and run HKDF-SHA256 over their concatenation with salt `maic-tunnel-v1` and the four public keys (phone static, phone ephemeral, workstation static, workstation ephemeral) as info, taking 64 bytes: the first 32 are the key the phone seals with and the workstation opens, the last 32 the other way. The workstation's first sealed message is `Ready`; a phone that cannot open it knows the pairing was not accepted. Both static keys contribute, so neither a relay nor anyone who saw the hellos can derive the session; the ephemerals give a fresh session per connection.

Nonces. 24 bytes: a 64-bit big-endian frame counter in the first 8, zero after, one counter per direction starting at 0. A receiver accepts only the counter it expects next, so a replayed, reordered or dropped frame ends the connection rather than being accepted.

Inside a frame: `kind(1) | stream(4, big endian) | payload`. The phone opens a stream per request; several run at once, which is how the page streams a turn while it polls status.

| kind | from | payload |
| :--- | :--- | :--- |
| `0 Ready` | workstation | empty; the handshake is complete |
| `1 Request` | phone | JSON `{method, path, headers}`, a newline, then the request body bytes |
| `2 Head` | workstation | JSON `{status, headers}` |
| `3 Data` | workstation | a piece of the response body, as the server wrote it (one server-sent event block at a time while a turn streams) |
| `4 End` | both | from the workstation: the response is complete; from the phone: cancel this stream |
| `5 Error` | workstation | text: the server did not answer |

On the workstation, the tunnel replays each request against the server's own listener over loopback (`https://127.0.0.1:PORT` when TLS is on, with its own certificate), through cpp-httplib's client, so the token check, the rate limit, the routes and the audit line are exactly the LAN's; the server code does not know which way a request came. The one trace is the header `X-Maic-Via: relay/<phone name>` the tunnel adds, which the audit line prints in place of the loopback address. Nothing about the relay is in `execution`, the agent or the harness.

What the relay sees, in full: the pairing id, that one side is `home` and the other `phone`, the time and size of every frame, and TLS connection metadata. It sees no token, no path, no prompt, no file, no approval, and not even which request a frame belongs to. The web client's implementation of the tunnel is the block marked `tunnel crypto` in `index.html`: ChaCha20, Poly1305 and HChaCha20 written out from RFC 8439 and the XChaCha draft because browsers do not ship them, X25519 and HKDF from WebCrypto; `server/tests/tunnel_js_test.mjs` runs it against the same vectors as the C++ test when `node` is on the machine.

**Rate limiting and audit** stay on the workstation, where the tokens are. The relay adds nothing to security; it only adds reachability.

Why not a VPN (Tailscale, WireGuard)? It works today and is a fine answer for anyone who already runs one; the relay is for the case where MAIC should not depend on a third party's control plane for something it can do with a public key and a few hundred lines. Both keep the same rule: the server end is always the workstation, and the harness is always in front of it.

### Why the tripwire stays local

The tripwire (`/var/lib/maic/tripwire`, root-owned) exists so that once something has gone wrong, nothing running as the user, on the machine or through it, can quietly make the problem go away. Resetting it needs the sudo password, typed at the workstation. A remote reset would need one of two things: the password sent over the network, or a standing capability on the workstation that resets the lock without a password. The first puts the sudo password in a phone's memory, in a relay's traffic, and behind a bearer token. The second is exactly the capability the tripwire is designed not to have: any process running as the user could invoke it, which includes a compromised agent.

So the API has no reset route at all, not a disabled one, not one that needs a second token, and the relay has no `/api` at all: a request through the tunnel reaches exactly the LAN routes and no others. If the lock trips while she is out, the agent keeps talking but nothing runs until she is back at the keyboard. Tripping from the phone is fine, because tripping can only add restriction.

## Setup

On the workstation:

```sh
cmake --preset default && cmake --build --preset default    # builds maic, maic-server and maic-relay
maic server token new phone        # prints a token once; only its SHA-256 is stored
maic server start                  # http://127.0.0.1:7373, for a browser on this machine
maic server start --listen 0.0.0.0:7373     # every interface, TLS on, certificate made on first use
maic server pair                   # with server.relay set: a code and string for the phone, then pair on the LAN
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
  relay = "https://relay.example.net:7474",         -- optional; a maic-relay the server dials out to and holds open
  relay_cert = maic.home .. "/.config/maic/relay.crt",  -- optional; pins a self-signed relay certificate (else the system CAs)
}
```

A remote session's workspace must be inside one of `workspaces`, resolved with symlinks followed; anything else is a 403. The first root is the default workspace.

State, under `~/.local/state/maic/server/` (`$XDG_STATE_HOME/maic/server/`):

| File | What |
| :--- | :--- |
| `tokens.json` | token names and SHA-256 hashes, 0600 |
| `audit.log` | one line per request, 0600 |
| `cert.pem`, `key.pem` | the self-signed pair, made on first TLS start; the key is 0600 |
| `pairs.json` | the pairing id, this workstation's X25519 key pair, and the paired phones (name, public key, when), 0600 |
| `pairing.json` | a pairing offer from `maic server pair`: the code's SHA-256, its expiry, wrong guesses so far; gone once claimed or void |
| `relay.json` | what the relay link last reported: connected, since when, last connected, the last error; read by `maic server status` |

Session transcripts go where the CLI's go: `~/.local/state/maic/sessions/`, kind `server`.

## Tokens

One token per device, made on the workstation: `maic server token new NAME`. It is 24 random bytes from OpenSSL, shown once as 32 URL-safe characters. Only its SHA-256 hash is stored, so the file on disk cannot be turned back into a token. `token list` shows names and creation times; `token revoke NAME` removes one, and the running server notices on its next request.

Every request under `/api/` needs `Authorization: Bearer <token>`. A missing or wrong token is a 401 with a JSON error. The check hashes the presented token and compares it against every stored hash with `CRYPTO_memcmp`, never stopping early, so timing says nothing about which entry came close. After five failures inside a minute from one source address, that address gets 429 until the minute is up; a success clears the count.

Every request, accepted or not, adds a line to `audit.log`: time, source address (or `relay/<phone>` for one that came through the tunnel), token name (or `-`), method, path, status.

```
2026-09-30T10:40:38Z 192.168.1.20 phone POST /api/sessions/20260930-104012-server-4121/messages 200
2026-09-30T10:41:02Z 192.168.1.33 - GET /api/status 401
2026-10-01T08:12:40Z relay/phone phone GET /api/status 200
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
| `GET /api/status` | | `harness {tripped, reason}`, `relay {url, connected, since, last_connected, error}` or null, `services [{name, state, where}]`, `model`, `provider`, `remote_model`, `mode`, `sessions`, `listen`, `tls`, `fingerprint`, `workspaces`, `version` |
| `GET /api/sessions` | | `{sessions: [session]}` |
| `POST /api/sessions` | `{workspace?, model?, mode?}` or `{resume: ID, mode?}` | 201 and the session; 403 when the workspace is outside the allowed roots; 400 for a missing directory or unknown mode |
| `GET /api/sessions/{id}` | | the session plus `entries` (the folded transcript), `usage`, `pending_approval` |
| `POST /api/sessions/{id}/messages` | `{text}` | streams the turn's events; when a turn is already running the text is queued for the model's next call and the stream continues from there |
| `GET /api/sessions/{id}/events?after=N` | | streams events from sequence N: the way to reattach after a lost connection |
| `POST /api/sessions/{id}/approvals/{approval_id}` | `{choice: yes/no/always/trip, feedback?}` | answers a pending approval; 404 when none by that id is waiting |
| `POST /api/sessions/{id}/interrupt` | | stops the running turn; a pending approval is answered no |
| `POST /api/sessions/{id}/mode` | `{mode}` | changes the session's mode |
| `POST /api/pair` | `{code, name, public_key}` | the LAN half of pairing a phone for the relay: 201 with `{name, pairing_id, relay, public_key}` when the code matches the offer from `maic server pair`; 403 for a wrong, expired or missing offer; 409 with no `server.relay` |
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

`always` from a client counts as a yes for that call only: a remote request is asked every time, so it is never remembered, and an "always" given at the terminal does not cover a remote request either (before v0.3.1 both were remembered for the session; [harness.md](harness.md) rule 5). A denied call with `feedback` reaches the model as "DENIED by the user, who says: ...".

Reading the stream from a browser: `fetch` with the bearer header, then `response.body.getReader()`, split on blank lines, parse the `data:` lines. `EventSource` cannot send a header, which is why the client does not use it.

## A native client

The web client is enough to start. A native Android or iOS app would add:

* **Notifications** when an approval is waiting or a turn finishes, so the phone need not stay on the page. The server would gain a long-lived event channel per device for this (the relay tunnel itself is one), never a third-party push service carrying the content; the notification says only "MAIC is asking", the content stays on the workstation until the app fetches it.
* **Its own code**, so the page served by the relay stops mattering, and **certificate pinning** in the app rather than in the browser's exception list; the pairing flow as it is, with a QR code in place of the pasted string.
* **Background reattach**: keep `seq` per session and resume streams when the app returns, without the page-reload dance.
* **Voice input**, which the browser has too but a native app can wire to a hardware button.
* **A denser conversation view** with folding, search, and the transcript export the CLI has.

The API would not change for any of this; it is the same one the browser uses.

## Tests

`build/server/server_test` (`ctest --test-dir build -R server`) starts the server on a random port against a fake OpenAI-compatible server and covers: token creation, verification, revocation and file permissions; the rate limit; 401 with no token and with a wrong one; the audit line for both; 403 for a workspace outside the roots; session creation, streaming, the folded transcript and replay from any `seq`; an approval round trip in auto mode where the call is still asked about because the origin is remote; a denial with feedback reaching the model; interrupt of a running turn and of a pending approval; per-session mode changes; the self-signed certificate and a pinned HTTPS client; and that no reset route exists.

`build/server/relay_test` (`ctest -R relay`) covers the relay and the tunnel: frames forwarded whole and in order between two sides, a frame split across posts reassembled, keepalives dropped, both sides ended when one leaves and the pair forgotten, the log holding ids and counts and no content; the idle expiry, the pair cap and the throughput cap; the crypto against known answers (HKDF-SHA256 from RFC 5869, the session keys and two sealed frames from fixed X25519 keys, replay, reorder and a flipped byte refused, the other direction's key not opening); the pairing store and the offer (wrong code, three wrong codes, expiry); and end to end, a `maic-server` with `server.relay` pointing at a loopback relay, a fake phone pairing over the LAN through `/api/pair` then asking `/api/status` through the relay and getting the real answer, a token-less request still a 401, the audit line naming the phone, the relay log free of anything but the id, `POST /api/unlock` a 404 through the tunnel and at the relay, the home reconnecting after the phone leaves, and an unpaired phone refused. `tunnel_js` runs the web client's copy of the crypto under node against the same vectors.
