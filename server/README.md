# server

Remote access to MAIC: `maic-server` (also `maic server start`) owns agent sessions and serves a small JSON API with server-sent events, plus the single-file web client in `web/index.html`. The design, setup, API reference and the relay plan are in [docs/remote.md](../docs/remote.md).

Constraints it keeps:

* It is a **client of the core**, like the CLI. It never manages processes or runs tools itself; `Agent` does, through the harness.
* Everything it asks for is **`Origin::Remote`**: the harness asks for approval whatever the mode. The client answers approvals through the API.
* It **cannot reset the tripwire.** There is no route for it; a client can only trip.
* Loopback by default. Any other address needs TLS (self-signed on first use, or a configured pair) and every request needs a per-device bearer token. Failed attempts are rate limited per source; every request is written to an audit log.

Layout: `src/server.cpp` (sessions, routes, streaming), `src/auth.cpp` (tokens, hashing, rate limit), `src/tls.cpp` (certificate generation), `src/command.cpp` (`maic server ...`), `tests/server_test.cpp`.
