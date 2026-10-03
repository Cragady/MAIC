"""The projection itself. No function here opens a file for writing."""
import json
import re

from cai.grammar import maid
from cai.grammar import records as grecords
from cai.redact import redactor

HEADER = re.compile(r"(?m)^── L\d+ · (?:user|assistant) ──$")
ROLES = ("user", "assistant")


def records(data):
    """Parse JSONL into records. **Delegates to the grammar.**

    This function is where the dict-only rule was first fixed, after a peer
    reported that a bare scalar reached `.get()` and raised. It was fixed HERE
    and in no other loader, so four others kept their own answers. The rule lives
    in `cai.grammar.records` now and this is the caller.

    A MAID session (detected by content, `cai.grammar.maid`) is projected onto the
    Claude Code grammar here, so every selection and every compaction slice below
    works on it unchanged: a `compact` record is the boundary, a `tool` record is
    the tool traffic.
    """
    recs = grecords.load(data)
    return maid.to_claude(recs) if maid.is_maid(recs) else recs


def boundaries(recs):
    """Indices of every compaction summary. **A transcript can have several.**

    Six transcripts on this machine carry more than one; two carry three. The
    flags below anchor to the LAST, which leaves the middle regions unreachable
    -- and lumps live conversation together with earlier summaries when there is
    more than one boundary.
    """
    return [i for i, r in enumerate(recs) if r.get("isCompactSummary")]


def regions(recs):
    """Every span between boundaries, as `(index, start, stop)`.

    **With B boundaries there are B+1 regions**, numbered 0 upward:

        region 0      before the first boundary -- the oldest material
        region k      between boundary k and k+1
        region B      after the last -- the same span as `since_compaction`

    **Region B-1 is usually the one wanted and was the hardest to reach.** It is
    the live conversation the most recent compaction replaced; `before_compaction`
    returns it MIXED with every earlier region, so summaries of summaries arrive
    folded in with the real material.
    """
    edges = [0] + boundaries(recs) + [len(recs)]
    return [(k, edges[k], edges[k + 1]) for k in range(len(edges) - 1)]


def slice_bounds(recs, since_compaction=False, before_compaction=False, chunk=None):
    """Which records a compaction flag selects. **They are opposite halves.**

    **Reported by peer session `RE: Flow`, 2026-09-04, and it is a real gap.**
    A compaction replaces everything BEFORE its boundary with a summary. So:

        since_compaction    the boundary onward -- the summary and what came
                            after it. **This is what a resumed agent already
                            holds.** Right for handing a fresh agent the live
                            chunk; wrong for recovering anything.
        before_compaction   everything before the boundary -- **what the
                            summary replaced.** This is the recovery slice, and
                            until now no flag selected it.

    **The help called `--since-compaction` \"only what the last compaction
    dropped\", which is backwards**: it gives what SURVIVED. Anyone reaching for
    recovery followed that wording to the half they already had.

    **This session hit the gap and did not notice.** It hand-rolled a Python
    extraction for a pre-boundary chunk rather than using the tool -- the tool
    could not do it, and the workaround hid that.
    """
    if chunk is not None:
        regs = regions(recs)
        if not -len(regs) <= chunk < len(regs):
            raise ValueError(
                "chunk %d does not exist: this transcript has %d boundaries and therefore "
                "%d regions, numbered 0..%d. Negative indices count from the end, so -1 is "
                "the newest and -2 is what the last compaction replaced."
                % (chunk, len(regs) - 1, len(regs), len(regs) - 1))
        return regs[chunk][1], regs[chunk][2]
    bounds = boundaries(recs)
    if not bounds:
        return 0, len(recs)
    if before_compaction:
        return 0, bounds[-1]
    if since_compaction:
        return bounds[-1], len(recs)
    return 0, len(recs)


def messages(recs, since_compaction=False, before_compaction=False, chunk=None):
    """The ordered message texts, walked straight off the records.

    Independent of `project()` on purpose: this is the witness, and a witness
    sharing an implementation with the thing it witnesses is not one.
    """
    start, stop = slice_bounds(recs, since_compaction, before_compaction, chunk)
    out = []
    for rec in recs[start:stop]:
        if rec.get("type") not in ROLES:
            continue
        m = rec.get("message") or {}
        c = m.get("content")
        body = c if isinstance(c, str) else "\n".join(
            b.get("text", "") for b in (c or [])
            if isinstance(b, dict) and b.get("type") == "text")
        if body.strip():
            out.append(body)
    return out


def signature(texts):
    return re.sub(r"\s+", " ", "\n".join(texts)).strip()


def read(data, select="conversation", since_compaction=False, max_block=None,
         before_compaction=False, chunk=None):
    """Project, and report whether every message survived.

    Returns `(text, report)`. **Nothing is written anywhere**; the caller decides
    what to do with the text, and the caller cannot be this module.
    """
    recs = records(data)
    if not recs:
        return "", {"records_in": 0, "signature_held": False,
                    "note": "no transcript records found -- every line either failed to "
                            "parse or was not an object. If this file was a transcript, "
                            "it is not one now."}
    offset = 0
    if before_compaction or chunk is not None:
        # `project` slices forward from a boundary; any other span is its
        # complement or a middle, so the records are cut before it is called.
        start, stop = slice_bounds(recs, before_compaction=before_compaction, chunk=chunk)
        recs = recs[start:stop]
        since_compaction = False
        offset = start
    result = redactor.project(recs, select=select, since_compaction=since_compaction,
                              neutralise=False, max_block=max_block, offset=offset)
    text = result["text"]
    want = signature(messages(recs, since_compaction=since_compaction))
    held = True if select != "conversation" else want in signature([HEADER.sub("", text)])
    return text, {
        "records_in": len(recs), "records_emitted": result["records_emitted"],
        # Offset back to the SOURCE, so the report agrees with the `Ln` markers
        # in the text. It said 1 for every chunk while the markers were correct.
        "started_at_record": result["started_at_record"] + offset,
        "select": select, "since_compaction": bool(since_compaction),
        "before_compaction": bool(before_compaction), "chunk": chunk,
        "bytes_in": len(data), "bytes_out": len(text),
        "ratio": round(len(text) / len(data), 4) if data else None,
        "signature_held": held,
        "note": ("every message survived; only the representation moved" if held else
                 "messages did not survive the projection -- do not rely on this output"),
    }
