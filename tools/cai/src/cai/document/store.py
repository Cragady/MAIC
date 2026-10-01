"""Shapes: how a thing is arranged, as against what its parts mean.

**The split, owner 2026-09-02:** *the entire shape of the name is the document,
the symbols are the notations, and `cai name` is the implementation of both.*

That generalises past naming. **A tool is an implementation over documents and
notations** -- it arranges atoms whose meanings it does not own, into shapes it
does not own either. Keeping the three apart is what stops any one of them
becoming the place everything accumulates.

| holds | example |
| --- | --- |
| `cai notation` | what a symbol MEANS -- the fork glyph, the fence markers |
| `cai document` | how parts are ARRANGED -- the field order of a session name |
| a tool | what to DO with an arrangement -- render it, check it, change it |

**Shapes live in SOPIA and are fetched**, same as everything else, and for the
same reason: SOPIA defines, cai enforces.
"""
import os

from cai import source

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_VAR = "CAI_DOCUMENTS"
SOURCE_CANDIDATES = (os.path.join(source.SOPIA, "docs", "shapes.json"),)
FALLBACK_PATH = os.path.join(HERE, "shapes.json")


def load(path=None):
    if path:
        return source.load("__none__", (path,), None, empty={"shapes": {}})
    return source.load(ENV_VAR, SOURCE_CANDIDATES, FALLBACK_PATH, empty={"shapes": {}}, package="document",
                       remote_path="docs/shapes.json")


def get(doc, name):
    return (doc.get("shapes") or {}).get(name)


def names(doc):
    return sorted((doc.get("shapes") or {}).keys())


def check(doc, name, value):
    """Whether a value satisfies a shape's stated requirements.

    **Reports; never repairs.** A shape describes what should be true, and a
    value that is not is a finding for a person -- rewriting it here would be the
    tool deciding what the operator meant.
    """
    shape = get(doc, name)
    if not shape:
        return {"shape": name, "known": False,
                "note": "no shape named %r is recorded" % name}
    findings = []
    for part in shape.get("parts", []):
        if part.get("required") and part.get("key") not in (value or {}):
            findings.append("missing required part %r -- %s"
                            % (part["key"], part.get("why", "")))
    return {"shape": name, "known": True, "ok": not findings, "findings": findings,
            "parts": [p.get("key") for p in shape.get("parts", [])]}
