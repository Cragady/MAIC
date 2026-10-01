"""cai trans-fairy -- move a conversation out of one body into another.

A claude.ai export becomes a Claude Code session transcript that `claude -r`
will resume. See docs/transfairy/ (README is the map, DESIGN the spec,
PIPELINE the flow).
"""
import argparse
import io
import json
import os
import sys

from cai.transfairy import graft as graftmod
from cai.transfairy import stages
from cai.transfairy.paths import Tree

DEFAULT_CVERSION = "2.1.231"
DEFAULT_MODEL = "claude-opus-5"


def _pipeline_md():
    """Locate docs/transfairy/PIPELINE.md so --agent prints from one source.

    Editable install: it sits at <repo>/docs/transfairy/. Fall back to an env
    override, then to None (a pointer is printed instead of a stale copy).
    """
    env = os.environ.get("CAI_TRANSFAIRY_DOCS")
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = []
    if env:
        candidates.append(os.path.join(env, "PIPELINE.md"))
    # src/cai/transfairy/ -> ../../../docs/transfairy/PIPELINE.md
    candidates.append(os.path.normpath(os.path.join(here, "..", "..", "..", "docs", "transfairy", "PIPELINE.md")))
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def _pipeline_text(stage):
    """The PIPELINE.md section for a stage (### NN -- `stage`), or a pointer."""
    md = _pipeline_md()
    if not md:
        return "(PIPELINE.md not found; see docs/transfairy/PIPELINE.md for the '%s' stage.)" % stage
    lines = io.open(md, encoding="utf-8").read().splitlines()
    out, capturing = [], False
    for ln in lines:
        if ln.startswith("### "):
            capturing = ("`%s`" % stage) in ln
            if capturing:
                out.append(ln)
            continue
        if capturing:
            if ln.startswith("## "):
                break
            out.append(ln)
    return "\n".join(out).strip() or "(no PIPELINE.md section found for '%s'.)" % stage


def _cversion(explicit):
    if explicit:
        return explicit
    try:
        import subprocess
        out = subprocess.check_output(["claude", "--version"], stderr=subprocess.DEVNULL).decode()
        tok = out.strip().split()[0]
        return tok if tok[:1].isdigit() else DEFAULT_CVERSION
    except Exception:
        sys.stderr.write("warning: `claude --version` not found; pinning %s. "
                         "This tool may not be reliably usable without claude.\n" % DEFAULT_CVERSION)
        return DEFAULT_CVERSION


def _resolve_target_cwd(args):
    val = args.target_cwd or os.environ.get("TRANSFAIRY_CWD") or os.getcwd()
    return os.path.abspath(val)


def _tree(args):
    return Tree(_resolve_target_cwd(args),
                work_root_override=args.work_root, data_root_override=args.data_root,
                stage_dir_override=args.stage_dir, pool_override=args.pool,
                claude_projects_override=args.claude_projects)


def _emit(obj):
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def _next_command(stage, args, result):
    """The exact command that runs the following stage, flags filled in."""
    tc = _resolve_target_cwd(args)
    base = "cai trans-fairy --agent --target-cwd %s" % tc
    order = stages.STAGES
    try:
        nxt = order[order.index(stage) + 1]
    except (ValueError, IndexError):
        return None
    if nxt == "split":
        return base + " split --export <export.json> [--target-uuid UUID]"
    if nxt == "build":
        ex = result.get("extracted", "<extracted.json>")
        return base + " build --conversation %s" % ex
    if nxt == "install":
        st = result.get("staged", "<staged.jsonl>")
        return base + " install --staged %s" % st
    if nxt == "resume":
        return result.get("resume", "claude -r <sid> --fork-session")
    return None


def _run_stage(name, args, tree):
    if name == "init":
        return stages.init(tree, dry_run=args.dry_run, verbose=args.verbose)
    if name == "compose":
        src = {"kind": "repo-sweep"}
        if getattr(args, "source_repo", None):
            src["repo"] = args.source_repo
        if getattr(args, "source_commit", None):
            src["commit"] = args.source_commit
        return stages.compose(
            tree, args.root, args.suffix, source=src, out=args.out,
            insert_at=getattr(args, "insert_at", None),
            notice_extra=getattr(args, "notice_extra", None),
            notice_only=getattr(args, "notice_only", None),
            root_kind=getattr(args, "root_kind", "synth"),
            mode=("true-silent" if getattr(args, "true_silent", False)
                  else "loud" if getattr(args, "loud", False) else "silent"),
            dry_run=args.dry_run)
    if name == "split":
        if getattr(args, "truncate", None):
            return stages.truncate(tree, args.truncate, at_line=args.at_line,
                                   at_uuid=args.at_uuid, before_text=args.before_text,
                                   out=args.out,
                                   mode=("true-silent" if getattr(args, "true_silent", False)
                                         else "loud" if getattr(args, "loud", False) else "silent"),
                                   keep=getattr(args, "keep", "prefix"),
                                   loud_lineage=getattr(args, "loud_lineage", False),
                                   dry_run=args.dry_run)
        if not args.export:
            _empty_value_error("split", "--export")
        return stages.split(tree, args.export, target_uuid=args.target_uuid, dry_run=args.dry_run)
    if name == "build":
        conv = args.conversation or os.path.join(tree.download, "extracted.json")
        return stages.build(tree, conv, _cversion(args.c_version), args.model or DEFAULT_MODEL, dry_run=args.dry_run)
    if name == "install":
        direction = "before" if getattr(args, "prepend", False) else "after"
        if getattr(args, "graft_onto", None):
            return stages.graft_install(tree, args.graft_onto, staged_path=args.staged,
                                        direction=direction, dry_run=args.dry_run)
        if getattr(args, "inject", False):
            source = {"kind": "repo-sweep" if args.source_repo else "unspecified"}
            if args.source_repo:
                source["repo"] = args.source_repo
            if args.source_commit:
                source["commit"] = args.source_commit
            mode = "silent" if getattr(args, "silent", False) else "loud"
            return stages.inject_install(tree, mode=mode, source=source,
                                         staged_path=args.staged, dry_run=args.dry_run)
        return stages.install(tree, staged_path=args.staged, reflow=getattr(args, "reflow", False), dry_run=args.dry_run)
    raise ValueError("unknown stage: %s" % name)


def _empty_value_error(subcmd, flag):
    sys.stderr.write("error: %s requires %s.\n\n"
                     "  cai trans-fairy %s ...\n" % (subcmd, flag, subcmd))
    raise SystemExit(2)


def _add_global(p):
    p.add_argument("--target-cwd", help="injection target; env TRANSFAIRY_CWD; default $PWD")
    p.add_argument("--work-root", help="working area; default platform temp dir")
    p.add_argument("--data-root", help="durable dir (bak/, pool); default XDG data / LOCALAPPDATA")
    p.add_argument("--stage-dir", help="staged transcript dir; default <work-root>/trans-fairy-<job>/staged")
    p.add_argument("--claude-projects", help="projects dir; default derived from --target-cwd")
    p.add_argument("--pool", help="uuid pool path; default <data-root>/trans-fairy/uuid-pool.txt")
    p.add_argument("--c-version", help="Claude Code version stamped into records; default from `claude --version`")
    p.add_argument("--model", help="model stamped into assistant records; default %s" % DEFAULT_MODEL)
    p.add_argument("--agent", action="store_true", help="one stage per invocation; print the next command; exit 0")
    p.add_argument("--stage", help="run exactly this stage")
    p.add_argument("--stop-after", help="run through this stage, then stop")
    p.add_argument("-y", "--yes", action="store_true", help="never prompt; fail instead (excludes --agent)")
    p.add_argument("-n", "--dry-run", action="store_true", help="do not write")
    p.add_argument("--verbose", action="store_true")



MAN_HELP = """cai trans-fairy -- full reference

STAGES (00-04; resume is human-only)
  init      scaffold the working tree; report every write target; write nothing outside it
  split     export -> extracted conversation  (--export PATH [--target-uuid UUID])
  build     extracted -> staged transcript    (--conversation PATH)
  install   staged -> projects dir, create-only, never overwrite (invariant 8)
            --graft-onto SID   graft onto an existing base (new sessionId, base untouched)
            --inject [--silent] [--prepend] [--source-repo P] [--source-commit SHA]
  state     report project-dir non-code state  (not yet implemented)

GLOBAL FLAGS  (resolution: flag -> env -> derived default -> prompt; non-TTY fails with a hint)
  --target-cwd PATH     injection target; env TRANSFAIRY_CWD; default $PWD. NAMED ONLY, never written.
  --work-root PATH      working area; default the platform temp dir (TMPDIR/TEMP/TMP, /tmp)
  --data-root PATH      durable dir (bak/, uuid-pool.txt); default $XDG_DATA_HOME/cai or ~/.local/share/cai
  --stage-dir PATH      staged transcript dir; default <work-root>/trans-fairy-<job>/staged
  --claude-projects P   projects dir; default derived from --target-cwd by the '/' and '.' -> '-' rule
  --pool PATH           uuid pool; default <data-root>/trans-fairy/uuid-pool.txt
  --c-version STR       Claude Code version stamped into records; default from `claude --version`
  --model STR           model stamped into assistant records; default claude-opus-5
  --agent               one stage per invocation; print the PIPELINE.md text, what was written,
                        what to verify, and the exact next command; exit 0 at each boundary.
                        Mutually exclusive with --yes. An agent never answers a y/N.
  --stage / --stop-after   run exactly / through a named stage
  -y, --yes             never prompt; fail instead
  -n, --dry-run         do not write

POOL      absent -> mint; present and long enough -> read, never touch; short -> append, never shuffle.
          Ids come from a random source (uuid4), never from model reasoning (invariant 9).

NAMING    plain name = living doc; NN- = point-in-time log; NNN- inside a capture = frozen snapshot.

EXIT      0 ok (a stage boundary is success under --agent);  1 stage failed;  2 usage.
"""


def _help_dispatch(argv, parser):
    """Handle the help family before argparse. Returns an exit code, or None.

    Most verbose wins silently: --mahd > --man-help > -h/--help. A help flag
    combined with any non-help token is a hard drop (usage + blurb, non-zero).
    """
    help_flags = {"-h", "--help", "--man-help", "--mahd"}
    present = [a for a in argv if a in help_flags]
    if not present:
        return None
    non_help = [a for a in argv if a not in help_flags]
    if non_help:
        parser.print_help(sys.stderr)
        sys.stderr.write("\nA help flag was combined with other arguments. If that was "
                         "unintended, drop the help flag and re-run.\n")
        return 2
    if "--mahd" in present:
        parser.print_help()
        print("\n" + MAN_HELP)
        return 0
    if "--man-help" in present:
        print(MAN_HELP)
        return 0
    parser.print_help()
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(prog="cai trans-fairy", description=__doc__.splitlines()[0], add_help=False)
    ap.add_argument("-h", "--help", action="store_true")
    ap.add_argument("--man-help", action="store_true")
    ap.add_argument("--mahd", action="store_true")
    _add_global(ap)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("init", help="scaffold the working tree; report write targets")
    sp = sub.add_parser("split", help="export -> extracted conversation; or tail-truncate a transcript")
    sp.add_argument("--export"); sp.add_argument("--target-uuid")
    sp.add_argument("--truncate", help="tail-truncate this .jsonl transcript instead of splitting an export")
    sp.add_argument("--at-line", type=int, help="cut point: 1-indexed record number (keep through it)")
    sp.add_argument("--at-uuid", help="cut point: keep through the record with this uuid")
    sp.add_argument("--before-text", help="cut point: keep everything before the first record containing this text")
    sp.add_argument("--keep", choices=("prefix", "suffix"), default="prefix",
                    help="prefix: drop everything after the cut (default). suffix: drop everything before it")
    sp.add_argument("--out", help="write the truncated transcript here (default: staged/)")
    sp.add_argument("--loud", action="store_true", help="also append a visible notice, so the resumed agent knows it is reading a prefix")
    sp.add_argument("--true-silent", action="store_true", help="append nothing at all, not even lineage -- the cut leaves no trace")
    sp.add_argument("--loud-lineage", action="store_true", help="also announce the lineage in context, so the resumed agent knows where it came from")
    cp = sub.add_parser("compose", help="invisible root + visible cut notice + real suffix")
    cp.add_argument("--root", required=True, help="transcript whose messages become the invisible root")
    cp.add_argument("--suffix", required=True, help="transcript whose messages become the real conversation")
    cp.add_argument("--out", help="write the composed transcript here (default: staged/)")
    cp.add_argument("--notice-extra", help="append an instruction to the visible notice (the only half the model reads)")
    cp.add_argument("--notice-only", help="REPLACE the default notice with this text; the notice still appears, but says only what you give it")
    cp.add_argument("--root-kind", default="synth", choices=list(graftmod.ROOT_KINDS),
                    help="what this root IS; written automatically unless --true-silent")
    cp.add_argument("--insert-at", type=int, help="place the root INSIDE the conversation after N messages, with the notice leading (default: root first)")
    cp.add_argument("--loud", action="store_true", help="mark the root loudly instead of meta-only")
    cp.add_argument("--true-silent", action="store_true", help="mark the root not at all")
    cp.add_argument("--source-repo", help="root provenance: repo path")
    cp.add_argument("--source-commit", help="root provenance: commit sha")
    bp = sub.add_parser("build", help="extracted conversation -> staged transcript")
    bp.add_argument("--conversation")
    ip = sub.add_parser("install", help="staged transcript -> projects dir (never overwrite)")
    ip.add_argument("--staged")
    ip.add_argument("--reflow", action="store_true", help="on an id collision, mint a fresh id and install as a new transcript")
    ip.add_argument("--graft-onto", help="graft the staged transcript onto this base sessionId")
    ip.add_argument("--inject", action="store_true", help="install the staged transcript as an injection")
    ip.add_argument("--silent", action="store_true", help="inject silently (no marker; never to a projects dir)")
    ip.add_argument("--prepend", action="store_true", help="place ahead of the conversation (default: append)")
    ip.add_argument("--source-repo", help="inject/graft provenance: repo path")
    ip.add_argument("--source-commit", help="inject provenance: commit sha (the AT: delta stamp)")
    stp = sub.add_parser("state", help="report project-dir non-code state; --split extracts it")
    stp.add_argument("--split", help="extract into a role dir: previous-agent | working-agent")
    stp.add_argument("--to", help="destination dir (default <work>/trans-fairy-<job>/<role>/)")
    stp.add_argument("--include", action="append", default=[], help="transcripts (repeatable)")
    stp.add_argument("--ledger", action="store_true", help="regenerate the mapping ledger from the transcripts present")
    stp.add_argument("--audit", action="store_true", help="report transcripts the ledger cannot account for")

    hc = _help_dispatch(argv, ap)
    if hc is not None:
        return hc
    args = ap.parse_args(argv)

    if args.agent and args.yes:
        sys.stderr.write("error: --agent and --yes are mutually exclusive.\n")
        return 2
    if not args.cmd:
        ap.print_help()
        return 0

    tree = _tree(args)

    if args.cmd == "state":
        if getattr(args, "audit", False):
            try:
                _emit(stages.audit_ledger(tree))
            except FileNotFoundError as e:
                sys.stderr.write("error: %s\n" % e)
                return 1
            return 0
        if getattr(args, "ledger", False):
            try:
                _emit(stages.rebuild_ledger(tree, dry_run=args.dry_run))
            except FileNotFoundError as e:
                sys.stderr.write("error: %s\n" % e)
                return 1
            return 0
        if getattr(args, "split", None):
            try:
                _emit(stages.state_split(tree, args.split, to=args.to,
                                         include=tuple(args.include), dry_run=args.dry_run))
            except (FileNotFoundError, ValueError) as e:
                sys.stderr.write("error: %s\n" % e)
                return 1
            return 0
        _emit(stages.state(tree, dry_run=args.dry_run))
        return 0

    try:
        result = _run_stage(args.cmd, args, tree)
    except SystemExit:
        raise
    except (FileExistsError, FileNotFoundError, ValueError, AssertionError, RuntimeError) as e:
        if args.agent:
            # failure exits non-zero with a distinct, legible message (DESIGN, agent path)
            sys.stderr.write("STAGE FAILED (%s): %s\n" % (args.cmd, e))
            return 1
        sys.stderr.write("error: %s\n" % e)
        return 1

    if args.agent:
        print("# ---- PIPELINE.md: stage '%s' ----" % args.cmd)
        print(_pipeline_text(args.cmd))
        print("# ---- what was written ----")
    _emit(result)
    if args.agent:
        nxt = _next_command(args.cmd, args, result)
        print("\n# stage '%s' complete (exit 0 is success at a boundary)." % args.cmd)
        if nxt:
            print("# next:\n%s" % nxt)
        else:
            print("# no further stage; the flow is complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
