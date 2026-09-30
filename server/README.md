# server

Planned: remote access to MAIC (web UI / API) so services and agent sessions can be reached from another device.

Not built yet. Constraints it has to keep:

* It is a **client of the core**, like the CLI. It never manages processes or runs tools itself.
* Everything it asks for counts as a **remote origin**: the harness always asks for approval and labels the request as remote, whatever mode is set (see [docs/harness.md](../docs/harness.md)).
* It **can't unlock the tripwire.** Unlocking needs a local sudo password.
* Listens on localhost by default. Exposing it beyond the machine needs authentication and TLS.
