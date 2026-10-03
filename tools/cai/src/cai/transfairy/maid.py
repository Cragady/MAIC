"""trans-fairy's operations on a MAID session: the same stages, MAID's file in and out.

**The contract is the stage; the format is detected from the records.** A cut, a
composition, a graft, an injection or an install given a MAID session produces a
MAID session: one `start` record, MAID's own record types, and this suite's two
additions that MAID ignores on load, a `cai` marker for lineage and boundaries and
MAID's own `inject` + `msg` + `context` triple wherever cai would have the agent
read a notice (MAID marks a placed note that way; a typed turn it is not).

**Lineage lives on the marker, not on the last message.** Claude Code records carry
`previousSessionId` and `originSessionId` on the final message record; a MAID
record is a typed line with no spare field for it, so the marker that announces the
operation carries the same three fields, and `state --audit` and `--ledger` read
them from there.

**What cai does with a uuid chain is done here with positions.** A cut point is a
record index (`--at-line`) or a text match (`--before-text`); `--at-uuid` has nothing
to name in a MAID file and says so. Re-rooting a suffix re-emits the `start` record,
as the Claude Code path re-emits its opening frames.
"""
import os

from cai.grammar import maid as fmt
from cai.transfairy import graft as graftmod

MODES = ("silent", "loud", "true-silent")


def _wrap(text, open_t, close_t):
    return "%s\n%s\n%s" % (open_t, text, close_t)


def _mark(records, kind, bracket=None):
    """`origin.kind` on every conversational record, as cai marks grafted and injected material."""
    for r in records:
        if r.get("type") in ("msg", "user", "assistant", "tool"):
            r["origin"] = {"kind": kind}
            if bracket and r.get("type") == "msg" and isinstance(r.get("content"), str):
                r["content"] = _wrap(r["content"], *bracket)
            elif bracket and r.get("type") in ("user", "assistant"):
                r["text"] = _wrap(r.get("text", ""), *bracket)
    return records


def _lineage(previous, origin, included=()):
    out = {"previousSessionId": previous, "originSessionId": origin or previous}
    inc = sorted(set(included) - {None})
    if len(inc) > 1:
        out["includedSessionIds"] = inc
    return out


def conversation(records):
    """A session's records minus its frame: no `start`, `title`, `imported_from`,
    `resumed_from` (the fork is flattened by the caller) and no system `msg`."""
    return [r for r in records
            if r.get("type") not in ("start", "title", "imported_from", "resumed_from")
            and not (r.get("type") == "msg" and r.get("role") == "system")]


def truncate(lines, idx, keep, mode, source_id, at, cwd, loud_lineage=False, out_dir=None):
    """Cut a MAID session at record index `idx`, keeping the prefix or the suffix."""
    if mode not in MODES:
        raise ValueError("mode must be one of: %s" % ", ".join(MODES))
    sid = fmt.mint_id(out_dir)
    if keep == "prefix":
        kept, dropped = list(lines[:idx + 1]), lines[idx + 1:]
    else:
        kept, dropped = list(lines[idx:]), lines[:idx]
        if not kept:
            raise ValueError("the cut would leave nothing")
        if not any(r.get("type") in ("user", "assistant", "msg") for r in kept):
            raise ValueError("the suffix holds no message records; there is nothing to root")
        start = dict(fmt.first_start(lines)) or {"type": "start", "workspace": cwd}
        start["type"] = "start"
        start["time"] = at
        kept = [start] + [r for r in kept if r.get("type") != "resumed_from"]
    if not kept:
        raise ValueError("the cut would leave nothing")
    prior = fmt.lineage_of(lines)
    added = 0
    if mode != "true-silent":
        note = ("this transcript is a prefix; %d records after the cut were removed" % len(dropped)
                if keep == "prefix" else
                "this transcript is a suffix; %d records before the cut were removed and the start "
                "record was re-emitted" % len(dropped))
        m = fmt.marker("truncate-boundary", at=at, position="close" if keep == "prefix" else "open",
                       truncatedAt=at, truncatedFrom=source_id, keep=keep, cutAtRecord=idx + 1,
                       recordsDropped=len(dropped), sessionId=sid, mode=mode, note=note,
                       **_lineage(source_id, prior.get("originSessionId"),
                                  (prior.get("includedSessionIds") or []) + [source_id]))
        if keep == "suffix":
            m["modifications"] = ["start-record-re-emitted", "resumed_from-dropped"]
        kept.append(m)
        added += 1
    if mode == "loud":
        text = graftmod.truncate_notice(sid, None, cwd, "", at, idx + 1, len(dropped), "", "",
                                        keep=keep)["message"]["content"]
        kept.extend(fmt.notice(text, at=at, extra={"origin": {"kind": "truncate"}}))
        added += 3
    if loud_lineage and mode != "true-silent":
        text = graftmod.lineage_notice(sid, None, cwd, "", at, source_id,
                                       prior.get("originSessionId") or source_id,
                                       prior.get("includedSessionIds") or [], "", "")["message"]["content"]
        kept.extend(fmt.notice(text, at=at, extra={"origin": {"kind": "lineage"}}))
        added += 3
    return sid, kept, added, dropped


def compose(root_lines, suf_lines, at, source, mode, cwd, insert_at=None,
            notice_extra=None, notice_only=None, root_kind="synth", out_dir=None,
            root_id=None, suf_id=None):
    """An invisible root, a visible cut notice, and the real suffix, as one MAID session."""
    if root_kind not in graftmod.ROOT_KINDS:
        raise ValueError("root_kind must be one of %s" % ", ".join(graftmod.ROOT_KINDS))
    if mode not in MODES:
        raise ValueError("mode must be one of: %s" % ", ".join(MODES))
    if mode == "loud" and notice_only is None:
        declared = graftmod.ROOT_TEXT[root_kind]
        notice_extra = (declared + "\n\n" + notice_extra) if notice_extra else declared
    sid = fmt.mint_id(out_dir)
    root = conversation(root_lines) if fmt.is_maid(root_lines) else fmt.from_claude(root_lines, at)
    suffix = conversation(suf_lines) if fmt.is_maid(suf_lines) else fmt.from_claude(suf_lines, at)
    if not root:
        raise ValueError("the root holds no message records")
    if not suffix:
        raise ValueError("the suffix holds no message records")
    start = dict(fmt.first_start(suf_lines) or fmt.first_start(root_lines) or {})
    start.update({"type": "start", "time": at, "workspace": start.get("workspace") or cwd})
    if mode != "true-silent":
        root = _mark(root, "inject")
        span = ([fmt.marker("inject-boundary", at=at, position="open", injectedAt=at, mode=mode, source=source, sessionId=sid)]
                + root
                + [fmt.marker("inject-boundary", at=at, position="close", injectedAt=at, mode=mode, source=source, sessionId=sid)])
    else:
        span = root
    turns = [i for i, r in enumerate(suffix) if r.get("type") == "user"]
    out = [start]
    if insert_at is None:
        text = graftmod.truncate_notice(sid, None, cwd, "", at, None, None, "", "", keep="suffix",
                                        retained=len(suffix), extra=notice_extra, only=notice_only)["message"]["content"]
        out += span
        out += fmt.notice(text, at=at, extra={"origin": {"kind": "truncate"}})
        out += suffix
    else:
        if not (0 <= insert_at <= len(turns)):
            raise ValueError("insert_at %d is outside the suffix (%d user turns)" % (insert_at, len(turns)))
        cut = len(suffix) if insert_at == len(turns) else turns[insert_at]
        head, tail = suffix[:cut], suffix[cut:]
        if not tail:
            raise ValueError("insert_at leaves nothing after the root; use insert_at < the number of turns")
        text = graftmod.truncate_notice(sid, None, cwd, "", at, None, None, "", "", keep="suffix", leading=True,
                                        retained=len(suffix), extra=notice_extra, only=notice_only)["message"]["content"]
        out += fmt.notice(text, at=at, extra={"origin": {"kind": "truncate"}})
        out += head
        out += span
        out += tail
    if mode != "true-silent":
        out.append(fmt.marker("root-boundary", at=at, position="root", sessionId=sid, rootKind=root_kind,
                              syntheticRootSessionId=root_id, composedAt=at, mode=mode,
                              note="this root was SYNTHESISED and has no ancestor."))
        prior = fmt.lineage_of(suf_lines)
        out.append(fmt.marker("compose-boundary", at=at, position="close", composedAt=at, sessionId=sid,
                              rootSessionId=root_id, suffixSessionId=suf_id,
                              **_lineage(suf_id, prior.get("originSessionId"),
                                         (prior.get("includedSessionIds") or []) + [suf_id, root_id])))
    return sid, out


def graft(base_lines, graft_lines, base_id, graft_id, at, direction="after", out_dir=None):
    """Graft one MAID session onto another: a new file, the base and the graft copied,
    the graft bracketed and marked, both sources untouched."""
    base = conversation(base_lines)
    grafted = conversation(graft_lines) if fmt.is_maid(graft_lines) else fmt.from_claude(graft_lines, at)
    if not base or not grafted:
        raise ValueError("graft needs a non-empty base and graft")
    sid = fmt.mint_id(out_dir)
    _mark(grafted, "graft", bracket=("⟦ GRAFT @ %s ⟧" % at, "⟦/GRAFT⟧"))
    first, second = (grafted, base) if direction == "before" else (base, grafted)
    start = dict(fmt.first_start(base_lines) or {})
    start.update({"type": "start", "time": at})
    seam = fmt.marker("graft-boundary", at=at, position="close", graftedAt=at, baseSessionId=base_id,
                      graftSessionId=graft_id, sessionId=sid,
                      **_lineage(base_id, fmt.lineage_of(base_lines).get("originSessionId"), [base_id, graft_id]))
    return sid, [start] + first + [seam] + second


def mark_injected(lines, at, mode, source, sid):
    """Mark a whole MAID session as injected material: `loud` marks and brackets, `silent` leaves it seamless."""
    if mode == "silent":
        return lines
    if mode != "loud":
        raise ValueError("inject mode must be 'loud' or 'silent'")
    frame = [r for r in lines if r.get("type") in ("start", "title", "imported_from")]
    body = _mark([r for r in lines if r not in frame], "inject")
    return (frame
            + [fmt.marker("inject-boundary", at=at, position="open", injectedAt=at, mode=mode, source=source, sessionId=sid)]
            + body
            + [fmt.marker("inject-boundary", at=at, position="close", injectedAt=at, mode=mode, source=source, sessionId=sid)])


def reflow_boundary(sid, old_sid, at):
    return fmt.marker("reflow-boundary", at=at, position="open", reflowedAt=at, reflowedFrom=old_sid, sessionId=sid,
                      note="this transcript was re-minted on an id collision; content is identical to "
                           "the transcript named in reflowedFrom", **_lineage(old_sid, None))


def install_dir(tree):
    """Where a MAID session for the target cwd lands: `--claude-projects` when given, else the
    home MAID would give that workspace (`projects/<encoded>` under a `MAID.md`, else `general`)."""
    if tree.projects_override:
        return tree.projects_dir
    return fmt.home_for(tree.target_cwd)


def resume_commands(tree, sid):
    return ("cd %s && maid -r %s" % (tree.target_cwd, sid),
            "cd %s && maid -r %s --no-append" % (tree.target_cwd, sid))
