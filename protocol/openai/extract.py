#!/usr/bin/env python3
"""Writes subset.json: the schemas of the pinned openapi.json that MAIC references, with everything they $ref.

    python3 protocol/openai/extract.py           rewrite subset.json
    python3 protocol/openai/extract.py --check   exit 1 when openapi.json is not the pinned file or subset.json
                                                 differs from what this would write (ctest openai_subset)

The schemas are copied as they stand upstream, key order kept, schema names sorted, so the output depends on
openapi.json alone and bumping the pin shows as a diff of subset.json. Standard library only; no network.
"""
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE / "openapi.json"
OUT = HERE / "subset.json"

REPO = "https://github.com/openai/openai-openapi"
COMMIT = "de3a025c40f84b99d1401ee1c5fe69fbf8de789b"
SHA256 = "942fd516753229e5c3cc4df9a3ab71055ac3c8dec937dd0ebdbd632620f35045"

# What MAIC reads, sends or will send. Adding a root is how a schema joins the subset.
ROOTS = [
    "ResponseStreamEvent",                  # every Responses stream event, the engine protocol's event union
    "Response",                             # the response object those events carry
    "OutputItem",                           # output items: message, reasoning, function_call, shell_call, compaction, ...
    "ResponseUsage",                        # usage on a finished response
    "ResponseError",                        # a failed response's error
    "ErrorResponse",                        # {"error": Error}, the error body of every operation
    "CreateChatCompletionStreamResponse",   # one chat-completions chunk, what core/src/openai.cpp parses
    "ConversationResource",                 # the conversation object: createConversation, getConversation
    "ConversationItem",                     # a conversation's items: maic.session.attach, listConversationItems
    "CreateResponse",                       # response.create's fields, the Responses WebSocket's client event
    "ResponseSteerEvent",                   # response.steer, the WebSocket's other client event
    "ResponseSteerAcceptedEvent",           # the WebSocket's steering events, outside ResponseStreamEvent
    "ResponseSteerFailedEvent",
]

PREFIX = "#/components/schemas/"


def refs(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "$ref":
                yield v
            else:
                yield from refs(v)
    elif isinstance(node, list):
        for v in node:
            yield from refs(v)


def build(raw):
    spec = json.loads(raw)
    schemas = spec["components"]["schemas"]
    keep, todo = set(), list(ROOTS)
    while todo:
        name = todo.pop()
        if name in keep:
            continue
        if name not in schemas:
            sys.exit(f"extract: {name} is not a schema of openapi.json")
        keep.add(name)
        for ref in refs(schemas[name]):
            if not ref.startswith(PREFIX):
                sys.exit(f"extract: {name} has a $ref outside components.schemas: {ref}")
            todo.append(ref[len(PREFIX):])
    subset = {
        "x-maic-subset": {
            "source": REPO,
            "commit": COMMIT,
            "file": "openapi.json",
            "roots": ROOTS,
            "generated_by": "protocol/openai/extract.py",
        },
        "openapi": spec["openapi"],
        "info": {"title": spec["info"]["title"], "version": spec["info"]["version"], "license": spec["info"]["license"]},
        "components": {"schemas": {name: schemas[name] for name in sorted(keep)}},
    }
    return json.dumps(subset, indent=2, ensure_ascii=False) + "\n"


def main():
    raw = SOURCE.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SHA256:
        sys.exit(f"extract: openapi.json is not the pinned file (sha256 {digest}, pinned {SHA256})")
    text = build(raw)
    if sys.argv[1:] == ["--check"]:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            sys.exit("extract: subset.json differs from what extract.py writes; run python3 protocol/openai/extract.py")
        print(f"subset.json matches openapi.json at {COMMIT[:7]}")
        return
    if sys.argv[1:]:
        sys.exit("usage: extract.py [--check]")
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(HERE.parent.parent)}: {text.count(chr(10))} lines")


if __name__ == "__main__":
    main()
