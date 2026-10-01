"""Structural guards for an edit, checked before the write.

**The failure this exists for, recorded 2026-09-01: four functions destroyed in
ten minutes by index-based slice replacement**, each one removing more than its
author intended and each one noticed only when a test failed afterwards. Twice
after having already noticed the pattern.

**What finally held was not more care.** It was asserting that the structure was
unchanged and refusing to write when it was not. So that assertion belongs in
the tool rather than in whoever is editing.

**Two guards, and neither is about the text being replaced:**

  anchored     the old text must appear EXACTLY once. Zero means the edit
               silently did nothing; more than one means it hit something it
               was not aimed at.

  structural   the count of top-level definitions (or headings) must not change
               unless the caller says by how much. A replacement that removes a
               function nobody mentioned is the defect above.
"""
from cai import safewrite
import ast
import json
import os
import re

PY_DEF = re.compile(r"^(?:def |class |[A-Z][A-Z0-9_]* = )", re.M)


def parses(text, path):
    """Does the result still parse as what the file claims to be?

    **Structure counts cannot catch a replacement that was itself garbage.** The
    guard compares definition counts before and after, so text pasted in from a
    mangled heredoc -- a stray terminator, a truncated block -- passes with delta
    0 and lands a file that will not import. That happened: an anchor file was
    corrupted upstream, `cai edit` read it as legitimate replacement text, and
    applied it faithfully. Garbage in, garbage applied, structure unchanged.

    So the last gate is the cheapest one available: the file must still be the
    kind of thing it was. Returns None for formats with no parser here -- an
    unknown format is not a finding, and refusing one would make the guard
    useless on prose.

    **The limit, stated rather than discovered later: corruption that is still
    valid syntax passes.** A stray heredoc terminator on its own line is a bare
    name expression and parses fine; the same terminator where a block body
    belongs does not. So this catches the structural half and nothing more, and
    it is a last gate rather than a reason to trust the input.
    """
    ext = os.path.splitext(path)[1].lower()
    try:
        if ext == ".py":
            ast.parse(text)
        elif ext == ".json":
            json.loads(text)
        else:
            return None
    except (SyntaxError, ValueError) as e:
        return str(e)
    return ""
MD_HEAD = re.compile(r"^#{1,6} ", re.M)


def structure(text, path=""):
    """A count of the things an edit should not silently remove."""
    ext = os.path.splitext(path)[1].lower()
    if ext in (".md", ".markdown"):
        return {"kind": "headings", "count": len(MD_HEAD.findall(text))}
    return {"kind": "definitions", "count": len(PY_DEF.findall(text))}


def apply(path, old, new, expect_delta=0, dry_run=False, force=False):
    """Replace `old` with `new`, refusing anything the caller did not name.

    Returns a report. **Every refusal leaves the file untouched**, which is the
    point: an edit that half-applied is worse than one that did not run, because
    the failure is discovered later and somewhere else.
    """
    report = {"path": path, "ok": False, "findings": []}
    token = safewrite.stat_token(path)
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as e:
        report["findings"].append({"level": "refuse", "detail": "unreadable: %s" % e})
        return report

    n = text.count(old)
    report["occurrences"] = n
    if n == 0:
        report["findings"].append({"level": "refuse",
                                   "detail": "the anchor appears 0 times -- this edit would "
                                             "do nothing, which is what a silently failed "
                                             "replacement looks like from outside"})
        return report
    if n > 1:
        report["findings"].append({"level": "refuse",
                                   "detail": "the anchor appears %d times -- it would hit "
                                             "more than it names. Make it unique." % n})
        return report

    before = structure(text, path)
    after_text = text.replace(old, new)
    after = structure(after_text, path)
    delta = after["count"] - before["count"]
    report["structure"] = {"kind": before["kind"], "before": before["count"],
                           "after": after["count"], "delta": delta}
    if delta != expect_delta:
        report["findings"].append({
            "level": "refuse",
            "detail": ("%s count would go %d -> %d (delta %+d), and %+d was declared. "
                       "A replacement that removes something nobody mentioned is the "
                       "defect this guard exists for."
                       % (before["kind"], before["count"], after["count"], delta,
                          expect_delta))})
        return report

    broken = parses(after_text, path)
    report["parses"] = "n/a" if broken is None else (broken == "")
    if broken:
        report["findings"].append({
            "level": "refuse",
            "detail": ("the result would not parse: %s. The structure delta was as "
                       "declared, so the counts alone accepted it -- which is what a "
                       "corrupted replacement looks like from inside this guard."
                       % broken)})
        return report

    # **This is the in-place writer where content genuinely can be lost.** The
    # anchor, structure and parse gates all prove the NEW content is well formed;
    # none of them keeps a copy of the old. Git is the backup, which is fine until
    # the file is not tracked -- and nothing was checking that.
    ok_rec, why_rec = safewrite.recoverable(path, force=force)
    report["recoverable"] = ok_rec
    if not ok_rec:
        report["findings"].append({"level": "refuse", "detail": why_rec})
        return report

    # The file must not have moved between the read and this write. An anchored
    # replacement computed against stale content lands on a file that no longer
    # says what the anchor matched.
    moved_ok, moved_why = safewrite.unchanged_since(path, token)
    if not moved_ok and not force:
        report["findings"].append({"level": "refuse", "detail": moved_why})
        return report

    report["ok"] = True
    report["dry_run"] = dry_run
    if not dry_run:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(after_text)
        report["written"] = True
    return report
