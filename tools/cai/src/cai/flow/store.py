"""Flows: an ordered sequence of tool operations, and what must hold between them.

**A flow describes and checks. It does not run.** Executing a multi-step
destructive sequence automatically is the one thing this design has consistently
refused, and **a flow that ran itself would be an agent answering a destructive
prompt in instalments** -- each step small enough to seem harmless, the sequence
amounting to exactly what the gates exist to prevent.

**It is a composition, like `fence` and `mode`, but of a different kind.** Those
compose a shape with a notation to make a noun. A flow composes *tools* into an
order, and carries the thing an order alone cannot: **why each step comes where
it does, and what has to be true before the next one.**

**Every step's `why` is a failure that happened.** The re-root flow records
verifying before the write rather than after, and marking both edges of a window
rather than only where it starts -- because both were done the wrong way round
first.
"""
import os

from cai import source

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_VAR = "CAI_FLOWS"
SOURCE_CANDIDATES = (os.path.join(source.SOPIA, "docs", "flows.json"),)
FALLBACK_PATH = os.path.join(HERE, "flows.json")


def load(path=None):
    if path:
        return source.load("__none__", (path,), None, empty={"flows": {}})
    return source.load(ENV_VAR, SOURCE_CANDIDATES, FALLBACK_PATH,
                       empty={"flows": {}}, package="flow",
                       remote_path="docs/flows.json")


def names(doc):
    return sorted((doc.get("flows") or {}).keys())


def get(doc, name):
    return (doc.get("flows") or {}).get(name)


def tools(doc, name):
    """Which tools a flow touches, in order, skipping the human steps."""
    f = get(doc, name) or {}
    return [s["tool"] for s in f.get("steps", []) if s.get("tool")]


def pointers(doc, name=None):
    """Every document a flow points at, and whether it resolves.

    **A pointer to something that is not there is worse than no pointer**: it
    reads as though the reasoning exists and has been written down, and the
    reader who follows it finds nothing and cannot tell whether the document
    moved, was never written, or was renamed.

    Reports; never repairs. Where a pointer is broken, only a person knows
    whether the target moved or the flow is wrong.
    """
    import os as _os
    flows = doc.get("flows") or {}
    targets = []
    for fname, f in sorted(flows.items()):
        if name and fname != name:
            continue
        for ref in f.get("see", []):
            targets.append((fname, None, ref))
        for i, step in enumerate(f.get("steps", [])):
            for ref in step.get("see", []):
                targets.append((fname, i, ref))
    out = []
    for fname, idx, ref in targets:
        path = _os.path.join(source.SOPIA, ref)
        out.append({"flow": fname, "step": idx, "see": ref,
                    "resolves": _os.path.exists(path)})
    return {"pointers": out,
            "broken": [p for p in out if not p["resolves"]],
            "note": ("a pointer that does not resolve reads as though the reasoning was "
                     "written down; reported rather than repaired, since only a person "
                     "knows whether the target moved or the flow is wrong")}


def check(doc, name, done=()):
    """Where a flow stands, given what has been done.

    **Reports the NEXT step and what must hold before it** -- it does not verify
    that a step was performed, because a flow cannot see the work. Claiming
    otherwise would be the tool asserting something it has no witness for, which
    is the failure it exists to prevent in its own steps.
    """
    f = get(doc, name)
    if not f:
        return {"flow": name, "known": False,
                "note": "no flow named %r is recorded" % name}
    steps = f.get("steps", [])
    done = set(done)
    remaining = [s for i, s in enumerate(steps) if str(i) not in done and s["do"] not in done]
    nxt = remaining[0] if remaining else None
    return {"flow": name, "known": True, "steps": len(steps),
            "done": len(steps) - len(remaining),
            "next": nxt,
            "complete": nxt is None,
            "note": ("this reports the next step and what must hold before it. It cannot "
                     "verify a step was performed -- a flow has no witness to the work, "
                     "and claiming otherwise would be the tool asserting something it "
                     "cannot see")}
