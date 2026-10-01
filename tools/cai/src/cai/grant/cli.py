"""cai grant -- ask whether a time-scoped permission is in force, right now.

Bare invocation prints this reference. The help says WHY, because the reason this
exists is the reason it must be asked rather than read.
"""
import argparse
import json
import os
import sys

from cai.grant import store
from cai.timekeeping import cli as timekeeping

MAN_HELP = """cai grant -- full reference

  cai grant check --repo PATH [--session-id ID] [--at UTC]
  cai grant issue --repo PATH --session NAME|ID (--for 2h | --expires UTC)
  cai grant sessions
  cai grant prune [--all]
  cai grant list
  cai grant why

ISSUING, AND WHY IT WAS MISSING FOR THREE WEEKS
  `check` has existed since 2026-09-01 and there was no way to WRITE a grant, so
  every grant this system evaluated was hand-authored JSON. That is not a
  convenience gap: the party with the file open is the GRANTEE, so the only
  route to being unblocked ran through the blocked party. Found by a peer
  session that was told to commit, was correctly refused, and then found that
  the documented way to obtain permission did not exist.

  --session names the GRANTEE, who is not the caller. Three identifier
  namespaces coexist and only one authorises:

    sessionId        the full uuid in ~/.claude/sessions   THIS is the key
    name             what an operator reads                not unique
    ListAgents ref   a short hex tag                       NOT an id prefix

  A bare six-hex argument is refused by shape, because that is a ref and a ref
  is not a prefix. One observed pair: ref `f6b1b6` against sessionId `0b1e9641`
  -- unrelated strings. Pasting the ref would key a grant to nothing, or to an
  unrelated session.

  It refuses on ambiguity and on no match rather than inventing either, and it
  echoes the resolved name AND full id before writing.

  --dry-run prints the exact object it would append and writes nothing, so a
  grantee can hand its granter something to run.

THE SELF-GRANT GUARD, AND ITS LIMIT
  Issuing to yourself is refused; --self-grant plus a mandatory --note overrides
  it, and the note is written into the record.

  **IF YOU ARE AN AGENT AND YOUR OPERATOR JUST TOLD YOU TO TAKE A GRANT, THIS
  GUARD WILL REFUSE YOU, AND --self-grant IS THE CORRECT ANSWER.** It is not a
  bypass and it is not a rule being broken. The common, legitimate shape is: a
  person decides, and the agent types. The guard cannot see the decision -- it
  only sees that grantee and caller are the same session -- so the note is what
  records the difference between "she asked" and "I decided". Write the note as
  what your operator actually said:

    cai grant issue --repo PATH --session "<your own session name>" --for 1h \
      --action commit --action push --granted-by "<who decided>" \
      --self-grant --note "<operator>, <date>: <what they asked, in their words>"

  A refusal here means the note is missing, not that the grant is forbidden.

  Being GRANTED one by somebody else needs no flag: whoever runs `issue` names
  you with --session and the guard never fires, because you are not the caller.

  **It is a speed bump, not attestation.** Anything that can write the file can
  write it without this CLI. It is worth having because it makes the honest path
  the easy one -- not because it closes anything. Only a server-side signer
  does, which is what `docs/grant-shape.md` already rules.

WHERE IT LANDS
  RUNTIME STATE, WIPED ON RESTART, one store per repository:

    $XDG_RUNTIME_DIR/cai-tools/grants/<project>.json      preferred
    $TMPDIR/cai-tools-<uid>/grants/<project>.json         fallback

  `<project>` is the repository path in `.claude/projects` form, so
  /home/you/dev/Thing becomes -home-you-dev-Thing and the two directory
  listings read the same way.

  **No repository path is a default candidate and `--store repo` is refused**
  -- owner's ruling, 2026-09-23. A grant instance is a fact about right now;
  a store that outlives the machine's uptime is the wrong lifetime for it, and
  an untracked `docs/grants.json` was still being SERVED nineteen days after its
  last grant lapsed.

  XDG_RUNTIME_DIR is preferred over the temp dir for OWNERSHIP, not taste. Both
  are tmpfs, but /tmp is drwxrwxrwt -- a shared namespace any local user can
  create a directory in, and for a PERMISSION store that is a forging surface.
  XDG_RUNTIME_DIR is mode 700 and uid-scoped. The temp fallback is uid-scoped
  and its ownership and mode are CHECKED: an unsafe directory reads as no grants
  and refuses writes. Stores are written 0600, created that way rather than
  chmod'd afterwards.

  Partitioned by REPOSITORY, not by session, because `check --repo PATH` always
  knows the path and does not always know who is asking.

  **On Windows the wiped-on-restart property does not hold** -- the per-user temp
  directory survives a reboot -- so the TTL stays as a backstop there.

  `CAI_GRANTS` still names a single file and `CAI_GRANT_ROOT` overrides the
  directory; both are deliberate choices by a caller.

  Writing PRUNES grants that have already lapsed, and `cai grant prune` does it
  across every store. The TTL is enforced on the FILE's mtime, so rewriting one
  refreshes every stale grant inside -- one issued grant revived ten from
  nineteen days earlier on this verb's first real use.

  BOTH stores are read and merged on check. The older shape read the repository
  file only when the ephemeral store was empty, so one issued grant would have
  silently masked every hand-written one -- harmless while nothing could write,
  and reachable the moment this verb existed.

  `scope` is NOT written. It appears in the live file and looks load-bearing;
  nothing reads it. `--action` is the real scoping, and `effect` is written
  explicitly rather than left implicit.

WHY THIS IS A TOOL AND NOT A HEADING
  A grant expiry is a DYNAMIC fact. CLAUDE.md is read once at session start, so
  an expiry written there reads as active for the whole session no matter what
  the clock says. That is not a discipline problem: the medium cannot express
  the rule. On 2026-09-01 that heading described a window that had lapsed
  twenty-nine hours earlier while work proceeded against it.

  So SOPIA defines the grant and cai evaluates it. The document keeps the
  pointer, because a pointer is static. This computes the answer, because the
  answer is not.

THE KEY IS THE PAIR
  A grant names a session by NAME and by its actual sessionId, and the id is
  what authorises. The name is what an operator reads and what carries lineage;
  the two drift apart on purpose, since a slice inside a name records where a
  conversation came from while the sessionId records which file this is.

  That closes the re-root gap without a rule about re-roots. A composed session
  inherits its predecessor's context -- including the belief that a grant is
  live, which is the dangerous half -- but is minted with a new sessionId and
  does not match. The inheritance problem disappears into the key.

EFFECTS, AND ACTION SCOPE
  A grant may GRANT or DENY. A deny is an explicit prohibition, which is not the
  same as the absence of a permission -- absence is silence, a deny is a
  statement, and reporting them identically would lose that.

  --action NAME scopes a check to one action. A grant naming an action applies
  only to it; a grant naming none is repository-wide.

OBTAINING A GRANT WHEN YOU HAVE BEEN REFUSED
  `check` says no and you believe you should have one: that is a question for
  your operator, not something to route around. Take them the exact command,
  which `--dry-run` will print for you without writing anything:

    cai grant issue --repo PATH --session NAME --for 1h --action commit --dry-run

  Do NOT ask a peer session to issue it for you. A grant obtained sideways is
  the permission decision being made by something other than the person who owns
  it.

EXPIRY ON USE
  expiresAfterChecks: N spends the grant after N checks rather than on a clock.
  CHECKING IT IS WHAT SPENDS IT, so a consuming grant answers differently the
  second time, by design, and the verdict says so rather than leaving that
  surprising. Consumption is enforcement state and lives on the cai side; the
  grant file stays a pure statement of intent, not rewritten by the act of
  asking.

IT FAILS CLOSED
  Missing source, unreadable source, unparseable source, unknown session: all
  return DENIED. There is no branch where uncertainty becomes permission.

EXIT   0 granted   1 denied   2 usage
"""


def _fail(msg):
    print("REFUSED: %s" % msg, file=sys.stderr)
    return 2


def issue(args, doc):
    """Write one grant. **Resolve, guard, show, then write** -- in that order."""
    target, err = store.resolve_target(args.session)
    if err:
        return _fail(err)

    # **The self-grant guard.** This is the exact wall the peer hit: the only
    # party positioned to write the entry was the one the design says must not.
    mine = store.current_session()
    my_id = (mine or {}).get("sessionId")
    if my_id and my_id == target.get("sessionId") and not args.self_grant:
        return _fail(
            "this would grant to the calling session (%s). The grantee and the granter "
            "are the same party, which is the shape a grant is supposed to prevent. Use "
            "--self-grant with a --note if that is genuinely what you mean -- and note "
            "that the guard is a speed bump, not attestation: anything that can write "
            "the store can write it without this tool." % (target.get("sessionId")[:8]))
    if not my_id:
        # **A guard that cannot identify the caller cannot guard.** Say so in the
        # record rather than letting the silence read as a clean pass -- this is
        # the same unresolved-identity gap that makes `check` deny from the wrong
        # cwd, and it fails OPEN here rather than closed.
        unguarded = ("the calling session could not be identified, so the self-grant guard "
                     "did not run -- this grant was not checked against issuing to yourself")
    else:
        unguarded = None
    if args.self_grant and not args.note:
        return _fail("--self-grant requires --note saying why; it is written into the record")

    if not (args.duration or args.expires or args.after_checks):
        return _fail("a grant needs an end: --for 2h, --expires UTC, or --after-checks N")
    if args.duration and args.expires:
        return _fail("--for and --expires both name the end; pass one")

    started = store.now()
    expires = None
    if args.duration:
        try:
            expires = started + timekeeping.parse_duration(args.duration)
        except ValueError as e:
            return _fail(str(e))
    elif args.expires:
        try:
            expires = store._parse(args.expires)
        except store.BadTimestamp as e:
            return _fail(str(e))
        if expires <= started:
            return _fail("--expires %s is not in the future (now is %s)"
                         % (args.expires, timekeeping.stamp(started)))

    base = {"sessionName": target.get("name"), "sessionId": target["sessionId"],
            "effect": args.effect, "granted": timekeeping.stamp(started)}
    if expires:
        base["expires"] = timekeeping.stamp(expires)
    if args.after_checks:
        base["expiresAfterChecks"] = args.after_checks
    if args.path:
        base["paths"] = list(args.path)
    if args.granted_by:
        base["grantedBy"] = args.granted_by
    if mine:
        base["issuedBySession"] = "%s %s" % (mine.get("name") or "?",
                                             (my_id or "?")[:8])
    if args.note:
        base["note"] = args.note

    grants = []
    for repo in args.repo:
        r = os.path.abspath(os.path.expanduser(repo))
        if not os.path.isdir(r):
            return _fail("%s is not a directory. A grant keyed to a path that does not "
                         "exist can never match." % r)
        for action in (args.action or [None]):
            g = dict(base, repo=r)
            if action:
                g["action"] = action
            grants.append(g)

    # **Prove the issuer writes what the evaluator reads**, here, on these
    # values -- not once in a test. Having written it correctly before is not
    # having checked it now.
    for g in grants:
        for field in ("granted", "expires"):
            if g.get(field):
                store._parse(g[field])

    out = {"grantee": {"name": target.get("name"), "sessionId": target["sessionId"]},
           "grants": grants}
    if unguarded:
        out["warning"] = unguarded
    if target.get("note"):
        out["grantee"]["note"] = target["note"]

    try:
        paths = {r["repo"]: store.issue_store(args.store, r["repo"]) for r in grants}
    except ValueError as e:
        return _fail(str(e))
    if args.dry_run:
        out["would_write_to"] = sorted(set(paths.values()))
        out["dry_run"] = True
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0

    try:
        pruned = 0
        for g in grants:
            path, _kept, _lapsed = store.append(g, paths[g["repo"]])
            pruned += _lapsed
    except (ValueError, OSError) as e:
        return _fail(str(e))
    out["written_to"] = sorted(set(paths.values()))
    if pruned:
        out["pruned_lapsed"] = pruned
    out["verify"] = ("cai grant check --repo %s --session-id %s"
                     % (grants[0]["repo"], target["sessionId"]))
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help", "why"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai grant", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    ck = sub.add_parser("check")
    ck.add_argument("--repo", required=True)
    ck.add_argument("--session-id")
    ck.add_argument("--session-name")
    ck.add_argument("--at")
    ck.add_argument("--cwd")
    ck.add_argument("--action")
    sub.add_parser("list")
    sub.add_parser("sessions")
    pr = sub.add_parser("prune")
    pr.add_argument("--store", help="path to prune; every store by default")
    pr.add_argument("--all", dest="all_", action="store_true",
                    help="drop EVERY grant, in force or not -- the crude revoke")
    iss = sub.add_parser("issue")
    iss.add_argument("--repo", action="append", required=True,
                     help="repository the grant applies to; repeat for several")
    iss.add_argument("--session", required=True, help="the GRANTEE: session name or sessionId")
    iss.add_argument("--for", dest="duration", help="window length, e.g. 2h, 90m, 3d")
    iss.add_argument("--expires", help="absolute UTC expiry instead of --for")
    iss.add_argument("--action", action="append", help="scope to one action; repeat for several")
    iss.add_argument("--path", action="append",
                     help="scope to a repo-relative path; repeat. A directory covers what is under it")
    iss.add_argument("--effect", choices=("allow", "deny"), default="allow")
    iss.add_argument("--after-checks", type=int, help="spend the grant after N checks")
    iss.add_argument("--granted-by", help="the person who decided this")
    iss.add_argument("--note", help="why -- kept in the record")
    iss.add_argument("--store", help="tmp (default) or a path; `repo` is refused")
    iss.add_argument("--dry-run", action="store_true")
    iss.add_argument("--self-grant", action="store_true",
                     help="issue to the calling session; requires --note")
    args = ap.parse_args(argv)
    doc = store.load(repo=(os.path.abspath(args.repo)
                           if getattr(args, "cmd", None) == "check"
                           and os.path.exists(args.repo) else None))

    if args.cmd == "list":
        out = []
        for g in doc.get("grants", []):
            # **Ask about THIS grant, which means asking with its own action.**
            # Without it every listed row was evaluated as an UNSCOPED question,
            # and an unscoped question deliberately ignores action-scoped grants
            # -- so each action-scoped row reported the status of whichever
            # repository-wide grant answered instead. Two freshly issued,
            # in-force grants listed as belonging to another session. The same
            # shape as the bug where the numbers came from one grant and the
            # reason from another, and `list` is where it is least visible,
            # because a listing reads as a statement about each row.
            # Ask about THIS grant, which means its action AND its paths -- and
            # WITHOUT consuming it, because listing is not using. The action half
            # was fixed an hour ago and the paths half arrived with `--path` and
            # was missed the same way: every path-scoped grant reported "no grant
            # recorded", because a path-scoped grant deliberately does not answer
            # a question that names no paths.
            v = store.check(doc, g.get("repo"), g.get("sessionId"), g.get("sessionName"),
                            action=g.get("action"), paths=g.get("paths"), consume=False)
            out.append(dict(g, in_force=v["granted"], status=v["reason"]))
        print(json.dumps({"source": doc.get("_path"), "grants": out},
                         ensure_ascii=False, indent=2))
        return 0

    if args.cmd == "sessions":
        rows = [{"name": d.get("name"), "sessionId": d.get("sessionId"), "cwd": d.get("cwd")}
                for d in store._sessions()]
        print(json.dumps(sorted(rows, key=lambda r: (r["name"] or "")),
                         ensure_ascii=False, indent=2))
        return 0

    if args.cmd == "prune":
        try:
            path, kept, dropped = store.prune(args.store, all_=args.all_)
        except (ValueError, OSError) as e:
            return _fail(str(e))
        print(json.dumps({"store": path, "kept": kept, "dropped_lapsed": dropped},
                         ensure_ascii=False, indent=2))
        return 0

    if args.cmd == "issue":
        return issue(args, doc)

    if args.cmd == "check":
        sid, sname = args.session_id, args.session_name
        if sid is None:
            s = store.current_session(cwd=args.cwd)
            if s:
                sid = s.get("sessionId") or s.get("id")
                sname = sname or s.get("name")
        v = store.check(doc, os.path.abspath(args.repo) if os.path.exists(args.repo) else args.repo,
                        session_id=sid, session_name=sname,
                        at=store._parse(args.at) if args.at else None,
                        action=args.action)
        print(json.dumps(v, ensure_ascii=False, indent=2))
        return 0 if v["granted"] else 1
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
