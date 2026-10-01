"""Known-answer fixture, plus a negative control per case.

The second assertion is the one doing the work: it confirms a naive reader --
one that handles only the common shape -- returns the WRONG answer. Without it,
a regression that reintroduces shape-blindness passes silently.
"""
import io, json, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.grammar import records  # noqa: E402


def _try(l):
    try:
        json.loads(l)
        return True
    except ValueError:
        return False

FX = os.path.join(os.path.dirname(__file__), "defaults-frozen", "shapes.jsonl")
recs = [json.loads(l) for l in io.open(FX, encoding="utf-8") if l.strip()]

fails = []


def check(name, got, want, note=""):
    if got != want:
        fails.append(f"{name}: got {got!r} want {want!r}")
        return False
    if note:
        print(f"PASS  {name}\n        ({note})")
    else:
        print(f"PASS  {name}")
    return True


def naive(rec):
    """What a shape-blind reader sees: only string content, only .text."""
    c = (rec.get("message") or {}).get("content")
    return c if isinstance(c, str) else ""


check("line count equals record count", len(recs), 5)
check("every shape yields its text",
      [records.text_of(r) for r in recs],
      ["bare string content", "list block content", "result as string",
       "result as object", "assistant text\nhidden reasoning"])
check("NEGATIVE CONTROL: a naive reader misses four of five",
      sum(1 for r in recs if not naive(r)), 4,
      "asserts the hazard is real, so the guard cannot be quietly removed")
check("tool_result object shape is not skipped",
      records.text_of(recs[3]), "result as object",
      "this is case 15 in SOPIA tools/CASES.md")

# --- loading a record IS grammar, and was the part left outside ---------------
# Five loaders across the suite gave three different answers to what a record is:
# dict-only in one, any JSON type in three, and one that raised on a bad line.
_N = [0]


def ck(name, ok, note=""):
    _N[0] += 1
    if not ok:
        fails.append(name + (" -- " + note if note else ""))


_JUNK = '200\n"a string"\ntrue\n{"type":"user"}\n\nnot json at all\n'

ck("a bare scalar is not a record", records.load(_JUNK) == [{"type": "user"}])
ck("NEGATIVE CONTROL: a naive loader keeps FOUR things, three of them not records",
   len([json.loads(l) for l in _JUNK.splitlines() if l.strip() and _try(l)]) == 4,
   "asserts the hazard is real, so the dict-only rule cannot be quietly removed")
ck("an unparseable line is skipped, not fatal", len(records.load(_JUNK)) == 1)

_raised = None
try:
    records.load(_JUNK, strict=True, path="/x/y.jsonl")
except ValueError as e:
    _raised = str(e)
ck("strict raises instead", _raised is not None)
ck("and names the FILE and the LINE, which the old error did not",
   "/x/y.jsonl" in (_raised or "") and "line 1" in (_raised or ""),
   "the bare JSONDecodeError said 'line 1 column 1' -- a position INSIDE the bad line")

_v = records.string_values(['{"a":"keep me","b":{"c":["deep"]},"n":3}'])
ck("string_values reaches nested values", "keep me" in _v and "deep" in _v)
ck("and does not invent values from numbers", "3" not in _v.split("\n"))
ck("an unparseable line is scanned as TEXT, since it can still carry a secret",
   "raw secret" in records.string_values(["this is a raw secret"]))

# The distinction that caused the worst defect: a candidate can exist in the
# SERIALIZED form and in no value at all.
_line = json.dumps({"message": {"content": "\n" + "A" * 60}})
ck("NEGATIVE CONTROL: the escape letter is in the serialized text",
   "\\n" + "A" * 8 in _line)
ck("but never in the string values -- which is why derivation must use values",
   "n" + "A" * 60 not in records.string_values([_line]))

for f in fails:
    print("FAIL ", f)
print(f"\n{4 + _N[0] - len(fails)}/{4 + _N[0]} checks passed")
sys.exit(1 if fails else 0)
