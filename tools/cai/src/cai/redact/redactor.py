"""Scrub credential material out of a Claude Code session transcript.

Ported from reference/redact/redact-transcript.py onto the shared grammar, which
owns the payload and structural key sets. That module exists because this
knowledge was once held in two places and drifted into a defect: a blanker that
handled a payload key as a string and skipped the same key where it held an
object, reporting success over data it never touched (SOPIA CASES 15).

Two rules shape everything here:

  - Candidates are derived from a FILE, never from an argument. A pattern list
    passed on the command line is recorded in the very transcript being cleaned.
  - The tool reports position and shape, never a value. Printing what it found
    reintroduces it one layer up.
"""
import json
import os
import re
import shutil

from cai.grammar import maid
from cai.grammar import records

# Shape-only patterns for locating candidates. This file's own text must never
# contain a real secret, so every pattern describes a shape rather than a value.
#: Named shapes, so a map can say WHAT KIND of thing was removed without saying
#: which thing. The names are the only new disclosure and they are categories.
PATTERN_NAMES = (
    ("uuid", r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"),
    ("hex-32", r"[0-9A-F]{32}"),
    ("base64-blob", r"[A-Za-z0-9+/]{48,}={0,2}"),
    ("version", r"\d+\.\d{4,}\.\d+"),
    ("store-key", r"[\w+.\-]+:\w*[Tt]oken\w*"),
    ("store-file", r"[\w.\-]+(?:tokens|registry|secrets)\.json"),
    ("numeric-handle", r"\d{16,}"),
)

CANDIDATE_PATTERNS = (
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",  # uuid
    r"\b[0-9A-F]{32}\b",                       # hex salt / device id
    r"[A-Za-z0-9+/]{48,}={0,2}",               # long base64 blob
    r"\b\d+\.\d{4,}\.\d+\b",                   # build-style version
    # credential-store key. The leading class is deliberately NOT \w+: the
    # character before the colon is often '+', '-' or '.', and \w+ cannot match
    # those -- a pattern that could not match its own target and returned a
    # false zero (SOPIA CASES 16). Pinned by a positive control in tests.
    r"[\w+.\-]+:\w*[Tt]oken\w*",
    r"\b[\w.\-]+(?:tokens|registry|secrets)\.json\b",
    r"\b\d{16,}\b",                            # long opaque numeric handle
)

NOTICE = "[REDACTED: credential-bearing tool output removed.]"
MARK = "[REDACTED]"
MIN_LEN = 8


def mark(index, numbered=False):
    """`[REDACTED]` by default; `[REDACTED:3]` when numbering is asked for.

    **Numbering is OPT-IN, restored to that on 2026-09-04.** I made it the
    default when adding the map, which silently broke anything matching the
    literal `[REDACTED]` -- a change to a security tool's output that nobody
    asked for. The original marker is the default again; `--map` turns numbering
    on because a map needs markers it can tell apart, and `--number` asks for it
    alone.

    One anonymous marker collapses every distinct secret into the same token, so
    a redacted file cannot answer the first question anyone asks of it: *is this
    the same thing as that one?* Numbering is stable per distinct value across
    the whole file, so four occurrences of `[REDACTED:3]` are four occurrences of
    ONE thing, and `[REDACTED:4]` beside it is something else.

    **What that discloses is the COUNT of distinct secrets, which is shape.**
    This tool already reports position and shape and never a value; a number is
    the same class of disclosure as a length.
    """
    return ("[REDACTED:%d]" % index) if numbered else MARK


def load(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


#: MAID's threading keys, protected the way Claude Code's are, for a MAID session only:
#: `tool_call_id` pairs a tool message with its call, `id` names the call inside `tool_calls`
#: and the parent inside `resumed_from`, `child` names a subagent's transcript.
MAID_STRUCTURAL_KEYS = ("tool_call_id", "id", "child")


def structural_values(lines, extra_keys=()):
    """Threading identifiers, which must survive: scrubbing them breaks resume."""
    keep = set()
    pat = re.compile(r'"(?:%s)":\s*"([^"]+)"' % "|".join(records.STRUCTURAL_KEYS + tuple(extra_keys)))
    for line in lines:
        keep.update(pat.findall(line))
    return keep


def is_maid_lines(lines):
    """Whether these raw lines are a MAID session, by content."""
    return maid.is_maid(records.load("\n".join(lines)))


# `string_values` moved to `cai.grammar.records`, which is where a fact about what
# a record IS belongs -- this file needed it first and that made it look local.
string_values = records.string_values


def substitute_strings(obj, subs, counts):
    """Apply operator substitutions to string VALUES, not to serialized text.

    **Applying them to the serialized line produced invalid JSON on 18 lines of a
    real transcript, 2026-09-05.** The shape-derived candidates can be replaced in
    the serialized form safely because they are uuids, base64 and the like -- they
    cannot straddle JSON syntax and their replacement is a fixed marker. An
    operator's find is arbitrary: a short one matches inside an escape sequence or
    across a quote boundary, and a replacement carrying a quote or backslash
    breaks the escaping.

    **The check caught it and refused, which is the tool working** -- but refusing
    was all it could do, because the damage was already in the text by then.
    Walking the structure makes the failure unreachable rather than detected:
    `json.dumps` re-escapes whatever the replacement contains, and a find can only
    ever match within one value.
    """
    if isinstance(obj, dict):
        return {k: substitute_strings(v, subs, counts) for k, v in obj.items()}
    if isinstance(obj, list):
        return [substitute_strings(v, subs, counts) for v in obj]
    if isinstance(obj, str):
        # Longest first: a shorter find that is a substring of a longer one would
        # otherwise consume part of it and leave a fragment behind.
        for find in sorted(subs, key=len, reverse=True):
            if find in obj:
                counts[find] = counts.get(find, 0) + obj.count(find)
                obj = obj.replace(find, subs[find])
        return obj
    return obj


def blank_payloads(obj, notice=NOTICE):
    """Replace every payload STRING at any depth, whatever shape holds it.

    The shape-blindness this guards against is the defect in CASES 15: matching
    a key as a string and silently skipping it where it holds an object.
    """
    if isinstance(obj, dict):
        return {k: (notice if k in records.PAYLOAD_KEYS and isinstance(v, str)
                    else blank_payloads(v, notice))
                for k, v in obj.items()}
    if isinstance(obj, list):
        return [blank_payloads(v, notice) for v in obj]
    return obj


def candidates(text, keep):
    found = set()
    for pattern in CANDIDATE_PATTERNS:
        found |= {m for m in re.findall(pattern, text) if len(m) > MIN_LEN}
    return found - keep


def check_json(lines):
    """Return the line numbers that are not valid JSON. A corrupt line makes the
    whole session unloadable, and the failure is silent until the next resume."""
    bad = []
    for n, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            json.loads(line)
        except ValueError:
            bad.append(n)
    return bad


def backup(transcript, dest, force=False):
    """Fail-closed. Never `cp -n`, which declines silently and returns success.

    A copy, never a move: the original must survive (invariant 10).
    """
    if os.path.isdir(dest):
        dest = os.path.join(dest, os.path.basename(transcript) + ".bak")
    if os.path.exists(dest) and not force:
        raise FileExistsError(
            "backup already exists: %s -- refusing to overwrite it. A backup that "
            "disagrees with its source is the only reference a verify has." % dest)
    shutil.copy2(transcript, dest)
    return dest


def redaction_map(secrets, index, sites):
    """Marker -> shape, count and sites. **Never a value, and that is the point.**

    A map that carried the strings would be a second artifact holding exactly
    what the first one was cleaned of -- and a more dangerous one, because it is
    small, looks like metadata, and travels easily. **The original is the
    resolver**: every entry points at the lines where its marker landed, and
    anyone holding the source can look there.

    What each entry carries:

        marker        `[REDACTED:3]`, stable across the file
        kind          which shape pattern matched -- uuid, base64 blob, and so on
        length        of the original, which is shape and not content
        occurrences   how many times it appeared
        lines         where, so the source can be consulted at those points
    """
    kinds = {}
    for sec in secrets:
        kinds[sec] = next((name for name, pat in PATTERN_NAMES if re.fullmatch(pat, sec)),
                          "unclassified")
    return {
        "schema": 1,
        "note": ("Shape and position only. No redacted VALUE appears here, deliberately: a "
                 "map naming what it replaced would be a second file holding the thing the "
                 "first was cleaned of. Resolve a marker by opening the source or its backup "
                 "at the lines named below."),
        "distinct": len(secrets),
        "entries": [
            # Numbered unconditionally: a map exists to tell entries apart, and
            # `redact` forces numbering whenever a map is requested.
            {"marker": mark(index[sec], numbered=True), "kind": kinds[sec], "length": len(sec),
             "occurrences": len(sites[index[sec]]), "lines": sites[index[sec]]}
            for sec in secrets
        ],
    }


def load_substitutions(path):
    """A file of *what to redact, and what it becomes*. **A file, never arguments.**

    Two accepted shapes, because both are natural to write by hand:

        {"real.example.com": "<host>", "Jane Roe": "<person-a>"}      JSON object
        real.example.com\t<host>                                      TSV, one per line

    **This file holds the things being removed, so it never travels with the
    output.** That is the same hazard as a values-bearing redaction map, arriving
    from the other direction: here the operator authored it deliberately, which
    makes it useful and does not make it safe to share.
    """
    with open(path, encoding="utf-8") as fh:
        raw = fh.read()
    stripped = raw.strip()
    if stripped.startswith("{"):
        return {k: v for k, v in json.loads(stripped).items() if k}
    out = {}
    for line in raw.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        find, _, repl = line.partition("\t")
        if find:
            out[find] = repl
    return out


def redact(transcript, backup_dest, blank_lines=(), only_lines=(),
           candidates_from=None, force=False, dry_run=False, to_path=None,
           map_path=None, numbered=False, substitutions_from=None):
    """Scrub the transcript in place. Returns a report dict; never a value."""
    # **Taken BEFORE the read**, so the window this covers is the whole operation.
    # `liveness` predicts whether a client might write; this detects whether one
    # did -- and it is the check a backup cannot substitute for, since a backup
    # holds the pre-read state and therefore never contains the write it raced.
    from cai import safewrite
    token = safewrite.stat_token(transcript)
    lines = load(transcript)
    # `to_path` makes this non-destructive: the source is read and the result lands
    # elsewhere, which is the only shape safe against a file something else holds
    # open. An in-place rewrite truncates, so a concurrent append is destroyed and
    # the backup cannot recover it -- a backup holds the pre-write state, not the
    # write it raced. With --to there is nothing to back up, because nothing is
    # overwritten.
    writing_in_place = not to_path or os.path.abspath(to_path) == os.path.abspath(transcript)
    # **A live transcript is refused, and the check comes BEFORE the backup.**
    # This module has described the hazard since it was written -- an in-place
    # rewrite truncates, so a concurrent append is destroyed and the backup
    # cannot recover it, because a backup holds the pre-write state and not the
    # write it raced -- and did not refuse. It described the wall and did not
    # build it.
    #
    # The primitives come from `trans-fairy-write`, which already refuses a live
    # target, rather than a third implementation. Taking the backup first would
    # produce a copy that LOOKS like insurance and predates the race, so the
    # refusal happens before anything is written at all.
    #
    # Only for an in-place run. `--to` never touches the source, which is why it
    # is the shape to reach for when a session is open.
    #
    # **A successful in-place run leaves its own target looking live**, since the
    # write updates the mtime -- so a second pass within the threshold is refused
    # by the first one's success. `trans-fairy-write` documented the same effect
    # rather than working around it, and the reasoning holds here: the check
    # cannot tell "modified because I wrote it" from "modified because a client
    # did", and refusing is the safe direction. Use `--to` for a second pass, or
    # `--force` when you know what touched it.
    if writing_in_place and not force:
        from cai.transfairywrite import writer as tfw
        live = tfw.liveness(transcript)
        if live:
            raise RuntimeError(
                "refusing to redact a transcript that looks live: %s. A write underneath a "
                "running client loses whatever it wrote in between, and the backup predates "
                "that. Use --to PATH to write elsewhere, or --force if you are certain."
                % "; ".join(live))
    bdest = None
    if not dry_run and writing_in_place:
        bdest = backup(transcript, backup_dest, force=force)

    # **Operator substitutions run FIRST and are exact.** Owner's ask: a map of
    # *what to redact and what it is replaced by*, so meaning survives -- a real
    # hostname becoming `<host>` rather than a marker that erases what kind of
    # thing was there. They run before shape-derived candidates so a value the
    # operator has named keeps the replacement she chose rather than becoming an
    # anonymous marker.
    #
    # **Sourced from a FILE, never arguments** -- the rule this module opens
    # with. A substitution passed on the command line is recorded in the very
    # transcript being cleaned, and here it would record BOTH halves.
    subs = load_substitutions(substitutions_from) if substitutions_from else {}

    keep = structural_values(lines, MAID_STRUCTURAL_KEYS if is_maid_lines(lines) else ())
    source_lines = load(candidates_from) if candidates_from else lines
    scoped = source_lines
    if only_lines:
        scoped = [source_lines[n - 1] for n in only_lines if n <= len(source_lines)]
    # Derived from string VALUES, because that is where replacement happens.
    secrets = sorted(candidates(string_values(scoped), keep), key=len, reverse=True)

    # **The map is numbered, and it holds no values.** Owner asked for a map;
    # the constraint is that a map naming what it replaced IS the secret, and
    # would be a second file to leak. So a marker is an INDEX and the map records
    # shape, count and sites -- the original resolves it, the same way a reader's
    # map restores readability while the artifact holds the truth.
    # **A map forces numbering, and the rule lives HERE rather than in the CLI.**
    # An unnumbered map is useless -- every entry would name `[REDACTED]` and no
    # entry could be told from another. I put this in the CLI first, so calling
    # the library directly produced a map that could not be read. An invariant
    # belongs with the function that has it, not with one caller of it.
    if map_path:
        numbered = True

    index = {sec: i for i, sec in enumerate(secrets, 1)}
    sites = {i: [] for i in index.values()}
    sub_counts = {}

    blank = set(blank_lines)
    out, blanked, substituted = [], 0, 0
    for n, line in enumerate(lines, 1):
        if not line.strip():
            out.append(line)
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            out.append(line)  # leave unparseable lines untouched rather than mangle them
            continue
        if n in blank:
            obj = blank_payloads(obj)
            blanked += 1
        if subs:
            obj = substitute_strings(obj, subs, sub_counts)
        # **Secrets are replaced inside string VALUES, not in serialized text.**
        # Measured 2026-09-05 on a real transcript: replacing in the serialized
        # form broke 18 lines. A candidate can BEGIN at the escaped character of
        # a `\X` pair, so removing it orphans the backslash and `\[REDACTED]` is
        # an invalid escape. `check_json` caught it and refused -- which is the
        # tool working, and was the only thing between this and a corrupted
        # transcript.
        #
        # Walking the structure makes it unreachable rather than detected:
        # `json.dumps` owns the escaping, and a find can only match within one
        # value.
        marks = {sec: mark(index[sec], numbered) for sec in secrets}
        hit = {}
        obj = substitute_strings(obj, marks, hit)
        for sec, c in hit.items():
            sites[index[sec]].append(n)
            substituted += c
        text = json.dumps(obj, ensure_ascii=False)
        out.append(text)

    bad = check_json(out)
    if bad:
        raise RuntimeError("refusing to write: would produce invalid JSON on line(s) %s" % bad)

    dest = transcript if writing_in_place else to_path
    if not dry_run and writing_in_place:
        # Checked as late as possible: everything above is reading and computing,
        # so the narrower this window is, the less it can miss.
        ok, why = safewrite.unchanged_since(transcript, token)
        if not ok and not force:
            raise RuntimeError("refusing to write: %s" % why)
    if not dry_run:
        with open(dest, "w", encoding="utf-8") as fh:
            fh.write("\n".join(out) + "\n")

    if map_path and not dry_run:
        from cai import safewrite
        safewrite.write_new(map_path, json.dumps(redaction_map(secrets, index, sites),
                                                 ensure_ascii=False, indent=2) + "\n",
                            force=force)

    return {"transcript": transcript, "written_to": dest, "in_place": writing_in_place,
            "map": map_path if (map_path and not dry_run) else None,
            "backup": bdest, "candidates": len(secrets),
            "lines_blanked": blanked, "substitutions": substituted,
            "operator_substitutions": {"pairs": len(subs),
                                       "applied": sum(sub_counts.values())},
            "lines_written": len(out), "dry_run": dry_run,
            "note": "candidates derived from a file, never an argument; no value is reported"}


def verify(transcript, backup_path, only_lines=()):
    """Compare the live file against the pristine backup. Non-zero if anything survived."""
    live_lines = load(transcript)
    live = "\n".join(live_lines)
    backup_lines = load(backup_path)
    keep = structural_values(live_lines, MAID_STRUCTURAL_KEYS if is_maid_lines(live_lines) else ())

    scope = backup_lines
    if only_lines:
        scope = [backup_lines[n - 1] for n in only_lines if n <= len(backup_lines)]

    # **Same text as `redact` derives from**, or verify checks for things that
    # were never replaceable and misses the ones that were.
    expected = candidates(string_values(scope), keep)
    live_values = string_values(live_lines)
    leaked = sorted(x for x in expected if x in live_values)
    # Position and shape only -- never the value.
    residual = [{"length": len(x),
                 "lines": [n for n, l in enumerate(live_lines, 1)
                           if x in string_values([l])]}
                for x in leaked]
    bad = check_json(live_lines)
    return {"checked": len(expected), "residual": len(leaked), "residual_detail": residual,
            "invalid_json_lines": bad, "clean": not leaked and not bad}


# --- projection -------------------------------------------------------------
#
# Stripping thinking blocks and tool traffic to reach the conversation is
# REMOVAL, which is this tool's verb -- so the projection lives here rather than
# becoming a second surface on trans-fairy. What it removes is structural
# categories instead of secrets; the operation is the same one pointed at a
# different target.

# The fence definition lives in cai.fence. It used to live here too, and the two
# copies had already diverged -- this one captured the closer marker and the one
# in notation.audit did not.
from cai import fence as _fence  # noqa: E402

FENCE = _fence.PATTERN

SELECTIONS = {
    "conversation": ("user", "assistant"),
    "turns": ("user",),
    "tools": (),
    "all": ("user", "assistant"),
}


def load_records(path):
    """Parse a transcript into records.

    `load` deliberately returns raw lines, because secret scanning works on the
    text as written -- parsing and re-serialising would move the very bytes it
    is looking for. The projection needs structure instead, so it gets its own
    loader rather than changing what `load` means.
    """
    out = []
    for line in load(path):
        if not line.strip():
            continue
        try:
            out.append(json.loads(line))
        except ValueError:
            continue
    # A MAID session is projected onto the grammar, so the selections below read it unchanged.
    return maid.to_claude(out) if maid.is_maid(out) else out


def _cap(text, limit):
    """Truncate only when asked, and say so in the output when it happens.

    A projection that silently drops content is the failure this repository
    keeps closing: material that was cut looks identical to material that was
    never there. So there is no default cap, and a cap that fires leaves a mark.
    """
    if not limit or len(text) <= limit:
        return text
    return text[:limit] + ("\n[... %d characters truncated by --max-block ...]" % (len(text) - limit))


def _blocks(rec):
    c = (rec.get("message") or {}).get("content")
    if isinstance(c, str):
        return [{"type": "text", "text": c}]
    return [b for b in c if isinstance(b, dict)] if isinstance(c, list) else []


def neutralise_fences(text):
    """Turn fence markers into evidence that one was there.

    Delegates to `cai.fence`, which owns the definition. Only a delimiter ALONE
    ON A LINE is a fence; the same characters inside a sentence are a mention of
    the convention, not a use of it.
    """
    return _fence.neutralise(text)


def compaction_boundaries(lines):
    """Indices of compaction summary records. A field, never a phrase (PROBES P13)."""
    return [i for i, r in enumerate(lines) if r.get("isCompactSummary")]


def _warn_if_vocabulary_drifted(local):
    """Report, never repair, when the dictionary and this table disagree."""
    try:
        from cai.notation import store as nstore
        doc = nstore.load()
        for e in nstore.live(doc):
            if e["id"] == "projection-selection" and sorted(e["symbols"]) != local:
                import sys as _s
                _s.stderr.write(
                    "cai redact: selection vocabulary differs from notation "
                    "(%s here, %s recorded). The dictionary is what a person reads.\n"
                    % (",".join(local), ",".join(sorted(e["symbols"]))))
    except Exception:                                            # noqa: BLE001
        pass


def project(lines, select="conversation", since_compaction=False, neutralise=False,
            max_block=None, offset=0):
    """Project a transcript to the material a reader actually wants.

    Thinking blocks are excluded from every selection: they are the single
    largest component, and their signatures do not validate outside the session
    that minted them.
    """
    if select not in SELECTIONS:
        raise ValueError("select must be one of: %s" % ", ".join(sorted(SELECTIONS)))
    # The selection names are a notation entry an operator reads. If the
    # dictionary and this table ever disagree, the dictionary is the one being
    # read by a person, so the disagreement is reported rather than swallowed.
    _warn_if_vocabulary_drifted(sorted(SELECTIONS))
    start = 0
    if since_compaction:
        bounds = compaction_boundaries(lines)
        if bounds:
            start = bounds[-1]
    want_roles = SELECTIONS[select]
    want_tools = select in ("tools", "all")
    out, kept, neutralised = [], 0, 0
    # **`offset` keeps the `Ln` markers pointing at the SOURCE.** A caller that
    # pre-slices its records would otherwise renumber from 1, so every citation
    # into the output points at the wrong line -- and the alignment between these
    # markers and the source's physical line numbers is a property at least one
    # peer session relies on for anchoring quotes.
    for i, rec in enumerate(lines[start:], start + 1 + offset):
        if rec.get("type") not in ("user", "assistant"):
            continue
        if rec.get("type") not in want_roles and not want_tools:
            continue
        parts = []
        for b in _blocks(rec):
            t = b.get("type")
            if t == "text" and rec.get("type") in want_roles:
                parts.append(b.get("text", ""))
            elif t == "tool_use" and want_tools:
                parts.append("[tool_use %s] %s" % (b.get("name", "?"),
                                                   _cap(json.dumps(b.get("input", {})), max_block)))
            elif t == "tool_result" and want_tools:
                c = b.get("content")
                parts.append("[tool_result] %s"
                             % _cap(c if isinstance(c, str) else json.dumps(c), max_block))
            # 'thinking' is never emitted, in any selection
        body = "\n".join(x for x in parts if x and x.strip())
        if not body:
            continue
        if neutralise:
            body, n = neutralise_fences(body)
            neutralised += n
        kept += 1
        out.append("\u2500\u2500 L%d \u00b7 %s \u2500\u2500\n%s" % (i, rec["type"], body))
    return {"text": "\n\n".join(out), "records_emitted": kept,
            "started_at_record": start + 1, "fences_neutralised": neutralised,
            "selection": select, "since_compaction": bool(since_compaction)}
