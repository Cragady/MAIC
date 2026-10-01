"""Fences: one definition, composed from a shape and a notation.

**Built because the two existing definitions had already diverged.** `redact` and
`notation.audit` each carried a regex for the same concept, and they were not the
same regex -- one captured the `/` that distinguishes an opener from a closer and
one did not. **Two definitions of one thing is a duplication; two that disagree
is a defect waiting for the case that tells them apart.**

**What a fence is, is a shape:** a delimiter ALONE ON A LINE, `---- <name> ----`,
optionally closing with `/`. **Which fences exist, is a notation:** `Source`,
`Draft`, `Raw Source`, the `comms` family. This composes the two and owns
neither.

**Positional, never textual.** The same characters inside a sentence are a
mention of the convention rather than a use of it -- measured at fourteen
fence-shaped lines against twelve inline mentions in the thread that produced the
rule, so a substring match would have mangled the discussion that designed it.
"""
import re

# The shape. One definition, and it captures the closer marker, because telling
# an opener from a closer is the thing the divergent copy could not do.
PATTERN = re.compile(r"^-{4}\s*(?P<closer>/?)\s*(?P<name>[A-Za-z][A-Za-z ]*?)\s*-{4}\s*$")

NEUTRALISED = "⟦ neutralised: %s%s ⟧"


def match(line):
    """The fence on this line, or None. Whitespace-tolerant, position-strict."""
    m = PATTERN.match(line.strip())
    if not m:
        return None
    return {"name": m.group("name"), "closer": bool(m.group("closer")),
            "raw": line.strip()}


def is_fence(line):
    return match(line) is not None


def names(doc=None):
    """Fence names that notation knows about. Empty if it cannot be reached."""
    try:
        from cai.notation import store as nstore
        doc = doc or nstore.load()
        out = set()
        for e in nstore.live(doc):
            for sym in e["symbols"]:
                m = PATTERN.match(sym.strip())
                if m:
                    out.add(m.group("name").lower())
        return out
    except Exception:                                            # noqa: BLE001
        return set()


def scan(text, known=None):
    """Fence lines in a body of text, split into recognised and unrecognised."""
    known = names() if known is None else known
    seen, unknown = [], []
    for n, line in enumerate(text.split("\n"), 1):
        f = match(line)
        if not f:
            continue
        (seen if f["name"].lower() in known else unknown).append(dict(f, line=n))
    return {"recognised": seen, "unrecognised": unknown}


def neutralise(text):
    """Rewrite fence markers into evidence that one was there.

    **This is itself a redaction**: carried history is edited, and the mark says
    what was done rather than letting the edit pass as untouched material.
    """
    out, n = [], 0
    for line in text.split("\n"):
        f = match(line)
        if f:
            n += 1
            out.append(NEUTRALISED % ("/" if f["closer"] else "", f["name"]))
        else:
            out.append(line)
    return "\n".join(out), n
