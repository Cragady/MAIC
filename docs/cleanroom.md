# Cleanroom policy

MAIC is written so that nothing closed-source ends up in it. This page says what may go in, where the design so far came from, and what the third-party code is licensed under.

## Rules

Allowed as input:

* Public API documentation and published protocol formats (the Anthropic Messages API, the OpenAI-compatible HTTP API, SSE, JSON, bubblewrap's command line, Linux man pages).
* The observed behaviour of tools used as products (how Claude Code's modes, queued messages, `-c`/`-r` or approval prompts feel). Behaviour and conventions are reimplemented from a description; no source is involved.
* Open-source code under MIT, BSD, Apache-2.0 or similar. Reading it for ideas is fine. Copying code into MAIC requires keeping its license notice next to the copied part and listing it below.
* Design documents, generated or written, as long as they contain ideas rather than someone else's code.

Not allowed:

* Source, decompiled output or internal documents of closed-source software, including anything that was leaked. If such material is seen, the part of MAIC it relates to is not touched until it can be written from a clean description.
* Code under copyleft licenses that would change MAIC's licensing (GPL, AGPL) unless that decision is made deliberately and recorded here.
* Anything that phones home or reports usage; see the No Internet rule in [harness.md](harness.md).

## Where the design came from

| Source | Kind | How it was used |
| :--- | :--- | :--- |
| Micaiah's own requirements and the language notes in `programming-lang-for-agentic-cli.md` | design | The architecture: C++ core, tripwire with sudo unlock, sandboxed tools, vim interface. |
| Claude Code, as a product | behaviour | Modes, the approval prompt, queued mid-turn messages, `-c` / `-r`, `-p`, instruction files. Reimplemented from the observed behaviour. No source was available or used. |
| opencode (MIT, `~/dev2/tools-and-things/opencode`) | open source, read only | Feature comparison in [opencode-comparison.md](opencode-comparison.md). No code copied. |
| A generated "cleanroom harness spec" (Google) | design | Two ideas kept for the plan: per-role permission profiles (sandbox paths, network, budgets per agent role), and a forkable transcript tree. Its SQL and JSON schema were not adopted; see below. |
| Anthropic and OpenAI API references | public docs | The provider clients in `core/src/anthropic.cpp` and `openai.cpp`. |
| RFC 7748 (X25519), RFC 5869 (HKDF), RFC 8439 (ChaCha20-Poly1305) and the IETF XChaCha draft | public specs | The relay tunnel in `server/src/tunnel.cpp` (through libsodium) and the web client's copy of ChaCha20, Poly1305 and HChaCha20, written from the RFC text and checked against the RFCs' own vectors. |
| OpenAI's API description, [openai/openai-openapi](https://github.com/openai/openai-openapi) (MIT, commit `de3a025`) | open source, read only | The engine protocol's names, objects, event order and lifecycle ([design/engine-protocol.md](design/engine-protocol.md) sections 10 to 12). The protocol adopts its names and shapes exactly. The description itself is copied whole into `protocol/openai/` as data, with its license; see the table below. |
| llama.cpp's server (MIT, `vendor/llama.cpp` at `b11284`) | open source, read only | The chunk shapes in `core/tests/fixtures/llamacpp-b11284-chat.sse` and the error and logprobs shapes in `jsonschema_test`, written by hand from `tools/server/server-task.cpp` and `server-common.cpp`. No code copied. |
| nvim's API documentation (`:h api`, `:h treesitter`) and the msgpack format specification | public docs | The optional input highlighter (`cli/src/highlight.cpp`) runs the user's own nvim as `nvim --embed --headless` and talks msgpack-rpc to it; `cli/src/msgpack.cpp` is MAIC's own small codec written from the format description. nvim is executed as a program, never linked; no code was copied from it. |

### How the spec's ideas map onto MAIC

Both fit as layers on the existing design; neither replaces the tripwire.

* **Permission profiles by role** (orchestrator, builder, scout, reviewer): a profile is a named bundle of mode, allowed write paths, network yes/no and budgets, chosen per subagent (built 2026-10-01, [harness.md](harness.md) layer 24; per session is still to come). It can narrow what the harness allows or pre-approve harmless commands. It can never widen past the fixed rules: trip patterns, secrets, system paths and remote-origin asking stay as they are in every profile. This is the same rule as the additive `permission` block (layer 23).
* **Forkable transcripts**: sessions stay JSONL (one file, append-only, readable with any tool) instead of a relational tree. A fork is a new session file whose `start` record names the parent file and the record index it forked at; `maic -r ID --fork-at N` would replay the parent up to N and continue in the new file. The parent is never edited, which is also what the Anthropic history check needs.
* **Budgets**: per-command time limits exist; memory limits are planned with `prlimit`; token and cost budgets need the usage figures every provider returns, which will be recorded per turn in the session file first.

Where an outside approach and MAIC's rules cannot both hold, MAIC's rules win and the other approach becomes an opt-in mode with a clear name, never the default. Two things get no mode at all: turning approvals off globally, and a permanent "always allow" that outlives the session.

## Third-party code in the build

| Component | License | Used for |
| :--- | :--- | :--- |
| FTXUI 5 | MIT | terminal UI |
| cpp-httplib 0.15 | MIT | HTTP client (providers), the server, test servers |
| nlohmann-json 3.11 | MIT | JSON |
| OpenSSL 3 (through vcpkg) | Apache-2.0 | HTTPS to remote providers; the server's and the relay's TLS, self-signed certificates, token hashing and random tokens |
| libsodium 1.0.20 (through vcpkg) | ISC | the relay tunnel: X25519, HKDF-SHA256, XChaCha20-Poly1305, constant-time compares, random keys and codes |
| bubblewrap (system package, called as a program) | LGPL-2.0+ | the command sandbox; not linked, only executed |
| nvim (the user's own install, optional, called as a program) | Apache-2.0 and the Vim license | `highlight = "nvim"`, the input highlighter, `:e`, and maic.nvim's host connection (msgpack-rpc over its `$NVIM` socket, from `:h api`); not linked, only executed or talked to |
| glibc regex (`regcomp`) | LGPL, system library | `search_files` |
| pyte (fetched by `uv run --with pyte`, tests only) | LGPL-3.0 | the terminal emulator behind `tests/tui_driver.py`; not linked, not shipped |
| OpenAI's API description, [openai/openai-openapi](https://github.com/openai/openai-openapi) commit `de3a025` (`protocol/openai/openapi.json`, and `subset.json` extracted from it) | MIT, Copyright (c) OpenAI; the license is copied beside it as `protocol/openai/LICENSE` | the shapes MAIC's messages are validated against; data read by the tests, not code, nothing links it ([protocol/openai/README.md](../protocol/openai/README.md)) |

No code has been copied from another project into MAIC's sources. OpenAI's description in `protocol/openai/` is copied whole, as data, under its MIT license.
