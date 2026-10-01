"""Known-answer tests for cai.fence -- one definition, composed from two sources.

The module exists because two copies of this concept had already diverged: one
captured the closer marker and one did not. Run: python3 tests/test_fence.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai import fence  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


m = fence.match("---- Source ----")
check("matches an opener", m and m["name"] == "Source" and m["closer"] is False)
c = fence.match("---- /Source ----")
check("and distinguishes a CLOSER, which the divergent copy could not",
      c and c["name"] == "Source" and c["closer"] is True)
check("tolerates surrounding whitespace", fence.match("  ---- Raw Source ----  ") is not None)

check("POSITIVE CONTROL -- the same characters inside a sentence are NOT a fence",
      fence.match("I mention ---- Source ---- in a sentence") is None)
check("nor is an unrelated line", fence.match("just text") is None)

txt = ("---- Raw Source ----\nbody\n---- /Raw Source ----\n"
       "discussing ---- Raw Source ---- inline\n")
out, n = fence.neutralise(txt)
check("neutralises both the opener and the closer", n == 2)
check("marking the closer as a closer", "neutralised: /Raw Source" in out)
check("POSITIVE CONTROL -- and leaves the inline mention alone",
      "discussing ---- Raw Source ---- inline" in out)

known = fence.names()
check("fence names come from NOTATION, not a local list",
      "source" in known and "raw source" in known)
s = fence.scan(txt + "---- Ghost ----\n", known)
check("scan separates recognised from unrecognised",
      len(s["unrecognised"]) == 1 and s["unrecognised"][0]["name"] == "Ghost")
check("and reports the line number, so a finding is locatable",
      s["unrecognised"][0]["line"] == 5)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
