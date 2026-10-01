"""`lines` -- the line-breaking member of the reflow family.

**One member, not the whole of `reflow`.** It owns how text is BROKEN into
lines and nothing else; see `cai/reflow/__init__.py` for the level above and
why it exists. Within it there is a mode axis:

**Owner's reading, 2026-09-02: two different MODES of a subset of reflow** -- not
two implementations of one thing, and not two things sharing a name. Both earlier
readings were wrong, and the difference matters because each implies a different
remedy: duplication wants a merge, coincidence wants a rename, **and a mode axis
wants one operation that takes a parameter.**

    sentence    one SENTENCE per line   -- splits on sentence boundaries
    paragraph   one PARAGRAPH per line  -- joins, and never splits

**They are opposite on the same input.** Three lines against one. Both are
correct for the rule their repository keeps, which is why neither could simply
absorb the other.

**The safety belongs to the OPERATION, not to a mode.** The paragraph
implementation refused to write when a content signature changed; the sentence
implementation rewrote files with no check at all. **Under one operation that
asymmetry is not defensible** -- a mode is a choice about line breaks, not about
whether the text is allowed to be silently altered.

**Nothing here is heavy.** Both originals import only `re`, `sys`, `glob` and
`pathlib`, and the sentence one merely lived in diction's directory without being
part of its package -- so putting reflow in `cai` costs neither repository a
dependency. That was checked rather than assumed, after being assumed wrongly.
"""
import re

#: This member rewrites markdown in place -- same kind of data in and out,
#: with a content signature proving the text survived. That is what earns it
#: the right to write over its input, and it is declared rather than assumed.
IN_PLACE = True

SENTENCE, PARAGRAPH = "sentence", "paragraph"

# **These two are a SUBSET of reflow, not the whole of it.** Owner, 2026-09-02.
# Line-breaking is one axis; a reflow could also normalise whitespace, hard-wrap
# to a column, or unwrap without re-splitting. The mode list is therefore read
# from notation rather than fixed here, so adding one is a dictionary entry and
# a handler -- not a change to what the tool is allowed to be.
BUILTIN_MODES = (SENTENCE, PARAGRAPH)


def modes():
    """The reflow modes, from notation where reachable."""
    try:
        from cai.notation import store as nstore
        doc = nstore.load()
        for e in nstore.live(doc):
            if e["id"] == "reflow-mode":
                return tuple(e["symbols"])
    except Exception:                                            # noqa: BLE001
        pass
    return BUILTIN_MODES


MODES = BUILTIN_MODES

# Guards against splitting on an abbreviation's full stop. Only the sentence mode
# needs them, because only it splits.
ABBREV = (r"(?<!\be\.g)(?<!\bi\.e)(?<!\betc)(?<!\bvs)(?<!\bcf)(?<!\bDr)(?<!\bMr)"
          r"(?<!\bMs)(?<!\bSt)(?<!\bNo)(?<!\b[A-Z])")
SPLIT = re.compile(ABBREV + r"([.!?])([)\"'`\]]*)\s+(?=[\"'`(\[]*[A-Z0-9])")

FENCE = re.compile(r"^\s*(```|~~~)")
LIST = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$")
BLOCK = re.compile(r"^\s*>")
TABLE = re.compile(r"^\s*\|")
HEAD = re.compile(r"^\s*#")
HTML = re.compile(r"^\s*<")
INDENT_CODE = re.compile(r"^(\t| {4,})\S")
RULE = re.compile(r"^[-*_]{3,}\s*$")


def check_mode(mode):
    """Resolve a mode, saying WHY it is unavailable rather than ruling on it.

    **A mode this build has no handler for is not a rejected proposal.** It is a
    fact about this installation, and the two messages differ because the remedy
    differs -- write the handler, or correct the name. Neither says the mode does
    not belong to `lines`; that is not this function's to decide.
    """
    if mode in BUILTIN_MODES:
        return mode
    if mode in modes():
        raise ValueError(
            "reflow mode %r is recorded in notation and has no handler in this build. "
            "Write one; the mode axis is open." % mode)
    raise ValueError(
        "reflow mode %r is not implemented here and not recorded in notation. "
        "implemented: %s" % (mode, ", ".join(BUILTIN_MODES)))


def sentences(text):
    return [s for s in SPLIT.sub(r"\1\2\n", text).split("\n") if s.strip()]


def signature(text):
    """Content signature: whitespace- and blockquote-marker-insensitive.

    **Markers are stripped because they are markup, not content.** A hard-wrapped
    quote carries one `>` per LINE and a reflowed one carries one per PARAGRAPH,
    so counting them reports a difference that is not a difference. The quoted
    text itself is still compared in full.
    """
    t = re.sub(r"(?m)^\s*>\s?", "", text)
    return re.sub(r"\s+", " ", t).strip()


def reflow(text, mode=PARAGRAPH):
    """Rewrap, keeping code, tables, headings and structure untouched."""
    check_mode(mode)
    lines = text.split("\n")
    out, buf, prefix = [], [], ""
    i, in_fence, fence_tok = 0, False, None

    def emit(joined, pre):
        if mode == SENTENCE:
            # A LIST prefix pads on continuation lines; a BLOCKQUOTE prefix
            # REPEATS. Padding a quote turns its later sentences into indented
            # text -- which reads as code, and silently changes what the markup
            # says the material is.
            cont = pre if pre.lstrip().startswith(">") else " " * len(pre)
            parts = sentences(joined)
            out.extend([(pre if n == 0 else cont) + s for n, s in enumerate(parts)])
        else:
            out.append((pre + joined).rstrip())

    def flush():
        nonlocal buf, prefix
        if buf:
            emit(" ".join(x.strip() for x in buf), prefix)
            buf, prefix = [], ""

    while i < len(lines):
        ln = lines[i]
        m = FENCE.match(ln)
        if m:
            flush()
            if not in_fence:
                in_fence, fence_tok = True, m.group(1)
            elif m.group(1) == fence_tok:
                in_fence = False
            out.append(ln)
            i += 1
            continue
        if in_fence:
            out.append(ln)
            i += 1
            continue
        if ln.strip() == "":
            flush()
            out.append("")
            i += 1
            continue
        if HEAD.match(ln) or TABLE.match(ln) or HTML.match(ln) or INDENT_CODE.match(ln) \
                or RULE.match(ln.lstrip()):
            flush()
            out.append(ln.rstrip())
            i += 1
            continue
        if BLOCK.match(ln):
            # A bare `>` is a paragraph break inside the quote and must survive:
            # dropping it silently merges two quoted paragraphs into one.
            flush()
            parts = []
            while i < len(lines) and BLOCK.match(lines[i]):
                if lines[i].strip() == ">":
                    if parts:
                        emit(" ".join(parts).strip(), "> ")
                        parts = []
                    out.append(">")
                else:
                    parts.append(re.sub(r"^\s*>\s?", "", lines[i]).strip())
                i += 1
            if parts:
                emit(" ".join(parts).strip(), "> ")
            continue
        lm = LIST.match(ln)
        if lm:
            flush()
            prefix = "%s%s " % (lm.group(1), lm.group(2))
            buf = [lm.group(3)]
            i += 1
            while (i < len(lines) and lines[i].strip()
                   and not LIST.match(lines[i]) and not FENCE.match(lines[i])
                   and not TABLE.match(lines[i]) and not HEAD.match(lines[i])
                   and not BLOCK.match(lines[i]) and lines[i].startswith(" ")):
                buf.append(lines[i].strip())
                i += 1
            flush()
            continue
        buf.append(ln)
        i += 1
    flush()
    return "\n".join(out)


def apply(text, mode=PARAGRAPH):
    """Reflow and VERIFY. Returns (text, report); refuses by reporting, never by raising.

    **The signature check applies to both modes**, which is the point of making
    this one operation: the sentence implementation had no verification and could
    rewrite a file with content silently altered.
    """
    new = reflow(text, mode=mode)
    if not new.endswith("\n"):
        new += "\n"
    same = signature(text) == signature(new)
    return (new if same else text), {
        "mode": mode, "changed": same and new != text, "signature_held": same,
        "note": ("content signature changed -- REFUSED, nothing rewritten. A reflow that "
                 "alters content is not a reflow" if not same else
                 "signature identical; only line breaks moved"),
    }
