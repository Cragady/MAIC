"""cai commit -- commit only when the preconditions hold.

Every failure this exists for is one shape: the commit ran without verifying
what it depended on. Chaining with `&&` fixes that for whoever remembers; this
fixes it for whoever does not.
"""
import argparse
import json
import os
import sys

from cai.commit import gate

MAN_HELP = """cai commit -- full reference

  cai commit -m MESSAGE [--repo PATH] [--verify CMD] [--grant] [-n]
  cai commit -F FILE    [...]

WHAT IT REFUSES
  An EMPTY staged diff. A commit with nothing staged is what an edit that
  silently failed looks like from the outside, and it produces a message
  describing work the repository does not contain.

  A FAILING verification. --verify CMD runs first and refuses on non-zero. Three
  commits in one session carried failing tests because the test run and the
  commit were separate statements in one shell line, and the commit did not care
  how the tests went.

  A DENIED grant, with --grant. Asks `cai grant` for this repository and this
  session, and fails closed if it cannot evaluate one.

WHAT IT WARNS ABOUT
  A subject naming something absent from the change. Commit subjects here are
  `<scope>: <what>`, and a scope that appears nowhere in the staged paths is
  usually a message written for an edit that did not land. It WARNS rather than
  refuses, because a scope may legitimately name a component instead of a path,
  and a false positive must not block a correct commit.

PUSHING IS A SEPARATE PERMISSION
  --push pushes after committing and asks for the `push` action in its own
  right. A commit is local and reversible; a push is outward-facing and is where
  work becomes someone else's problem. A grant for one is not a grant for the
  other.

WHY IT IS A TOOL
  None of these failures were prevented by care, and all were found afterwards.
  That is the signature of a check belonging at the point of effect rather than
  in a habit.

EXIT   0 committed   1 refused   2 usage
"""


def _resolve_session(stated):
    """Turn a stated id -- possibly a PREFIX -- into the full one a grant names.

    **A prefix is not an id, and passing one through as if it were produced a
    denial that read as nonsense**: `names session b5d42ad9; this is b5d42ad9`,
    because the verdict truncates both for display and the grant held the full
    UUID. The env var is a convenience for a human typing eight characters, so
    resolving it is this layer's job rather than the comparison's.
    """
    stated = stated or os.environ.get("CAI_SESSION_ID")
    if not stated:
        return None
    # **A full id IS an id; only a prefix needs resolving.** Resolution consults
    # the client's session records, which are ITS bookkeeping and not cai's --
    # this session's own record vanished from ~/.claude/sessions while it was
    # still running, and requiring a lookup made a correctly-stated full id
    # unusable. The grant is cai's to evaluate; the record is not needed to say
    # what id you are.
    #
    # No new exposure: `CAI_SESSION_ID` could already name any session, and a
    # local grant file is written by the party it authorises -- which the store
    # documents as needing a server-side signer to fix.
    # The full-id rule moved into `store.current_session`, so both callers get it.
    # Kept here only as a comment: duplicating it was what let `cai commit` and
    # `cai grant check` disagree about the same session.
    try:
        from cai.grant import store as gstore
        s = gstore.current_session(session_id=stated)
        if s and s.get("sessionId"):
            return s["sessionId"]
    except Exception:                                            # noqa: BLE001
        pass
    # **Unresolvable is not the same as mismatched, and saying so matters.**
    # Returning the prefix here let it be COMPARED as an id, producing "names
    # session <full> and this is <prefix> -- the same prefix, different values",
    # which reads as a wrong session rather than as a missing record. Session
    # records are ephemeral: this session's own disappeared from
    # ~/.claude/sessions while it was still running, and the prefix that had
    # resolved an hour earlier stopped resolving.
    #
    # Returning None makes the verdict say "unknown session" and fail closed,
    # which is the honest description of what happened.
    return None


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai commit", add_help=False)
    ap.add_argument("-m", "--message")
    ap.add_argument("-F", "--file")
    ap.add_argument("--repo", default=".")
    ap.add_argument("--verify")
    # **The grant is checked by default from 2026-09-03.** `--grant` is kept as a
    # no-op so existing callers and hooks do not break; `--no-grant` is the bypass
    # and it is the OPERATOR'S. An agent that meets a refusal reports it and
    # stops -- it does not reach for this and does not propose reaching for it.
    ap.add_argument("--grant", action="store_true",
                    help="no-op; the grant is checked by default now")
    ap.add_argument("--no-grant", dest="no_grant", action="store_true",
                    help="OPERATOR ONLY: commit without a grant check. An agent that "
                         "meets a refusal reports it and stops.")
    ap.add_argument("--session-id")
    ap.add_argument("--push-only", dest="push_only", action="store_true",
                    help="push already-committed work through the gate, committing nothing")
    ap.add_argument("--push", action="store_true",
                    help="push after committing; asks for the `push` action separately")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)

    # **A refused push must leave a GATED way to retry.** `--push` only ran after
    # a successful commit, so a push refused on its own left a local commit and
    # no route but raw `git push` -- which is the gate's own bypass, reached for
    # by the gated party. Found when a one-time commit-and-push pair had its push
    # half refused by a key collision: the work was committed, correct, and
    # unsendable without going around the thing that refused it.
    if args.push_only:
        r = gate.push(os.path.abspath(args.repo),
                      require_grant=(False if args.no_grant else None),
                      session_id=_resolve_session(args.session_id), dry_run=args.dry_run)
        print(json.dumps(r, ensure_ascii=False, indent=2))
        return 0 if r["ok"] else 1

    msg = args.message
    if args.file:
        msg = sys.stdin.read() if args.file == "-" else open(args.file, encoding="utf-8").read()
    if not msg:
        sys.stderr.write("error: -m or -F is required\n")
        return 2

    # **None means ASK THE REPOSITORY**, which is the whole point of the
    # declaration. Passing `not args.no_grant` computed a boolean every time, so
    # the gate's resolution never ran and an `off` repository was still gated --
    # a fix that could not reach the path it was written for.
    rep = gate.commit(os.path.abspath(args.repo), msg, verify=args.verify,
                      require_grant=(False if args.no_grant else None), session_id=_resolve_session(args.session_id),
                      dry_run=args.dry_run)

    if args.push and rep["ok"]:
        rep["push"] = gate.push(os.path.abspath(args.repo), require_grant=(False if args.no_grant else None),
                                session_id=_resolve_session(args.session_id), dry_run=args.dry_run)
    print(json.dumps(rep, ensure_ascii=False, indent=2))
    if args.push and rep.get("push") and not rep["push"]["ok"]:
        return 1
    return 0 if rep["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
