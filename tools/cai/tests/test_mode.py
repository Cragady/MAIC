"""Known-answer tests for cai.mode -- one three-way branch, not twelve.

Run: python3 tests/test_mode.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai import mode  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


check("silent records meta and says nothing to the agent",
      mode.emits_meta("silent") and not mode.emits_message("silent"))
check("loud does both", mode.emits_meta("loud") and mode.emits_message("loud"))
check("true-silent does neither",
      not mode.emits_meta("true-silent") and not mode.emits_message("true-silent"))
check("and only true-silent breaks the contract",
      mode.breaks_contract("true-silent")
      and not mode.breaks_contract("silent")
      and not mode.breaks_contract("loud"))

check("the expected record count is derived, not written out per tool",
      (mode.expected_records("silent"), mode.expected_records("loud"),
       mode.expected_records("true-silent")) == (1, 2, 0))
check("and it scales when an operation emits more than one of a kind",
      mode.expected_records("loud", meta_records=2, message_records=3) == 5)

try:
    mode.check("quiet")
    bad = False
except ValueError as e:
    bad = "must be one of" in str(e)
check("POSITIVE CONTROL -- an unknown mode raises, with one message not four", bad)

check("the vocabulary comes from notation where reachable",
      set(mode.known()) == {"silent", "loud", "true-silent"})
check("the warning names what true-silent discards",
      "nothing an audit can find" in mode.WARNING)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
