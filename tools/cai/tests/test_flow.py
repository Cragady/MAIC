"""Known-answer tests for cai flow -- order, checks, and pointers.

Run: python3 tests/test_flow.py
"""
import copy
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

# **Pin the tier.** These checks assert on the CONTENT of the flow data, and that
# data resolves through a priority chain -- remote, then working tree, then a
# snapshot. Unpinned, the suite silently tests whichever tier happened to answer:
# it failed once because the remote served the PUBLISHED flows while the pointers
# under test were still uncommitted locally. The check was right and the data was
# a different vintage, which reads exactly like a code defect.
os.environ["CAI_NO_REMOTE"] = "1"

from cai.flow import store, cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


doc = store.load()
check("flows load", set(store.names(doc)) >= {"re-root", "commit", "sync", "enroll"})
check("a flow is an ORDER, not a set", len(store.get(doc, "re-root")["steps"]) == 7)
check("each step names the tool that performs it, where a tool does",
      store.tools(doc, "commit") and all(t.startswith("cai ") for t in store.tools(doc, "commit")))
check("and steps may have no tool, because some are a person's",
      any(s.get("tool") is None for s in store.get(doc, "re-root")["steps"]))
check("every step carries WHY it sits where it does",
      all(s.get("why") for s in store.get(doc, "re-root")["steps"]))
check("and what must hold before the next one",
      all(s.get("check") for s in store.get(doc, "re-root")["steps"]))

nxt = store.check(doc, "re-root")
check("next reports the first step when nothing is done",
      nxt["next"]["do"] == "choose the window" and nxt["done"] == 0)
nxt2 = store.check(doc, "re-root", done=["choose the window"])
check("and advances as steps are named done", nxt2["done"] == 1)
check("it says plainly that it cannot verify a step was performed",
      "no witness to the work" in nxt["note"])
check("POSITIVE CONTROL -- an unknown flow is reported, not invented",
      store.check(doc, "nope")["known"] is False)

p = store.pointers(doc)
check("steps point at documents", len(p["pointers"]) > 10)
check("and every pointer resolves", p["broken"] == [])

_d2 = copy.deepcopy({k: v for k, v in doc.items() if not k.startswith("_")})
_d2["flows"]["re-root"]["steps"][0]["see"] = ["docs/does-not-exist.md"]
check("POSITIVE CONTROL -- a broken pointer IS detected",
      len(store.pointers(_d2, "re-root")["broken"]) == 1)
check("and the note says why a dangling pointer is worse than none",
      "worse than no pointer" in store.pointers(_d2)["note"]
      or "reads as though" in store.pointers(_d2)["note"])

check("bare invocation prints the reference", cli.main([]) == 0)
check("list exits 0", cli.main(["list"]) == 0)
check("show on an unknown flow exits 1", cli.main(["show", "nope"]) == 1)
check("the help states it describes and does not run",
      "IT DOES NOT RUN" in cli.MAN_HELP)
check("and why: a flow that ran itself answers a destructive prompt in instalments",
      "in instalments" in cli.MAN_HELP)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
