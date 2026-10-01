"""cai edit -- an anchored replacement that refuses to take more than it names."""
import argparse
import json
import sys

from cai.edit import guard

MAN_HELP = """cai edit -- full reference

  cai edit PATH --old-file OLD --new-file NEW [--expect-delta N] [-n]
  cai edit PATH --old STR --new STR [...]

WHY IT EXISTS
  Four functions were destroyed in ten minutes on 2026-09-01 by index-based
  slice replacement -- each removing more than intended, each noticed only when
  a test failed afterwards, twice after the pattern had already been noticed.

  What finally held was not more care. It was asserting the structure was
  unchanged and refusing to write when it was not, which is a check that belongs
  in a tool rather than in whoever is editing.

WHAT IT REFUSES
  An anchor appearing ZERO times -- the edit would do nothing, which is what a
  silently failed replacement looks like from outside, and what leaves a commit
  describing work the diff does not contain.

  An anchor appearing MORE THAN ONCE -- it would hit something it was not aimed
  at.

  A change in the number of top-level definitions (or markdown headings) that
  the caller did not declare with --expect-delta. Removing a function nobody
  mentioned is the defect this exists for.

  Every refusal leaves the file untouched. An edit that half-applied is worse
  than one that did not run, because it is found later and somewhere else.

EXIT   0 applied   1 refused   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai edit", add_help=False)
    ap.add_argument("path")
    ap.add_argument("--old")
    ap.add_argument("--new")
    ap.add_argument("--old-file")
    ap.add_argument("--new-file")
    ap.add_argument("--expect-delta", type=int, default=0)
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)

    def read(inline, path):
        if path:
            return sys.stdin.read() if path == "-" else open(path, encoding="utf-8").read()
        return inline

    old = read(args.old, args.old_file)
    new = read(args.new, args.new_file)
    if old is None or new is None:
        sys.stderr.write("error: give --old/--new or --old-file/--new-file\n")
        return 2
    rep = guard.apply(args.path, old, new, expect_delta=args.expect_delta,
                      dry_run=args.dry_run)
    print(json.dumps(rep, ensure_ascii=False, indent=2))
    return 0 if rep["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
