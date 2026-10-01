"""Known-answer tests for cai reflow -- an open family over data, with a content invariant.

Run: python3 tests/test_reflow.py
"""
import json
import os
import sys
import tempfile
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
import cai.reflow as family  # noqa: E402
from cai.reflow import lines as engine, context, cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


SRC = ("This is one sentence. This is a second sentence in the\n"
       "same paragraph, hard-wrapped. And a third.\n\n"
       "> A quoted line.\n> Still the same quoted paragraph.\n")

# --- lines: the two modes ------------------------------------------------------
sent = engine.reflow(SRC, mode="sentence")
para = engine.reflow(SRC, mode="paragraph")
check("sentence mode puts one sentence per line",
      len([l for l in sent.split("\n") if l and not l.startswith(">")]) == 3)
check("paragraph mode puts one paragraph per line",
      len([l for l in para.split("\n") if l and not l.startswith(">")]) == 1)
check("they are OPPOSITE on the same input", sent != para)
check("an abbreviation is not a sentence boundary",
      "Dr. Smith arrived" in engine.reflow("Dr. Smith arrived. Then left.", mode="sentence"))
check("POSITIVE CONTROL -- a real boundary still splits",
      len(engine.reflow("One. Two.", mode="sentence").split("\n")) == 2)
check("a blockquote continuation keeps its marker rather than being padded",
      all(l.startswith(">") for l in sent.split("\n") if l.strip() and "quoted" in l))
check("POSITIVE CONTROL -- padding it would have made it look like code",
      "\n  Still" not in sent)
check("a bare > is a paragraph break and survives",
      engine.reflow("> one\n>\n> two\n", mode="paragraph").count(">") >= 3)
_code = "text\n\n```\nnot  reflowed   here\n```\n"
check("fenced code is untouched", "not  reflowed   here" in engine.reflow(_code, mode="paragraph"))
_tbl = "| a | b |\n| - | - |\n"
check("tables are untouched",
      engine.reflow(_tbl, mode="paragraph").rstrip("\n") == _tbl.rstrip("\n"))

# --- the invariant: shape may move, content may not ----------------------------
out, rep = engine.apply(SRC, mode="paragraph")
check("apply verifies and reports the signature held", rep["signature_held"] is True)

_orig = engine.reflow
engine.reflow = lambda t, mode="paragraph": _orig(t, mode=mode).replace("third", "")
out2, rep2 = engine.apply(SRC, mode="paragraph")
check("POSITIVE CONTROL -- a reflow that drops content is REFUSED",
      rep2["signature_held"] is False and rep2["changed"] is False)
check("and the original is returned untouched", "third" in out2)
engine.reflow = _orig

# --- modes: unavailable is not rejected ----------------------------------------
try:
    engine.check_mode("nonsense")
    msg = ""
except ValueError as e:
    msg = str(e)
check("an unimplemented, unrecorded mode names what IS implemented",
      "not recorded in notation" in msg and "implemented:" in msg)
check("and does NOT frame it as a refused proposal",
      "proposes it" not in msg and "refused" not in msg)

_m = engine.modes
engine.modes = lambda: ("sentence", "paragraph", "columns")
try:
    engine.check_mode("columns")
    msg2 = ""
except ValueError as e:
    msg2 = str(e)
check("a mode recorded in notation with no handler says WRITE ONE, axis is open",
      "the mode axis is open" in msg2)
engine.modes = _m

# --- the family is OPEN --------------------------------------------------------
check("members are DISCOVERED",
      set(family.discover()) == {"lines", "context"})
check("every member can prove content survived",
      all(hasattr(m, "apply") for m in family.discover().values()))

try:
    family.operation("nowhere")
    raised = None
except family.NotAvailable as e:
    raised = e
check("an absent member raises NotAvailable, a LookupError -- not a ValueError",
      isinstance(raised, LookupError))
check("it says reflow is OPEN rather than that the name was rejected",
      "OPEN family" in str(raised) and "operates on data" in str(raised))
check("it reports what IS available", "lines" in str(raised))
check("and distinguishes recorded-elsewhere from unknown", raised.known is False)

_fake = types.ModuleType("fake")
_fake.apply = lambda data, **k: (data, {"changed": False, "signature_held": True})
family.register("fake", _fake)
check("register() extends the family in-process, with no package and no edit here",
      "fake" in family.discover())
try:
    family.register("bad", types.ModuleType("bad"))
    refused_bad = False
except ValueError as e:
    refused_bad = "not a reflow" in str(e)
check("POSITIVE CONTROL -- a module that cannot prove content survived is refused",
      refused_bad)
del family._REGISTERED["fake"]

# `context` MOVED to `cai read`, 2026-09-02 -- see tests/test_read.py. Filing a
# read inside a family whose CLI writes results back is what destroyed a live
# transcript; the tests moved with it.
check("REGRESSION -- context is a member AGAIN; the name was never the defect",
      "context" in family.discover())
check("and it is the one member that does not transform in place",
      [n for n, m in family.discover().items() if not family.in_place(m)] == ["context"])

# --- CLI -----------------------------------------------------------------------
d = tempfile.mkdtemp()
f = os.path.join(d, "a.md")
open(f, "w", encoding="utf-8").write(SRC)
check("the CLI defaults to `lines`", cli.main([f, "--mode", "paragraph"]) == 0)
check("and the file actually changed",
      len(open(f, encoding="utf-8").read().split("\n")[0]) > 60)
check("a member can be named explicitly", cli.main(["lines", f, "-n"]) == 0)
check("an absent member exits 4, not 2 -- unavailable, not a usage error",
      cli.main(["nowhere", f]) == 4)
check("a flag a member does not accept is a USAGE error, exit 2",
      cli.main(["lines", f, "--neutralise"]) == 2)
check("--members lists what is installed", cli.main(["--members"]) == 0)
check("the help says reflow operates on data", "OPERATING ON DATA" in cli.MAN_HELP)
# Matched on flattened text: the phrase spans a line break in the help, and an
# anchored match would report absent when it is present -- tonight's recurring shape.
_flat = " ".join(cli.MAN_HELP.split())
check("and that an absent member is not a refused proposal",
      "NOT a refused proposal" in _flat)
check("POSITIVE CONTROL -- the flattened match can fail",
      "NOT a refused banana" not in _flat)

# --- `context` is BACK, 2026-09-03, and the name was never the defect ----------
# Owner: the name says the CONTEXT is what gets reflowed. What was wrong is that
# the code operated on the FILE. Removing the name was the over-correction; the
# member returns with the transcript as its SOURCE and the context as its target.
from cai.reflow import context as ctxmod  # noqa: E402

check("`context` is a member again", "context" in family.discover())
check("and it declares that it does NOT transform in place",
      family.in_place(ctxmod) is False)

_tj = os.path.join(d, "t.jsonl")
_rec = ('{"type":"user","uuid":"u1","parentUuid":null,"sessionId":"s","timestamp":"t",'
        '"message":{"role":"user","content":"the ruling stands"}}\n')
open(_tj, "w", encoding="utf-8").write(_rec)
_before = open(_tj, encoding="utf-8").read()

check("REGRESSION -- the command that destroyed a transcript now EMITS instead",
      cli.main(["context", _tj]) == 0)
check("and the source is byte-identical",
      open(_tj, encoding="utf-8").read() == _before)

try:
    ctxmod.write(_tj, "x")
    _mw = False
except ValueError as e:
    _mw = "reflows the CONTEXT, not the transcript" in str(e)
check("the member refuses a write on its own account", _mw)
try:
    family.write(ctxmod, _tj, "x")
    _fw = False
except ValueError:
    _fw = True
check("and the family refuses it too -- both, so either surviving is enough", _fw)

check("POSITIVE CONTROL -- --to keeps a copy somewhere else",
      cli.main(["context", _tj, "--to", os.path.join(d, "keep.txt")]) == 0)
check("and the source is STILL untouched",
      open(_tj, encoding="utf-8").read() == _before)
check("POSITIVE CONTROL -- an in-place member still writes in place",
      family.in_place(engine) is True)

# --- INCIDENT 2026-09-02, reflow's half ----------------------------------------
# A projection filed in this family inherited its write path and destroyed a live
# transcript. The projection moved to `cai read`; what stayed here is the rule
# that made it possible, now inverted.
check("the family defaults to REFUSING an in-place write",
      family.in_place(types.ModuleType("nodecl")) is False)
check("POSITIVE CONTROL -- a member that transforms in place declares it",
      family.in_place(engine) is True)
_bare = types.ModuleType("bare")
_bare.apply = lambda data, **k: (data, {"changed": False, "signature_held": True})
check("a member that says NOTHING is treated as projecting, not as safe",
      family.in_place(_bare) is False)

_pd = os.path.join(d, ".claude", "projects", "-x")
os.makedirs(_pd, exist_ok=True)
_md = os.path.join(_pd, "a.md")
open(_md, "w", encoding="utf-8").write("one. two.\nthree.\n")
check("REGRESSION -- a file under .claude/projects is refused even for `lines`",
      cli.main([_md, "--mode", "paragraph"]) == 1)
check("because a live client appends to it and a write underneath loses that",
      open(_md, encoding="utf-8").read() == "one. two.\nthree.\n")
check("POSITIVE CONTROL -- the same file writes fine with --to",
      cli.main([_md, "--mode", "paragraph", "--to", os.path.join(d, "safe.md")]) == 0)

# --- the destructive reflow, asked for and gated ------------------------------
# Owner: replacing a transcript with its reflowed shape is wanted, and must back
# up first. The primitives come from trans-fairy-write rather than a third copy.
def _mk(name, sid_tail):
    p = os.path.join(d, name)
    open(p, "w", encoding="utf-8").write(json.dumps({
        "type": "user", "uuid": "11111111-1111-1111-1111-111111111111",
        "parentUuid": None, "sessionId": "9f9f9f9f-0000-0000-0000-00000000000%s" % sid_tail,
        "timestamp": "t", "message": {"role": "user", "content": "kept"}}) + "\n")
    os.utime(p, (0, 0))
    return p


_a = _mk("a.jsonl", "1")
check("--replace WITHOUT --backup is a usage error",
      cli.main(["context", _a, "--replace"]) == 2)
check("and nothing happened", "sessionId" in open(_a, encoding="utf-8").read())
check("--backup without --replace is also a usage error",
      cli.main(["context", _a, "--backup", os.path.join(d, "x.bak")]) == 2)

_bak = os.path.join(d, "a.bak")
check("POSITIVE CONTROL -- with both, it replaces",
      cli.main(["context", _a, "--replace", "--backup", _bak]) == 0)
check("the source is now the projection",
      "── L" in open(_a, encoding="utf-8").read())
check("and the BACKUP is the resumable artifact",
      "sessionId" in open(_bak, encoding="utf-8").read())

_b = _mk("b.jsonl", "2")
check("REGRESSION -- an existing backup is refused, never overwritten",
      cli.main(["context", _b, "--replace", "--backup", _bak]) == 1)
check("and the source survives that refusal",
      "sessionId" in open(_b, encoding="utf-8").read())

_c = _mk("c.jsonl", "3")
os.utime(_c, None)
_cbak = os.path.join(d, "c.bak")
check("REGRESSION -- a LIVE transcript is refused even with a good backup path",
      cli.main(["context", _c, "--replace", "--backup", _cbak]) == 1)
check("the source survives", "sessionId" in open(_c, encoding="utf-8").read())
check("and no backup was taken -- refusal comes BEFORE the copy",
      not os.path.exists(_cbak))

# --- two slice selectors is a usage error, reported by RE: Flow ----------------
# `--before-compaction --full` returned the before-half and exited 0, so a caller
# asking for everything got two thirds of it with nothing saying so. I had claimed
# this was already refused; it was refused in `cai read` and never here.
_ct = os.path.join(d, "compact.jsonl")
open(_ct, "w", encoding="utf-8").write("\n".join(json.dumps(r) for r in [
    {"type": "user", "message": {"role": "user", "content": "old"}},
    {"type": "user", "isCompactSummary": True, "message": {"role": "user", "content": "SUM"}},
    {"type": "user", "message": {"role": "user", "content": "new"}},
]) + "\n")

check("REGRESSION -- --before-compaction with --full is a usage error",
      cli.main(["context", "--before-compaction", "--full", "-n", _ct]) == 2)
check("and in the other order too",
      cli.main(["context", "--full", "--before-compaction", "-n", _ct]) == 2)
check("POSITIVE CONTROL -- either alone is fine",
      cli.main(["context", "--full", "-n", _ct]) == 0
      and cli.main(["context", "--before-compaction", "-n", _ct]) == 0)
check("the reference documents the flag it gained",
      "--before-compaction" in cli.MAN_HELP)
check("and says the two cannot be combined",
      "cannot be combined" in " ".join(cli.MAN_HELP.split()))

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
