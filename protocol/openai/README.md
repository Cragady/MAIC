# protocol/openai

OpenAI's API description, pinned, and the subset of it MAID validates against. The engine protocol takes OpenAI's names and shapes unchanged ([docs/design/engine-protocol.md](../../docs/design/engine-protocol.md) section 10, [docs/standards.md](../../docs/standards.md)); these files are where those shapes come from.

| File | What |
| :--- | :--- |
| `openapi.json` | the whole description, byte for byte as upstream has it; kept for reference (Micaiah's decision on open question 22) |
| `LICENSE` | upstream's MIT license, copied unchanged |
| `extract.py` | writes `subset.json` from `openapi.json`; standard library only |
| `subset.json` | the schemas MAID references and everything they `$ref`: what the tests validate against |

## Provenance

* Repository: [openai/openai-openapi](https://github.com/openai/openai-openapi), MIT, Copyright (c) OpenAI.
* Commit: `de3a025c40f84b99d1401ee1c5fe69fbf8de789b`, committed 2026-10-02 00:39:17 UTC ("Add client_secret object to realtime transcription session responses").
* Description: OpenAPI 3.1.0, API version 2.3.0.
* Fetched 2026-10-02 (UTC) from `https://raw.githubusercontent.com/openai/openai-openapi/de3a025c40f84b99d1401ee1c5fe69fbf8de789b/openapi.json` and `.../LICENSE`; both match the git blob ids in that commit's tree (`d208ea1141a16d3fa3c7f177324bc9fc24859b73` and `4f14854c3243516f3de1487a60f14882874d7e3a`).
* `openapi.json`: 4,648,971 bytes, sha256 `942fd516753229e5c3cc4df9a3ab71055ac3c8dec937dd0ebdbd632620f35045`, 1,957 schemas. The repository's `openapi.yaml` is the same description in YAML and is not copied.

## The subset

```sh
python3 protocol/openai/extract.py           # rewrite subset.json
python3 protocol/openai/extract.py --check   # ctest openai_subset: openapi.json is the pinned file, subset.json is current
```

The roots are listed in `extract.py`: `ResponseStreamEvent` (the 59 stream events), `Response`, `OutputItem`, `ResponseUsage`, `ResponseError`, `ErrorResponse`, `CreateChatCompletionStreamResponse`, `ConversationResource`, `ConversationItem`, `CreateResponse`, and the WebSocket's steering events (`ResponseSteerEvent`, `ResponseSteerAcceptedEvent`, `ResponseSteerFailedEvent`, which are not in `ResponseStreamEvent`). With their `$ref` closure that is 357 schemas, copied unchanged with upstream's key order, sorted by name, under `components.schemas`, so a `$ref` reads the same in both files. A schema joins the subset by adding a root.

Validated by MAID's own validator (`core/src/jsonschema.cpp`, no new dependency): `jsonschema_test` checks that every schema in the subset uses only keywords it implements, a Responses stream shaped as the engine will send it, and llama-server's chat-completions chunks; `agent_test` checks every chunk its `FakeServer` sends; `llm_test` parses the llama-server stream.

## Bumping the pin

A change of its own: fetch `openapi.json` and `LICENSE` at the new commit and check them against the commit's tree, set `COMMIT` and `SHA256` in `extract.py`, run it, review the diff of `subset.json` (the reviewable part; `.gitattributes` keeps `openapi.json` out of text diffs), update this page and the OpenAI rows of `docs/standards.md` and `docs/cleanroom.md`.
