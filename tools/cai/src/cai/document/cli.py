"""cai document -- the shapes, where notation holds the atoms and a tool does the work."""
import argparse
import json
import sys

from cai.document import store

MAN_HELP = """cai document -- full reference

  cai document list
  cai document show NAME
  cai document check NAME --json '{"cwd": "...", ...}'

THE SPLIT, AND WHY IT IS THREE THINGS
  The owner's framing: the entire shape of a name is the DOCUMENT, the symbols
  are the NOTATIONS, and `cai name` is the IMPLEMENTATION of both.

  That generalises. A tool is an implementation over documents and notations --
  it arranges atoms whose meanings it does not own, into shapes it does not own
  either.

    cai notation   what a symbol MEANS
    cai document   how parts are ARRANGED
    a tool         what to DO with an arrangement

  Keeping them apart is what stops any one becoming the place everything
  accumulates -- which is the failure that produced this whole suite.

WHERE SHAPES LIVE
  SOPIA/docs/shapes.json, fetched. SOPIA defines, cai enforces, with an embedded
  fallback that answers only when the source cannot be reached.

CHECKING
  `check` reports whether a value satisfies a shape's stated requirements. It
  never repairs: a shape describes what should be true, and a value that is not
  is a finding for a person. Rewriting it here would be the tool deciding what
  the operator meant.

EXIT   0 ok   1 unknown shape or failing check   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai document", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list")
    sh = sub.add_parser("show")
    sh.add_argument("name")
    ck = sub.add_parser("check")
    ck.add_argument("name")
    ck.add_argument("--json", dest="value", default="{}")
    args = ap.parse_args(argv)
    doc = store.load()

    if args.cmd == "list":
        out = []
        for n in store.names(doc):
            s = store.get(doc, n)
            out.append({"shape": n, "summary": s.get("summary"),
                        "parts": [p.get("key") for p in s.get("parts", [])]})
        print(json.dumps({"source": doc.get("_path"), "shapes": out},
                         ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "show":
        s = store.get(doc, args.name)
        if not s:
            print(json.dumps({"shape": args.name, "known": False}, indent=2))
            return 1
        print(json.dumps(s, ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "check":
        try:
            value = json.loads(args.value)
        except ValueError as e:
            sys.stderr.write("error: --json is not valid JSON (%s)\n" % e)
            return 2
        rep = store.check(doc, args.name, value)
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0 if rep.get("ok") else 1
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
