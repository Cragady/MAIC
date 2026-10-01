"""Flushing a transient record onto the board, and checking it is safe to delete.

**Owner, 2026-09-02:** *"if a temp file comes up, and it wants things flushed
out, it can move items to the board. That way, we have a board to check against,
and a file to delete once that check clears... as long as it's on the board that
means it's visible to be worked on."*

THE PROBLEM THIS SOLVES
-----------------------
**A file that declares itself disposable can be the only home for open work.**
`pre-compact-state.md` says of itself that it is disposable and should be
deleted once folded -- and it is currently the sole durable record of at least
two designed-unbuilt items. Deleting it per its own contract would lose them;
keeping it recreates the two-homes drift it was meant to avoid. Neither is
acceptable, and the marker breaks the deadlock: **transfer first, then delete on
a check rather than on a judgement.**

TWO GATES, DELIBERATELY LOOSE, STRONG TOGETHER
----------------------------------------------
**Every item carries `▣`**, and **every `▣` verifies against the board.** The
owner's rule: two moderate gates make one strong one. Marking alone is a claim
by the party making it -- the same weakness as an internal freeze stamp or a
locally-signed grant -- so the claim is checked against something outside the
file.

WHY THE MARK CARRIES THE BOARD'S OWN WORDING
--------------------------------------------
    - some item ▣ IMPROVEMENTS.md :: the phrase as it appears on the board

**Because a transfer rewords.** An item moved onto the board is written in the
board's vocabulary, not the temp file's, so matching the temp file's text against
the board fails on exactly the transfers that were done properly. That is not
hypothetical: reconciling one state file against its design documents produced
**five false negatives in a day**, every one of them a search for the state
file's shorthand where the document used its own words.

So the person doing the transfer states the landing phrase. **That is the
reconciliation cost paid once, deliberately, at the moment the knowledge exists**
-- rather than paid repeatedly by whoever reads the file later without it.

**Without `::` the item's own text is used**, which is honest but fragile, and
the report says so rather than letting a lucky match look like a verified one.
"""
import os
import re

MARK = "▣"

#: **An item is a LIST ENTRY.** A transient record lists what it holds and uses
#: headings to organise; counting headings made the file's own title an untransferred
#: item, which is noise that would block deletion forever. Records that list under
#: headings instead can opt in with `headings=True`.
ITEM = re.compile(r"^\s*(?:[-*+]\s+|\d+[.)]\s+)(?P<text>\S.*)$")
ITEM_WITH_HEADINGS = re.compile(r"^\s*(?:[-*+]\s+|\d+[.)]\s+|#{2,6}\s+)(?P<text>\S.*)$")

#: `▣ path :: phrase`, with both tails optional.
CLAIM = re.compile(r"▣\s*(?P<path>[^\s:][^:]*?)?\s*(?:::\s*(?P<phrase>.+?))?\s*$")


def flatten(text):
    return re.sub(r"\s+", " ", text).strip().lower()


def items(text, headings=False):
    """Every item in a transient record, with its mark and claim if it has one."""
    pattern = ITEM_WITH_HEADINGS if headings else ITEM
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        m = pattern.match(line)
        if not m:
            continue
        body = m.group("text")
        marked = MARK in body
        path = phrase = None
        if marked:
            c = CLAIM.search(body)
            if c:
                path = (c.group("path") or "").strip() or None
                phrase = (c.group("phrase") or "").strip() or None
                # **Backticks are stripped, so a phrase can be quoted.** SOPIA's
                # form check requires a prose line to end in terminal
                # punctuation, and a line ending in a bare landing phrase fails
                # it -- 24 violations, introduced and committed before I read my
                # own check output. Quoting the phrase satisfies both rules at
                # once, and quoting a literal string was the natural form anyway.
                if phrase:
                    phrase = phrase.strip("`\u201c\u201d\"'").strip() or None
            body = body.split(MARK)[0].strip()
        out.append({"line": n, "text": body, "marked": marked,
                    "board": path, "phrase": phrase})
    return out


def verify(entry, root=".", default_board=None):
    """Does this item's claim hold against the board it names?

    **A claim that cannot be checked is reported as unchecked, never as passing.**
    An unverifiable pass is the failure this whole gate exists to prevent.
    """
    board = entry.get("board") or default_board
    if not board:
        return {"ok": False, "why": "marked, but names no board and no default was given. "
                                    "The claim cannot be checked, so it does not pass."}
    p = os.path.join(root, board)
    if not os.path.exists(p):
        return {"ok": False, "board": board,
                "why": "names a board that does not exist here -- the same defect as a flow "
                       "pointing at a document that is not there"}
    try:
        with open(p, encoding="utf-8", errors="replace") as fh:
            hay = flatten(fh.read())
    except OSError as e:
        return {"ok": False, "board": board, "why": "unreadable: %s" % e}
    needle = entry.get("phrase") or entry["text"]
    found = flatten(needle) in hay
    exact = entry.get("phrase") is not None
    return {"ok": found, "board": board, "matched_on": "stated phrase" if exact else "item text",
            "why": (None if found else
                    ("the stated phrase is not on that board" if exact else
                     "the item's own text is not on that board. A transfer rewords into the "
                     "board's vocabulary, so matching the temp file's wording fails on exactly "
                     "the transfers done properly -- state the landing phrase with `:: `"))}


def check(text, root=".", default_board=None, headings=False):
    """**Is this transient record safe to delete?**

    Safe means: every item marked, and every mark verified. One unmarked item
    blocks it, which is the safe direction -- clutter costs less than an item
    nobody knows is open.
    """
    found = items(text, headings=headings)
    unmarked = [e for e in found if not e["marked"]]
    results, failed = [], []
    for e in found:
        if not e["marked"]:
            continue
        v = verify(e, root=root, default_board=default_board)
        results.append(dict(e, verification=v))
        if not v["ok"]:
            failed.append(dict(e, verification=v))
    safe = not unmarked and not failed and bool(found)
    return {
        "items": len(found),
        "marked": len(results),
        "unmarked": [{"line": e["line"], "text": e["text"]} for e in unmarked],
        "failed_claims": [{"line": e["line"], "text": e["text"],
                           "why": e["verification"]["why"]} for e in failed],
        # **Passing claims say how they matched.** Without it, a claim verified
        # against a stated landing phrase looks identical to one that happened to
        # match the temp file's own wording -- and the second is fragile, since
        # any rewording of the board breaks it. A reader deciding whether to
        # trust the gate needs to see which kind each one was.
        "verified": [{"line": e["line"], "text": e["text"],
                      "board": e["verification"].get("board"),
                      "matched_on": e["verification"].get("matched_on")}
                     for e in results if e["verification"]["ok"]],
        "safe_to_delete": safe,
        "verdict": ("every item is on the board and every claim verifies -- safe to delete"
                    if safe else
                    "NOT safe to delete" + (
                        "; the record holds no items at all, which is not the same as being "
                        "flushed" if not found else "")),
        "why": "one unmarked item blocks deletion. Clutter costs less than an item nobody "
               "knows is open, so this fails toward keeping the file.",
    }
