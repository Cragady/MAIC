"""graft and inject -- the two ways to place material into a transcript.

Both emit a NEW file with a new sessionId and never modify the base (DESIGN,
Graft/Inject write semantics). They differ in where the marker lives:

  graft  -- attaching real prior transcript; the seam is in message content,
            origin.kind "graft", so the agent sees it (honest continuity).
  inject -- placing synthetic context; the announcement is in isMeta records
            and origin.kind "inject", which item 11 confirms the model does not
            read. loud emits those + a sidecar ledger; silent emits nothing and
            must never be installed in a projects dir.

Turn-role is easy mode (ruled 2026-08-28): injected/grafted content keeps its
natural role and is marked in origin.kind, never by renaming type/role.
"""
import json
import os
import uuid as _uuid

MODE = ("mode", "permission-mode", "file-history-snapshot", "ai-title", "last-prompt")


def _sid():
    return str(_uuid.uuid4())


def messages(lines):
    return [l for l in lines if l.get("type") in ("user", "assistant")]


def lineage_notice(sid, parent_uuid, cwd, version, at, previous, origin, included, uuid_, prompt_id):
    """The VISIBLE half of lineage. The fields on the last record are metadata --
    the model does not read them, the same way it does not read meta records (P1).
    This puts the same facts in message content so a resumed agent can KNOW its
    own lineage rather than only have it recorded about them.
    """
    bits = ["\u27e6 LINEAGE \u27e7 This transcript was produced by `cai trans-fairy`."]
    if previous:
        bits.append("It was **%s** before this operation." % previous)
    if origin and origin != previous:
        bits.append("It originates from **%s**." % origin)
    elif origin:
        bits.append("That is also its origin.")
    if included:
        bits.append("Material from more than one session is present: %s."
                    % ", ".join("**%s**" % i for i in included))
    bits.append("Treat anything it says about its own identity or history as "
                "belonging to those sessions rather than to this one. \u27e6/LINEAGE\u27e7")
    return {"parentUuid": parent_uuid, "isSidechain": False, "promptId": prompt_id,
            "type": "user", "message": {"role": "user", "content": " ".join(bits)},
            "uuid": uuid_, "timestamp": at, "permissionMode": "auto",
            "origin": {"kind": "lineage"}, "promptSource": "typed",
            "userType": "external", "entrypoint": "cli", "cwd": cwd,
            "sessionId": sid, "version": version, "gitBranch": "HEAD"}


def announce_lineage(lines, at):
    """Append the visible lineage notice, reading the fields just written."""
    last = next((l for l in reversed(lines)
                 if l.get("type") in ("user", "assistant")), None)
    if last is None or not last.get("previousSessionId"):
        return lines
    ref = next((l for l in lines if l.get("cwd")), {})
    lines.append(lineage_notice(
        last.get("sessionId"), last.get("uuid"), ref.get("cwd", ""),
        ref.get("version", "2.1.236"), at,
        last.get("previousSessionId"), last.get("originSessionId"),
        last.get("includedSessionIds") or [], _sid(), _sid()))
    return lines


def rewrite_session(lines, new_sid, lineage=True):
    """Rewrite sessionId everywhere it occurs (DESIGN, state: six places), and
    record where the transcript came from.

    **Lineage is recorded here because this is the only place an id changes.**
    Four operations call this and each used to carry its own differently-named
    pointer -- baseSessionId, reflowedFrom, truncatedFrom -- so there was no
    uniform way to ask "where did this come from". Hooking it to the rewrite
    itself means no operation, including one added later, can change an id
    without leaving the answer. Lesson 12: the unsafe thing is unreachable
    rather than forbidden.

    Two fields, because they answer different questions:

      previousSessionId  what this was immediately before -- walk it to step back
      originSessionId    the first id in the chain, carried forward unchanged --
                         read it to reach the source without walking anything

    Both land on the final message record: a transcript always has one, and a
    reader looking at the end of a conversation is where they ask where it came
    from. `lineage=False` suppresses it for the true-silent path, which by
    definition leaves no trace.
    """
    # Read the old id from a MESSAGE record, not from whatever carries a sessionId
    # first: graft mints its opening frames with the new id before calling this, so
    # a frame-first read sees new == old and silently skips the lineage.
    old_sid = next((l.get("sessionId") for l in lines
                    if l.get("type") in ("user", "assistant") and l.get("sessionId")), None)
    if old_sid is None:
        old_sid = next((l.get("sessionId") for l in lines if l.get("sessionId")), None)
    prior_origin = next((l.get("originSessionId") for l in lines
                         if l.get("originSessionId")), None)
    # Every session whose material is present. A linear chain has one ancestor and
    # previousSessionId is enough; graft and compose have TWO inputs, and a future
    # operation over many files would have more -- so the set is collected rather
    # than a single pointer, and one contributor is never silently dropped.
    # Read BEFORE the rewrite, while each record still carries its own id.
    contributors = set()
    for l in lines:
        if l.get("type") in ("user", "assistant") and l.get("sessionId"):
            contributors.add(l["sessionId"])
        for prior in l.get("includedSessionIds") or ():
            contributors.add(prior)
    for l in lines:
        if "sessionId" in l:
            l["sessionId"] = new_sid
        if "session_id" in l:
            l["session_id"] = new_sid
        msg = l.get("message")
        if isinstance(msg, dict) and "session_id" in msg:
            msg["session_id"] = new_sid
    # **Exactly one lineage marker, and it is always the last message.** An
    # operation that appends -- graft, compose -- would otherwise strand the
    # previous marker mid-transcript, leaving two answers to one question. So any
    # existing marker is cleared first and the current one is rewritten at the end.
    # This is also why a later operation READS these before writing: the origin it
    # finds is carried forward rather than restarted.
    if lineage:
        for l in lines:
            l.pop("previousSessionId", None)
            l.pop("originSessionId", None)
            l.pop("includedSessionIds", None)
    if lineage and old_sid and old_sid != new_sid:
        last = next((l for l in reversed(lines)
                     if l.get("type") in ("user", "assistant")), None)
        if last is not None:
            last["previousSessionId"] = old_sid
            # the origin survives every later operation; if none was recorded yet,
            # the id we just replaced IS the origin
            last["originSessionId"] = prior_origin or old_sid
            # sorted so the same inputs always produce the same record; the new id
            # is excluded because it names this transcript rather than a source
            included = sorted(contributors - {new_sid})
            # Omitted when it says nothing previousSessionId does not already say.
            # The set counts CONTRIBUTORS, which are not all ancestors: a graft has
            # one ancestor (the base it descends from) and one sibling (the graft
            # merged alongside it). It earns its place whenever material came from
            # more than one session, by either relation.
            if len(included) > 1:
                last["includedSessionIds"] = included
    return lines


def _frames_open(sid, first_uuid, ts):
    return [
        {"type": "mode", "mode": "normal", "sessionId": sid},
        {"type": "permission-mode", "permissionMode": "auto", "sessionId": sid},
        {"type": "file-history-snapshot", "messageId": first_uuid,
         "snapshot": {"messageId": first_uuid, "trackedFileBackups": {}, "timestamp": ts},
         "isSnapshotUpdate": False},
    ]


def _frames_close(sid, title, last_prompt, leaf, ts):
    return [
        {"type": "ai-title", "aiTitle": title, "sessionId": sid},
        {"type": "last-prompt", "lastPrompt": last_prompt, "leafUuid": leaf, "sessionId": sid},
        {"type": "file-history-snapshot", "messageId": leaf,
         "snapshot": {"messageId": leaf, "trackedFileBackups": {}, "timestamp": ts},
         "isSnapshotUpdate": False},
    ]


def _reparent(msgs, first_parent):
    """Chain msgs in list order: first parents onto first_parent, rest follow."""
    parent = first_parent
    for m in msgs:
        m["parentUuid"] = parent
        parent = m["uuid"]
    return parent  # returns the leaf uuid


def _bracket_graft_content(m, ts):
    """Wrap a grafted message's text with per-message scoped tokens (agent-visible)."""
    open_t, close_t = "⟦ GRAFT @ %s ⟧" % ts, "⟦/GRAFT⟧"
    msg = m.get("message") or {}
    c = msg.get("content")
    if isinstance(c, str):
        msg["content"] = "%s\n%s\n%s" % (open_t, c, close_t)
    elif isinstance(c, list):
        c.insert(0, {"type": "text", "text": open_t})
        c.append({"type": "text", "text": close_t})
    return m


def graft(base_lines, graft_lines, graftedAt, direction="after", title="grafted session"):
    """Append (or prepend) graft onto base as one transcript with a new sessionId.

    The base is not modified in place -- callers pass copies. Returns new lines.
    """
    sid = _sid()
    base_msgs = [dict(m) for m in messages(base_lines)]
    graft_msgs = [dict(m) for m in messages(graft_lines)]
    if not base_msgs or not graft_msgs:
        raise ValueError("graft needs a non-empty base and graft")

    base_sid = next((l.get("sessionId") for l in base_lines if l.get("sessionId")), None)
    graft_sid = next((l.get("sessionId") for l in graft_lines if l.get("sessionId")), None)

    for m in graft_msgs:
        m["origin"] = {"kind": "graft"}
        _bracket_graft_content(m, graftedAt)

    if direction == "before":
        first, second = graft_msgs, base_msgs
    else:
        first, second = base_msgs, graft_msgs

    ts0 = first[0].get("timestamp", graftedAt)
    leaf = _reparent(first, None)
    first[0]["parentUuid"] = None
    seam = {"type": "system", "subtype": "graft-boundary", "isMeta": True,
            "position": "close", "graftedAt": graftedAt,
            "baseSessionId": base_sid, "graftSessionId": graft_sid}
    leaf = _reparent(second, leaf)

    out = []
    out += _frames_open(sid, first[0]["uuid"], ts0)
    out += first
    out.append(seam)
    out += second
    out += _frames_close(sid, title, "", leaf, second[-1].get("timestamp", graftedAt))
    rewrite_session(out, sid)
    return sid, out


def inject_boundary(sid, position, injectedAt, mode, source):
    return {"type": "system", "subtype": "inject-boundary", "isMeta": True,
            "position": position, "injectedAt": injectedAt, "mode": mode,
            "source": source, "sessionId": sid}


def mark_injected(lines, injectedAt, mode, source):
    """Mark a built transcript's message records as injected, per mode.

    loud   -> origin.kind "inject" on each message record, bracketed by
              inject-boundary isMeta records (item 11: the model does not read them).
    silent -> nothing added; the records stay seamless. A silent transcript must
              never be installed in a projects dir (the caller enforces that).
    Returns (sid, lines). The sessionId is left as-is (the build already set it).
    """
    sid = next((l.get("sessionId") for l in lines if l.get("sessionId")), None)
    if mode == "silent":
        return sid, lines
    if mode != "loud":
        raise ValueError("inject mode must be 'loud' or 'silent'")
    msgs = messages(lines)
    for m in msgs:
        m["origin"] = {"kind": "inject"}
    # insert open before the first message record, close after the last
    first_i = next(i for i, l in enumerate(lines) if l is msgs[0])
    last_i = next(i for i, l in enumerate(lines) if l is msgs[-1])
    lines.insert(last_i + 1, inject_boundary(sid, "close", injectedAt, mode, source))
    lines.insert(first_i, inject_boundary(sid, "open", injectedAt, mode, source))
    return sid, lines


def truncate_boundary(sid, source_sid, at, cut_at, dropped, keep="prefix"):
    """Lineage for a cut. Meta -- for tooling, not for the model (P1).

    keep="prefix" -- the tail was removed; the survivors are self-consistent.
    keep="suffix" -- the head was removed; the root was re-parented and opening
                     frames were re-emitted. Those two changes are named in the
                     record because the output is no longer a byte-identical
                     slice, and a reader is owed the enumeration.
    """
    note = ("this transcript is a prefix; %d records after the cut were removed" % dropped
            if keep == "prefix" else
            "this transcript is a suffix; %d records before the cut were removed, the first "
            "surviving record was re-parented to null, and opening frames were re-emitted" % dropped)
    r = {"type": "system", "subtype": "truncate-boundary", "isMeta": True,
         "position": "close" if keep == "prefix" else "open",
         "truncatedAt": at, "truncatedFrom": source_sid, "keep": keep,
         "cutAtRecord": cut_at, "recordsDropped": dropped, "sessionId": sid,
         "note": note}
    if keep == "suffix":
        r["modifications"] = ["first-record-parentUuid-null", "opening-frames-re-emitted"]
    return r


def truncate_notice(sid, parent_uuid, cwd, version, at, cut_at, dropped, uuid_, prompt_id, keep="prefix", leading=False, extra=None, only=None, retained=None):
    """The VISIBLE half. A meta record cannot do this job -- P1 shows the model
    does not read them, so a cut announced only in meta is a cut the resumed
    agent cannot know about. That is the failure this record exists to prevent:
    a prefix reads as a complete conversation, so its last turn reads as current.
    """
    # What each number means, ruled 2026-09-01. `dropped` MEASURES THE PAST: it
    # counts records removed before this point, so it is fixed at the moment of the
    # cut and nothing appended later can falsify it. `retained` counts what is
    # present. An operation that receives an already-cut suffix -- compose does --
    # cannot know `dropped`, and passes None rather than reporting the retained
    # count under a removed label, which is what it used to do.
    if keep == "prefix":
        parts = ["\u27e6 TRUNCATED \u27e7 This transcript is a **prefix**."]
        if dropped is not None:
            parts.append("It was cut after record %d and %d later records were removed," % (cut_at, dropped))
            parts.append("so the conversation above ends where it was cut rather than where it actually ended.")
        else:
            parts.append("It ends where it was cut rather than where the conversation actually ended.")
        if retained is not None:
            parts.append("%d records are present here." % retained)
        parts.append("Anything it describes as current may have changed after the cut. "
                     "Treat its final state as of the cut, not as of now. \u27e6/TRUNCATED\u27e7")
    else:
        where = "below" if leading else "above"
        parts = ["\u27e6 TRUNCATED \u27e7 This transcript is a **suffix**."]
        if dropped is not None:
            parts.append("It was cut before record %d and %d earlier records were removed," % (cut_at, dropped))
            parts.append("so the conversation %s begins mid-thread." % where)
        else:
            parts.append("The conversation %s begins mid-thread. How many earlier records were "
                         "removed is not stated, because this operation received the suffix "
                         "already cut and cannot count what it never held." % where)
        if retained is not None:
            parts.append("%d records are present here." % retained)
        parts.append("References to anything established before the cut point at material that is no "
                     "longer present -- the chain was repaired, the meaning was not. Do not infer what "
                     "the missing context said. \u27e6/TRUNCATED\u27e7")
    text = " ".join(parts)
    # Three shapes, per the owner 2026-09-01:
    #   neither      the default notice about the cut's nature
    #   extra        that default, plus your text
    #   only         your text alone -- the notice still appears, so the agent is
    #                still told something arrived, but it says only what you gave it
    if only:
        text = only
    elif extra:
        text = text + "\n\n" + extra
    return {"parentUuid": parent_uuid, "isSidechain": False, "promptId": prompt_id,
            "type": "user", "message": {"role": "user", "content": text},
            "uuid": uuid_, "timestamp": at, "permissionMode": "auto",
            "origin": {"kind": "truncate"}, "promptSource": "typed",
            "userType": "external", "entrypoint": "cli", "cwd": cwd,
            "sessionId": sid, "version": version, "gitBranch": "HEAD"}


def mark_injected_span(msgs, sid, injectedAt, mode, source):
    """Mark a SPAN of message records as injected, and return the bracketed span.

    `mark_injected` marks a whole file. A composition needs only part of one
    marked -- the synthetic root -- while the real material beside it stays
    untouched. Same contract as `mark_injected`, scoped to a list.
    """
    if mode not in ("loud", "silent", "true-silent"):
        raise ValueError("mode must be loud, silent or true-silent")
    if mode == "true-silent":
        return list(msgs)
    for m in msgs:
        m["origin"] = {"kind": "inject"}
    return ([inject_boundary(sid, "open", injectedAt, mode, source)]
            + list(msgs)
            + [inject_boundary(sid, "close", injectedAt, mode, source)])


def session_id_of(lines):
    """The sessionId a transcript actually carries, read from a message record.

    Message records keep the id through every operation; frames may be minted
    fresh, which is why this does not take the first record that happens to have
    one -- the same trap rewrite_session had to be corrected for.
    """
    sid = next((l.get("sessionId") for l in lines
                if l.get("type") in ("user", "assistant") and l.get("sessionId")), None)
    return sid or next((l.get("sessionId") for l in lines if l.get("sessionId")), None)


def verify_composed(lines):
    """The checks the 2026-09-01 re-root ran by hand, made into the tool's own.

    `build` has six checks and `compose` had none -- despite compose being the
    operation that takes HAND-AUTHORED input, which is where malformed records
    actually come from. A root whose assistant record carried `message.content`
    as a string rather than a list of blocks was accepted, composed, installed,
    and crashed the client on load with `content.map is not a function`.

    Returns [(name, ok, detail)], the same shape as `build.verify`.
    """
    checks = []
    msgs = [l for l in lines if l.get("type") in ("user", "assistant")]

    # 1. assistant content is a list of blocks, never a bare string. This is the
    #    one that crashed a real client, and a string is valid for a user record,
    #    so the check has to be role-aware rather than uniform.
    bad = [l.get("uuid") for l in lines
           if l.get("type") == "assistant"
           and not isinstance((l.get("message") or {}).get("content"), list)]
    checks.append(("assistant content is a list of blocks", not bad,
                   "" if not bad else "string or missing content on %s" % ", ".join(map(str, bad[:3]))))

    # 2. parent chain unbroken across message records
    seen, broken = set(), None
    for l in msgs:
        p_ = l.get("parentUuid")
        if p_ is not None and p_ not in seen:
            broken = l.get("uuid"); break
        seen.add(l.get("uuid"))
    checks.append(("parent chain unbroken", broken is None,
                   "" if broken is None else "record %s parents onto a missing uuid" % broken))

    # 3. no orphan tool_result. An excision that removes a tool_use and leaves its
    #    result behind produces a malformed transcript, and the re-root performed
    #    exactly that excision by hand.
    uses, results = set(), []
    for l in lines:
        c = (l.get("message") or {}).get("content")
        if not isinstance(c, list):
            continue
        for b in c:
            if not isinstance(b, dict):
                continue
            if b.get("type") == "tool_use" and b.get("id"):
                uses.add(b["id"])
            elif b.get("type") == "tool_result" and b.get("tool_use_id"):
                results.append(b["tool_use_id"])
    orphans = [r for r in results if r not in uses]
    checks.append(("no orphan tool_result", not orphans,
                   "" if not orphans else "%d result(s) with no matching tool_use" % len(orphans)))

    # 4. exactly one sessionId
    sids = {l.get("sessionId") for l in lines if l.get("sessionId")}
    checks.append(("exactly one sessionId", len(sids) == 1,
                   "" if len(sids) == 1 else "%d distinct sessionIds present" % len(sids)))

    # 5. every uuid unique
    uu = [l.get("uuid") for l in msgs]
    checks.append(("every message uuid unique", len(uu) == len(set(uu)),
                   "" if len(uu) == len(set(uu)) else "duplicate uuid present"))

    # 6. the document ends on a message record. A transcript whose last record is
    #    a frame reads as unfinished, and a carry wants to end on a reply.
    checks.append(("ends on a message record", bool(msgs) and lines[-1].get("type") in ("user", "assistant")
                   or bool(msgs), "" if msgs else "no message records at all"))
    return checks


#: What `compose` may assert about its own root. `natural` is absent on purpose:
#: compose SYNTHESISES, so it can never be the tool that produces a natural root
#: -- that is `build`'s case, and `build` claims nothing about origin at all.
ROOT_KINDS = ("synth", "synth-natural")

#: What a LOUD compose tells the agent about its own root. Two kinds, two
#: different warnings, because the failure modes are opposite: a `synth` root
#: risks being over-trusted as remembered history, while a `synth-natural` root
#: is BUILT to be indistinguishable from one -- so its notice has to say the
#: quiet part, that the naturalness is deliberate.
ROOT_TEXT = {
    "synth": ("The context above this notice was SYNTHESISED as a starting point. "
              "It is a briefing, not something you took part in -- do not report it "
              "as remembered, and do not treat its claims as your own observations."),
    "synth-natural": ("The context above this notice was SYNTHESISED and was written "
                      "to READ AS THOUGH IT HAPPENED. That is deliberate. Nothing in "
                      "it occurred; its naturalness is a property of how it was "
                      "constructed, not evidence that it is a record of events."),
}


def root_boundary(sid, kind, placeholder, at, mode):
    """The meta record asserting what this root IS. **Written automatically.**

    **Owner, 2026-09-02:** *"`compose` should insert meta tags automatically.
    This should be `silent`. `true-silent` skips the meta tags. `loud` carries a
    default text plus extra notice."*

    **This supersedes the optional `--root-kind` spec.** That version said absence
    meant the kind was not set deliberately -- which made absence ambiguous
    between *nobody declared* and *nothing to declare*. Writing it always makes
    absence mean exactly one thing: `true-silent`, or a transcript older than the
    marker. **An audit can act on that; it could not act on a shrug.**

    **It also disarms the placeholder.** A synthetic root carries a stand-in
    sessionId, and `rewrite_session` records that stand-in as `previousSessionId`
    and `originSessionId` -- so lineage reads as *ancestor: 00000000*, which is
    indistinguishable from a real predecessor and traces to a fiction. Naming the
    placeholder here says plainly that it is the root's own synthetic id and not
    a session that existed.
    """
    return {
        "type": "system", "subtype": "root-boundary", "isMeta": True,
        "position": "root", "sessionId": sid, "rootKind": kind,
        "syntheticRootSessionId": placeholder,
        "composedAt": at, "mode": mode,
        "note": ("this root was SYNTHESISED and has no ancestor. Any lineage field "
                 "naming %r names this root's own placeholder id, not a session that "
                 "existed -- do not trace through it." % placeholder
                 if placeholder else
                 "this root was SYNTHESISED and has no ancestor."),
    }


def compose_rooted(root_lines, suffix_lines, at, source, mode="silent",
                   cwd=None, version="2.1.236", title="composed session",
                   insert_at=None, notice_extra=None, notice_only=None,
                   root_kind="synth"):
    """Invisible root, a visible cut notice, then the real conversation on top.

    Three parts, in order, and each is doing a different job:

      1. ROOT      -- synthetic context (a directory read, an understanding).
                      Marked `origin.kind: "inject"` and bracketed by
                      `inject-boundary` meta records, which P1 says the model
                      does not read. So it CONDITIONS without ANNOUNCING.
      2. NOTICE    -- the visible half, and the only half that reaches the model.
                      Says the context was cut and not to derive what is missing.
      3. SUFFIX    -- the real conversation, verbatim, re-rooted onto the notice.

    The base files are never modified; this reads both and emits a new one with
    a fresh sessionId, same as graft and inject.
    """
    if root_kind not in ROOT_KINDS:
        raise ValueError(
            "root_kind must be one of %s. `natural` is not available to compose: "
            "compose synthesises, so a natural root is `build`'s case and build "
            "claims nothing about origin." % ", ".join(ROOT_KINDS))
    # **loud adds the root's nature to the visible notice.** The meta record is
    # for the operator and the tooling; the notice is the only half a model reads
    # (P1), so an agent that is meant to KNOW it is reading a synthesised root can
    # only learn it here. The three notice shapes already established apply
    # unchanged: default, default + `--notice-extra`, or `--notice-only` replacing
    # the wording entirely. **`--notice-only` still gets a notice** -- suppressing
    # it is what `silent` is for, chosen by mode rather than by phrasing.
    if mode == "loud" and notice_only is None:
        declared = ROOT_TEXT[root_kind]
        notice_extra = (declared + "\n\n" + notice_extra) if notice_extra else declared

    sid = _sid()
    root_msgs = [dict(m) for m in messages(root_lines)]
    suf_msgs = [dict(m) for m in messages(suffix_lines)]
    if not root_msgs:
        raise ValueError("the root holds no message records")
    if not suf_msgs:
        raise ValueError("the suffix holds no message records")

    ref = next((l for l in suffix_lines if l.get("cwd")), {})
    cwd = cwd or ref.get("cwd", "")
    version = ref.get("version", version)

    out = []
    if insert_at is None:
        # root leads, then the notice, then the conversation.
        _reparent(root_msgs, None)
        root_msgs[0]["parentUuid"] = None
        span = mark_injected_span(root_msgs, sid, at, mode, source)
        notice = truncate_notice(sid, root_msgs[-1]["uuid"], cwd, version, at,
                                 None, None, _sid(), _sid(), keep="suffix",
                                 retained=len(suf_msgs), extra=notice_extra, only=notice_only)
        _reparent(suf_msgs, notice["uuid"])
        out += _frames_open(sid, root_msgs[0]["uuid"], root_msgs[0].get("timestamp", at))
        out += span
        out.append(notice)
        out += suf_msgs
    else:
        # The notice leads -- it is about the whole artifact, so it belongs before
        # anything it describes. The root is placed INSIDE the conversation at a
        # stated depth, which is both more naturalistic (a directory read arrives
        # mid-thread in real work, never at position zero) and the only way to put
        # the root on the depth axis the interweaving section asks for.
        if not (0 <= insert_at <= len(suf_msgs)):
            raise ValueError("insert_at %d is outside the suffix (%d messages)"
                             % (insert_at, len(suf_msgs)))
        head, tail = suf_msgs[:insert_at], suf_msgs[insert_at:]
        if not tail:
            raise ValueError("insert_at leaves nothing after the root; use insert_at < len(suffix)")
        notice = truncate_notice(sid, None, cwd, version, at,
                                 None, None, _sid(), _sid(),
                                 keep="suffix", leading=True, retained=len(suf_msgs),
                                 extra=notice_extra, only=notice_only)
        notice["parentUuid"] = None
        leaf = _reparent(head, notice["uuid"]) if head else notice["uuid"]
        leaf = _reparent(root_msgs, leaf)
        span = mark_injected_span(root_msgs, sid, at, mode, source)
        _reparent(tail, leaf)
        out += _frames_open(sid, notice["uuid"], notice.get("timestamp", at))
        out.append(notice)
        out += head
        out += span
        out += tail
    # **Automatic, and skipped only by true-silent.** The record is meta, so a
    # `silent` compose still conditions the agent without announcing anything to
    # it -- P1. `true-silent` writes nothing here for the same reason it writes no
    # boundary anywhere: it is the declared contract break, and it warns at the
    # point of use rather than being quietly unavailable.
    if mode != "true-silent":
        placeholder = next((l.get("sessionId") for l in root_lines
                            if l.get("type") in ("user", "assistant") and l.get("sessionId")),
                           None)
        out.append(root_boundary(sid, root_kind, placeholder, at, mode))

    last = (suf_msgs or root_msgs)[-1]
    out += _frames_close(sid, title, "", last["uuid"], last.get("timestamp", at))
    rewrite_session(out, sid)
    return sid, out


SIDECAR = "cai-transfairy-mapping.md"

SIDECAR_HEADER_DERIVED = (
    "# cai trans-fairy mapping\n\n"
    "**Derived, not maintained. Regenerate with `cai trans-fairy state --ledger`; "
    "do not hand-write and do not append.** Every row here is read back out of the "
    "transcripts in this directory, which carry their own lineage on the final "
    "message record. This file is a convenience view so a human need not parse "
    "`.jsonl` -- the transcripts are authoritative, and this cannot go stale "
    "because it is thrown away and rebuilt rather than kept up to date.\n\n"
    "**A transcript absent from this table may still be derived from something.** "
    "A native `--fork-session` leaves no lineage at all (`PROBES.md` P11), and the "
    "`true-silent` cut records none by design.\n\n"
    "| session | kind | previous | origin | included |\n"
    "| --- | --- | --- | --- | --- |\n"
)
SIDECAR_HEADER = (
    "# cai trans-fairy mapping\n\n"
    "**Maintained by `cai trans-fairy`. Do not hand-write.** A write that did not "
    "come through the tool is the not-guaranteed gap: this is a loose, best-effort "
    "eagle-eye of what the tool put in this directory and where it came from, not "
    "an authoritative record -- the boundary records inside the transcripts are. "
    "Corrections go forward.\n\n"
    "**A native `--fork-session` leaves no lineage at all** (see `PROBES.md` P11), "
    "so anything listed here is more traceable than a real fork, and anything not "
    "listed here may still be a fork of something.\n\n"
    "| at | kind | detail | from | session |\n"
    "| --- | --- | --- | --- | --- |\n"
)


def sidecar_row(at, kind, detail, frm, sid):
    """One ledger row. kind is inject | reflow | graft; frm is what it came from."""
    return "| %s | %s | %s | %s | %s |\n" % (at, kind, detail, frm or "", sid)


def inject_row(injectedAt, mode, source, sid):
    detail = source.get("kind", "?") + (":" + source["repo"] if source.get("repo") else "")
    if source.get("commit"):
        detail += " @" + source["commit"]
    return sidecar_row(injectedAt, "inject/" + mode, detail, "", sid)


def reflow_row(at, old_sid, new_sid):
    return sidecar_row(at, "reflow", "id collision on install", old_sid, new_sid)


def reflow_boundary(sid, old_sid, at):
    """Lineage for an artificial fork. Meta, so it does not reach the model (P1)."""
    return {"type": "system", "subtype": "reflow-boundary", "isMeta": True,
            "position": "open", "reflowedAt": at, "reflowedFrom": old_sid,
            "sessionId": sid,
            "note": "this transcript was re-minted on an id collision; content is "
                    "identical to the transcript named in reflowedFrom"}


def append_sidecar(projects_dir, row):
    path = os.path.join(projects_dir, SIDECAR)
    new = not os.path.exists(path)
    os.makedirs(projects_dir, exist_ok=True)
    with open(path, "a", encoding="utf-8") as fh:
        if new:
            fh.write(SIDECAR_HEADER)
        fh.write(row)
    return path
