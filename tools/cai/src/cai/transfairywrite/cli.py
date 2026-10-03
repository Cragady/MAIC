"""cai trans-fairy-write -- overwrite an installed transcript, safely and deliberately.

The sibling that admits to writing. `trans-fairy` creates and never overwrites;
this is where a genuine in-place write goes, so that contract stays
unconditional rather than becoming a flag.
"""
import argparse
import json
import os
import sys

from cai.transfairywrite import writer

MAN_HELP = """cai trans-fairy-write -- full reference

  cai trans-fairy-write <target.jsonl> --from SOURCE --backup PATH [options]
  cai trans-fairy-write <target.jsonl> --backup PATH --verify
  cai trans-fairy-write list-backups ID
  cai trans-fairy-write restore ID [--backup TS] [--ignore-live] [-n]

WHAT IT IS FOR
  Replacing the contents of a transcript that is already installed -- repairing a
  malformed record, or landing a corrected version. `trans-fairy` cannot do this
  and should not learn to: it creates, and a create-only contract with an
  exception is not a contract.

OPTIONS
  --from SOURCE         the replacement transcript. REQUIRED unless --verify.
  --backup PATH         backup file, or a directory to place one in. REQUIRED.
                        Fail-closed: refuses if the destination exists (--force).
                        A copy, never a move -- the original must survive.
  --verify              compare the target against the backup and report.
  --force               overwrite an existing backup. Never point this at a
                        pristine backup; it destroys the only reference a verify
                        has.
  --ignore-live         write even though the target looks live. See below.
  -n, --dry-run         run every check and report, writing nothing.

WHAT IT REFUSES
  A target that does not exist -- this overwrites, it does not create.
  A target that looks LIVE, by either signal: its sessionId appears in a live
    session file, or it was modified within five minutes. A running client
    appends on every turn, so a rewrite destroys whatever it wrote between the
    read and the write, and the backup cannot recover that -- a backup holds the
    pre-write state, not the write it raced. --ignore-live exists for a stale
    registry and says so loudly. Note that a
    successful write leaves its own target looking fresh -- the tool cannot tell
    "modified because I wrote it" from "modified because a client did" -- so a
    second write inside five minutes needs --ignore-live or a wait. Refusing is
    the safe direction.
  A replacement that fails verification: malformed JSON, assistant content that
    is not a list of blocks, a broken parent chain, an orphaned tool_result, or
    more than one sessionId.
  A replacement whose sessionId disagrees with the target's, which would leave a
    file whose name and contents disagree.

MAID SESSIONS
  A MAID session (~/.local/state/maid/sessions/<home>/<id>.jsonl, detected by its
  records, never by its path) is a valid target and a valid --from. It is checked
  with MAID's shapes: msg content, tool messages paired with their calls, a
  resumed_from pointer only first. It looks LIVE when the maid process that last
  opened it is still running here, or by the five-minute rule above.

  Before ANY write, whatever the target, a second copy is taken under
  ~/.local/state/maid/sessions/.backups/<id>/<UTC>.jsonl (0600; $XDG_STATE_HOME
  when set), beside the --backup you named, and its path goes to stderr; the
  write lands through a temp file and a rename. A MAID session then
  gets a `rewritten` record appended naming that copy and this invocation, so the
  file says it was rewritten. A Claude Code transcript gets the copy and the
  rename and nothing else: it stays byte for byte what --from gave.

  list-backups ID        every copy taken for a session (a MAID id or unique
                         prefix, or any transcript's path)
  restore ID [--backup TS]
                         put the newest copy (or the one stamped TS) back, after
                         copying the current file to the same place first. A MAID
                         session gets a `rewritten` record naming both.

THE AGENT CONTRACT
  An agent never answers a destructive prompt. This tool has no --agent path and
  will not grow one; where an agent needs a write performed, it prints the
  command and a person runs it.

EXIT   0 ok   1 refused or verification failed   2 usage/backup refused
"""


def _backups(argv):
    """`list-backups ID` and `restore ID [--backup TS]`: the copies MAID's rule keeps."""
    ap = argparse.ArgumentParser(prog="cai trans-fairy-write " + argv[0])
    ap.add_argument("id", help="a MAID session id, a unique prefix, or a transcript path")
    if argv[0] == "restore":
        ap.add_argument("--backup", help="the UTC stamp of the copy to restore (default: the newest)")
        ap.add_argument("--ignore-live", action="store_true")
        ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv[1:])
    try:
        if argv[0] == "list-backups":
            print(json.dumps(writer.list_backups(args.id), ensure_ascii=False, indent=2))
            return 0
        if args.ignore_live:
            sys.stderr.write("WARNING: --ignore-live. If the target really is live, whatever the client\n"
                             "  wrote since this command read it will be destroyed.\n")
        rep = writer.restore(args.id, backup_ts=args.backup, ignore_live=args.ignore_live,
                             dry_run=args.dry_run,
                             invocation="trans-fairy-write " + " ".join(argv))
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0
    except (RuntimeError, ValueError, FileNotFoundError) as e:
        sys.stderr.write("error: %s\n" % e)
        return 1


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--man-help" in argv:
        print(MAN_HELP)
        return 0
    if "--agent" in argv:
        sys.stderr.write(
            "error: this tool has no --agent path, deliberately. An agent never answers a\n"
            "destructive prompt -- print the command and let a person run it.\n")
        return 2
    if argv and argv[0] in ("restore", "list-backups") and not os.path.isfile(argv[0]):
        return _backups(argv)
    ap = argparse.ArgumentParser(prog="cai trans-fairy-write",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("target")
    ap.add_argument("--from", dest="source")
    ap.add_argument("--backup", required=True)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--ignore-live", action="store_true")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.verify:
            rep = writer.verify(args.target, args.backup)
            print(json.dumps(rep, ensure_ascii=False, indent=2))
            return 0 if rep["ok"] else 1
        if not args.source:
            sys.stderr.write("error: --from is required unless --verify\n")
            return 2
        if args.ignore_live:
            sys.stderr.write(
                "WARNING: --ignore-live. If the target really is live, whatever the client\n"
                "  wrote since this command read it will be destroyed, and the backup holds\n"
                "  the state before that write rather than the write itself.\n")
        rep = writer.write(args.target, args.source, args.backup,
                           force=args.force, ignore_live=args.ignore_live,
                           dry_run=args.dry_run,
                           invocation="trans-fairy-write " + " ".join(argv))
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0
    except FileExistsError as e:
        sys.stderr.write("error: %s\n" % e)
        return 2
    except (RuntimeError, ValueError, FileNotFoundError) as e:
        sys.stderr.write("error: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
