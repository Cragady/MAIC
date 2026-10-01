"""Known-answer tests for cai fabricate, the inverse of redact.

Run: python3 tests/test_fabricate.py
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.fabricate import maker  # noqa: E402
from cai.fabricate import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


BASE = [
    {"type": "user", "uuid": "u", "sessionId": "s", "cwd": "/x",
     "message": {"role": "user", "content": "hi"}},
    {"type": "assistant", "uuid": "a", "parentUuid": "u", "sessionId": "s", "cwd": "/x",
     "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}},
]
TS = "2026-09-02T06:00:00Z"

out, made = maker.insert(BASE, [("user", "inserted")], 0, "/x", TS)
check("inserts a turn", len(out) == 3 and len(made) == 1)
check("marks it by default", made[0]["fabricated"] is True
      and made[0]["origin"]["kind"] == "fabricated")
check("the marking is META, so it does not reach the model",
      "fabricated" not in json.dumps(made[0]["message"]))
check("what followed is re-parented, so the chain is whole",
      out[2]["parentUuid"] == made[0]["uuid"])
check("the source transcript is untouched", len(BASE) == 2)

lo, lomade = maker.insert(BASE, [("user", "inserted")], 0, "/x", TS, mode="loud")
check("loud announces itself in message content, which is the half the model reads",
      "FABRICATED" in lomade[0]["message"]["content"])
check("and is still marked in meta", lomade[0]["fabricated"] is True)

ts_out, ts_made = maker.insert(BASE, [("user", "inserted")], 0, "/x", TS,
                               mode="true-silent")
check("true-silent leaves NO marker of any kind",
      "fabricated" not in ts_made[0] and "origin" not in ts_made[0])
check("POSITIVE CONTROL -- and the audit therefore cannot find it",
      maker.audit(ts_out)["fabricated"] == 0)
check("while the marked one IS found", maker.audit(out)["fabricated"] == 1)
check("the audit says silence is not evidence of authenticity",
      "not evidence of authenticity" in maker.audit(ts_out)["note"])

try:
    maker.insert(BASE, [("user", "x")], 0, "/x", TS, mode="nonsense")
    bad = False
except ValueError:
    bad = True
check("POSITIVE CONTROL -- an unknown mode raises", bad)

try:
    maker.insert(BASE, [("user", "x")], 99, "/x", TS)
    oob = False
except ValueError:
    oob = True
check("refuses an index outside the transcript", oob)

d = tempfile.mkdtemp()
src = os.path.join(d, "t.jsonl")
with open(src, "w", encoding="utf-8") as fh:
    for r in BASE:
        fh.write(json.dumps(r) + "\n")

check("REFUSES without --to, because it never writes in place",
      cli.main([src, "--at", "0", "--user", "x"]) == 2)
check("REFUSES an --agent invocation outright",
      cli.main([src, "--to", os.path.join(d, "o.jsonl"), "--agent"]) == 2)
check("POSITIVE CONTROL -- with --to it writes",
      cli.main([src, "--to", os.path.join(d, "o.jsonl"), "--at", "0", "--user", "x"]) == 0)
check("and the source is byte-unchanged",
      len(open(src, encoding="utf-8").read().splitlines()) == 2)
check("bare invocation prints the reference", cli.main([]) == 0)
check("the help names why it is separate from redact",
      "honest by construction" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
