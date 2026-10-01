"""build -- extracted conversation -> staged Claude Code transcript.

Ported from reference/transfairy/build-transcript.py onto the shared grammar
(cai.grammar.records) and the deterministic pool. Two corrections from the
2026-08-28 read of the reference are carried in:

  - an assistant turn that folds to empty is skipped AND disclosed (the
    reference dropped it silently, counting it nowhere);
  - the key-set verification checks against cai.grammar.records field sets,
    which is the shared owner of that knowledge.
"""
import json

from cai.grammar import records

# Fields the tool adds to a message record deliberately. They are not part of
# the client's grammar and must not be reported as drift: `origin` marks how a
# record got here (P2 -- it loads on any role), and the two lineage fields
# record where the transcript came from, written by rewrite_session at the one
# place a sessionId changes.
MARKER_FIELDS = {"origin", "previousSessionId", "originSessionId", "includedSessionIds"}

RESULT_CAP = 6000


def ts(s):
    """2026-08-24T06:19:12.353380Z -> ...353Z (microseconds truncated to ms)."""
    base, _, frac = s.rstrip("Z").partition(".")
    return "%s.%sZ" % (base, (frac + "000")[:3])


def _linear_chain(msgs):
    """Walk the parent chain from the root; at a branch take the longest subtree."""
    by = {m["uuid"]: m for m in msgs}
    kids = {}
    for m in msgs:
        kids.setdefault(m["parent_message_uuid"], []).append(m["uuid"])

    depth = {}

    def d(u):
        if u in depth:
            return depth[u]
        depth[u] = 1 + max((d(c) for c in kids.get(u, [])), default=0)
        return depth[u]

    root = next(m["uuid"] for m in msgs if m["parent_message_uuid"] not in by)
    chain, cur = [], root
    while cur:
        chain.append(by[cur])
        cs = kids.get(cur, [])
        cur = max(cs, key=d) if cs else None
    return chain


def _render_result(b):
    out = []
    for c in b.get("content") or []:
        if c.get("type") == "text":
            out.append(c["text"])
        else:
            out.append(json.dumps(c, ensure_ascii=False))
    s = "\n".join(out)
    return s if len(s) <= RESULT_CAP else s[:RESULT_CAP] + "\n… [result truncated]"


def build(conv, pool, target_cwd, version, model):
    """Return (sid, lines, stats). Consumes ids from pool in order.

    Call pool.ensure(size_for(chain)) before this; size_for needs the chain,
    which _linear_chain produces, so ensure is done here.
    """
    from cai.transfairy.pool import size_for

    msgs = conv["chat_messages"]
    chain = _linear_chain(msgs)
    pool.ensure(size_for(chain))
    dropped_offbranch = len(msgs) - len(chain)

    sid = next(pool)
    stats = {"thinking": 0, "tool_use": 0, "tool_result": 0,
             "attachments": 0, "files": 0, "skipped_empty_assistant": 0}

    def env():
        return {"userType": "external", "entrypoint": "cli", "cwd": target_cwd,
                "sessionId": sid, "version": version, "gitBranch": "HEAD"}

    def assistant_text(m):
        parts = []
        for b in m.get("content", []):
            t = b.get("type")
            if t == "text":
                parts.append(b["text"])
            elif t == "thinking":
                stats["thinking"] += 1  # empty text + foreign signature: dropped
            elif t == "tool_use":
                stats["tool_use"] += 1
                parts.append("```tool_use %s\n%s\n```" % (
                    b.get("name", ""), json.dumps(b.get("input", {}), indent=2, ensure_ascii=False)))
            elif t == "tool_result":
                stats["tool_result"] += 1
                parts.append("```tool_result %s\n%s\n```" % (b.get("name", ""), _render_result(b)))
        return "\n\n".join(p for p in parts if p.strip())

    def human_text(m):
        parts = [b["text"] for b in m.get("content", []) if b.get("type") == "text"]
        s = "\n\n".join(parts) or m.get("text", "")
        for a in m.get("attachments") or []:
            stats["attachments"] += 1
            body = a.get("extracted_content") or ""
            name = a.get("file_name") or ""
            label = ("pasted file: %s" % name) if name else "pasted file (name not recorded in the export)"
            s += "\n\n[%s, %s, %s bytes]\n\n%s" % (
                label, a.get("file_type") or "txt", a.get("file_size", len(body)), body)
        extra = len(m.get("files") or []) - len(m.get("attachments") or [])
        if extra > 0:
            stats["files"] += extra
            s += "\n\n[%d attached file(s) in the original conversation had no extractable text]" % extra
        return s

    lines = []
    lines.append({"type": "mode", "mode": "normal", "sessionId": sid})
    lines.append({"type": "permission-mode", "permissionMode": "auto", "sessionId": sid})

    parent = first_uuid = last_uuid = None
    last_human = ""
    for m in chain:
        u, t = next(pool), ts(m["created_at"])
        if m["sender"] == "human":
            txt = human_text(m)
            last_human = txt
            line = {"parentUuid": parent, "isSidechain": False, "promptId": next(pool),
                    "type": "user", "message": {"role": "user", "content": txt},
                    "uuid": u, "timestamp": t, "permissionMode": "auto",
                    "origin": {"kind": "human"}, "promptSource": "typed", **env()}
            if first_uuid is None:
                first_uuid = u
                lines.append({"type": "file-history-snapshot", "messageId": u,
                              "snapshot": {"messageId": u, "trackedFileBackups": {}, "timestamp": t},
                              "isSnapshotUpdate": False})
        else:
            txt = assistant_text(m)
            if not txt.strip():
                # reference dropped this silently; we count it. It can leave two
                # consecutive user records, which verify()'s alternation check catches.
                stats["skipped_empty_assistant"] += 1
                next(pool)  # msg_ id, discarded
                next(pool)  # req_ id, discarded
                continue
            line = {"parentUuid": parent, "isSidechain": False,
                    "message": {"model": model, "id": "msg_" + next(pool).replace("-", ""),
                                "type": "message", "role": "assistant",
                                "content": [{"type": "text", "text": txt}],
                                "stop_reason": "end_turn", "stop_sequence": None,
                                "stop_details": None,
                                "usage": {"input_tokens": 0, "cache_creation_input_tokens": 0,
                                          "cache_read_input_tokens": 0, "output_tokens": 0,
                                          "service_tier": "standard"},
                                "diagnostics": None},
                    "requestId": "req_" + next(pool).replace("-", ""), "type": "assistant",
                    "uuid": u, "timestamp": t, "effort": "high", "session_id": sid, **env()}
        lines.append(line)
        parent = last_uuid = u

    lines.append({"type": "ai-title", "aiTitle": conv.get("name", ""), "sessionId": sid})
    lines.append({"type": "last-prompt", "lastPrompt": last_human, "leafUuid": last_uuid, "sessionId": sid})
    fin = next(pool)
    lines.append({"type": "file-history-snapshot", "messageId": fin,
                  "snapshot": {"messageId": fin, "trackedFileBackups": {}, "timestamp": ts(chain[-1]["created_at"])},
                  "isSnapshotUpdate": False})

    # Where the chain actually begins. Lineage records session -> session, but a
    # built transcript has no session predecessor -- its source is a conversation
    # in an export, which is knowable and was going unrecorded. Without this a
    # freshly built transcript is indistinguishable from one with no provenance
    # at all, and audits as unaccounted for the rest of its life.
    # isMeta, so it does not reach the model (P1); appended, so the record order
    # above is untouched.
    lines.append({"type": "system", "subtype": "build-boundary", "isMeta": True,
                  "position": "close", "sessionId": sid,
                  "sourceConversationUuid": conv.get("uuid"),
                  "sourceConversationName": conv.get("name"),
                  "sourceMessages": len(msgs),
                  "note": "identity of the input this was built from. build makes "
                          "no claim about where that input came from -- it cannot "
                          "verify it, and a synthesised conversation is accepted "
                          "here exactly as an exported one is"})
    stats["off_branch_dropped"] = dropped_offbranch
    return sid, lines, stats


def verify(lines):
    """The six build checks (DESIGN / PIPELINE). Returns list of (name, ok, detail)."""
    checks = []
    msg_lines = [l for l in lines if l.get("type") in ("user", "assistant")]

    # 1. parent chain unbroken
    seen = set()
    broken = None
    for l in msg_lines:
        p = l.get("parentUuid")
        if p is not None and p not in seen:
            broken = l.get("uuid")
            break
        seen.add(l.get("uuid"))
    checks.append(("parent chain unbroken", broken is None,
                   "" if broken is None else "record %s parents onto a missing uuid" % broken))

    # 2. no two consecutive turns the same role
    roles = [l["type"] for l in msg_lines]
    dup = next((i for i in range(1, len(roles)) if roles[i] == roles[i - 1]), None)
    checks.append(("no consecutive same-role turns", dup is None,
                   "" if dup is None else "two %s records adjacent at message %d" % (roles[dup], dup)))

    # 3. every uuid unique
    uuids = [l.get("uuid") for l in msg_lines]
    checks.append(("every uuid unique", len(uuids) == len(set(uuids)),
                   "" if len(uuids) == len(set(uuids)) else "duplicate uuid present"))

    # 4. first message record is a user
    checks.append(("first message is a user record", bool(msg_lines) and msg_lines[0]["type"] == "user",
                   "" if msg_lines and msg_lines[0]["type"] == "user" else "first message record is not a user"))

    # 5. cwd uniform
    cwds = {l.get("cwd") for l in msg_lines if "cwd" in l}
    checks.append(("cwd uniform across records", len(cwds) <= 1,
                   "" if len(cwds) <= 1 else "multiple cwd values: %r" % cwds))

    # 6. key-set conformance against the shared grammar
    ref = {"user": set(records.USER_FIELDS), "assistant": set(records.ASSISTANT_FIELDS)}
    bad = []
    for l in msg_lines:
        want = ref[l["type"]]
        have = set(l.keys())
        missing, extra = want - have, have - want - MARKER_FIELDS
        if missing or extra:
            bad.append("%s: missing %s extra %s" % (l["type"], sorted(missing), sorted(extra)))
        if l["type"] == "assistant":
            uhave = set((l.get("message") or {}).get("usage", {}).keys())
            if uhave and uhave != set(records.USAGE_FIELDS):
                bad.append("usage: %s" % sorted(uhave ^ set(records.USAGE_FIELDS)))
    checks.append(("key set matches grammar", not bad, "; ".join(bad)))
    return checks
