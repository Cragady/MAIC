"""The write half of trans-fairy, kept in a separate tool on purpose.

`trans-fairy` creates: it mints a new sessionId, writes a new transcript, and
never overwrites or removes. Every protection in its design rests on that being
unconditional, and a flag that made it conditional would end it -- which is the
same argument that keeps `redact` separate, with more force here, because this
write lands in a projects directory.

So this tool admits to being the thing that writes, and pays for it with the
checks below.
"""
import datetime
import filecmp
import glob
import json
import os
import shutil
import sys
import time

from cai.grammar import maic as fmt

SESSIONS_GLOB = os.path.expanduser("~/.claude/sessions/*.json")
LIVE_MTIME_SECONDS = 300


def load(path):
    """Parse a transcript, raising on the first malformed line rather than skipping."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip():
                continue
            try:
                out.append(json.loads(line))
            except ValueError as e:
                raise ValueError("%s line %d is not valid JSON: %s" % (path, n, e))
    return out


def session_id_of(lines):
    """The id a transcript carries, read from a message record.

    Frames may be minted fresh by an operation, so the first record holding a
    sessionId is not reliably the transcript's own -- the trap rewrite_session
    had to be corrected for.
    """
    sid = next((l.get("sessionId") for l in lines
                if l.get("type") in ("user", "assistant") and l.get("sessionId")), None)
    return sid or next((l.get("sessionId") for l in lines if l.get("sessionId")), None)


def liveness(target, sessions_glob=SESSIONS_GLOB, now=None):
    """Why this transcript looks live, or an empty list if it does not.

    **A live transcript cannot be written to.** The client holds it open and
    appends on every turn, so a whole-file rewrite destroys whatever it wrote
    between the read and the write -- and the backup does not save that, because
    a backup preserves the pre-write state rather than the write it raced. This
    was established the hard way: a session cannot perform an operation on its
    own transcript, because the only session able to do it safely is one that is
    not running in it.

    Two signals, and either is enough to refuse:
      registry  the id appears in a live session file
      mtime     the file was written within LIVE_MTIME_SECONDS
    """
    reasons = []
    try:
        lines = load(target)
        sid = session_id_of(lines)
    except (ValueError, OSError):
        lines, sid = [], None
    if lines and fmt.is_maic(lines):
        # MAIC keeps no registry; the `start` record of the last open names the host and pid.
        reasons.extend(fmt.live_reasons(lines))
    if sid:
        # **Compare ids as IDS, not as substrings.** `sid in fh.read()` matched a
        # one-character sessionId against every session file on the machine and
        # reported the transcript live -- a false refusal that fails safe and
        # still teaches an operator to reach past the guard. Third instance of
        # substring-versus-id in two days, after a grant prefix compared as a
        # full id and a stale prefix read as a mismatch.
        for path in glob.glob(sessions_glob):
            try:
                with open(path, encoding="utf-8") as fh:
                    rec = json.load(fh)
            except (OSError, ValueError):
                continue
            if isinstance(rec, dict) and rec.get("sessionId") == sid:
                reasons.append("session %s is listed live in %s"
                               % (sid[:8], os.path.basename(path)))
                break
    try:
        age = (time.time() if now is None else now) - os.path.getmtime(target)
        if age < LIVE_MTIME_SECONDS:
            reasons.append("written %d seconds ago, under the %d-second threshold"
                           % (age, LIVE_MTIME_SECONDS))
    except OSError:
        pass
    return reasons


def backup(target, dest, force=False):
    """Fail-closed, and a copy rather than a move: the original must survive.

    Never `cp -n`, which declines silently and returns success.
    """
    if os.path.isdir(dest):
        dest = os.path.join(dest, os.path.basename(target) + ".bak")
    if os.path.exists(dest) and not force:
        raise FileExistsError(
            "backup already exists: %s -- refusing to overwrite it. A backup that "
            "disagrees with its source is the only reference a verify has." % dest)
    shutil.copy2(target, dest)
    return dest


def maic_checks(lines):
    """What a MAIC replacement must satisfy: the same question, MAIC's shapes.

    There is no uuid chain and no sessionId to agree on (the id is the file name), so
    the structural checks are the ones MAIC's loader relies on: `msg` content a string
    or a list of blocks, every tool message paired with a call the model made, and a
    `resumed_from` pointer only where MAIC writes one, first.
    """
    checks = []
    msgs = [l for l in lines if l.get("type") in ("user", "assistant", "msg")]
    checks.append(("has message records", bool(msgs), "" if msgs else "no user, assistant or msg records"))
    bad = [i for i, l in enumerate(lines, 1) if l.get("type") == "msg"
           and not isinstance(l.get("content"), (str, list))]
    checks.append(("msg content is a string or a list of blocks", not bad,
                   "" if not bad else "missing or odd content on line(s) %s" % ", ".join(map(str, bad[:3]))))
    calls, orphans = set(), []
    for l in lines:
        if l.get("type") != "msg":
            continue
        if l.get("role") == "assistant":
            calls.update(c.get("id") for c in l.get("tool_calls") or [] if isinstance(c, dict))
        elif l.get("role") == "tool" and l.get("tool_call_id") not in calls:
            orphans.append(l.get("tool_call_id"))
    checks.append(("no orphan tool message", not orphans,
                   "" if not orphans else "%d tool message(s) with no matching tool call" % len(orphans)))
    late = [i for i, l in enumerate(lines, 1) if l.get("type") == "resumed_from" and i > 1]
    checks.append(("resumed_from only as the first record", not late,
                   "" if not late else "a resumed_from pointer on line %d" % late[0]))
    untyped = [i for i, l in enumerate(lines, 1) if not l.get("type")]
    checks.append(("every record has a type", not untyped,
                   "" if not untyped else "no type on line(s) %s" % ", ".join(map(str, untyped[:3]))))
    return checks


def safety_backup(target, now=None):
    """MAIC's copy before any in-place write: `<state>/sessions/.backups/<id>/<UTC>.jsonl`, 0600.

    Taken for every target, MAIC session or Claude Code transcript, beside the backup
    the operator named: the operator's copy is the one `--verify` compares against,
    this one is the one `restore` finds by id without being told where it is.
    """
    d = os.path.join(fmt.backups_dir(), fmt.session_id(target))
    os.makedirs(fmt.backups_dir(), mode=0o700, exist_ok=True)
    os.makedirs(d, mode=0o700, exist_ok=True)
    stamp = (now or datetime.datetime.now(datetime.timezone.utc)).strftime("%Y%m%dT%H%M%SZ")
    n = 0
    while True:
        dest = os.path.join(d, stamp + ("-%d" % n if n else "") + ".jsonl")
        try:
            fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            break
        except FileExistsError:
            n += 1
    with os.fdopen(fd, "wb") as out, open(target, "rb") as src:
        shutil.copyfileobj(src, out)
    return dest


def replace_file(target, source):
    """Land `source` as `target` through a temp file in the same directory and a rename,
    so a crash leaves either the old file or the new one, never a torn one."""
    tmp = os.path.join(os.path.dirname(os.path.abspath(target)) or ".",
                       ".%s.cai-new" % os.path.basename(target))
    shutil.copy2(source, tmp)
    try:
        shutil.copymode(target, tmp)
    except OSError:
        pass
    os.replace(tmp, target)


def rewritten_record(invocation, backup, restored_from=None, before=None, after=None):
    rec = {"type": fmt.REWRITTEN, "time": fmt.stamp(), "tool": "trans-fairy-write",
           "invocation": invocation, "backup": backup}
    if restored_from:
        rec["restored_from"] = restored_from
    if before is not None:
        rec["records_before"] = before
    if after is not None:
        rec["records_after"] = after
    return rec


def append_record(target, rec):
    with open(target, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(rec, ensure_ascii=False) + "\n")


def list_backups(spec):
    """Every safety backup of a session, oldest first: `{"backup", "taken", "bytes"}` rows."""
    path = fmt.find_session(spec)
    sid = fmt.session_id(path) if path else spec
    d = os.path.join(fmt.backups_dir(), sid)
    rows = []
    if os.path.isdir(d):
        for f in os.listdir(d):
            if f.endswith(".jsonl"):
                p = os.path.join(d, f)
                rows.append({"backup": p, "taken": f[:-len(".jsonl")], "bytes": os.path.getsize(p)})

    def order(row):
        # `<stamp>-<n>` is the n-th copy within the same second, so it sorts after `<stamp>`.
        stamp, _, n = row["taken"].partition("-")
        return stamp, int(n) if n.isdigit() else 0
    return {"session": sid, "path": path, "backups": sorted(rows, key=order)}


def restore(spec, backup_ts=None, ignore_live=False, dry_run=False, invocation=None):
    """Put a safety backup back in place, **after backing up what is there now**.

    The newest backup unless `--backup TS` names one. The current file is copied to
    the same `.backups/<id>/` first, so a restore is itself undoable by another restore.
    """
    path = fmt.find_session(spec)
    if not path:
        raise FileNotFoundError("no session matching %r (a MAIC session id, a prefix, or a path)" % spec)
    rows = list_backups(path)["backups"]
    if not rows:
        raise FileNotFoundError("no backups recorded for %s under %s" % (fmt.session_id(path), fmt.backups_dir()))
    if backup_ts:
        want = backup_ts[:-len(".jsonl")] if backup_ts.endswith(".jsonl") else backup_ts
        hits = [r for r in rows if r["taken"] == want] or [r for r in rows if r["taken"].startswith(want)]
        if len(hits) != 1:
            raise FileNotFoundError("%d backups match %r; the recorded ones are: %s"
                                    % (len(hits), backup_ts, ", ".join(r["taken"] for r in rows)))
        chosen = hits[0]["backup"]
    else:
        chosen = rows[-1]["backup"]
    live = liveness(path)
    if live and not ignore_live:
        raise RuntimeError("refusing to restore over a transcript that looks live:\n"
                           + "".join("  - %s\n" % r for r in live)
                           + "Close the session, or pass --ignore-live if you know it is not running.")
    chosen_lines = load(chosen)
    # The file being replaced may be the broken one a restore exists for: count what
    # still parses rather than refusing to restore it.
    with open(path, encoding="utf-8", errors="replace") as fh:
        current = fmt.grecords.load(fh.read())
    taken = None
    if not dry_run:
        taken = safety_backup(path)
        replace_file(path, chosen)
        if not filecmp.cmp(chosen, path, shallow=False):
            raise RuntimeError("cmp failed after restore: %s != %s" % (chosen, path))
        if fmt.is_maic(chosen_lines):
            append_record(path, rewritten_record(invocation or "trans-fairy-write restore %s" % spec,
                                                 taken, restored_from=chosen,
                                                 before=len(current), after=len(chosen_lines)))
    return {"target": path, "restored_from": chosen, "backup_of_previous": taken,
            "records_before": len(current), "records_restored": len(chosen_lines),
            "rewritten_record": fmt.is_maic(chosen_lines) and not dry_run,
            "liveness_overridden": bool(live and ignore_live), "dry_run": dry_run}


def check_source(lines):
    """What a replacement must satisfy before it is allowed to land.

    Checked BEFORE the write, not after: a malformed transcript that reaches a
    projects directory crashes the client on load, and by then the damage is
    done whether or not a backup exists. A MAIC session gets MAIC's checks.
    """
    if fmt.is_maic(lines):
        return maic_checks(lines)
    checks = []
    msgs = [l for l in lines if l.get("type") in ("user", "assistant")]
    checks.append(("has message records", bool(msgs), "" if msgs else "no user or assistant records"))

    bad = [l.get("uuid") for l in lines
           if l.get("type") == "assistant"
           and not isinstance((l.get("message") or {}).get("content"), list)]
    checks.append(("assistant content is a list of blocks", not bad,
                   "" if not bad else "string or missing content on %s" % ", ".join(map(str, bad[:3]))))

    seen, broken = set(), None
    for l in msgs:
        p = l.get("parentUuid")
        if p is not None and p not in seen:
            broken = l.get("uuid")
            break
        seen.add(l.get("uuid"))
    checks.append(("parent chain unbroken", broken is None,
                   "" if broken is None else "record %s parents onto a missing uuid" % broken))

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

    sids = {l.get("sessionId") for l in lines if l.get("sessionId")}
    checks.append(("exactly one sessionId", len(sids) == 1,
                   "" if len(sids) == 1 else "%d distinct sessionIds present" % len(sids)))
    return checks


def write(target, source, backup_dest, force=False, ignore_live=False, dry_run=False,
          invocation=None):
    """Replace `target` with `source`, having earned the right to.

    Order matters and is the whole safety of the operation: refuse a live
    target, validate the replacement, back up the original, write, verify the
    copy. A failure at any step leaves the target untouched.

    MAIC's additions, after cai's own backup and before the write: a second copy under
    `<state>/sessions/.backups/<id>/`, the write through a temp file and a rename, and
    for a MAIC session a `rewritten` record appended naming that copy and this invocation.
    A Claude Code transcript gets the copy and the rename and stays byte for byte what
    `--from` gave, as it always has.
    """
    if not os.path.exists(target):
        raise FileNotFoundError(
            "target does not exist: %s\n"
            "This tool overwrites; it does not create. Creating a transcript is "
            "`cai trans-fairy install`, which never overwrites." % target)

    live = liveness(target)
    if live and not ignore_live:
        raise RuntimeError(
            "refusing to write a transcript that looks live:\n"
            + "".join("  - %s\n" % r for r in live)
            + "A running client appends on every turn, so a rewrite destroys whatever it\n"
              "wrote between the read and this write -- and the backup cannot recover that,\n"
              "because it holds the pre-write state rather than the write it raced.\n"
              "Close the session, or pass --ignore-live if you know the registry is stale.")

    src_lines = load(source)
    failed = [(n, d) for n, ok, d in check_source(src_lines) if not ok]
    if failed:
        raise RuntimeError("replacement failed verification, refusing to write:\n"
                           + "".join("  %s: %s\n" % (n, d) for n, d in failed))

    tgt_sid = session_id_of(load(target))
    src_sid = session_id_of(src_lines)
    if tgt_sid and src_sid and tgt_sid != src_sid:
        raise RuntimeError(
            "sessionId mismatch: target carries %s, replacement carries %s.\n"
            "Writing this would leave a file whose name and contents disagree, which is\n"
            "the defect `install` was corrected for. Pass a replacement for THIS session."
            % (tgt_sid[:8], src_sid[:8]))

    bdest = None if dry_run else backup(target, backup_dest, force=force)
    if not dry_run:
        safety = safety_backup(target)
        sys.stderr.write("copy kept for `cai trans-fairy-write restore`: %s\n" % safety)
        before = len(load(target))
        replace_file(target, source)
        if not filecmp.cmp(source, target, shallow=False):
            raise RuntimeError("cmp failed after write: %s != %s" % (source, target))
        if fmt.is_maic(src_lines):
            append_record(target, rewritten_record(
                invocation or "trans-fairy-write %s --from %s" % (target, source), safety,
                before=before, after=len(src_lines)))
    return {"target": target, "source": source, "backup": bdest,
            "sessionId": tgt_sid, "records": len(src_lines),
            "liveness_overridden": bool(live and ignore_live),
            "dry_run": dry_run,
            "checks_passed": [n for n, ok, _ in check_source(src_lines) if ok]}


def verify(target, backup_path):
    """Compare the written file against the backup it replaced. Reports, never repairs."""
    t, b = load(target), load(backup_path)
    tsid, bsid = session_id_of(t), session_id_of(b)
    failed = [(n, d) for n, ok, d in check_source(t) if not ok]
    return {"target": target, "backup": backup_path,
            "records_now": len(t), "records_before": len(b),
            "delta": len(t) - len(b),
            "sessionId_unchanged": tsid == bsid,
            "failing_checks": [{"check": n, "detail": d} for n, d in failed],
            "ok": not failed and tsid == bsid}
