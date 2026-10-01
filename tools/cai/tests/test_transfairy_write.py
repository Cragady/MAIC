"""Known-answer tests for trans-fairy-write, with a positive control on each refusal.

Every check below asserts a refusal AND that the refusal was reachable -- the
lesson this repository learned nine times in one day: a negative result is only
as good as evidence the check can return a positive.

Run: python3 tests/test_transfairy_write.py   (exit 0 = pass)
"""
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.transfairywrite import writer as W  # noqa: E402
from cai.transfairywrite import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


def refuses(fn, *a, **k):
    """Returns the error message, or None if it did not refuse."""
    try:
        fn(*a, **k)
        return None
    except Exception as e:
        return str(e)


SID = "aaaaaaaa-1111-2222-3333-444444444444"
GOOD = [{"type": "user", "uuid": "u", "sessionId": SID,
         "message": {"role": "user", "content": "hi"}},
        {"type": "assistant", "uuid": "a", "parentUuid": "u", "sessionId": SID,
         "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}}]

d = tempfile.mkdtemp()
sessions = os.path.join(d, "sessions")
os.makedirs(sessions)
SGLOB = os.path.join(sessions, "*.json")


def write_jsonl(path, recs):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(json.dumps(r) for r in recs) + "\n")
    return path


def age(path, seconds=9999):
    os.utime(path, (time.time() - seconds, time.time() - seconds))
    return path


target = age(write_jsonl(os.path.join(d, SID + ".jsonl"), GOOD))
source = write_jsonl(os.path.join(d, "replacement.jsonl"),
                     GOOD + [{"type": "user", "uuid": "u2", "parentUuid": "a", "sessionId": SID,
                              "message": {"role": "user", "content": "more"}}])

# --- liveness, the central refusal, with a positive control on each signal ---
check("liveness: an aged file with no registry entry is not live",
      not W.liveness(target, sessions_glob=SGLOB))
fresh = write_jsonl(os.path.join(d, "fresh.jsonl"), GOOD)
check("liveness: POSITIVE CONTROL -- a just-written file is detected",
      any("seconds ago" in r for r in W.liveness(fresh, sessions_glob=SGLOB)))
write_jsonl(os.path.join(sessions, "s.json"), [{"sessionId": SID}])
check("liveness: POSITIVE CONTROL -- a registry entry is detected",
      any("listed live" in r for r in W.liveness(target, sessions_glob=SGLOB)))
os.remove(os.path.join(sessions, "s.json"))
check("liveness: removing the registry entry clears it",
      not W.liveness(target, sessions_glob=SGLOB))

# --- refusals ---
b = os.path.join(d, "b1.bak")
check("refuses a target that does not exist, and says it does not create",
      "does not create" in (refuses(W.write, os.path.join(d, "nope.jsonl"), source, b) or ""))

bad_str = os.path.join(d, "bad_str.jsonl")
write_jsonl(bad_str, [GOOD[0], {"type": "assistant", "uuid": "a", "parentUuid": "u",
                                "sessionId": SID,
                                "message": {"role": "assistant", "content": "STRING"}}])
check("refuses a replacement whose assistant content is a string (the client-crashing defect)",
      "assistant content is a list" in (refuses(W.write, target, bad_str, b) or ""))

orphan = os.path.join(d, "orphan.jsonl")
write_jsonl(orphan, [{"type": "user", "uuid": "u", "sessionId": SID,
                      "message": {"role": "user",
                                  "content": [{"type": "tool_result", "tool_use_id": "gone"}]}}])
check("refuses a replacement with an orphan tool_result",
      "orphan tool_result" in (refuses(W.write, target, orphan, b) or ""))

wrong_sid = os.path.join(d, "wrong.jsonl")
write_jsonl(wrong_sid, [dict(r, sessionId="bbbbbbbb-9999-9999-9999-999999999999") for r in GOOD])
check("refuses a replacement whose sessionId disagrees with the target",
      "sessionId mismatch" in (refuses(W.write, target, wrong_sid, b) or ""))

badjson = os.path.join(d, "badjson.jsonl")
open(badjson, "w").write('{"type":"user"}\nNOT JSON\n')
check("refuses malformed JSON, naming the line",
      "line 2" in (refuses(W.write, target, badjson, b) or ""))

# --- the happy path, and what it leaves behind ---
rep = W.write(target, source, b)
check("writes when every check passes", os.path.exists(b) and rep["records"] == 3)
check("the backup holds the ORIGINAL, not the replacement",
      len(W.load(b)) == 2 and len(W.load(target)) == 3)
check("the target now matches the source byte for byte",
      open(target, "rb").read() == open(source, "rb").read())
check("reports the sessionId it wrote for", rep["sessionId"] == SID)

# a write leaves its own target looking fresh, so age it before writing again --
# the tool cannot tell "fresh because I wrote it" from "fresh because a client did"
age(target)
check("refuses to clobber an existing backup",
      "backup already exists" in (refuses(W.write, target, source, b) or ""))
age(target)
check("--force allows it", W.write(target, source, b, force=True)["backup"] == b)
check("a write leaves its target tripping the liveness heuristic (documented, not a bug)",
      any("seconds ago" in r for r in W.liveness(target, sessions_glob=SGLOB)))

# --- live refusal end to end, and the override ---
live_t = write_jsonl(os.path.join(d, "live.jsonl"), GOOD)
b2 = os.path.join(d, "b2.bak")
msg = refuses(W.write, live_t, source, b2) or ""
check("refuses a live target end to end", "looks live" in msg)
check("and explains why the backup does not save you", "raced" in msg)
check("--ignore-live overrides it, and the result says it was overridden",
      W.write(live_t, source, b2, ignore_live=True)["liveness_overridden"] is True)

# --- dry run ---
t3 = age(write_jsonl(os.path.join(d, "t3.jsonl"), GOOD))
b3 = os.path.join(d, "b3.bak")
r3 = W.write(t3, source, b3, dry_run=True)
check("dry run writes nothing at all",
      r3["dry_run"] and not os.path.exists(b3) and len(W.load(t3)) == 2)

# --- verify ---
# an independent pair, so verify does not depend on what earlier writes left behind
tv = age(write_jsonl(os.path.join(d, "tv.jsonl"), GOOD))
bv = os.path.join(d, "bv.bak")
W.write(tv, source, bv)
v = W.verify(tv, bv)
check("verify reports the record delta", v["delta"] == 1 and v["records_before"] == 2)
check("verify confirms the sessionId did not change", v["sessionId_unchanged"] is True)
check("verify passes on a well-formed result", v["ok"] is True)

# --- the agent contract ---
check("the CLI declines --agent outright",
      cli.main([target, "--from", source, "--backup", os.path.join(d, "b9.bak"), "--agent"]) == 2)
check("--man-help works and documents the refusals",
      cli.main(["--man-help"]) == 0 and "WHAT IT REFUSES" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
