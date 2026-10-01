"""cai reflow -- reshape data with any member of an open family, verifying content survived."""
from cai import safewrite
from cai.grammar import maic as mfmt
from cai.grammar import records as grecords
from cai.transfairywrite import writer as tfw
import argparse
import glob as globmod
import inspect
import json
import os
import re
import shlex
import sys

#: A transcript under a projects directory is held open by a live client.
PROJECTS = re.compile(r"[/\\]\.claude[/\\]projects[/\\]")
CONFIRM = "yes, rewrite it"


def _session_kind(path):
    """Which refusal guards this file: "claude" (a Claude projects directory), "maic" (a MAIC
    session, in MAIC's sessions folder or recognised by its records anywhere), or None."""
    full = os.path.abspath(path)
    if PROJECTS.search(full):
        return "claude"
    sessions = os.path.abspath(mfmt.sessions_dir()) + os.sep
    if full.startswith(sessions):
        return "maic"
    try:
        if full.endswith(".jsonl") and mfmt.is_maic(mfmt.load(full)):
            return "maic"
    except OSError:
        pass
    return None


def _refusal(kind, path):
    if kind == "claude":
        return ("this file is in a Claude projects directory. A live "
                "session appends to it, so a write underneath one loses "
                "whatever it wrote in between. Use --to.")
    where = ("in MAIC's sessions directory" if os.path.abspath(path).startswith(
        os.path.abspath(mfmt.sessions_dir()) + os.sep) else "a MAIC session")
    return ("this file is %s. MAIC appends to a session as it runs, and a reflow "
            "rewrites its records into a shape MAIC cannot load. Use --to." % where)


def _counts(records):
    turns = sum(1 for r in records if isinstance(r, dict) and (
        r.get("type") in ("user", "assistant") or
        (r.get("type") == "msg" and r.get("role") in ("user", "assistant"))))
    return len(records), turns


def _verdict(kind, before_text, after_text):
    """Would the reflowed text still load as the same kind of transcript? trans-fairy-write's
    own checks decide, as they do before any of its writes."""
    if kind is None:
        return True, "not a transcript: no validation applies"
    label = "a MAIC session" if kind == "maic" else "a Claude Code transcript"
    before, after = grecords.load(before_text), grecords.load(after_text)
    rb, tb = _counts(before)
    ra, ta = _counts(after)
    failed = [(name, detail) for name, ok, detail in tfw.check_source(after) if not ok]
    if kind == "maic" and not mfmt.is_maic(after):
        failed.insert(0, ("is a MAIC session", "its records no longer read as one"))
    if failed:
        name, detail = failed[0]
        return False, ("the result would NOT load as %s: %s%s (records %d -> %d, turns %d -> %d)"
                       % (label, name, (": " + detail) if detail else "", rb, ra, tb, ta))
    return True, ("the result would load as %s (%d records, %d turns; before: %d, %d)"
                  % (label, ra, ta, rb, tb))


def _rewrite_session(op, op_name, f, kind, src, new, invocation):
    """The operator's way past the refusal: says what will happen, recommends the dry run,
    asks for the exact phrase at a terminal, backs up, writes, then checks what landed."""
    ok, verdict = _verdict(kind, src, new)
    label = "a MAIC session" if kind == "maic" else "a Claude Code transcript"
    dry = shlex.join(invocation + ["--dry-run"])
    err = sys.stderr
    print("cai reflow: %s is %s and will be rewritten IN PLACE by %r." % (f, label, op_name), file=err)
    print("  validation: %s" % verdict, file=err)
    print("  see it first without writing anything:\n    %s" % dry, file=err)
    if not sys.stdin.isatty():
        return {"refused": "--rewrite-session needs a person at a terminal to confirm; "
                           "nothing was written"}
    live = tfw.liveness(f)
    if live:
        return {"refused": "the transcript looks live (%s); stop that session first" % "; ".join(live)}
    print("  type '%s' to rewrite it, anything else stops: " % CONFIRM, end="", file=err, flush=True)
    answer = sys.stdin.readline().strip()
    if answer != CONFIRM:
        return {"refused": "not confirmed; nothing was written"}
    made = tfw.safety_backup(f)
    print("  backup: %s" % made, file=err)
    family.write(op, f, new)
    with open(f, encoding="utf-8", errors="replace") as fh:
        landed = fh.read()
    if landed not in (new, new + "\n"):
        tfw.replace_file(f, made)
        return {"refused": "what landed differs from what was validated; the backup %s was put "
                           "back" % made, "backup": made}
    if kind == "maic":
        rec = tfw.rewritten_record(shlex.join(invocation), made)
        rec["tool"] = "reflow"
        tfw.append_record(f, rec)
    print("  done: %s" % verdict.replace("would NOT load", "does NOT load")
          .replace("would load", "loads"), file=err)
    if not ok:
        stamp = os.path.basename(made)[:-len(".jsonl")]
        print("  to put the original back:\n    cai trans-fairy-write restore %s --backup %s"
              % (mfmt.session_id(f), stamp), file=err)
    return {"written": True, "wrote_to": f, "backup": made, "valid": ok}

import cai.reflow as family
from cai.reflow import lines as lines_op

MAN_HELP = """cai reflow -- full reference

  cai reflow FILE...                       the default member, `lines`
  cai reflow lines FILE... --mode sentence
  cai reflow context TRANSCRIPT.jsonl      the conversation a transcript carries
  cai reflow context TRANSCRIPT            the messages, into YOUR context
  cai reflow context T --replace --backup B  replace the transcript, lossily
  cai reflow --members                     what this installation can run

OPERATING ON DATA TO PRODUCE A RESULT
  `reflow` is a family, and it is not a text-rewrapping tool. It reshapes DATA.
  Line formatting is one shape of that; constraining reflow to line formatting
  is what takes its versatility away.

  **The invariant is the whole definition: a reflow changes the SHAPE of data,
  not its CONTENT.** That is what separates it from an edit, and every member
  must be able to prove it -- each defines its own signature, the family
  requires that one exists. A member that cannot say whether the content
  survived is an edit wearing the name.

  **Members are DISCOVERED**, from the `cai.reflow.operations` entry-point
  group, so a package adds one without editing cai, and `cai.reflow.register()`
  adds one in-process. **A member this build has no code for is NOT a refused
  proposal** -- it is a fact about this installation, reported with what is
  available and how to add one. Whether a name belongs to reflow is not this
  tool's ruling to make.

WHAT `context` REFLOWS IS THE CONTEXT
  Owner's definition: a reflow is a change in data that changes its overall
  shape. `cai reflow context` reflows the MESSAGES INTO YOUR CONTEXT -- the
  transcript is the SOURCE, not the target, and by default nothing is written
  anywhere. The output goes to stdout and the agent reading it now holds it.

  **Getting that backwards destroyed a live transcript on 2026-09-02**: the name
  said context and the code operated on the file. The name was never the defect.

  --replace REVERSES the target, deliberately and never by default. It is lossy:
  the transcript becomes a view of itself, readable and never resumable. So
  --backup is REQUIRED, an existing backup is refused rather than overwritten,
  and a live transcript is refused BEFORE the copy is taken -- a backup predates
  whatever a running client writes next and cannot recover it.

MEMBERS
  lines      one SENTENCE per line, or one PARAGRAPH per line -- opposite on
             the same input, three lines against one, each correct for the rule
             a different repository keeps. Abbreviations are guarded in sentence
             mode so `Dr. Smith` does not split.

  context    a transcript reflowed to the conversation it carries. Roughly
             twenty to one on representation; the messages survive byte for
             byte. It is a reflow rather than a redaction: nothing is removed
             from the artifact, a second shape of it is produced.

OPTIONS REACH ONLY THE MEMBERS THAT ACCEPT THEM
  An open family cannot have a fixed option set, so flags are filtered against
  each member's signature and a flag a member does not take is reported as a
  wrong flag -- not raised from inside the member, where it would read as a
  defect in the member.

  --mode        member-specific: line-breaking rule, or projection selection
  --full        members that slice by default operate on everything
  --before-compaction
                the RECOVERY half: what the last compaction REPLACED. The
                default is the half AFTER the boundary, which a resumed agent
                already holds -- right for a re-root, wrong for recovering
                anything. --full and --before-compaction select different
                slices and cannot be combined.
  --neutralise  rewrite carried fences (`context`) -- CANNOT hold the signature,
                and says so: neutralisation edits what the record said
  -n            dry run
  --rewrite-session
                OPERATOR ONLY, at a terminal: rewrite a transcript the refusal
                protects (a Claude projects file, a MAIC session) IN PLACE. Shows
                whether the result still loads, recommends the dry run, asks for
                the exact phrase "yes, rewrite it", copies the original to MAIC's
                backups first and checks what landed. A MAIC session is refused
                like a Claude projects file; with -n its validity is reported.

EXIT   0 ok   1 a file was refused   2 usage   4 no such member here
"""


def _members():
    found = family.discover()
    out = ["members here:"]
    for n in sorted(found):
        doc = (found[n].__doc__ or "").strip().split("\n")[0]
        out.append("  %-12s %s" % (n, doc))
    absent = family.named_elsewhere()
    if absent:
        out.append("")
        out.append("named in notation, no code installed here:")
        for n in sorted(absent):
            out.append("  %-12s add it under %r, or cai.reflow.register()" % (n, family.GROUP))
    return "\n".join(out)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    invocation = ["cai", "reflow"] + list(argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        print(_members())
        return 0
    if argv[0] == "--members":
        print(_members())
        return 0

    op_name = family.DEFAULT
    if argv[0] in family.discover():
        op_name = argv.pop(0)
    elif not argv[0].startswith("-") and not globmod.glob(argv[0]) and len(argv) > 1:
        op_name = argv.pop(0)
    try:
        op = family.operation(op_name)
    except family.NotAvailable as e:
        print("cai reflow: %s" % e, file=sys.stderr)
        return 4

    ap = argparse.ArgumentParser(prog="cai reflow " + op_name, add_help=False)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--mode")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--before-compaction", action="store_true",
                    help="the recovery half: what the last compaction REPLACED")
    ap.add_argument("--neutralise", action="store_true")
    ap.add_argument("--to", help="write the result HERE instead of over the input; "
                                 "required for a member that projects")
    ap.add_argument("--replace", action="store_true",
                    help="REPLACE the source with the reflowed result. Lossy for a "
                         "transcript, and --backup is required.")
    ap.add_argument("--backup", metavar="PATH",
                    help="where the original is copied before a --replace")
    ap.add_argument("--force", action="store_true",
                    help="OPERATOR ONLY: overwrite the destination, or rewrite an\n                          unrecoverable file")
    ap.add_argument("-n", "--dry-run", action="store_true")
    ap.add_argument("--rewrite-session", action="store_true",
                    help="OPERATOR ONLY, at a terminal: rewrite a refused transcript in\n"
                         "                          place; shows the validation, asks, backs up first")
    args = ap.parse_args(argv)

    accepted = set(inspect.signature(op.apply).parameters)
    opts = {}
    if args.mode is not None:
        opts["mode"] = args.mode
    elif op is lines_op:
        opts["mode"] = lines_op.PARAGRAPH
    # **Two slice selectors is a usage error, not a precedence question.**
    # Reported by peer session `RE: Flow`, 2026-09-04: `--before-compaction
    # --full` returned the before-half and exited 0, so a caller who asked for
    # everything received two thirds of it with nothing saying so. **A silently
    # short result is the failure shape both our records keep naming** -- well
    # formed, plausible, and missing the part you asked for.
    #
    # I had claimed this was already refused. It was refused in `cai read` and
    # never here, and I asserted it about a path I had not run.
    _slices = [n for n, v in (("--full", args.full),
                              ("--before-compaction", args.before_compaction)) if v]
    if len(_slices) > 1:
        print("cai reflow: %s select different slices and cannot be combined. Pick one; "
              "omit both for the default (since the last compaction)." % " and ".join(_slices),
              file=sys.stderr)
        return 2
    if args.full:
        opts["since_compaction"] = False
    if args.before_compaction:
        opts["since_compaction"] = False
        opts["before_compaction"] = True
    if args.neutralise:
        opts["neutralise"] = True
    rejected = sorted(k for k in opts if k not in accepted)
    if rejected:
        print("cai reflow: member %r does not take: %s"
              % (op_name, ", ".join(rejected)), file=sys.stderr)
        return 2

    if args.replace and not args.backup:
        print("cai reflow: --replace is lossy and requires --backup PATH. The original "
              "is copied there first, and the copy is the only resumable artifact "
              "afterwards.", file=sys.stderr)
        return 2
    if args.backup and not args.replace:
        print("cai reflow: --backup only means something with --replace; without it "
              "nothing is being replaced.", file=sys.stderr)
        return 2

    files = []
    for p in args.paths:
        files.extend(sorted(globmod.glob(p, recursive=True)) or [p])
    reports, refused = [], 0
    for f in files:
        try:
            src = family.read(op, f)
        except OSError as e:
            reports.append({"file": f, "error": str(e), "signature_held": False})
            refused += 1
            continue
        try:
            new, rep = op.apply(src, **opts)
        except ValueError as e:
            print("cai reflow: %s" % e, file=sys.stderr)
            return 2
        rep["file"] = f
        kind = _session_kind(f) if family.in_place(op) else None
        if args.dry_run and kind and (kind == "maic" or args.rewrite_session):
            valid, verdict = _verdict(kind, src, new)
            print("cai reflow: %s: %s" % (f, verdict), file=sys.stderr)
            if args.rewrite_session and not valid:
                refused += 1
        if not rep["signature_held"]:
            refused += 1
        elif args.replace and not args.dry_run:
            # **The destructive path, and it is explicit twice over.** Owner asked
            # for it back: replacing a transcript with its projection is a real
            # operation and she wants it available. What makes it defensible is
            # not the flag, it is the backup -- so `--backup` is REQUIRED, the
            # liveness and copy primitives come from `trans-fairy-write` rather
            # than being written a third time, and a live target is refused
            # outright because a backup predates the race and cannot recover it.
            try:
                live = tfw.liveness(f)
                if live and not args.force:
                    raise ValueError("this transcript looks live (%s). A write "
                                     "underneath a running client loses whatever it "
                                     "wrote in between, and the backup predates that."
                                     % "; ".join(live))
                made = tfw.backup(f, args.backup, force=args.force)
                with open(f, "w", encoding="utf-8") as fh:
                    fh.write(new)
            except (ValueError, FileExistsError, OSError) as e:
                rep["written"] = False
                rep["refused"] = str(e)
                refused += 1
                reports.append(rep)
                continue
            rep["written"] = True
            rep["replaced"] = f
            rep["backup"] = made
            rep["lossy"] = ("the source was replaced by a view of itself. Readable, "
                            "and never resumable -- the backup is the only resumable "
                            "copy now.")
            reports.append(rep)
            continue
        elif not family.in_place(op) and not args.to and not args.dry_run:
            # **A member whose target is not the file emits to the caller.** For
            # `context` the thing being reflowed is the CONTEXT, so the result
            # belongs in the reading agent's context and there is nothing to write
            # anywhere. Treating that as a refusal was the CLI still assuming every
            # member rewrites its input -- the same assumption that destroyed a
            # transcript, surviving in the exit code after the write was closed.
            sys.stdout.write(new if new.endswith("\n") else new + "\n")
            rep["emitted"] = True
            reports.append(rep)
            continue
        elif rep["changed"] and not args.dry_run:
            # **A projects directory is never a write target.** Even for a member
            # that transforms in place, a live client holds these files open and
            # appends to them; a write underneath it loses whatever it wrote in
            # between, and the backup cannot help because it predates the race.
            dest = args.to or f
            if not args.to and kind:
                if args.rewrite_session:
                    out = _rewrite_session(op, op_name, f, kind, src, new, invocation)
                    rep.update(out)
                    if "refused" in out:
                        rep["written"] = False
                        refused += 1
                    reports.append(rep)
                    continue
                rep["written"] = False
                rep["refused"] = _refusal(kind, f)
                refused += 1
                reports.append(rep)
                continue
            try:
                if args.to:
                    safewrite.write_new(dest, new, force=args.force)
                else:
                    # **Recoverability is required only where content can be LOST.**
                    # `lines` proves the content survived with a signature that has
                    # already been checked above, so an untracked in-place reflow
                    # loses formatting at worst -- and refusing it would tax the
                    # ordinary case for a hazard that is not present. `cai edit`
                    # changes content by design and carries the check instead.
                    family.write(op, f, new)
            except (ValueError, FileExistsError) as e:
                rep["written"] = False
                rep["refused"] = str(e)
                refused += 1
                reports.append(rep)
                continue
            rep["written"] = True
            rep["wrote_to"] = dest
        reports.append(rep)
    if any(r.get("emitted") for r in reports):
        return 1 if refused else 0
    print(json.dumps({"member": op_name, "files": len(files), "refused": refused,
                      "changed": sum(1 for r in reports if r.get("changed")),
                      "reports": [r for r in reports
                                  if r.get("changed") or not r.get("signature_held", True)]},
                     ensure_ascii=False, indent=2))
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
