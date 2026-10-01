"""Known-answer tests for `cai read` -- a projection in a tool that cannot write.

Run: python3 tests/test_read.py
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
os.environ.pop("CAI_SESSION_ID", None)
from cai.read import reader, cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


RECS = [
    {"type": "user", "message": {"role": "user", "content": "the ruling stands"}},
    {"type": "assistant", "message": {"role": "assistant",
                                      "content": [{"type": "thinking", "thinking": "hmm"},
                                                  {"type": "text", "text": "understood"}]}},
    {"type": "assistant", "message": {"role": "assistant",
                                      "content": [{"type": "tool_use", "name": "Bash",
                                                   "input": {"command": "ls"}}]}},
    {"type": "summary", "summary": "meta"},
]
DATA = "\n".join(json.dumps(r) for r in RECS) + "\n"
d = tempfile.mkdtemp()
T = os.path.join(d, "t.jsonl")
open(T, "w", encoding="utf-8").write(DATA)
BEFORE = open(T, encoding="utf-8").read()

# --- it projects -------------------------------------------------------------
text, rep = reader.read(DATA)
check("it projects a transcript to what was said",
      "the ruling stands" in text and "understood" in text)
check("thinking is never emitted", "hmm" not in text)
check("the messages survive", rep["signature_held"] is True)
check("and the representation shrinks", rep["bytes_out"] < rep["bytes_in"])

# --- it CANNOT write over what it reads --------------------------------------
check("REGRESSION -- --out refuses the input path",
      cli.main([T, "--out", T]) == 2)
check("and the input is byte-identical", open(T, encoding="utf-8").read() == BEFORE)

existing = os.path.join(d, "taken.txt")
open(existing, "w", encoding="utf-8").write("mine\n")
check("REGRESSION -- --out refuses a path that exists",
      cli.main([T, "--out", existing]) == 2)
check("and does not touch it", open(existing, encoding="utf-8").read() == "mine\n")

fresh = os.path.join(d, "fresh.txt")
check("POSITIVE CONTROL -- a NEW path is written", cli.main([T, "--out", fresh]) == 0)
check("with the conversation in it", "the ruling stands" in open(fresh, encoding="utf-8").read())
check("and the source STILL untouched", open(T, encoding="utf-8").read() == BEFORE)

check("there is no in-place mode to reach for",
      not any("in-place" in str(a) for a in ["--out", "--select", "--since-compaction"]))
check("the module exposes no writer at all",
      not hasattr(reader, "write"))

# --- the damaged-file behaviours, reported by peer session RE: Flow -----------
JUNK = '200\n"a string"\ntrue\n{"type":"user","message":{"role":"user","content":"hi"}}\n'
check("REGRESSION -- a record must be an OBJECT; bare scalars are dropped",
      reader.records(JUNK) == [{"type": "user", "message": {"role": "user", "content": "hi"}}])
_raised = False
try:
    reader.read(JUNK)
except AttributeError:
    _raised = True
check("so reading a damaged file does not raise", _raised is False)
check("POSITIVE CONTROL -- the real record still comes through",
      reader.records(JUNK)[0]["type"] == "user")

_, wreck = reader.read('200\ntrue\n"x"\n')
check("a file with no records is refused rather than projected to nothing",
      wreck["records_in"] == 0 and wreck["signature_held"] is False)
check("and it says the file is not a transcript any more", "not one now" in wreck["note"])
W = os.path.join(d, "wreck.jsonl")
open(W, "w", encoding="utf-8").write('200\ntrue\n"x"\n')
check("the CLI exits 1 on it, rather than writing an empty result",
      cli.main([W, "--out", os.path.join(d, "empty.txt")]) == 1)
check("and wrote nothing", not os.path.exists(os.path.join(d, "empty.txt")))

# --- selections ---------------------------------------------------------------
tools_text, _ = reader.read(DATA, select="tools")
check("POSITIVE CONTROL -- the tools selection carries tool traffic",
      "tool_use" in tools_text)
check("and still never thinking", "hmm" not in tools_text)

check("bare invocation prints the reference", cli.main([]) == 0)
check("the help says it cannot write over anything",
      "IT CANNOT WRITE OVER ANYTHING" in cli.MAN_HELP)
check("and names what a projection costs",
      "Never resumable" in cli.MAN_HELP)

# --- the two halves of a compaction, reported by peer session RE: Flow ---------
# A compaction replaces everything BEFORE its boundary with a summary. Only the
# after-half was selectable, and the help called it "what the last compaction
# dropped" -- backwards. Anyone recovering lost material was pointed at the half
# they still had.
_C = [
    {"type": "user", "message": {"role": "user", "content": "old one"}},
    {"type": "user", "message": {"role": "user", "content": "old two"}},
    {"type": "user", "isCompactSummary": True, "message": {"role": "user", "content": "SUMMARY"}},
    {"type": "user", "message": {"role": "user", "content": "new one"}},
]
_CD = "\n".join(json.dumps(r) for r in _C) + "\n"

_after, _ = reader.read(_CD, since_compaction=True)
check("--since-compaction gives the boundary onward -- what you already hold",
      "SUMMARY" in _after and "new one" in _after and "old one" not in _after)

_before, _ = reader.read(_CD, before_compaction=True)
check("--before-compaction gives what the summary REPLACED",
      "old one" in _before and "old two" in _before)
check("and excludes the summary and everything after it",
      "SUMMARY" not in _before and "new one" not in _before)

_recs = reader.records(_CD)
_b = reader.slice_bounds(_recs, before_compaction=True)
_a = reader.slice_bounds(_recs, since_compaction=True)
check("the halves are DISJOINT", _b[1] <= _a[0])
check("and together cover every record",
      _b[0] == 0 and _a[1] == len(_recs) and _b[1] == _a[0])

_whole, _ = reader.read(_CD)
check("POSITIVE CONTROL -- neither flag gives the whole file",
      all(t in _whole for t in ("old one", "SUMMARY", "new one")))

_nb = reader.records("\n".join(json.dumps(r) for r in _C[:2]) + "\n")
check("with NO boundary, before-compaction is the whole file rather than nothing",
      reader.slice_bounds(_nb, before_compaction=True) == (0, 2))

check("REGRESSION -- the help no longer calls the after-half 'what was dropped'",
      "only what the last compaction dropped" not in cli.MAN_HELP)
# Flattened and case-insensitive: the phrase spans a line break in the help and
# is written in prose case there, while the flag help shouts it.
_fh = " ".join(cli.MAN_HELP.split()).lower()
check("and names which half is for recovery", "this is the recovery slice" in _fh)
check("POSITIVE CONTROL -- the flattened match can fail",
      "this is the banana slice" not in _fh)
check("asking for both halves at once is a usage error",
      cli.main([T, "--since-compaction", "--before-compaction"]) == 2)

# --- regions between boundaries, and markers that still point at the source ----
# A transcript can carry several boundaries; six on this machine do. Both flags
# anchored to the LAST, so middle regions were unreachable and --before-compaction
# lumped live conversation together with earlier summaries.
_MB = [
    {"type": "user", "message": {"role": "user", "content": "a0"}},
    {"type": "user", "isCompactSummary": True, "message": {"role": "user", "content": "S1"}},
    {"type": "user", "message": {"role": "user", "content": "a1"}},
    {"type": "user", "message": {"role": "user", "content": "b1"}},
    {"type": "user", "isCompactSummary": True, "message": {"role": "user", "content": "S2"}},
    {"type": "user", "message": {"role": "user", "content": "a2"}},
]
_MD = "\n".join(json.dumps(r) for r in _MB) + "\n"
_mr = reader.records(_MD)

check("boundaries are found", reader.boundaries(_mr) == [1, 4])
check("B boundaries give B+1 regions",
      [k for k, _, _ in reader.regions(_mr)] == [0, 1, 2])
check("and the regions cover every record with no overlap",
      [(a, b) for _, a, b in reader.regions(_mr)] == [(0, 1), (1, 4), (4, 6)])

_t1, _ = reader.read(_MD, chunk=1)
check("a middle region can be selected at last",
      "a1" in _t1 and "b1" in _t1 and "S1" in _t1)
check("and excludes what is outside it", "a0" not in _t1 and "a2" not in _t1)

check("chunk -1 is the same span as --since-compaction",
      reader.slice_bounds(_mr, chunk=-1) == reader.slice_bounds(_mr, since_compaction=True))
check("chunk -2 is what the LAST compaction replaced, alone",
      reader.slice_bounds(_mr, chunk=-2) == (1, 4))
_bt, _ = reader.read(_MD, before_compaction=True)
check("POSITIVE CONTROL -- --before-compaction lumps regions 0 and 1 together",
      "a0" in _bt and "a1" in _bt)

# REGRESSION: a pre-sliced projection renumbered from 1, so every citation into
# it pointed at the wrong line. The Ln markers align with source line numbers,
# and at least one peer session anchors quotes on that.
check("REGRESSION -- markers in a chunk point at SOURCE lines, not 1",
      "── L2 ·" in _t1 and "── L1 ·" not in _t1)
check("POSITIVE CONTROL -- the whole file starts at L1",
      "── L1 ·" in reader.read(_MD)[0])

try:
    reader.read(_MD, chunk=99)
    _oob = False
except ValueError as e:
    _oob = "does not exist" in str(e) and "numbered 0..2" in str(e)
check("an out-of-range chunk is refused, and says what the range IS", _oob)
check("two span selectors are a usage error",
      cli.main([T, "--chunk", "1", "--before-compaction"]) == 2)
check("--boundaries lists them without slicing anything",
      cli.main([T, "--boundaries"]) == 0)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
