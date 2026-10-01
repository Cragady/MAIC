"""The Claude Code transcript record grammar.

The shared definition. `transfairy` writes these records; `redact` reads and
rewrites them. Holding it once is the point of this package: two copies of this
knowledge already produced a defect -- a redaction routine that handled a
payload key as a string and skipped the same key when it held an object.

A .jsonl transcript is newline-delimited: one record per line, and JSON escapes
newlines inside strings, so a raw newline never appears within a record.
Line number is therefore the record index.
"""
import json


FRAME_TYPES = ("mode", "permission-mode", "file-history-snapshot",
               "ai-title", "last-prompt")

USER_FIELDS = ("parentUuid", "isSidechain", "promptId", "type", "message",
               "uuid", "timestamp", "permissionMode", "origin", "promptSource",
               "userType", "entrypoint", "cwd", "sessionId", "version", "gitBranch")

ASSISTANT_FIELDS = ("parentUuid", "isSidechain", "message", "requestId", "type",
                    "uuid", "timestamp", "effort", "session_id",
                    "userType", "entrypoint", "cwd", "sessionId", "version", "gitBranch")

USAGE_FIELDS = ("input_tokens", "cache_creation_input_tokens",
                "cache_read_input_tokens", "output_tokens", "service_tier")

# Keys whose STRING values are payload -- what a redaction blanks. toolUseResult
# appears both as a bare string and as {"stdout":..., "stderr":...}, so a consumer
# must match on key name at any depth rather than assuming one shape. That
# shape-blindness is the defect (SOPIA CASES 15) this module exists to prevent.
PAYLOAD_KEYS = frozenset({"content", "toolUseResult", "text", "stdout", "stderr", "command"})

# Threading identifiers. Scrubbing these breaks --resume; a promptId in particular
# is indistinguishable from a leaked credential under a generic UUID pattern, so a
# redaction must exclude them by key name before matching.
STRUCTURAL_KEYS = ("promptId", "uuid", "parentUuid", "sessionId", "requestId")


def _where(path):
    """Name the file in an error, when the caller gave one."""
    return "%s " % path if path else ""


def load(text, strict=False, path=None):
    """Parse JSONL into records. **A record is an OBJECT.**

    **This is grammar, and it was the one part of the grammar left outside.**
    Five loaders existed across the suite with three different answers to what a
    record is: dict-only in one, any JSON type in three, and one that raised on a
    bad line. That divergence has already cost twice.

        a bare `200` is valid JSON and is not a record. Letting it through
        reached code calling `.get()` on an int, so re-reading a damaged file
        raised instead of refusing -- reported by a peer session, 2026-09-04.

        raising on a bad line meant a single malformed record took down
        `install`, `audit` and `state` rather than being skipped.

    **Lenient by default, and lenient about the right thing.** A line that will
    not parse, or parses into something that is not an object, is skipped -- it
    is not content and inventing one would be worse. `strict=True` raises
    instead, for a caller that would rather stop than proceed on a partial read.

    **What this deliberately does NOT do is sniff a format.** A single-record
    JSONL file is valid JSON, which has already produced one bug: a one-line
    fixture parsed whole, found no stamp, and reported that it made no claim.
    Callers say what they have; guessing is where that trap lives.
    """
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except ValueError as e:
            if strict:
                raise ValueError("%sline %d is not valid JSON: %s" % (_where(path), n, e))
            continue
        if isinstance(rec, dict):
            out.append(rec)
        elif strict:
            raise ValueError("%sline %d parses but is not a record (got %s)"
                             % (_where(path), n, type(rec).__name__))
    return out


def string_values(records_or_lines):
    """Every string VALUE in these records, joined by newlines.

    **The text that text-matching must operate on**, and the distinction behind
    the worst defect this suite has had. `redact` derived its candidates from the
    SERIALIZED lines and replaced them there, so a candidate could begin at the
    escaped character of a `\\n` pair -- removing it orphaned the backslash and
    produced invalid JSON on 18 lines of a real transcript.

    Moving replacement to values without moving derivation was worse: the
    candidate existed in the serialization and never in any value, so it matched
    nothing, replaced nothing, and **reported success**. A refusal is loud; a
    silent miss looks like a clean file.

    **Derivation, replacement and verification must read the same text.** That is
    what this function is for, and why it belongs to the grammar rather than to
    whichever tool needed it first.

    Accepts records or raw lines; a line that is not a record is scanned as text
    rather than dropped, since unparseable material can still carry a secret.
    """
    out = []

    def walk(o):
        if isinstance(o, dict):
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
        elif isinstance(o, str):
            out.append(o)

    for item in records_or_lines:
        if isinstance(item, dict):
            walk(item)
            continue
        if not str(item).strip():
            continue
        try:
            rec = json.loads(item)
        except ValueError:
            out.append(item)
            continue
        if isinstance(rec, dict):
            walk(rec)
        else:
            out.append(str(item))
    return "\n".join(out)


def content_blocks(record):
    """Yield every content block of a record, whatever shape it takes.

    Content is a bare string on some user records and a list of typed blocks on
    others. A consumer that handles only one shape silently skips the other --
    that is exactly the defect this function exists to prevent.
    """
    msg = record.get("message") or {}
    content = msg.get("content")
    if isinstance(content, str):
        yield {"type": "text", "text": content}
    elif isinstance(content, list):
        for b in content:
            if isinstance(b, dict):
                yield b


def text_of(record):
    """All human-readable text in a record, across every content shape."""
    out = []
    for b in content_blocks(record):
        t = b.get("type")
        if t == "text":
            out.append(b.get("text") or "")
        elif t == "thinking":
            out.append(b.get("thinking") or "")
        elif t == "tool_result":
            c = b.get("content")
            if isinstance(c, str):
                out.append(c)
            elif isinstance(c, list):
                out.extend(x.get("text") or "" for x in c if isinstance(x, dict))
    return "\n".join(x for x in out if x)
