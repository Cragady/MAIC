"""cai redact -- scrub credential material out of a Claude Code transcript.

Keep it on disk and invoke it by path. Running the logic inline through a heredoc
records the pattern list as a tool-call argument in the very transcript being
cleaned, which reintroduces everything it just removed. See docs/redact/.
"""
from cai import safewrite
import argparse
import json
import sys

from cai.redact import redactor

MAN_HELP = """cai redact -- full reference

  cai redact <transcript.jsonl> --backup PATH [options]
  cai redact <transcript.jsonl> --backup PATH --verify

OPTIONS
  --backup PATH         backup file, or a directory to place one in. REQUIRED.
                        Fail-closed: refuses if the destination exists (see --force).
                        A copy, never a move -- the original must survive.
  --blank-lines LIST    1-indexed lines whose tool payloads are replaced wholesale,
                        for lines that are nothing but a credential dump.
  --only-lines LIST     derive candidates from these lines only.
  --candidates-from P   derive candidates from this file instead of the transcript.
                        Point at the PRISTINE BACKUP when cleaning a re-leak: the
                        live file no longer holds the value at its original
                        location, so the default pass finds nothing and no-ops.
  --verify              compare against the backup instead of redacting.
                        Exits non-zero if anything survived or JSON is invalid.
  --substitutions PATH  a FILE saying what to redact and what it becomes:

                          {"real.example.com": "<host>", "Jane Roe": "<person-a>"}
                          real.example.com<TAB><host>

                        **For preserving meaning, not for hiding secrets.** A
                        marker erases what KIND of thing was there; a chosen
                        replacement keeps the sentence readable. Applied before
                        the shape-derived candidates, longest find first, so a
                        value you named keeps the replacement you chose.

                        A FILE, never arguments -- a substitution on the command
                        line is recorded in the transcript being cleaned, and
                        this one would record both halves. **The file holds what
                        is being removed, so it never travels with the output.**

  --number              number the markers: [REDACTED:3]. Off by default, so the
                        original [REDACTED] is unchanged; --map turns it on.

  --map PATH            write a redaction map: which marker was which KIND of
                        thing, how long, how many times, and on which lines.
                        **No redacted value appears in it, ever.** A map naming
                        what it replaced would be a second file holding exactly
                        what the first was cleaned of -- smaller, more portable,
                        and looking like metadata. The source or its backup is
                        the resolver; the map says where to look.

                        Markers are NUMBERED -- `[REDACTED:3]` -- so a reader can
                        tell whether two redactions were the same thing. One
                        anonymous marker could never answer that.

  --to PATH             write the result HERE instead of over the source. Makes
                        the run non-destructive, so no backup is required and it
                        is safe against a file something else holds open. An
                        in-place rewrite truncates, so a concurrent append is
                        destroyed and a backup cannot recover it.

PROJECTION -- reading a transcript instead of scrubbing one
  --project             emit the conversation rather than a transcript. Stripping
                        thinking and tool traffic is REMOVAL, which is this
                        tool's verb, so it lives here rather than becoming a
                        second surface elsewhere.
  --select WHAT         conversation (default) | turns | tools | all
                          conversation  user and assistant text
                          turns         user turns only
                          tools         tool_use and tool_result
                          all           conversation plus tool traffic
                        THINKING IS NEVER EMITTED, in any selection: it is the
                        largest single component, and its signatures do not
                        validate outside the session that minted them.
  --since-compaction    start at the last compaction summary. That boundary is a
                        field (isCompactSummary), not a phrase -- searching for
                        the summary's wording over-counts.
  --neutralise-fences   rewrite fence markers into evidence that one was there.
                        Positional: a delimiter ALONE ON A LINE is a fence, the
                        same characters inside a sentence are a mention.
  --max-block N         truncate a tool block at N characters. OFF by default,
                        and a truncation that fires leaves a visible mark --
                        content silently dropped is indistinguishable from
                        content that was never there.
  --force               overwrite an existing backup. Never point this at a
                        pristine backup -- it destroys the only reference a
                        verify has.
  -n, --dry-run         report without writing.

WHAT IT DOES NOT DO
  It cannot cover anything appended after it runs, including the tool call that
  ran it. Candidate detection is shape-based and over-matches when unscoped. It
  measures PRESENCE only -- what the text contains -- not INFERENCE, what a
  reader could reconstruct from what remains. Those are two numbers and this
  reports the easier one. It never rewords surviving text to restore flow:
  that is fabrication, not redaction.

EXIT   0 clean/ok   1 residual found or write refused   2 usage/backup refused
"""


def _ints(s):
    return tuple(int(n) for n in s.split(",") if n.strip()) if s else ()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--man-help" in argv:
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai redact", description=__doc__.splitlines()[0])
    ap.add_argument("transcript")
    ap.add_argument("--backup")
    ap.add_argument("--to")
    ap.add_argument("--substitutions", metavar="PATH", dest="substitutions_from",
                    help="a FILE of what to redact and what it becomes: JSON object or "
                         "TAB-separated pairs. Preserves meaning where a marker would "
                         "erase it.")
    ap.add_argument("--number", action="store_true",
                    help="number the markers: [REDACTED:3]. Off by default; --map turns "
                         "it on, since a map needs markers it can tell apart.")
    ap.add_argument("--map", metavar="PATH", dest="map_path",
                    help="write a redaction map here: marker -> kind, length, count and "
                         "lines. Never a value; the source resolves it.")
    ap.add_argument("--project", action="store_true")
    ap.add_argument("--select", default="conversation")
    ap.add_argument("--since-compaction", action="store_true")
    ap.add_argument("--neutralise-fences", action="store_true")
    ap.add_argument("--max-block", type=int)
    ap.add_argument("--blank-lines")
    ap.add_argument("--only-lines")
    ap.add_argument("--candidates-from")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.project:
            rep = redactor.project(redactor.load_records(args.transcript),
                                   select=args.select,
                                   since_compaction=args.since_compaction,
                                   neutralise=args.neutralise_fences,
                                   max_block=args.max_block)
            text = rep.pop("text")
            if args.to:
                # `--to` makes the run non-destructive ABOUT THE SOURCE. It implies
                # nothing about the destination, and this clobbered one silently
                # until 2026-09-03.
                try:
                    safewrite.write_new(args.to, text, force=args.force)
                except FileExistsError as e:
                    sys.stderr.write("error: %s\n" % e)
                    return 2
                rep["written_to"] = args.to
                print(json.dumps(rep, ensure_ascii=False, indent=2))
            else:
                sys.stdout.write(text)
            return 0
        if not args.backup and not args.to:
            sys.stderr.write("error: --backup is required unless --to makes the run "
                             "non-destructive\n")
            return 2
        if args.verify:
            rep = redactor.verify(args.transcript, args.backup, only_lines=_ints(args.only_lines))
            print(json.dumps(rep, ensure_ascii=False, indent=2))
            return 0 if rep["clean"] else 1
        rep = redactor.redact(args.transcript, args.backup,
                              blank_lines=_ints(args.blank_lines),
                              only_lines=_ints(args.only_lines),
                              candidates_from=args.candidates_from,
                              force=args.force, dry_run=args.dry_run,
                              to_path=args.to, map_path=args.map_path,
                              numbered=args.number or bool(args.map_path),
                              substitutions_from=args.substitutions_from)
        print(json.dumps(rep, ensure_ascii=False, indent=2))
        return 0
    except FileExistsError as e:
        sys.stderr.write("error: %s\n" % e)
        return 2
    except (RuntimeError, FileNotFoundError) as e:
        sys.stderr.write("error: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
