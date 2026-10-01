"""cai sync -- carry what SOPIA defines, so cai works from a single download."""
import argparse
import json
import sys

from cai.sync import runner, spec

MAN_HELP = """cai sync -- full reference

  cai sync list
  cai sync <job> [-n]
  cai sync all [-n]

WHY IT EXISTS
  cai should work from a single download, with no SOPIA reachable and no
  network. That means carrying a copy of what SOPIA defines -- and a copy that
  is a SNAPSHOT rather than a mirror. A mirror is whatever the source says now;
  a snapshot is what it said at a stated instant, which is the only thing you
  can trust when the source is gone.

FROZEN AND SYNCED, WHICH ARE USUALLY OPPOSITES
  A synced copy tracks a source and is updated. A frozen file is immutable and
  carries exactly one commit. Each sync resolves that by writing a NEW DATED
  FILE rather than editing one, so every snapshot keeps its freeze, the set
  tracks the source, and the earlier ones stay valid.

  Append rather than mutate -- the same shape as re-freezing a file under a new
  name, and the same shape as a cut that appends instead of inserting.

RESOLUTION ORDER
  The priority is:

      hosted  ->  SOPIA  ->  local sync-frozen snapshot

  Two more exist and are not peers of those three. An ENVIRONMENT VARIABLE sits
  above everything as an override -- not a source of truth, but a way to say use
  this instead, and treating it as a tier would make an accident
  indistinguishable from an instruction. The EMBEDDED FALLBACK sits below
  everything as bootstrap, so a fresh install answers before any sync has run;
  once a snapshot exists the snapshot wins, because both are copies and only one
  states when it was taken.

  The hosted store is not built and its slot is already correct: it goes ahead
  of the SOPIA source, everything below stays as written, and no caller changes,
  because callers ask cai rather than reading a file.

THE JOBS ARE DATA
  Adding something to carry is an entry in specs.json, not a code change. That
  is what makes sync reusable for things other than the three it ships with.

WHEN THE SOURCE IS MISSING
  It reports and writes nothing. That is the case snapshots exist FOR, so it is
  named rather than worked around -- a sync that invented content would be
  fabricating the very thing it was meant to preserve.

EXIT   0 ok   1 a job failed   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai sync", add_help=False)
    ap.add_argument("job")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)
    doc = spec.load()

    if args.job == "list":
        out = []
        for name, j in sorted(spec.jobs(doc).items()):
            snaps = runner.snapshots(j["into"], j["prefix"])
            out.append({"job": name, "summary": j["summary"], "from": j["from"],
                        "snapshots": len(snaps), "newest": snaps[-1] if snaps else None})
        print(json.dumps({"jobs": out}, ensure_ascii=False, indent=2))
        return 0

    names = sorted(spec.jobs(doc)) if args.job == "all" else [args.job]
    reports = [runner.run(n, doc, dry_run=args.dry_run) for n in names]
    print(json.dumps(reports, ensure_ascii=False, indent=2))
    return 0 if all(r.get("ok") for r in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
