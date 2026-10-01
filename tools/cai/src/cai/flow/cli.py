"""cai flow -- what order, and what must hold between the steps."""
import argparse
import json
import sys

from cai.flow import store

MAN_HELP = """cai flow -- full reference

  cai flow list
  cai flow show NAME
  cai flow next NAME [--done STEP ...]

IT DESCRIBES AND CHECKS. IT DOES NOT RUN.
  Executing a multi-step destructive sequence automatically is the one thing
  this design has consistently refused. A flow that ran itself would be an agent
  answering a destructive prompt in instalments -- each step small enough to seem
  harmless, the sequence amounting to exactly what the gates exist to prevent.

  It also cannot verify that a step was performed. A flow has no witness to the
  work, and claiming otherwise would be the tool asserting something it cannot
  see, which is the failure its own steps exist to prevent.

STEPS POINT AT DOCUMENTS
  The flow carries what must hold -- compact and checkable. The document carries
  why, at length. Same rule-and-reason split used everywhere here.

  `cai flow pointers` reports whether they resolve, and exits non-zero if any do
  not. A pointer to something that is not there is worse than no pointer: it
  reads as though the reasoning exists and was written down, and whoever follows
  it cannot tell whether the target moved, was renamed, or was never written.

WHAT A STEP CARRIES
  what to do, which tool does it, what must hold before the next one, and WHY.

  Every `why` is a failure that happened. The re-root flow records verifying
  before the write rather than after, and marking both edges of a window rather
  than only where it starts, because both were done the wrong way round first.

EXIT   0 ok   1 unknown flow   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai flow", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list")
    pt = sub.add_parser("pointers")
    pt.add_argument("name", nargs="?")
    sh = sub.add_parser("show")
    sh.add_argument("name")
    nx = sub.add_parser("next")
    nx.add_argument("name")
    nx.add_argument("--done", action="append", default=[])
    args = ap.parse_args(argv)
    doc = store.load()

    if args.cmd == "list":
        out = [{"flow": n, "summary": store.get(doc, n).get("summary"),
                "steps": len(store.get(doc, n).get("steps", [])),
                "tools": store.tools(doc, n)} for n in store.names(doc)]
        print(json.dumps({"source": doc.get("_path"), "flows": out},
                         ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "pointers":
        rep = store.pointers(doc, getattr(args, "name", None))
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 1 if rep["broken"] else 0

    if args.cmd == "show":
        f = store.get(doc, args.name)
        if not f:
            print(json.dumps({"flow": args.name, "known": False}, indent=2))
            return 1
        print(json.dumps(f, ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "next":
        rep = store.check(doc, args.name, done=args.done)
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0 if rep.get("known") else 1
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
