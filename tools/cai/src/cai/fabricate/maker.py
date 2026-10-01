"""Adding content to a transcript, which is the opposite of removing it.

**Why this is a separate tool.** The owner's rejected proposal names the hazard
exactly: *what if we redacted the void between two characters with content?* It
would work, and it would make `redact` a tool that does two opposite things while
claiming one. **Not dishonest, but no longer honest by construction** -- and that
property is the point: a tool that can only remove cannot be asked to add.

**Its first caller was a measurement.** The re-root fidelity runs needed a
history that differed from the record, to ask whether an answer came from carried
content or from the shape of the conversation. Fabrication there is a condition
to vary, not an accident to avoid.

**The name does heavy lifting, and deliberately.** Owner, 2026-09-02: *fabricate*
means both **to manufacture** and **to deceive**, and this tool is exactly the
place where those two meanings meet. **Every use is manufacture; the failure mode
is deception**, and there is no version of the tool where that stops being true.

**So the word is a warning label that cannot fall off.** A neutral name --
`insert`, `compose`, `add` -- would describe the mechanism and hide the hazard,
and somebody would reach for it without the second meaning ever crossing their
mind.

**Everything it writes is marked, and the marking is not optional by default.**
A fabricated turn that cannot be told from a real one is the thing this whole
repository exists to prevent -- and `--true-silent` exists, warns, and breaks the
contract exactly as its namesake does elsewhere.
"""
import copy
import json
import os
import uuid

from cai.grammar import maic as fmt


def _record(sid, parent, text, role, cwd, at, marked=True, source=None):
    rec = {"parentUuid": parent, "isSidechain": False, "userType": "external",
           "cwd": cwd, "sessionId": sid, "version": "2.1.236", "gitBranch": "HEAD",
           "type": role,
           "message": ({"role": "user", "content": text} if role == "user" else
                       {"role": "assistant",
                        "content": [{"type": "text", "text": text}],
                        "model": "claude-opus-5"}),
           "uuid": str(uuid.uuid4()), "timestamp": at}
    if marked:
        rec["origin"] = {"kind": "fabricated"}
        rec["fabricated"] = True
        if source:
            rec["fabricatedFor"] = source
    return rec


def insert(lines, turns, at_index, cwd, timestamp, mode="marked", source=None):
    """Insert fabricated turns after `at_index`, re-parenting what follows.

    **Three modes, the same vocabulary used everywhere here:**

      marked       (default) each record carries `origin.kind: "fabricated"` and
                   a `fabricated` flag. Meta-level: it does not reach the model
                   (P1), so a measurement arm is unaffected by its presence,
                   while an operator and every tool can see it.
      loud         additionally announces itself in message content, for when
                   the subject is meant to know.
      true-silent  nothing. **This breaks the contract**, exactly as it does for
                   a cut: the result is indistinguishable from a real exchange
                   and no audit can recover that it was not.
    """
    if mode not in ("marked", "loud", "true-silent"):
        raise ValueError("mode must be marked, loud or true-silent")
    out = copy.deepcopy(lines)
    msgs = [i for i, r in enumerate(out) if r.get("type") in ("user", "assistant")]
    if not msgs:
        raise ValueError("no message records to insert against")
    if at_index < 0 or at_index >= len(out):
        raise ValueError("at_index %d is outside the transcript" % at_index)
    sid = next((r.get("sessionId") for r in out
                if r.get("type") in ("user", "assistant") and r.get("sessionId")), None)
    parent = out[at_index].get("uuid")
    made = []
    marked = mode != "true-silent"
    for role, text in turns:
        body = text
        if mode == "loud":
            body = ("⟦ FABRICATED ⟧ This turn did not occur. It was inserted by "
                    "`cai fabricate`. ⟦/FABRICATED⟧\n\n" + text)
        rec = _record(sid, parent, body, role, cwd, timestamp, marked=marked, source=source)
        made.append(rec)
        parent = rec["uuid"]
    nxt = at_index + 1
    if nxt < len(out) and out[nxt].get("parentUuid") is not None:
        out[nxt]["parentUuid"] = made[-1]["uuid"]
    out[at_index + 1:at_index + 1] = made
    return out, made


def insert_maic(lines, turns, at_index, cwd, timestamp, mode="marked", source=None):
    """The same insertion into a MAIC session: each turn as the `msg` the model replays
    and the transcript record a reader sees, placed after `at_index`.

    The three modes mean what they mean above. `marked` puts `origin.kind: "fabricated"`
    and the `fabricated` flag on both records, fields MAIC's loader ignores, so the
    model is unaffected and every tool can see them; `loud` says so in the text;
    `true-silent` writes nothing but the turn.
    """
    if mode not in ("marked", "loud", "true-silent"):
        raise ValueError("mode must be marked, loud or true-silent")
    out = copy.deepcopy(lines)
    if not any(r.get("type") in ("user", "assistant", "msg") for r in out):
        raise ValueError("no message records to insert against")
    if at_index < 0 or at_index >= len(out):
        raise ValueError("at_index %d is outside the transcript" % at_index)
    # A user record carries the provider, model and mode in effect; the nearest earlier
    # one says what they were at this point in the conversation.
    ref = next((r for r in reversed(out[:at_index + 1]) if r.get("type") == "user"), {})
    extra = {}
    if mode != "true-silent":
        extra = {"origin": {"kind": "fabricated"}, "fabricated": True}
        if source:
            extra["fabricatedFor"] = source
    made = []
    for role, text in turns:
        body = text
        if mode == "loud":
            body = ("\u27e6 FABRICATED \u27e7 This turn did not occur. It was inserted by "
                    "`cai fabricate`. \u27e6/FABRICATED\u27e7\n\n" + text)
        recs = fmt.turn(role, body, timestamp, extra)
        if role == "user":
            for k in ("provider", "model", "remote", "mode"):
                if k in ref:
                    recs[1][k] = ref[k]
        made.extend(recs)
    out[at_index + 1:at_index + 1] = made
    return out, made


def audit(lines):
    """Every fabricated record a transcript admits to.

    **Silence here is not evidence of authenticity**, which is the same limit the
    provenance audit carries: a `true-silent` fabrication records nothing, and
    nothing downstream can recover that it happened.
    """
    found = [{"uuid": r.get("uuid"), "type": r.get("type"),
              "for": r.get("fabricatedFor")}
             for r in lines if r.get("fabricated")]
    return {"fabricated": len(found), "records": found,
            "note": ("silence is not evidence of authenticity -- a true-silent "
                     "fabrication records nothing, and no audit can recover it")}
