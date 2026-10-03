"""MAID sessions as a transcript format: detection, the adapter, and the file conventions.

**A MAID session is one JSONL file, appended and never rewritten**, under
`~/.local/state/maid/sessions/<home>/<id>.jsonl`. Every record carries `type`
and `time` (local time with the offset); the id is the file name. The record
types are `start`, `msg`, `user`, `assistant`, `tool`, `usage`, `context`,
`title`, `compact`, `reset`, `clear`, `undo`, `resumed_from`, `imported_from`,
`inject`, `graft` and `compose` (MAID's docs/sessions.md). MAID ignores a type it
does not know, which is what lets this suite leave its own records in a session:
a `cai` marker for lineage and boundaries, a `rewritten` record for an in-place
write.

**Detection is by content, never by path.** A Claude Code transcript carries
`uuid`, `parentUuid` and `message` on its records; a MAID session carries none of
those and does carry shapes nothing else has (`start` with a `workspace`, `msg`
with a `role`, `resumed_from` with a `records` count, `user` and `assistant`
with `text`). One MAID signature and no Claude Code shape is a MAID session.

**The adapter projects a MAID session onto the Claude Code grammar** so every
reader in this suite works on it unchanged: `user` and `assistant` records become
message records, a `tool` record becomes a `tool_use` block and its
`tool_result`, a `compact` record becomes the compaction summary cai's slicing
keys on, a `context` notice becomes a user record marked as such, and every other
record becomes an `isMeta` system record carrying its original fields. Nothing is
dropped, and for reading the output is one record per input line, each naming its
line in `maid.index`.

**What has no counterpart is said here rather than invented.** MAID records have
no uuid chain, so the adapter mints positional ids and chains them in file order;
there is no `isSidechain` (a MAID subagent has its own transcript, linked from the
parent's `tool` record); tool ids on the transcript side are minted per record,
while the ids the model saw live in the `msg` records, which are carried as meta.
"""
import datetime
import json
import os
import socket
import time

from cai.grammar import records as grecords

TRANSCRIPT_TYPES = ("start", "msg", "user", "assistant", "tool", "usage", "context", "title",
                    "compact", "reset", "clear", "undo", "resumed_from", "imported_from",
                    "inject", "graft", "compose")
#: Records this suite writes into a MAID session. MAID ignores both when loading.
MARKER = "cai"
REWRITTEN = "rewritten"
OWN_TYPES = (MARKER, REWRITTEN)
TYPES = frozenset(TRANSCRIPT_TYPES + OWN_TYPES)

CLAUDE_KEYS = ("uuid", "parentUuid", "message", "isMeta", "sessionId")


def _signature(r):
    t = r.get("type")
    if t == "start":
        return "workspace" in r
    if t == "msg":
        return "role" in r
    if t == "resumed_from":
        return "records" in r
    if t in ("user", "assistant"):
        return "text" in r
    if t == "tool":
        return "tool" in r
    if t in TYPES:
        return "time" in r
    return False


def is_maid(records):
    """Is this a MAID session? **By content.** One MAID signature and no Claude Code shape
    outside this suite's own records (a `cai` marker carries the lineage ids by name)."""
    recs = [r for r in records if isinstance(r, dict)]
    if not recs:
        return False
    if any(k in r for r in recs if r.get("type") not in OWN_TYPES for k in CLAUDE_KEYS):
        return False
    return any(_signature(r) for r in recs)


def load(path):
    """A MAID session's records, read as MAID reads them: a line that is not an object is skipped."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return grecords.load(fh.read())


# --- where MAID keeps sessions ------------------------------------------------

def state_dir():
    xdg = os.environ.get("XDG_STATE_HOME")
    return os.path.join(xdg, "maid") if xdg else os.path.expanduser("~/.local/state/maid")


def sessions_dir():
    return os.path.join(state_dir(), "sessions")


def backups_dir():
    """Where `trans-fairy-write` keeps the copy it takes before any in-place write."""
    return os.path.join(sessions_dir(), ".backups")


def encode_workspace(path):
    """MAID's project home name: the canonical path with `/` and spaces as `-`."""
    s = os.path.realpath(os.path.abspath(path))
    return "".join("-" if c in "/ " else c for c in s) or "-"


def home_for(workspace):
    """The directory MAID would give a session of this workspace: `projects/<encoded>` when a
    `MAID.md` is in the workspace or a parent of it, else `general`."""
    d = os.path.abspath(workspace)
    while True:
        if os.path.exists(os.path.join(d, "MAID.md")):
            return os.path.join(sessions_dir(), "projects", encode_workspace(workspace))
        parent = os.path.dirname(d)
        if parent == d:
            return os.path.join(sessions_dir(), "general")
        d = parent


def stamp(t=None):
    """A MAID record stamp: local time with the offset, to the second."""
    return time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(t))


def mint_id(directory=None, kind="cai"):
    """A MAID session id, `<time>-<kind>-<pid>`, minted from the clock as MAID mints its own.

    A second id in the same second gets MAID's sequence suffix rather than colliding."""
    base = "%s-%s-%d" % (time.strftime("%Y%m%d-%H%M%S"), kind, os.getpid())
    if not directory:
        return base
    for seq in range(100):
        sid = base + ("-%d" % seq if seq else "")
        if not os.path.exists(os.path.join(directory, sid + ".jsonl")):
            return sid
    raise RuntimeError("could not mint a free session id in %s" % directory)


def session_files(root=None):
    """Every session file under the sessions tree, newest first. `.backups` is not a home."""
    root = root or sessions_dir()
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        out.extend(os.path.join(dirpath, f) for f in filenames if f.endswith(".jsonl"))
    return sorted(out, key=os.path.basename, reverse=True)


def find_session(spec, root=None):
    """A session by path, full id, or unique id prefix. **Refuses an ambiguous prefix.**"""
    if os.path.isfile(spec):
        return os.path.abspath(spec)
    exact, prefixed = None, []
    for p in session_files(root):
        stem = os.path.basename(p)[:-len(".jsonl")]
        if stem == spec:
            exact = p
        elif stem.startswith(spec):
            prefixed.append(p)
    if exact:
        return exact
    if len(prefixed) > 1:
        raise ValueError("%r is a prefix of %d sessions; be more specific" % (spec, len(prefixed)))
    return prefixed[0] if prefixed else None


def session_id(path):
    return os.path.basename(path)[:-len(".jsonl")] if path.endswith(".jsonl") else os.path.basename(path)


def start_of(records):
    """The last `start` record: where the session was last opened, and by whom."""
    return next((r for r in reversed(records) if r.get("type") == "start"), None) or {}


def first_start(records):
    return next((r for r in records if r.get("type") == "start"), None) or {}


def title_of(records):
    return next((r.get("text") for r in reversed(records) if r.get("type") == "title"), None)


def pid_alive(pid):
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError, TypeError):
        return False
    return True


def live_reasons(records):
    """Why this session looks open: the process that last opened it is still running here.

    MAID keeps no session registry; the `start` record of each open carries the host
    and pid, which is the signal a registry would have given."""
    st = start_of(records)
    if st.get("host") == socket.gethostname() and st.get("pid") and pid_alive(st["pid"]):
        return ["maid process %s that last opened this session is still running on %s"
                % (st["pid"], st["host"])]
    return []


def live_sessions(root=None):
    """Sessions whose last opener is still running: `{"sessionId", "name", "cwd"}` rows."""
    out = []
    for p in session_files(root):
        try:
            recs = load(p)
        except (OSError, ValueError):
            continue
        if not live_reasons(recs):
            continue
        st = start_of(recs)
        out.append({"sessionId": session_id(p), "name": title_of(recs) or session_id(p),
                    "cwd": st.get("workspace"), "path": p})
    return out


def flatten(path, limit=None, _seen=None):
    """The records a session loads: its parent's first `records` lines when it is a fork,
    then its own. The pointer is followed by path, then by id if the parent was rehomed."""
    seen = _seen or set()
    real = os.path.realpath(path)
    if real in seen:
        raise ValueError("resumed_from pointers loop through %s" % path)
    seen.add(real)
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    # MAID counts lines, blank or malformed ones included, and keeps the objects among them.
    own = grecords.load("\n".join(lines if limit is None else lines[:limit]))
    out = []
    for r in own:
        if r.get("type") == "resumed_from":
            parent = r.get("path")
            if not parent or not os.path.isfile(parent):
                parent = find_session(r.get("id", ""))
            if not parent:
                raise FileNotFoundError("the parent session %s of %s is gone" % (r.get("id"), path))
            out.extend(flatten(parent, r.get("records", 0), seen))
        else:
            out.append(r)
    return out


def with_parents(path, text):
    """The text of a MAID fork as MAID loads it: the parent's first `records` lines, then
    its own. Any other text comes back unchanged, and so does a fork whose parent is gone."""
    recs = grecords.load(text)
    if not is_maid(recs) or not any(r.get("type") == "resumed_from" for r in recs):
        return text
    try:
        recs = flatten(path)
    except (OSError, ValueError):
        return text
    return "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in recs)


# --- the adapter --------------------------------------------------------------

def utc(local_iso, default=None):
    """A MAID stamp (local with offset) as the UTC `...Z` form Claude Code records carry."""
    if not local_iso:
        return default or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    try:
        d = datetime.datetime.fromisoformat(local_iso.replace("Z", "+00:00"))
    except ValueError:
        return local_iso
    if d.tzinfo is None:
        d = d.astimezone()
    return d.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


FULL_OUTPUT_LABEL = "full output, display only: the model saw the capped result"


def kept_note(r):
    """For a `tool` record whose whole output MAID kept beside the session (`full_output`,
    docs/sessions.md): the labelled line every reader shows after the result, else ""."""
    kept = r.get("full_output")
    if not isinstance(kept, dict) or not isinstance(kept.get("path"), str):
        return ""
    parts = kept["path"].split("/")
    sid = parts[0][:-2] if parts[0].endswith(".d") else parts[0]
    call = parts[-1][:-4] if parts[-1].endswith(".out") else parts[-1]
    note = "[%s] %d bytes" % (FULL_OUTPUT_LABEL, kept.get("bytes", 0))
    if kept.get("dropped"):
        note += " (%d more dropped over full_output_max_mb)" % kept["dropped"]
    return note + ": maid sessions output %s %s" % (sid, call)


def to_claude(records, sid="maid", resumable=False):
    """A MAID session as Claude Code grammar. **Lossless by position**: every output
    record carries `maid.index`, the input line it came from (0-based), unless `resumable`.

    For reading, one record per input record, so `Ln` markers, boundaries and slices
    name the session's own lines: a `tool` record is one assistant record holding its
    `tool_use` and its `tool_result`. `resumable` is the shape grafted into a Claude Code
    transcript: the result split into the user record that follows, and no field the
    build checks do not know (the index, the compaction flag: a summary grafted in is a
    user turn marked `origin.kind: compact`, not a boundary of the transcript it joins)."""
    start = first_start(records)
    ws, model = start.get("workspace", ""), start.get("model", "")
    out, parent = [], None

    def env(i, t):
        rec = {"isSidechain": False, "userType": "external", "entrypoint": "cli", "cwd": ws,
               "sessionId": sid, "version": "", "gitBranch": "HEAD"}
        if not resumable:
            rec["maic"] = {"index": i, "type": t}
        return rec

    def message(i, r, role, content, uid, origin=None):
        # The shape `build` emits, field for field, so `build.verify` accepts adapted
        # records wherever a MAID session meets a Claude Code transcript.
        nonlocal parent
        rec = {"parentUuid": parent, "type": role, "uuid": uid, "timestamp": utc(r.get("time"))}
        if role == "user":
            rec.update({"promptId": uid + "-prompt", "message": {"role": "user", "content": content},
                        "permissionMode": "auto", "origin": {"kind": origin or "human"},
                        "promptSource": "typed"})
        else:
            rec.update({"message": {"model": model, "id": "msg_" + uid, "type": "message",
                                    "role": "assistant", "content": content,
                                    "stop_reason": "end_turn", "stop_sequence": None,
                                    "stop_details": None,
                                    "usage": {"input_tokens": 0, "cache_creation_input_tokens": 0,
                                              "cache_read_input_tokens": 0, "output_tokens": 0,
                                              "service_tier": "standard"},
                                    "diagnostics": None},
                        "requestId": "req_" + uid, "effort": "high", "session_id": sid})
        rec.update(env(i, r.get("type")))
        parent = uid
        return rec

    for i, r in enumerate(records):
        t = r.get("type")
        if t == "user":
            out.append(message(i, r, "user", r.get("text", ""), "maid-%d" % i))
        elif t == "assistant":
            out.append(message(i, r, "assistant", [{"type": "text", "text": r.get("text", "")}],
                               "maid-%d" % i))
        elif t == "tool":
            call = "maid-tool-%d" % i
            use = {"type": "tool_use", "id": call, "name": r.get("tool", ""), "input": r.get("arguments", {})}
            content = r.get("result", "")
            if not resumable and kept_note(r):
                # Read only: a resumed transcript gets exactly what the model got.
                content += "\n\n" + kept_note(r)
            result = {"type": "tool_result", "tool_use_id": call, "content": content,
                      "is_error": not r.get("ok", True)}
            if resumable:
                out.append(message(i, r, "assistant", [use], "maid-%d" % i))
                out.append(message(i, r, "user", [result], "maid-%d-result" % i))
            else:
                out.append(message(i, r, "assistant", [use, result], "maid-%d" % i))
        elif t == "context":
            out.append(message(i, r, "user", r.get("text", ""), "maid-%d" % i, origin="context"))
        elif t == "compact":
            text = r.get("summary") or "compacted (%s)" % r.get("stage", "")
            rec = message(i, r, "user", text, "maid-%d" % i, origin="compact")
            if not resumable:
                rec["isCompactSummary"] = True
            out.append(rec)
        else:
            rec = {"type": "system", "subtype": "maid-%s" % t, "isMeta": True}
            rec.update({k: v for k, v in r.items() if k != "type"})
            rec.update(env(i, t))
            out.append(rec)
    return out


def render_blocks(content):
    """Claude Code message content as text, the way `build` renders an export: tool
    traffic fenced, thinking dropped (its signature does not verify elsewhere)."""
    if isinstance(content, str):
        return content
    parts = []
    for b in content or []:
        if not isinstance(b, dict):
            continue
        t = b.get("type")
        if t == "text":
            parts.append(b.get("text", ""))
        elif t == "tool_use":
            parts.append("```tool_use %s\n%s\n```" % (b.get("name", ""),
                                                      json.dumps(b.get("input", {}), indent=2, ensure_ascii=False)))
        elif t == "tool_result":
            c = b.get("content")
            parts.append("```tool_result\n%s\n```" % (c if isinstance(c, str) else json.dumps(c, ensure_ascii=False)))
    return "\n\n".join(p for p in parts if p.strip())


def from_claude(records, at=None):
    """Claude Code message records as MAID `msg` plus transcript records (text only)."""
    out = []
    for r in records:
        if r.get("type") not in ("user", "assistant"):
            continue
        role = r["type"]
        text = render_blocks((r.get("message") or {}).get("content"))
        t = at or stamp()
        if r.get("timestamp"):
            try:
                t = stamp(datetime.datetime.fromisoformat(r["timestamp"].replace("Z", "+00:00")).timestamp())
            except ValueError:
                pass
        out.extend(turn(role, text, t))
    return out


def turn(role, text, at=None, extra=None):
    """One conversational turn as MAID writes it: the `msg` the model replays and the
    transcript record a reader sees. `extra` lands on both (marks, provenance)."""
    t = at or stamp()
    msg = {"type": "msg", "time": t, "role": role, "content": text}
    shown = {"type": role, "time": t, "text": text}
    for rec in (msg, shown):
        rec.update(extra or {})
    return [msg, shown]


def notice(text, role="user", at=None, extra=None):
    """A note placed by a tool, marked as MAID marks its own: an `inject` record, the
    `msg` the model reads, and a `context` notice the transcript shows, never a typed turn."""
    t = at or stamp()
    recs = [{"type": "inject", "time": t, "role": role},
            {"type": "msg", "time": t, "role": role, "content": text},
            {"type": "context", "time": t, "text": text}]
    for r in recs:
        r.update(extra or {})
    return recs


def marker(subtype, tool="trans-fairy", at=None, **fields):
    """A `cai` record: lineage or a boundary. MAID ignores it; `state --audit` and
    `--ledger` read it by its `-boundary` subtype, as they read Claude Code boundaries."""
    rec = {"type": MARKER, "subtype": subtype, "tool": tool, "time": at or stamp()}
    rec.update(fields)
    return rec


def lineage_of(records):
    """`previousSessionId`, `originSessionId`, `includedSessionIds` from the last marker carrying them."""
    for r in reversed(records):
        if r.get("type") == MARKER and r.get("previousSessionId"):
            return {k: r.get(k) for k in ("previousSessionId", "originSessionId", "includedSessionIds")}
    return {}


def write_new(path, records, dry_run=False):
    """Write a session file the way MAID creates one: 0600, one record per line."""
    if dry_run:
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        for r in records:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    return path
