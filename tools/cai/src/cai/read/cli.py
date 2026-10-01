"""cai read -- project a transcript to what was said."""
from cai import safewrite
import argparse
import json
import os
import sys

from cai.grammar import maic
from cai.read import reader

MAN_HELP = """cai read -- project a transcript to what was said

  cai read TRANSCRIPT.jsonl                     the conversation, to stdout
  cai read TRANSCRIPT.jsonl --before-compaction what the last compaction DROPPED
  cai read TRANSCRIPT.jsonl --since-compaction  what it KEPT (you already hold this)
  cai read TRANSCRIPT.jsonl --select tools      tool traffic, for debugging a run
  cai read TRANSCRIPT.jsonl --out FILE          save it somewhere NEW

THREE THINGS YOU MIGHT MEAN, AND ONLY TWO ARE HERE

  into YOUR context   cai read TRANSCRIPT.jsonl
                      The conversation goes to stdout and the agent reading it
                      now holds it. No file is written. **This is the one people
                      mean** -- it is the poor-woman's re-root, and it is the
                      default because it is the common case.

  into a NEW file     cai read TRANSCRIPT.jsonl --out FILE
                      For handing the projection to someone else, or keeping it.

  OVER the source     not available here, deliberately. See below.

  **The distinction is worth stating because getting it wrong destroyed a
  transcript.** An operator asked for the first, ran a tool that did the third,
  and every check passed on the way. The three read alike in a sentence and are
  nothing alike in effect.

IT CANNOT WRITE OVER ANYTHING
  There is no in-place mode and no way to ask for one. `--out` refuses a path
  that already exists and refuses the input path outright.

  **This tool exists because the projection was once a member of `reflow`.**
  reflow TRANSFORMS, so its CLI writes results back over their input -- and the
  projection inherited that and destroyed a live transcript, replacing 5.27 MB
  of records with 2.06 MB of its own output. Guards were added there afterwards
  and they hold. This is better than a guard: a reader has no write path to
  guard, so the destructive operation is not refused, it is unrepresentable.

WHAT IS LOST, MEASURED
  A projection keeps what was SAID and discards the scaffolding. On the affected
  transcript, measured by the reporting session: 823 of 927 records carried no
  message text at all -- thinking, tool_use, tool_result. **So 89% of the
  artifact does not survive a projection, while every content check passes
  honestly**, because the loss is structural and the checks are about content.

  Readable afterwards. Never resumable.

WHICH HALF OF A COMPACTION
  A compaction replaces everything BEFORE its boundary with a summary, so the
  two flags are opposite halves and the difference decides whether you get
  anything useful:

    --since-compaction   the boundary onward: the summary, and what came after.
                         **A resumed agent already holds this.** It is the right
                         slice for handing a fresh agent the live chunk, and the
                         wrong one for recovering anything.

    --before-compaction  everything before the boundary: **what the summary
                         replaced.** This is the recovery slice.

  **Until 2026-09-04 only the first existed, and this help called it "what the
  last compaction dropped" -- which is backwards.** Anyone following that wording
  to recover lost material was pointed at the half they still had. Reported by a
  peer session that worked around it by slicing a copy by hand.

SELECTIONS
  conversation  user and assistant text (default)
  turns         user turns only
  tools         tool_use and tool_result as well
  all           everything the projection can carry

  Thinking blocks are excluded from every selection. They are the largest single
  component and their signatures do not validate outside the session that minted
  them -- a correctness reason, so it does not relax when you have budget.

EXIT   0 ok   1 nothing to read, or the messages did not survive   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai read", add_help=False)
    ap.add_argument("transcript")
    ap.add_argument("--select", default="conversation",
                    choices=["conversation", "turns", "tools", "all"])
    ap.add_argument("--since-compaction", action="store_true",
                    help="the boundary onward -- the summary and what followed. A resumed "
                         "agent already holds this; it is the RE-ROOT slice.")
    ap.add_argument("--boundaries", action="store_true",
                    help="list the compaction boundaries and the regions between them, "
                         "then stop. Look before you slice.")
    ap.add_argument("--chunk", type=int,
                    help="one region between two boundaries. Negative counts from the "
                         "end: -1 is the newest, -2 is what the last compaction replaced.")
    ap.add_argument("--before-compaction", action="store_true",
                    help="everything before the boundary -- what the summary replaced. "
                         "This is the RECOVERY slice.")
    ap.add_argument("--max-block", type=int)
    ap.add_argument("--out", help="save to a NEW file; refuses to overwrite anything")
    ap.add_argument("--json", action="store_true", help="report only, no text")
    args = ap.parse_args(argv)

    try:
        with open(args.transcript, encoding="utf-8", errors="replace") as fh:
            data = fh.read()
    except OSError as e:
        print("cai read: %s" % e, file=sys.stderr)
        return 2
    data = maic.with_parents(args.transcript, data)

    _sel = [n for n, v in (("--since-compaction", args.since_compaction),
                           ("--before-compaction", args.before_compaction),
                           ("--chunk", args.chunk is not None)) if v]
    if len(_sel) > 1:
        print("cai read: %s select different spans and cannot be combined." % " and ".join(_sel),
              file=sys.stderr)
        return 2
    if args.since_compaction and args.before_compaction:
        print("cai read: --since-compaction and --before-compaction are opposite halves; "
              "pick one, or neither for the whole file.", file=sys.stderr)
        return 2
    if args.boundaries:
        recs = reader.records(data)
        regs = reader.regions(recs)
        print(json.dumps({
            "file": args.transcript, "records": len(recs),
            "boundaries": reader.boundaries(recs),
            "regions": [{"chunk": k, "start": a, "stop": b, "records": b - a,
                         "is": ("since the last compaction" if k == len(regs) - 1 else
                                "what the last compaction replaced" if k == len(regs) - 2 else
                                "older")}
                        for k, a, b in regs],
            "note": ("--before-compaction returns regions 0..%d TOGETHER, so live "
                     "conversation arrives mixed with earlier summaries. --chunk picks one."
                     % (len(regs) - 2)) if len(regs) > 2 else
                    "one boundary, so --before-compaction and --chunk 0 are the same span",
        }, ensure_ascii=False, indent=2))
        return 0

    try:
        text, rep = reader.read(data, select=args.select,
                                since_compaction=args.since_compaction,
                                before_compaction=args.before_compaction,
                                chunk=args.chunk, max_block=args.max_block)
    except ValueError as e:
        print("cai read: %s" % e, file=sys.stderr)
        return 2
    rep["file"] = args.transcript

    if args.out:
        # **Three refusals, and the first is the one that matters.** A reader that
        # can be pointed at its own input is a writer wearing a different name.
        if os.path.abspath(args.out) == os.path.abspath(args.transcript):
            print("cai read: --out is the input. This tool does not write over what it "
                  "reads, and naming the same path does not make it an exception.",
                  file=sys.stderr)
            return 2
        if os.path.exists(args.out):
            print("cai read: %s" % safewrite.write_new.__doc__.split("\n")[0]
                  + " %s exists." % args.out, file=sys.stderr)
            return 2
        if not rep["records_in"]:
            print("cai read: %s" % rep["note"], file=sys.stderr)
            return 1
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        rep["wrote"] = args.out

    if args.json or args.out:
        print(json.dumps(rep, ensure_ascii=False, indent=2))
    else:
        if not rep["records_in"]:
            print("cai read: %s" % rep["note"], file=sys.stderr)
            return 1
        sys.stdout.write(text if text.endswith("\n") else text + "\n")
    return 0 if rep["records_in"] and rep["signature_held"] else 1


if __name__ == "__main__":
    sys.exit(main())
