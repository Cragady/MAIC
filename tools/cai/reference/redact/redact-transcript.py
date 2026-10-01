#!/usr/bin/env python3
"""Redact secrets from a Claude Code session transcript (.jsonl), in place.

Keep this on disk and invoke it by path. Running the logic inline through a
heredoc records the pattern list as a tool-call argument in the very transcript
being cleaned, which reintroduces every secret it just removed.

See README.md for the failure modes this works around.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys

# Keys whose string values are replaced wholesale when a line is blanked.
# toolUseResult appears both as a bare string and as {"stdout":..., "stderr":...},
# so match on key name at any depth rather than assuming a shape.
PAYLOAD_KEYS = {"content", "toolUseResult", "text", "stdout", "stderr", "command"}

# Threading fields. Scrubbing these breaks --resume; a promptId in particular
# looks identical to a leaked credential under a generic UUID regex.
STRUCTURAL_KEYS = ("promptId", "uuid", "parentUuid", "sessionId", "requestId")

# Shape-only patterns for locating candidate secrets in the backup. These are
# deliberately generic: this file's own text must never contain a real secret.
CANDIDATE_PATTERNS = (
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",  # uuid
    r"\b[0-9A-F]{32}\b",                    # hex salt / device id
    r"[A-Za-z0-9+/]{48,}={0,2}",            # long base64 blob
    r"\b\d+\.\d{4,}\.\d+\b",                # build-style version
    r"\b\w+:\w*[Tt]oken\w*",                # credential store key
    r"\b[\w-]+(?:tokens|registry|secrets)\.json\b",
    r"\b\d{16,}\b",                         # long opaque numeric handle
)

NOTICE = "[REDACTED: credential-bearing tool output removed.]"


def load(path: str) -> list[str]:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def structural_values(lines: list[str]) -> set[str]:
    """Values that are threading identifiers, not secrets."""
    keep: set[str] = set()
    pat = re.compile(r'"(?:%s)":\s*"([^"]+)"' % "|".join(STRUCTURAL_KEYS))
    for line in lines:
        keep.update(pat.findall(line))
    return keep


def blank_payloads(obj, notice: str):
    if isinstance(obj, dict):
        return {
            k: (notice if k in PAYLOAD_KEYS and isinstance(v, str)
                else blank_payloads(v, notice))
            for k, v in obj.items()
        }
    if isinstance(obj, list):
        return [blank_payloads(v, notice) for v in obj]
    return obj


def candidates(text: str, keep: set[str]) -> set[str]:
    found: set[str] = set()
    for pattern in CANDIDATE_PATTERNS:
        found |= {m for m in re.findall(pattern, text) if len(m) > 7}
    return found - keep


def check_json(lines: list[str]) -> int:
    bad = 0
    for n, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            json.loads(line)
        except ValueError:
            bad += 1
            print(f"  !! line {n}: invalid JSON", file=sys.stderr)
    return bad


def do_redact(args) -> int:
    lines = load(args.transcript)

    dest = args.backup
    if os.path.isdir(dest):
        dest = os.path.join(dest, os.path.basename(args.transcript) + ".bak")
    if os.path.exists(dest) and not args.force:
        print(f"backup already exists: {dest} (use --force to overwrite)", file=sys.stderr)
        return 2
    shutil.copy2(args.transcript, dest)
    print(f"backup -> {dest}")

    keep = structural_values(lines)
    blank = {int(n) for n in args.blank_lines.split(",") if n.strip()} if args.blank_lines else set()

    # Candidates are derived from a file, never from the command line, so no
    # secret is written into this script or into the invocation that runs it.
    #
    # Default source is the transcript itself. For a *re-leak* -- a value that
    # was scrubbed and then reintroduced by later tooling -- the live file no
    # longer contains it at the original location, so point --candidates-from
    # at the pristine backup instead. Without this the pass finds 0 candidates
    # and silently no-ops.
    source_lines = load(args.candidates_from) if args.candidates_from else lines
    scoped_text = "\n".join(source_lines)
    if args.only_lines:
        want = {int(n) for n in args.only_lines.split(",") if n.strip()}
        scoped_text = "\n".join(source_lines[n - 1] for n in want if n <= len(source_lines))
    secrets = sorted(candidates(scoped_text, keep), key=len, reverse=True)

    print(f"{len(secrets)} candidate value(s); blanking {len(blank)} line(s)")

    out: list[str] = []
    for n, line in enumerate(lines, 1):
        if not line.strip():
            out.append(line)
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            out.append(line)
            continue
        if n in blank:
            obj = blank_payloads(obj, NOTICE)
        text = json.dumps(obj, ensure_ascii=False)
        for secret in secrets:
            text = text.replace(secret, "[REDACTED]")
        out.append(text)

    if check_json(out):
        print("refusing to write: produced invalid JSON", file=sys.stderr)
        return 3

    with open(args.transcript, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print(f"rewrote {len(out)} line(s)")
    return 0


def do_verify(args) -> int:
    live = "\n".join(load(args.transcript))
    backup = load(args.backup)
    keep = structural_values(live.splitlines())

    scope = backup
    if args.only_lines:
        want = {int(n) for n in args.only_lines.split(",") if n.strip()}
        scope = [backup[n - 1] for n in want if n <= len(backup)]

    expected = candidates("\n".join(scope), keep)
    leaked = sorted(x for x in expected if x in live)

    print(f"checked {len(expected)} carried-over value(s)")
    for item in leaked:
        # Report location and shape only -- never the value.
        rows = [n for n, l in enumerate(live.splitlines(), 1) if item in l]
        print(f"  !! len={len(item)} still present on line(s) {rows}", file=sys.stderr)

    bad = check_json(live.splitlines())
    print(f"RESIDUAL: {len(leaked)}   invalid_json: {bad}")
    return 1 if (leaked or bad) else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("transcript", help="path to the session .jsonl")
    ap.add_argument("--backup", required=True, help="backup file, or a directory to place one in")
    ap.add_argument("--blank-lines", help="1-indexed lines whose tool payloads are replaced wholesale")
    ap.add_argument("--only-lines", help="derive candidate secrets from these lines only")
    ap.add_argument("--candidates-from",
                    help="derive candidates from this file instead of the transcript; "
                         "point at the pristine backup when cleaning a re-leak")
    ap.add_argument("--verify", action="store_true", help="compare against the backup instead of redacting")
    ap.add_argument("--force", action="store_true", help="overwrite an existing backup")
    args = ap.parse_args()

    if args.verify:
        return do_verify(args)
    return do_redact(args)


if __name__ == "__main__":
    sys.exit(main())
