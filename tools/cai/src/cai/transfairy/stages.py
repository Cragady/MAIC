"""The stages, as functions. cli.py wires them; --agent walks them one per call.

Order (DESIGN, Stages): 00 init, 01 split, 02 build, 03 install, 04 resume.
resume is human-only and never run here -- install prints its command.
"""
import filecmp
import json
import os
import shutil
import sys

from datetime import datetime, timezone

from cai.transfairy import build as buildmod
from cai.grammar import records
from cai.transfairy import graft as graftmod
from cai.transfairy import maic as maicops
from cai.grammar import maic as fmt
from cai.transfairy.pool import Pool
from cai import mode as modemod

STAGES = ["init", "split", "build", "install", "resume"]


def _fmt(path):
    if not os.path.exists(path):
        return "absent"
    st = os.stat(path)
    return "%d bytes, mtime %d" % (st.st_size, int(st.st_mtime))


def init(tree, dry_run=False, verbose=False):
    """Scaffold the working tree; report every write target; write nothing outside it."""
    made = []
    for d in (tree.job_dir, tree.download, tree.staged, tree.previous_agent, tree.tf_data, tree.bak):
        if not dry_run:
            os.makedirs(d, exist_ok=True)
        made.append(d)
    report = {"job": tree.job, "job_dir": tree.job_dir,
              "write_targets": {p: _fmt(p) for p in tree.write_targets()},
              "injection_target (NAMED ONLY, never written)": tree.target_cwd,
              "projects_dir (install destination)": tree.projects_dir,
              "pool": tree.pool + " (" + _fmt(tree.pool) + ")"}
    return report


def split(tree, export_path, target_uuid=None, dry_run=False):
    """Extract one conversation from a claude.ai export into download/extracted.json.

    Verifies: target present in the extract, absent from the remainder, and the
    remainder count is exactly one fewer than the original. Never consumes the
    source export.
    """
    with open(export_path, encoding="utf-8") as fh:
        convs = json.load(fh)
    if isinstance(convs, dict):
        convs = [convs]
    if target_uuid is None:
        tgt_file = os.path.join(tree.download, "conversation-target.json")
        if os.path.exists(tgt_file):
            with open(tgt_file, encoding="utf-8") as fh:
                target_uuid = json.load(fh)["uuid"]
        elif len(convs) == 1:
            target_uuid = convs[0].get("uuid")
        else:
            raise ValueError("multiple conversations in export; pass --target-uuid")
    match = [c for c in convs if c.get("uuid") == target_uuid]
    rest = [c for c in convs if c.get("uuid") != target_uuid]
    if len(match) != 1:
        raise ValueError("expected exactly 1 conversation with uuid %s, got %d" % (target_uuid, len(match)))
    if any(c.get("uuid") == target_uuid for c in rest):
        raise AssertionError("target still present in remainder")
    if len(rest) != len(convs) - 1:
        raise AssertionError("remainder count is not one fewer than the original")
    out = os.path.join(tree.download, "extracted.json")
    if not dry_run:
        os.makedirs(tree.download, exist_ok=True)
        with open(out, "w", encoding="utf-8") as fh:
            json.dump(match[0], fh, ensure_ascii=False, indent=2)
    return {"target_uuid": target_uuid, "extracted": out,
            "name": match[0].get("name"), "messages": len(match[0].get("chat_messages", [])),
            "remaining": len(rest)}


def build(tree, conversation_path, version, model, dry_run=False):
    """Build the staged transcript and verify it before reporting success."""
    with open(conversation_path, encoding="utf-8") as fh:
        conv = json.load(fh)
    pool = Pool(tree.pool)
    sid, lines, stats = buildmod.build(conv, pool, tree.target_cwd, version, model)
    checks = buildmod.verify(lines)
    failed = [(n, d) for n, ok, d in checks if not ok]
    if failed:
        raise RuntimeError("build verification failed: " + "; ".join("%s (%s)" % (n, d) for n, d in failed))
    out = os.path.join(tree.staged, sid + ".jsonl")
    if not dry_run:
        os.makedirs(tree.staged, exist_ok=True)
        with open(out, "w", encoding="utf-8") as fh:
            for l in lines:
                fh.write(json.dumps(l, ensure_ascii=False) + "\n")
        os.chmod(out, 0o600)
    return {"sessionId": sid, "staged": out, "lines": len(lines),
            "stats": stats, "checks": [(n, ok) for n, ok, _ in checks]}


def install(tree, staged_path=None, reflow=False, dry_run=False):
    """cp -p the staged transcript into the projects dir. NEVER overwrite (invariant 8).

    The check lives here, at the write path -- not in argument parsing -- so no
    future flag can reach around it. Verifies the copy with cmp.
    """
    if staged_path is None:
        staged = [f for f in os.listdir(tree.staged) if f.endswith(".jsonl")] if os.path.isdir(tree.staged) else []
        if len(staged) != 1:
            raise ValueError("expected exactly one staged .jsonl; found %d -- pass --staged" % len(staged))
        staged_path = os.path.join(tree.staged, staged[0])
    # The id comes from the RECORDS, and the destination is named for it. Reading
    # it off the file name was the guess: a transcript composed with --out installs
    # under whatever name the caller chose, the name then disagrees with the
    # sessionId inside, and the client cannot find it. Ruled 2026-09-01 -- send it
    # to the destination renamed as the sessionId, rather than trusting the name.
    _lines = _read_jsonl(staged_path)
    if fmt.is_maic(_lines):
        return _install_maic(tree, staged_path, _lines, reflow=reflow, dry_run=dry_run)
    sid = graftmod.session_id_of(_lines)
    if not sid:
        raise ValueError("no sessionId found in %s; cannot name the destination" % staged_path)
    dest = os.path.join(tree.projects_dir, sid + ".jsonl")
    reflowed_from = None
    if os.path.exists(dest):
        if not reflow:
            st = os.stat(dest)
            raise FileExistsError(
                "a transcript with this id is already installed: %s\n"
                "  %d bytes, mtime %d\n"
                "trans-fairy is additive and never overwrites (invariant 8). This usually means a\n"
                "rebuild reused the same pool position, so the file already there is your own earlier\n"
                "install of the same transcript.\n"
                "  --reflow   mint a fresh id and install as a NEW transcript beside it\n"
                "Removing or moving the existing file is deliberately not this tool's job; see the\n"
                "housekeeping tool note in IMPROVEMENTS.md." % (dest, st.st_size, int(st.st_mtime)))
        # additive resolution: mint a fresh id, rewrite the transcript, install as new
        old_sid = sid
        lines = _lines
        new_sid = graftmod._sid()
        graftmod.rewrite_session(lines, new_sid)
        # An artificial fork, and unlike a native one it carries its lineage (P11).
        # isMeta, so it does not reach the model's context (P1).
        reflow_at = _now_iso()
        lines.insert(0, graftmod.reflow_boundary(new_sid, old_sid, reflow_at))
        staged_path = os.path.join(tree.staged, new_sid + ".jsonl")
        _write_jsonl(staged_path, lines, dry_run=dry_run)
        dest = os.path.join(tree.projects_dir, new_sid + ".jsonl")
        # the id moved, so what this install reports has to move with it
        sid = new_sid
        reflowed_from = old_sid
        if os.path.exists(dest):
            raise FileExistsError("reflow collided again on %s; the pool may be exhausted" % dest)
    # Two resume shapes, and which is right depends on whether a pristine copy
    # already exists elsewhere (P8: a plain resume appends in place; a fork
    # writes a new transcript and leaves this one byte-unchanged).
    resume_cmd = "cd %s && claude -r %s" % (tree.target_cwd, sid)
    resume_fork = "cd %s && claude -r %s --fork-session" % (tree.target_cwd, sid)
    if not dry_run:
        os.makedirs(tree.projects_dir, exist_ok=True)
        shutil.copy2(staged_path, dest)
        if not filecmp.cmp(staged_path, dest, shallow=False):
            raise RuntimeError("cmp failed after copy: %s != %s" % (staged_path, dest))
    if reflowed_from and not dry_run:
        graftmod.append_sidecar(tree.projects_dir,
                                graftmod.reflow_row(reflow_at, reflowed_from, sid))
    return {"installed": dest, "sessionId": sid,
            "reflowed_from": reflowed_from,
            "ledger": os.path.join(tree.projects_dir, graftmod.SIDECAR) if reflowed_from else None,
            "resume": resume_cmd,
            "resume_preserving": resume_fork,
            "note": "resume is a human step. Use `resume` to work in this transcript directly -- "
                    "correct when a pristine copy already exists elsewhere (a truncation still has "
                    "its source; a graft still has its base). Use `resume_preserving` when this IS "
                    "the only copy of the as-built and you want it kept byte-unchanged; the fork "
                    "writes a separate transcript."}


def _now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _install_maic(tree, staged_path, lines, reflow=False, dry_run=False):
    """A MAIC session into MAIC's sessions tree. **Never overwrite**, as for a projects dir.

    The id is the file name, as MAIC has it; the destination is `--claude-projects` when
    one was named, else the home MAIC would give the target cwd. A collision offers
    `--reflow`, which mints a fresh id and installs beside, with a boundary saying so.
    """
    dest_dir = maicops.install_dir(tree)
    sid = fmt.session_id(staged_path)
    dest = os.path.join(dest_dir, sid + ".jsonl")
    reflowed_from = None
    reflow_at = None
    if os.path.exists(dest):
        if not reflow:
            st = os.stat(dest)
            raise FileExistsError(
                "a session with this id is already installed: %s\n"
                "  %d bytes, mtime %d\n"
                "trans-fairy is additive and never overwrites (invariant 8).\n"
                "  --reflow   mint a fresh id and install as a NEW session beside it\n"
                "Removing or moving the existing file is deliberately not this tool's job."
                % (dest, st.st_size, int(st.st_mtime)))
        old_sid, reflow_at = sid, _now_iso()
        sid = fmt.mint_id(dest_dir)
        lines = [maicops.reflow_boundary(sid, old_sid, reflow_at)] + list(lines)
        staged_path = os.path.join(tree.staged, sid + ".jsonl")
        fmt.write_new(staged_path, lines, dry_run=dry_run)
        dest = os.path.join(dest_dir, sid + ".jsonl")
        reflowed_from = old_sid
        if os.path.exists(dest):
            raise FileExistsError("reflow collided again on %s" % dest)
    resume_cmd, resume_fork = maicops.resume_commands(tree, sid)
    if not dry_run:
        os.makedirs(dest_dir, mode=0o700, exist_ok=True)
        shutil.copy2(staged_path, dest)
        os.chmod(dest, 0o600)
        if not filecmp.cmp(staged_path, dest, shallow=False):
            raise RuntimeError("cmp failed after copy: %s != %s" % (staged_path, dest))
    if reflowed_from and not dry_run:
        graftmod.append_sidecar(dest_dir, graftmod.reflow_row(reflow_at, reflowed_from, sid))
    return {"installed": dest, "sessionId": sid,
            "reflowed_from": reflowed_from,
            "ledger": os.path.join(dest_dir, graftmod.SIDECAR) if reflowed_from else None,
            "resume": resume_cmd,
            "resume_preserving": resume_fork,
            "note": "resume is a human step. Use `resume` to work in this session directly; use "
                    "`resume_preserving` to keep the as-installed file byte-unchanged (maic forks "
                    "into a new file that points at it)."}


def _read_jsonl(path):
    """Strict, and now via the shared grammar.

    It was strict by ACCIDENT -- a bare `json.loads` in a comprehension -- so a
    malformed line raised a `JSONDecodeError` naming neither the file nor the
    line. Callers that survive a bad transcript (the audit) catch it; callers
    that must not proceed on a partial read (install, compose) still stop. What
    changes is that the error says which file and which line.
    """
    with open(path, encoding="utf-8") as fh:
        return records.load(fh.read(), strict=True, path=path)


def _write_jsonl(path, lines, dry_run=False):
    if dry_run:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        for l in lines:
            fh.write(json.dumps(l, ensure_ascii=False) + "\n")
    os.chmod(path, 0o600)


def graft_install(tree, base_sid, staged_path=None, direction="after", dry_run=False):
    """Graft the staged transcript onto an existing base (base_sid) in the projects dir.

    The base is read, never written. The result is a NEW sessionId written to
    staged and then installed create-only, so invariant 8 holds by construction.
    """
    if staged_path is None:
        staged_path = _sole_staged(tree)
    base_path = os.path.join(tree.projects_dir, base_sid + ".jsonl")
    if not os.path.exists(base_path):
        found = None if tree.projects_override else fmt.find_session(base_sid)
        if not found:
            raise FileNotFoundError("base transcript not found: %s" % base_path)
        base_path = found
    base = _read_jsonl(base_path)
    graft_lines = _read_jsonl(staged_path)
    if fmt.is_maic(base):
        at = _now_iso()
        base = fmt.flatten(base_path)
        sid, out = maicops.graft(base, graft_lines, fmt.session_id(base_path),
                                 fmt.session_id(staged_path) if fmt.is_maic(graft_lines)
                                 else graftmod.session_id_of(graft_lines),
                                 at, direction=direction, out_dir=tree.staged)
        new_staged = os.path.join(tree.staged, sid + ".jsonl")
        fmt.write_new(new_staged, out, dry_run=dry_run)
        res = install(tree, staged_path=new_staged, dry_run=dry_run)
        res["grafted_onto"] = base_sid
        res["direction"] = direction
        return res
    if fmt.is_maic(graft_lines):
        graft_lines = fmt.to_claude(graft_lines, fmt.session_id(staged_path), resumable=True)
    sid, out = graftmod.graft(base, graft_lines, _now_iso(), direction=direction)
    checks = buildmod.verify(out)
    failed = [(n, d) for n, ok, d in checks if not ok]
    if failed:
        raise RuntimeError("graft verification failed: " + "; ".join("%s (%s)" % (n, d) for n, d in failed))
    new_staged = os.path.join(tree.staged, sid + ".jsonl")
    _write_jsonl(new_staged, out, dry_run=dry_run)
    res = install(tree, staged_path=new_staged, dry_run=dry_run)
    res["grafted_onto"] = base_sid
    res["direction"] = direction
    return res


def inject_install(tree, mode="loud", source=None, staged_path=None, dry_run=False):
    """Mark the staged transcript as injected and, for loud, install it + sidecar.

    silent emits no marker and is NEVER installed in a projects dir (owner's
    trust rule); it stops at staged for the throwaway forked-resume harness.
    """
    if staged_path is None:
        staged_path = _sole_staged(tree)
    source = source or {"kind": "unspecified"}
    lines = _read_jsonl(staged_path)
    injectedAt = _now_iso()
    if fmt.is_maic(lines):
        sid = fmt.session_id(staged_path)
        out = maicops.mark_injected(lines, injectedAt, mode, source, sid)
    else:
        sid, out = graftmod.mark_injected(lines, injectedAt, mode, source)
    marked_path = os.path.join(tree.staged, sid + ".jsonl")
    _write_jsonl(marked_path, out, dry_run=dry_run)
    if mode == "silent":
        return {"mode": "silent", "staged": marked_path, "sessionId": sid,
                "installed": None,
                "note": "silent inject is never installed in a projects dir; run it via a "
                        "throwaway cwd + `claude -r %s --fork-session`" % sid}
    res = install(tree, staged_path=marked_path, dry_run=dry_run)
    if not dry_run:
        row = graftmod.inject_row(injectedAt, mode, source, sid)
        res["sidecar"] = graftmod.append_sidecar(tree.projects_dir, row)
    res["mode"] = "loud"
    return res


def _sole_staged(tree):
    staged = [f for f in os.listdir(tree.staged) if f.endswith(".jsonl")] if os.path.isdir(tree.staged) else []
    if len(staged) != 1:
        raise ValueError("expected exactly one staged .jsonl; found %d -- pass --staged" % len(staged))
    return os.path.join(tree.staged, staged[0])


def state(tree, dry_run=False):
    """Report the project dir's non-code state: transcripts, memory, CLAUDE.md.

    The base invocation reports what is there. --split extraction is designed
    (DESIGN, state) and not yet implemented here.
    """
    pd = tree.projects_dir
    transcripts = sorted(f for f in os.listdir(pd)) if os.path.isdir(pd) else []
    jsonl = [f for f in transcripts if f.endswith(".jsonl")]
    mem = os.path.join(pd, "memory")
    memory = sorted(os.listdir(mem)) if os.path.isdir(mem) else []
    sidecar = graftmod.SIDECAR in transcripts
    return {"projects_dir": pd, "exists": os.path.isdir(pd),
            "transcripts": jsonl, "count": len(jsonl),
            "memory_files": memory,
            "injections_sidecar_present": sidecar}


ROLES = ("previous-agent", "working-agent")


def audit_ledger(tree):
    """Scan the projects dir for transcripts the ledger cannot account for.

    A transcript with no lineage is one of three things and **the file cannot
    tell you which**: an original that was never derived, a native
    `--fork-session` (which records nothing -- P11), or a `true-silent` cut
    (which deliberately records nothing). That indistinguishability is the cost
    of `true-silent`, and the audit reports it rather than guessing.

    Reports rather than repairs. Provenance that was never written cannot be
    recovered by inspection, so there is nothing here to fix automatically.
    """
    pd = tree.projects_dir
    if not os.path.isdir(pd):
        raise FileNotFoundError("project dir does not exist: %s" % pd)
    traced, declared_original, unaccounted, unreadable = [], [], [], []
    for name in sorted(os.listdir(pd)):
        if not name.endswith(".jsonl"):
            continue
        sid = name[:-len(".jsonl")]
        try:
            recs = _read_jsonl(os.path.join(pd, name))
        except (ValueError, OSError):
            unreadable.append(sid)
            continue
        last = next((r for r in reversed(recs)
                     if r.get("type") in ("user", "assistant")), None)
        has_fields = bool(last and last.get("previousSessionId")) or bool(fmt.lineage_of(recs))
        has_boundary = any(str(r.get("subtype", "")).endswith("-boundary") for r in recs)
        # **The split, owner's design, buildable now that `rootKind` exists.**
        # A transcript that ASSERTS it has no ancestor is not unaccounted -- it
        # gave a complete answer. Filing it beside transcripts whose provenance
        # was lost made the bucket mean two opposite things: *nothing is missing*
        # and *something is missing and cannot be recovered*.
        root_kind = next((r.get("rootKind") for r in recs
                          if r.get("subtype") == "root-boundary" and r.get("rootKind")), None)
        if root_kind:
            declared_original.append({"session": sid, "rootKind": root_kind})
        elif has_fields or has_boundary:
            traced.append(sid)
        else:
            unaccounted.append(sid)
    return {"projects_dir": pd,
            "traced": traced, "declared_original": declared_original,
            "unaccounted": unaccounted, "unreadable": unreadable,
            "counts": {"traced": len(traced), "declared_original": len(declared_original),
                       "unaccounted": len(unaccounted), "unreadable": len(unreadable)},
            "note": "DECLARED ORIGINAL asserts it has no ancestor -- a complete answer, "
                    "not a gap. UNACCOUNTED is what remains: an original that predates "
                    "the marker, a native fork, or a true-silent cut. The file cannot "
                    "say which, and provenance never written cannot be recovered by "
                    "inspection. Splitting them is what makes the second bucket "
                    "actionable: before, a transcript with nothing missing sat beside "
                    "one whose provenance was lost, and the pile meant neither."}


def rebuild_ledger(tree, dry_run=False):
    """Regenerate the mapping ledger FROM the transcripts, rather than appending.

    The ledger was written independently as each operation ran, which made it a
    second home for a fact the transcripts now carry themselves -- lineage lives
    on the last message record of every transcript the tool produced. A second
    home is the drift this repository keeps closing, so the ledger becomes a
    DERIVED VIEW: throw it away, read the transcripts, write what they say.

    That is what makes "not authoritative" true rather than merely claimed. It
    also cannot go stale, because it is regenerated rather than maintained.
    """
    pd = tree.projects_dir
    if not os.path.isdir(pd):
        raise FileNotFoundError("project dir does not exist: %s" % pd)
    rows = []
    for name in sorted(os.listdir(pd)):
        if not name.endswith(".jsonl"):
            continue
        sid = name[:-len(".jsonl")]
        try:
            recs = _read_jsonl(os.path.join(pd, name))
        except (ValueError, OSError):
            continue
        last = next((r for r in reversed(recs)
                     if r.get("type") in ("user", "assistant")), None)
        if not last:
            continue
        if fmt.is_maic(recs):
            last = dict(last, **fmt.lineage_of(recs))
        prev = last.get("previousSessionId")
        origin = last.get("originSessionId")
        included = last.get("includedSessionIds") or []
        # what kind of operation produced it, read from its own boundary record
        kinds = sorted({r.get("subtype", "").replace("-boundary", "")
                        for r in recs if str(r.get("subtype", "")).endswith("-boundary")})
        if not (prev or origin or included or kinds):
            continue
        rows.append({"session": sid, "kind": "+".join(kinds) or "unknown",
                     "previous": prev, "origin": origin, "included": included})
    out = [graftmod.SIDECAR_HEADER_DERIVED]
    for r in rows:
        out.append("| %s | %s | %s | %s | %s |\n" % (
            r["session"], r["kind"], r["previous"] or "", r["origin"] or "",
            " ".join(r["included"]) if r["included"] else ""))
    path = os.path.join(pd, graftmod.SIDECAR)
    if not dry_run:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("".join(out))
    return {"ledger": path, "transcripts_scanned": len(
        [f for f in os.listdir(pd) if f.endswith(".jsonl")]),
        "rows": len(rows),
        "note": "derived from the transcripts; regenerate rather than edit"}


def state_split(tree, role, to=None, include=(), dry_run=False):
    """Extract the project dir's non-code state into a role dir (DESIGN, state).

    role is 'previous-agent' or 'working-agent'; the pair encodes role (item 9).
    Copies -- never moves (invariant 10) -- so the project dir stays intact.
    memory/ and CLAUDE.md come by default; transcripts only with
    include='transcripts', because they are the heavy, sensitive payload.
    The destination mirrors previous-agent/: memory/, CLAUDE.md, carry-forward.md,
    and (when included) the transcript(s).
    """
    if role not in ROLES:
        raise ValueError("--split must be one of %s" % (ROLES,))
    pd = tree.projects_dir
    if not os.path.isdir(pd):
        raise FileNotFoundError("project dir does not exist: %s" % pd)
    dest = os.path.abspath(to) if to else os.path.join(tree.job_dir, role)
    copied = {"memory": [], "claude_md": False, "transcripts": [], "carry_forward": None}

    if not dry_run:
        os.makedirs(dest, exist_ok=True)

    # memory/ (always)
    src_mem = os.path.join(pd, "memory")
    if os.path.isdir(src_mem):
        dst_mem = os.path.join(dest, "memory")
        for f in sorted(os.listdir(src_mem)):
            sp = os.path.join(src_mem, f)
            if os.path.isfile(sp):
                if not dry_run:
                    os.makedirs(dst_mem, exist_ok=True)
                    shutil.copy2(sp, os.path.join(dst_mem, f))
                copied["memory"].append(f)

    # CLAUDE.md (default; less stringent than transcripts)
    src_claude = os.path.join(pd, "CLAUDE.md")
    if os.path.isfile(src_claude):
        if not dry_run:
            shutil.copy2(src_claude, os.path.join(dest, "CLAUDE.md"))
        copied["claude_md"] = True

    # transcripts (only when explicitly included)
    if "transcripts" in include:
        for f in sorted(os.listdir(pd)):
            if f.endswith(".jsonl"):
                if not dry_run:
                    shutil.copy2(os.path.join(pd, f), os.path.join(dest, f))
                copied["transcripts"].append(f)

    # carry-forward.md is a slot in the role structure, seeded empty if absent
    cf = os.path.join(dest, "carry-forward.md")
    if not dry_run and not os.path.exists(cf):
        with open(cf, "w", encoding="utf-8") as fh:
            fh.write("# carry-forward\n\nNotes for the next session. Hand-authored; the tool seeds it empty.\n")
    copied["carry_forward"] = cf

    return {"role": role, "dest": dest, "copied": copied,
            "excluded_transcripts": "transcripts" not in include and len(jsonl_in(pd)) or 0,
            "note": "copied, not moved -- the project dir is intact (invariant 10)"}


def jsonl_in(pd):
    return [f for f in os.listdir(pd) if f.endswith(".jsonl")] if os.path.isdir(pd) else []


def compose(tree, root, suffix, source=None, mode="silent", out=None,
            insert_at=None, notice_extra=None, notice_only=None, dry_run=False,
            root_kind="synth"):
    """Compose an invisible root, a visible cut notice, and a real suffix.

    The cheap shape for a measurement arm: the root is the variable and is
    identical across arms, the suffix is the real conversational lead-up kept
    verbatim, and the distant history that neither needs is simply absent.
    """
    root_lines = _read_jsonl(root)
    suf_lines = _read_jsonl(suffix)
    source = source or {"kind": "unspecified"}
    if fmt.is_maic(root_lines) or fmt.is_maic(suf_lines):
        return _compose_maic(tree, root, suffix, root_lines, suf_lines, source, mode, out, insert_at,
                             notice_extra, notice_only, root_kind, dry_run)
    sid, lines = graftmod.compose_rooted(root_lines, suf_lines, _now_iso(), source,
                                         mode=mode, cwd=tree.target_cwd, insert_at=insert_at,
                                         notice_extra=notice_extra, notice_only=notice_only,
                                         root_kind=root_kind)
    # Verify BEFORE writing. compose takes hand-authored input, so this is where
    # a malformed record enters -- and one that reaches a projects directory
    # crashes the client on load rather than failing here.
    vchecks = graftmod.verify_composed(lines)
    vbad = [(n, d) for n, ok, d in vchecks if not ok]
    if vbad:
        raise RuntimeError("composed transcript failed verification:\n" + "\n".join(
            "  %s: %s" % (n, d) for n, d in vbad))
    dest = out or os.path.join(tree.staged, sid + ".jsonl")
    _write_jsonl(dest, lines, dry_run=dry_run)
    n_root = sum(1 for l in lines if l.get("origin", {}).get("kind") == "inject")
    return {"root": root, "suffix": suffix, "staged": dest, "sessionId": sid,
            "verified": [n for n, ok, _ in vchecks if ok],
            "mode": mode, "root_records": n_root,
            "suffix_records": sum(1 for l in lines if l.get("type") in ("user", "assistant")) - n_root,
            "note": "sources unmodified; the root is marked in meta only, the notice is the "
                    "only half the model reads, the suffix is verbatim"}


def _compose_maic(tree, root, suffix, root_lines, suf_lines, source, mode, out, insert_at,
                  notice_extra, notice_only, root_kind, dry_run):
    # A fork is composed as the conversation it holds, its parent's records first.
    root_lines = fmt.flatten(root) if fmt.is_maic(root_lines) else root_lines
    suf_lines = fmt.flatten(suffix) if fmt.is_maic(suf_lines) else suf_lines
    at = _now_iso()
    sid, lines = maicops.compose(root_lines, suf_lines, at, source, mode, tree.target_cwd,
                                 insert_at=insert_at, notice_extra=notice_extra, notice_only=notice_only,
                                 root_kind=root_kind, out_dir=tree.staged,
                                 root_id=fmt.session_id(root) if fmt.is_maic(root_lines) else graftmod.session_id_of(root_lines),
                                 suf_id=fmt.session_id(suffix) if fmt.is_maic(suf_lines) else graftmod.session_id_of(suf_lines))
    dest = out or os.path.join(tree.staged, sid + ".jsonl")
    if os.path.exists(dest) and not dry_run:
        raise FileExistsError("%s exists; compose never overwrites" % dest)
    fmt.write_new(dest, lines, dry_run=dry_run)
    n_root = sum(1 for l in lines if (l.get("origin") or {}).get("kind") == "inject" and l.get("type") in ("user", "assistant"))
    return {"root": root, "suffix": suffix, "staged": dest, "sessionId": sid,
            "verified": ["start record present", "notice present", "records are MAIC types"],
            "mode": mode, "root_records": n_root,
            "suffix_records": sum(1 for l in lines if l.get("type") in ("user", "assistant")) - n_root,
            "note": "sources unmodified; the root is marked in meta only, the notice is the "
                    "only half the model reads, the suffix is verbatim"}


def truncate(tree, transcript, at_line=None, at_uuid=None, before_text=None,
             out=None, mode="silent", keep="prefix", loud_lineage=False, dry_run=False):
    """Tail-truncate a transcript: keep a clean prefix, drop everything after.

    The higher-integrity operation precisely because it is dumber (DESIGN,
    Truncation): a cut point is one index, so you can prove nothing past it is
    present. The prefix keeps its own root, so unlike a head cut there is no
    re-rooting -- this is a cut, not a rewrite.

    **A prefix needs no repair.** Ordering authority is the parentUuid chain and
    every record's parent is earlier in the file, so the prefix of a valid
    transcript is a valid transcript. Records are therefore kept VERBATIM --
    every type, including the frame and meta records a real transcript carries
    per turn (ai-title, last-prompt, agent-name, atis-latch, attachment,
    system/turn_duration, bridge-session and the rest). Only `sessionId` is
    rewritten, so the result installs beside the original rather than replacing
    it (invariant 3).

    Verification here is CUT verification, deliberately not the six build checks.
    Those ask "did our builder emit conformant records" -- a real transcript is
    richer than our builder's output and fails them for reasons that have nothing
    to do with the cut. See IMPROVEMENTS.
    """
    lines = _read_jsonl(transcript)

    idx = None
    if at_line is not None:
        idx = int(at_line) - 1
    elif at_uuid and fmt.is_maic(lines):
        raise ValueError("a MAIC session carries no record uuids; cut with --at-line or --before-text")
    elif at_uuid:
        idx = next((i for i, l in enumerate(lines) if l.get("uuid") == at_uuid), None)
        if idx is None:
            raise ValueError("no record with uuid %s" % at_uuid)
    elif before_text:
        idx = next((i for i, l in enumerate(lines)
                    if before_text in json.dumps(l, ensure_ascii=False)), None)
        if idx is None:
            raise ValueError("no record containing that text")
        idx -= 1
    else:
        raise ValueError("give one of --at-line, --at-uuid, --before-text")
    if idx < 0 or idx >= len(lines):
        raise ValueError("cut point out of range (transcript has %d records)" % len(lines))

    if keep not in ("prefix", "suffix"):
        raise ValueError("keep must be 'prefix' (drop the tail) or 'suffix' (drop the head)")

    if fmt.is_maic(lines):
        modemod.check(mode)
        if mode == "true-silent":
            sys.stderr.write(
                "WARNING: --true-silent breaks trans-fairy's provenance contract.\n"
                "  This transcript will carry no lineage: no boundary record, no\n"
                "  previousSessionId, no originSessionId. It is indistinguishable from\n"
                "  an original, cannot be traced to its source, and will never appear in\n"
                "  the mapping ledger or an audit. Nothing downstream can recover what\n"
                "  this flag discards.\n")
        at = _now_iso()
        sid, kept, added, dropped = maicops.truncate(
            lines, idx, keep, mode, fmt.session_id(transcript), at, tree.target_cwd,
            loud_lineage=loud_lineage, out_dir=tree.staged)
        dest = out or os.path.join(tree.staged, sid + ".jsonl")
        fmt.write_new(dest, kept, dry_run=dry_run)
        last_msg = next((l for l in reversed(kept) if l.get("type") in ("user", "assistant")), None)
        return {"source": transcript, "staged": dest, "sessionId": sid,
                "mode": mode, "keep": keep, "appended_records": added,
                "cut_at_record": idx + 1, "records_kept": len(kept),
                "records_dropped": len(dropped),
                "last_kept_type": last_msg.get("type") if last_msg else None,
                "note": ("source unmodified; prefix kept verbatim; nothing past the cut is present"
                         if keep == "prefix" else
                         "source unmodified; nothing before the cut is present; the start record was "
                         "re-emitted and a resumed_from pointer dropped -- the only two changes")}

    sid = graftmod._sid()
    if keep == "prefix":
        kept = lines[:idx + 1]
        dropped = lines[idx + 1:]
    else:
        # A head cut is not the symmetric case. The survivors' root points at a
        # record that no longer exists, and the opening frames were dropped with
        # it -- so exactly two things are modified and both are enumerable:
        # the first record is re-parented to null, and opening frames are
        # re-emitted. Everything else stays verbatim. The provability claim
        # changes shape rather than disappearing: "nothing BEFORE the index
        # survives, and exactly these two fields were touched."
        kept = lines[idx:]
        dropped = lines[:idx]
        if not kept:
            raise ValueError("the cut would leave nothing")
        kept_u = {l.get("uuid") for l in kept if l.get("uuid")}
        orphans = [l for l in kept if l.get("parentUuid") and l["parentUuid"] not in kept_u]
        if len(orphans) > 1:
            raise RuntimeError(
                "a suffix cut leaves %d records whose parent is missing; the chain is not "
                "linear here and re-rooting would be a guess rather than a repair (first: %s)"
                % (len(orphans), orphans[0].get("uuid")))
        if orphans:
            orphans[0]["parentUuid"] = None
        first_msg = next((l for l in kept if l.get("type") in ("user", "assistant")), None)
        if first_msg is None:
            raise ValueError("the suffix holds no message records; there is nothing to root")
        kept = graftmod._frames_open(
            sid, first_msg.get("uuid"), first_msg.get("timestamp", _now_iso())) + kept
    if not kept:
        raise ValueError("the cut would leave nothing")
    graftmod.rewrite_session(kept, sid, lineage=(mode != "true-silent"))

    # --- cut verification: the three things a cut can actually get wrong ---
    kept_uuids = {l.get("uuid") for l in kept if l.get("uuid")}
    dropped_uuids = {l.get("uuid") for l in dropped if l.get("uuid")} - kept_uuids
    survivors = dropped_uuids & kept_uuids
    if survivors:
        raise RuntimeError("a dropped record survived the cut: %s" % sorted(survivors)[:3])

    dangling = [l.get("uuid") for l in kept
                if l.get("parentUuid") and l["parentUuid"] not in kept_uuids]
    if dangling:
        raise RuntimeError("the prefix has %d record(s) whose parent is missing; "
                           "the source was not a clean chain (first: %s)"
                           % (len(dangling), dangling[0]))

    stale = [l.get("type") for l in kept
             if l.get("sessionId") and l["sessionId"] != sid]
    if stale:
        raise RuntimeError("sessionId not fully rewritten on %d record(s)" % len(stale))

    last_msg = next((l for l in reversed(kept) if l.get("type") in ("user", "assistant")), None)
    # A cut manufactures context: a prefix presents as a whole conversation.
    # Lesson 14 -- announce it. Two records, because they do different jobs:
    # the meta one is lineage for tooling, the visible one is the only half the
    # model can actually read (P1).
    # Three modes. Everything is APPENDED after the cut point, never inserted.
    # The prefix is identical to the source except for two enumerable things:
    # the sessionId rewrite, and the lineage fields on the final record. Both
    # are asserted, which is the standard head truncation set -- a modification
    # that is enumerated and checked, rather than a claim of untouched bytes.
    #
    # The owner's vocabulary, and the axis is what the AGENT sees:
    #   silent       (default) meta, no message. Traceable by operator and tooling;
    #                the agent is blind, which is what a work continuation wants and
    #                what a measurement requires.
    #   loud         meta and message. The agent is told it is reading a prefix.
    #   true-silent  no meta, no message. The cut leaves no trace at all.
    modemod.check(mode)
    if mode == "true-silent":
        # Not a refusal -- it is a deliberate escape hatch and the operator may
        # want it. But it is the one mode that breaks what the tool otherwise
        # guarantees, so it says so at the point of use rather than in a doc
        # somebody has to have read.
        sys.stderr.write(
            "WARNING: --true-silent breaks trans-fairy's provenance contract.\n"
            "  This transcript will carry no lineage: no boundary record, no\n"
            "  previousSessionId, no originSessionId. It is indistinguishable from\n"
            "  an original, cannot be traced to its source, and will never appear in\n"
            "  the mapping ledger or an audit. Nothing downstream can recover what\n"
            "  this flag discards.\n")
    prefix_len = len(kept)
    at = _now_iso()
    source_sid = next((l.get("sessionId") for l in lines if l.get("sessionId")), None)
    if modemod.emits_meta(mode):
        kept.append(graftmod.truncate_boundary(sid, source_sid, at, idx + 1, len(dropped), keep=keep))
    if modemod.emits_message(mode):
        last = next((l for l in reversed(kept) if l.get("type") in ("user", "assistant")), None)
        ref = next((l for l in kept if l.get("cwd")), {})
        kept.append(graftmod.truncate_notice(
            sid, last.get("uuid") if last else None, ref.get("cwd", tree.target_cwd),
            ref.get("version", "2.1.236"), at, idx + 1, len(dropped),
            graftmod._sid(), graftmod._sid(), keep=keep))
    if loud_lineage and mode != "true-silent":
        graftmod.announce_lineage(kept, at)
    added = len(kept) - prefix_len
    expected = modemod.expected_records(mode)
    if loud_lineage and mode != "true-silent":
        expected += 1
    if added != expected:
        raise RuntimeError("expected %d appended record(s) in %s mode, got %d" % (expected, mode, added))
    dest = out or os.path.join(tree.staged, sid + ".jsonl")
    _write_jsonl(dest, kept, dry_run=dry_run)
    return {"source": transcript, "staged": dest, "sessionId": sid,
            "mode": mode, "keep": keep, "appended_records": added,
            "cut_at_record": idx + 1, "records_kept": len(kept),
            "records_dropped": len(dropped),
            "last_kept_type": last_msg.get("type") if last_msg else None,
            "note": ("source unmodified; prefix kept verbatim; nothing past the cut is present"
                     if keep == "prefix" else
                     "source unmodified; nothing before the cut is present; the first record was "
                     "re-parented to null and opening frames re-emitted -- the only two changes")}
