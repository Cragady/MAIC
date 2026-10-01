"""cai notation -- the living dictionary of markers, and the ledger of what changed.

Bare invocation prints this help, and the help says WHY rather than only what the
flags are: a tool whose explanation lives elsewhere is one whose explanation is
not read at the moment the question arrives.
"""
import argparse
import glob
import json
import os
import sys

from cai.notation import audit as auditmod
from cai.notation import flush as flushmod
from cai.notation import readers_map as rmap
from cai.notation import store

MAN_HELP = """cai notation -- full reference

  cai notation lookup <symbol> [--at UTC]
  cai notation list [--at UTC] [--all]
  cai notation audit <path>... [--json]
  cai notation stale

WHY IT EXISTS
  Eight markers are in live use across these repositories and their definitions
  were scattered across guides, with no record of what any of them used to mean.
  `---- Inner Source ----` was introduced, shipped in two commits, and renamed
  the same day -- after which the old name appeared nowhere, and a reader of
  those commits saw a marker they could not resolve. Git history holds the
  rename, but git history is not a dictionary and nobody consults it to read a
  symbol.

HOW MEANING MOVES
  Every definition carries the UTC instant of the decision behind it. A retired
  one also carries a fallout period, after which it stops being served. So the
  question is continuous -- what was live at this instant -- with no versions to
  declare and no era boundaries to place a document inside. A commit's own
  timestamp is the query key.

  Expired entries are filtered on READ, never deleted by the act of asking: a
  lookup that mutates means two reads disagree about the store. Physical removal
  is a separate step.

  Elapsed time is evidence a symbol is probably dead, never proof nothing will
  read it again. That is a stated trade.

STATUS
  in force   the marker is current and may be used
  candidate  shape agreed, not adopted -- do not rely on it yet
  declined   considered and deliberately REJECTED. It still resolves, so someone
             meeting it in old material learns it was declined rather than
             finding nothing, but it must not read as available. Recording a
             refusal is what stops it being re-proposed.
  retired    was in force, now past or inside its fallout period

ONE DEFINITION, SEVERAL SYMBOLS
  `⚑` and `(╯°Д°)╯︵ ┻━┻` are the same marker written two ways, so lookup is
  many-to-one and a rename is an added symbol rather than a new entry.

  Not every definition names a text marker. The confidence classes are the
  digits 1-5, meaningful only inside a map's grade field; entries like that
  carry `scannable: false` and are left out of a scan.

AUDITS
  Optimistically read, pessimistically audit. Nothing runs on a schedule -- a
  symbol that will not resolve is itself the error signal, and one such symbol
  is evidence more are likely, since a definition rarely drifts alone.

  A fence is a delimiter ALONE ON A LINE. The same characters inside a sentence
  are a mention of the convention, not a use of it, and a substring search
  cannot tell them apart.

  The audit finds and counts. It does not derive a definition from context or
  judge misuse: both need the material read rather than matched, and an entry
  produced without reading would be a guess wearing a count.

THE MAP REPORTS DIFFERENCE. IT DOES NOT RECOMMEND.
  At dissolution the operator drops it, or adopts part of it as definition
  additions, changes and deletions. Nothing here promotes anything into the
  dictionary. A grade is `<count> <class>` pairs rather than a score, because a
  score is a summary of guesses and reads like advice.

FREEZING -- two gates, and neither is sufficient alone
  A frozen file writes its instant into itself; git records the commit
  independently. Two records of one fact from different systems, neither able to
  forge the other.

  GATE ONE, the stamp. A file edited after freezing shows a commit later than
  the stamp it carries.

  GATE TWO, the history, and it is the stronger one. A frozen file has EXACTLY
  ONE commit touching its path -- the move that froze it. A second commit is an
  edit whatever the timestamps say, and this gate needs no clock at all. That
  matters: the stamp gate found its first real defect in a stamp written as
  local time labelled Z, a clock error inside the clock check.

  GATE THREE, the name. The stamp records the filename it was frozen under.
  Without it the history gate has a bypass: RENAME a tampered file and its new
  path carries exactly one commit, which passes. The stamp then names one file
  and the path names another, and the mismatch is the catch.

  It is also what makes re-freezing legitimate. A frozen file can never take a
  second commit, so an update is a move plus a name change -- the new path holds
  the contract on one commit while the old path keeps its own. Append rather than
  mutate, and both remain valid.

  It must also sit under a `defaults-frozen` directory. The name carries both
  halves -- frozen is the contract, defaults is what the contents are for -- and a
  freeze claim from outside one is refused rather than believed.

  A stamp with no name reports that gate unavailable rather than failing it, the
  same way a file that cannot carry a stamp at all is decided by history alone.

  The internal stamp is the claim; the commit is the validator. A file no commit
  records has a claim nothing can check, which is reported rather than passed.

  `synced` is not `frozen`. A copy that tracks a source is updated on purpose
  and has many commits; it reports staleness and makes no freeze claim.

  `cai notation frozen <path>...` exits non-zero if any freeze was broken. It
  reports and never repairs, and returns the delta either way so a person can
  judge a borderline case instead of being handed a verdict.

THIS IS THE LOCAL SET
  It is stale once a hosted store is live, and answers only when no connection
  can be established -- the case an outside adopter runs in permanently.

EXIT   0 ok   1 unresolved markers found   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai notation", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    lk = sub.add_parser("lookup")
    lk.add_argument("symbol")
    lk.add_argument("--at")
    ls = sub.add_parser("list")
    ls.add_argument("--at")
    ls.add_argument("--all", action="store_true")
    au = sub.add_parser("audit")
    au.add_argument("paths", nargs="+")
    au.add_argument("--json", action="store_true")
    au.add_argument("--map", metavar="REPO",
                    help="write THE reader's map for this repository (one; replaces)")
    mp = sub.add_parser("map")
    mp.add_argument("action", choices=["show", "verify", "discard", "resolve", "alias",
                                       "findings", "dissolve", "stance"])
    mp.add_argument("--repo", default=".")
    mp.add_argument("--symbol")
    mp.add_argument("--to", dest="target")
    mp.add_argument("--set", dest="stance", choices=list(rmap.STANCES),
                    help="archive: restore readability only. live: then update the symbols")
    mp.add_argument("--confirmed", action="store_true",
                    help="the alias was confirmed by a person, not inferred")
    fl = sub.add_parser("flush")
    fl.add_argument("file", help="the transient record to check")
    fl.add_argument("--board", help="default board, for marks that name none")
    fl.add_argument("--root", default=".", help="where board paths are resolved from")
    fl.add_argument("--headings", action="store_true",
                    help="records that list under headings rather than as bullets")
    sub.add_parser("stale")
    fz = sub.add_parser("frozen")
    fz.add_argument("paths", nargs="+")
    fz.add_argument("--tolerance", type=int, default=900)
    args = ap.parse_args(argv)

    doc = store.load()
    at = store._parse(args.at) if getattr(args, "at", None) else None

    if args.cmd == "lookup":
        hits = store.resolve(doc, args.symbol, at)
        if not hits:
            print(json.dumps({"symbol": args.symbol, "resolved": False,
                              "note": "no live definition claims this symbol at that instant"},
                             ensure_ascii=False, indent=2))
            return 1
        print(json.dumps({"symbol": args.symbol, "resolved": True, "definitions": hits},
                         ensure_ascii=False, indent=2))
        return 0

    if args.cmd == "list":
        entries = doc["definitions"] if args.all else store.live(doc, at)
        by_status = {}
        for e in entries:
            by_status.setdefault(store.status_of(e), []).append(e)
        for st in store.STATUSES:
            if st not in by_status:
                continue
            print("\n%s" % st.upper())
            for e in by_status[st]:
                print("  %-26s %s" % ("  ".join(e["symbols"])[:26], e["name"]))
                print("      %s" % e["definition"][:150])
        return 0

    if args.cmd == "audit":
        paths = []
        for p in args.paths:
            paths.extend(sorted(glob.glob(p, recursive=True)) or [p])
        rep = auditmod.audit(paths, doc)
        if args.json:
            print(json.dumps(rep, ensure_ascii=False, indent=2))
        else:
            print("scanned %d file(s)" % rep["scanned"])
            for s, n in sorted(rep["known"].items(), key=lambda x: -x[1]):
                print("  %-24s %d" % (s, n))
            if rep["unresolved"]:
                print("\nUNRESOLVED (%d) -- these need a reading, not a match:" % rep["unresolved"])
                for e in rep["entries"]:
                    print("  %-24s %d occurrence(s)  first at %s:%s"
                          % (e["symbol"], e["occurrences"],
                             e["sites"][0]["file"], e["sites"][0]["line"]))
            else:
                print("\nno unresolved markers")
        if getattr(args, "map", None):
            m = rmap.new(args.map, rep)
            written = rmap.save(args.map, m)
            v = m["stamp_validation"]
            print("\nmap written: %s" % written)
            print("  entries    %d" % len(m["entries"]))
            print("  stamp      %s" % m["taken_at"])
            print("  validate   %s" % (v["how"] if v["validatable"] else "NOT VALIDATABLE -- "
                                       + v["why"]))
        return 1 if rep["unresolved"] else 0

    if args.cmd == "flush":
        try:
            with open(args.file, encoding="utf-8") as fh:
                text = fh.read()
        except OSError as e:
            print("cai notation flush: %s" % e, file=sys.stderr)
            return 2
        rep = flushmod.check(text, root=args.root, default_board=args.board,
                             headings=args.headings)
        rep["file"] = args.file
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0 if rep["safe_to_delete"] else 1

    if args.cmd == "map":
        repo = os.path.abspath(args.repo)
        m = rmap.load(repo)
        if m is None and args.action != "discard":
            print(json.dumps({"repo": repo, "map": None,
                              "note": "no map here. `cai notation audit <paths> --map <repo>` "
                                      "builds one. There is exactly one per repository."},
                             ensure_ascii=False, indent=2))
            return 1
        if args.action == "show":
            print(json.dumps(m, ensure_ascii=False, indent=2))
            return 0
        if args.action == "verify":
            v = rmap.verify(m, doc, at)
            print(json.dumps(v, ensure_ascii=False, indent=2))
            return 0 if v["ok"] else 1
        if args.action == "discard":
            gone = rmap.discard(repo)
            print(json.dumps({"discarded": gone,
                              "note": "dissolution and failure end the same way. There is one "
                                      "map, so a broken one is replaced by auditing again."},
                             ensure_ascii=False, indent=2))
            return 0
        if args.action == "resolve":
            if not args.symbol:
                print("--symbol is required", file=sys.stderr)
                return 2
            print(json.dumps(rmap.resolve(m, doc, args.symbol, at),
                             ensure_ascii=False, indent=2))
            return 0
        if args.action == "findings":
            print(json.dumps(rmap.findings(m), ensure_ascii=False, indent=2))
            return 0
        if args.action == "dissolve":
            print(json.dumps(rmap.dissolution(m), ensure_ascii=False, indent=2))
            return 0
        if args.action == "stance":
            if args.stance:
                m["stance"] = args.stance
                rmap.save(repo, m)
            print(json.dumps({"stance": m.get("stance"),
                              "means": rmap.dissolution(m)["action"]},
                             ensure_ascii=False, indent=2))
            return 0
        if args.action == "alias":
            if not (args.symbol and args.target):
                print("--symbol and --to are required", file=sys.stderr)
                return 2
            try:
                e = rmap.alias(m, args.symbol, args.target, inferred=not args.confirmed)
            except KeyError as err:
                print(str(err), file=sys.stderr)
                return 2
            rmap.save(repo, m)
            print(json.dumps(e, ensure_ascii=False, indent=2))
            return 0

    if args.cmd == "frozen":
        reports, broken = [], 0
        for p in args.paths:
            r = store.freeze_status(p, tolerance_seconds=args.tolerance)
            reports.append(r)
            if r.get("frozen") and r.get("validated") and not r.get("kept"):
                broken += 1
        print(json.dumps(reports, ensure_ascii=False, indent=2))
        return 1 if broken else 0

    if args.cmd == "stale":
        print(json.dumps(store.is_stale(doc), ensure_ascii=False, indent=2))
        return 0
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
