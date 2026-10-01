"""Known-answer tests for cai document. Run: python3 tests/test_document.py"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.document import store  # noqa: E402
from cai.document import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


doc = store.load()
check("shapes load", len(store.names(doc)) >= 2)
check("the session-name shape is present", store.get(doc, "session-name") is not None)
check("and it records the field ORDER, which is the arrangement",
      "cwd-name" in store.get(doc, "session-name")["order"])
check("a part points at NOTATION rather than defining a symbol",
      any(p.get("notation") for p in store.get(doc, "session-name")["parts"]))
check("POSITIVE CONTROL -- an unknown shape is reported, not invented",
      store.get(doc, "no-such-shape") is None)

r = store.check(doc, "session-name", {"cwd": "SOPIA", "slice": "abcd1234"})
check("a value with every required part passes", r["ok"] is True)
r2 = store.check(doc, "session-name", {"cwd": "SOPIA"})
check("POSITIVE CONTROL -- a missing required part fails", r2["ok"] is False)
check("and the finding says which part and why",
      "slice" in r2["findings"][0] and "documentation" in r2["findings"][0])
check("an unknown shape reports unknown rather than passing",
      store.check(doc, "nope", {})["known"] is False)

check("the hook-definition shape points at the grant-action notation",
      any("grant-action" in (p.get("notation") or [])
          for p in store.get(doc, "hook-definition")["parts"]))
check("so a grant and a hook use the same word by construction",
      store.get(doc, "hook-definition") is not None
      and store.get(doc, "grant") is not None)
check("the enrollment marker is a recorded shape too",
      store.get(doc, "enrollment-marker") is not None)

check("bare invocation prints the reference", cli.main([]) == 0)
check("list exits 0", cli.main(["list"]) == 0)
check("show on an unknown shape exits 1", cli.main(["show", "nope"]) == 1)
check("the help states the three-way split",
      "IMPLEMENTATION" in cli.MAN_HELP and "notation" in cli.MAN_HELP)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
