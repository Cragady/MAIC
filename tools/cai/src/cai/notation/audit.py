"""The audit, and the reader's map it produces.

**Optimistically read, pessimistically audit.** Nothing here runs on a schedule.
An unparseable symbol -- one that will not resolve against the live definitions
-- is itself the error signal, and one such symbol is evidence that more are
likely, since a definition rarely drifts alone.

**The map reports difference. It never adopts.** At dissolution the operator
drops it or takes some part of it as definition additions, changes and
deletions. Nothing here promotes anything into the dictionary, which is what
stops a guess becoming a definition by attrition.
"""
import json
import os
import re

from cai.notation import store

# One definition, in cai.fence. This file used to carry its own, and the two had
# diverged: that copy could not tell an opener from a closer.
from cai import fence as _fence  # noqa: E402

FENCE_LINE = _fence.PATTERN

CONFIDENCE_CLASSES = {
    1: "agrees with the inferred reading",
    2: "deviates consistently -- the definition is suspect",
    3: "deviates inconsistently -- the material is suspect",
    4: "contradicts the inferred reading outright",
    5: "confirmed by a person",
}


def is_fence_symbol(symbol):
    """Fences match positionally; glyphs match literally.

    A fence is a delimiter ALONE ON A LINE, so a substring search for one would
    also match every discussion of the convention -- measured at fourteen fences
    against twelve mentions in the thread that produced the rule.
    """
    return symbol.strip().startswith("----")


def _fence_name(symbol):
    m = FENCE_LINE.match(symbol.strip())
    return m.group("name") if m else None


def scan_text(text, index):
    """Occurrences of known and unknown markers in one body of text.

    Returns (known, unknown) as symbol -> [line numbers].
    """
    known, unknown = {}, {}
    fence_names = {}
    for sym in index:
        if is_fence_symbol(sym):
            n = _fence_name(sym)
            if n:
                fence_names[n.lower()] = sym
    for lineno, line in enumerate(text.split("\n"), 1):
        stripped = line.strip()
        m = FENCE_LINE.match(stripped)
        if m:
            name = m.group("name").lower()
            canonical = fence_names.get(name)
            target = known if canonical else unknown
            key = canonical or ("---- %s ----" % m.group("name"))
            target.setdefault(key, []).append(lineno)
            continue
        for sym in index:
            if is_fence_symbol(sym):
                continue
            if sym in line:
                known.setdefault(sym, []).append(lineno)
    return known, unknown


def audit(paths, doc=None, at=None):
    """Scan files, resolve what is found, and build the map skeleton.

    **What this does mechanically**: find markers, resolve them against the live
    definitions, count occurrences and record where they are.

    **What it does not do**: derive a definition from surrounding context, or
    judge misuse. Both require reading the material rather than matching it, and
    both are left to an agent or a person -- an inferred entry is a proposal
    until confirmed, and one produced without reading would be a guess wearing a
    count.
    """
    doc = doc or store.load()
    index = store.symbols(doc, at)
    known, unknown, scanned = {}, {}, []
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        scanned.append(path)
        k, u = scan_text(text, index)
        for sym, lines in k.items():
            known.setdefault(sym, []).extend((path, n) for n in lines)
        for sym, lines in u.items():
            unknown.setdefault(sym, []).extend((path, n) for n in lines)
    entries = []
    for sym, hits in sorted(unknown.items()):
        entries.append({
            "symbol": sym,
            "resolution": None,
            "kind": None,
            "occurrences": len(hits),
            "confidence": [],
            "inferred": False,
            "sites": [{"file": p, "line": n} for p, n in hits[:10]],
            "note": "unresolved against the live definitions. Derivation needs a reading "
                    "of the surrounding material and is not performed here.",
        })
    return {
        "scanned": len(scanned),
        "known": {s: len(h) for s, h in sorted(known.items())},
        "unresolved": len(entries),
        "entries": entries,
        "legend": CONFIDENCE_CLASSES,
        "legend_taken_at": store.now().strftime("%Y-%m-%dT%H:%M:%SZ"),
        "note": "The map reports difference. Adoption is the operator's act.",
    }


def grade(entry):
    """Render a confidence tuple as `<count> <class>` pairs.

    A tuple rather than a score, deliberately: a score is a summary of guesses
    and reads like advice, which the map must not give. Counts are what was
    observed and cannot be mistaken for a recommendation.
    """
    return "  ".join("%d %d" % (count, cls) for count, cls in entry.get("confidence", []))


def observe(entry, cls, count=1):
    """Record an observation against an entry, moving its signal.

    A rating rises and falls: a later reading that agrees pushes it up, one that
    contradicts adds to the against-classes. A frozen rating can never be
    revised, which is why there is one live map rather than a pile of stamps.
    """
    if cls not in CONFIDENCE_CLASSES:
        raise ValueError("class must be one of %s" % sorted(CONFIDENCE_CLASSES))
    conf = entry.setdefault("confidence", [])
    for pair in conf:
        if pair[1] == cls:
            pair[0] += count
            return entry
    conf.append([count, cls])
    conf.sort(key=lambda p: p[1])
    return entry


def purge(mapping):
    """Drop everything with no pending decision. Kept entries are slated ones.

    An entry survives only where a decision is still open -- to be updated,
    kept, or dropped. Everything else goes, which is what stops the map becoming
    a permanent second dictionary. The pre-compact state file carried the same
    contract, went unpurged, and drifted inside a day.
    """
    kept = [e for e in mapping["entries"] if e.get("slated")]
    return dict(mapping, entries=kept, purged=len(mapping["entries"]) - len(kept))
