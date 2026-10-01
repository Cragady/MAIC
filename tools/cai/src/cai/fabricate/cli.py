"""cai fabricate -- add content to a transcript, marked, and never in place."""
from cai import safewrite
import argparse
import json
import os
import sys

from cai.fabricate import maker
from cai.grammar import maic as fmt

MAN_HELP = """cai fabricate -- full reference

  cai fabricate <transcript> --to OUT --user TEXT --assistant TEXT --at N
  cai fabricate <transcript> --audit

THE NAME IS A WARNING LABEL
  fabricate means both TO MANUFACTURE and TO DECEIVE, and this is exactly where
  those meanings meet. Every use is manufacture; the failure mode is deception.
  A neutral name -- insert, compose, add -- would describe the mechanism and hide
  the hazard, and somebody would reach for it without the second meaning ever
  crossing their mind.

WHY IT IS A SEPARATE TOOL
  redact removes. fabricate adds. The rejected proposal names the hazard exactly:
  what if we redacted the void between two characters with content? It would
  work, and it would make redact a tool that does two opposite things while
  claiming one -- not dishonest, but no longer honest by construction. A tool
  that can only remove cannot be asked to add.

MODES
  --marked       (default) origin.kind fabricated, plus a flag. Meta-level, so
                 it does not reach the model and a measurement arm is unaffected
                 by its presence, while an operator and every tool can see it.
  --loud         additionally announces itself in message content.
  --true-silent  nothing. THIS BREAKS THE CONTRACT, exactly as it does for a cut:
                 the result is indistinguishable from a real exchange and no
                 audit can recover that it was not. It warns at the point of use.

IT NEVER WRITES IN PLACE
  --to is required. The source is read and the result lands elsewhere, which is
  the only shape safe against a file something else holds open -- and it means a
  fabrication can never overwrite the record it was derived from.

  There is no --agent path and there will not be. An agent never answers a prompt
  that manufactures content into a record.

EXIT   0 ok   1 refused   2 usage
"""


def _load(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip():
                continue
            try:
                out.append(json.loads(line))
            except ValueError as e:
                raise ValueError("%s line %d is not valid JSON: %s" % (path, n, e))
    return out


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    if "--agent" in argv:
        sys.stderr.write("error: this tool has no --agent path, deliberately. An agent "
                         "never answers a prompt that manufactures content into a "
                         "record.\n")
        return 2
    ap = argparse.ArgumentParser(prog="cai fabricate", add_help=False)
    ap.add_argument("transcript")
    ap.add_argument("--force", action="store_true",
                    help="OPERATOR ONLY: overwrite the destination")
    ap.add_argument("--to")
    ap.add_argument("--user")
    ap.add_argument("--assistant")
    ap.add_argument("--at", type=int)
    ap.add_argument("--timestamp")
    ap.add_argument("--for", dest="source")
    ap.add_argument("--loud", action="store_true")
    ap.add_argument("--true-silent", action="store_true")
    ap.add_argument("--audit", action="store_true")
    args = ap.parse_args(argv)

    try:
        lines = _load(args.transcript)
    except (OSError, ValueError) as e:
        sys.stderr.write("error: %s\n" % e)
        return 1

    if args.audit:
        print(json.dumps(maker.audit(lines), ensure_ascii=False, indent=2))
        return 0

    if not args.to:
        sys.stderr.write("error: --to is required. This never writes in place: a "
                         "fabrication must not overwrite the record it came from.\n")
        return 2
    if args.at is None or not (args.user or args.assistant):
        sys.stderr.write("error: --at and at least one of --user/--assistant\n")
        return 2

    mode = "true-silent" if args.true_silent else ("loud" if args.loud else "marked")
    if mode == "true-silent":
        sys.stderr.write(
            "WARNING: --true-silent breaks the fabrication contract.\n"
            "  The inserted turns carry no marker of any kind. The result is\n"
            "  indistinguishable from a real exchange, and no audit can recover that\n"
            "  it was fabricated. Nothing downstream can undo what this discards.\n")

    turns = []
    if args.user:
        turns.append(("user", args.user))
    if args.assistant:
        turns.append(("assistant", args.assistant))
    is_maic = fmt.is_maic(lines)
    cwd = (fmt.first_start(lines).get("workspace") if is_maic
           else next((r.get("cwd") for r in lines if r.get("cwd")), None)) or os.getcwd()
    ts = args.timestamp
    if not ts and is_maic:
        ts = fmt.stamp()  # a MAIC record's own form: local time with the offset
    if not ts:
        from cai.timekeeping import cli as timecli
        ts = timecli.stamp()
    try:
        if is_maic:
            out, made = maker.insert_maic(lines, turns, args.at, cwd, ts, mode=mode,
                                         source=args.source)
        else:
            out, made = maker.insert(lines, turns, args.at, cwd, ts, mode=mode,
                                     source=args.source)
    except ValueError as e:
        sys.stderr.write("error: %s\n" % e)
        return 1
    # `--to` says nothing about the destination being free. It clobbered one
    # silently until 2026-09-03.
    try:
        safewrite.write_new(args.to,
                            "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in out),
                            force=args.force)
    except FileExistsError as e:
        sys.stderr.write("error: %s\n" % e)
        return 2
    print(json.dumps({"transcript": args.transcript, "written_to": args.to,
                      "mode": mode, "inserted": len(made),
                      "records": len(out),
                      "uuids": [r["uuid"] for r in made if r.get("uuid")]},
                     ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
