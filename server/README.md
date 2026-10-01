# server

Remote access to MAIC: `maic-server` (also `maic server start`) owns agent sessions and serves a small JSON API with server-sent events, plus the single-file web client in `web/index.html`. With `server.relay` set it also dials out to a `maic-relay` ([relay/](../relay)) and carries the same API to a paired phone through an end-to-end encrypted tunnel. The design, setup, API reference, the wire format and the pairing walkthrough are in [docs/remote.md](../docs/remote.md).

Constraints it keeps:

* It is a **client of the core**, like the CLI. It never manages processes or runs tools itself; `Agent` does, through the harness.
* Everything it asks for is **`Origin::Remote`**: the harness asks for approval whatever the mode. The client answers approvals through the API.
* It **cannot reset the tripwire.** There is no route for it; a client can only trip.
* Loopback by default. Any other address needs TLS (self-signed on first use, or a configured pair) and every request needs a per-device bearer token. Failed attempts are rate limited per source; every request is written to an audit log.

Layout: `src/server.cpp` (sessions, routes, streaming, `/api/pair`), `src/auth.cpp` (tokens, hashing, rate limit), `src/tls.cpp` (certificate generation, shared with the relay), `src/tunnel.cpp` (frames, the handshake and session keys, the pairing store and offer), `src/home_link.cpp` (the outbound connection to the relay and requests replayed against the server's own listener), `src/command.cpp` (`maic server ...`), `tests/server_test.cpp`, `tests/relay_test.cpp`, `tests/tunnel_js_test.mjs` (the web client's crypto under node).
