"""Known-answer tests for cai hook -- the behaviour an installed shim delegates to.

Run: python3 tests/test_hook.py   (exit 0 = pass)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.hook import cli  # noqa: E402

# **A test that seeds a session must own the environment.** CAI_SESSION_ID
# overrides cwd-based resolution, so an exported one makes the process claim the
# REAL session and the seeded grant stops matching. Found by this suite passing
# alone and failing in a loop that exported it.
os.environ.pop("CAI_SESSION_ID", None)

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


d = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q"], cwd=d, capture_output=True)

check("REFUSES when no grant covers the repository", cli.main(["pre-commit", "--repo", d]) == 1)
check("and pre-push refuses independently", cli.main(["pre-push", "--repo", d]) == 1)

sid = "aaaaaaaa-1111-2222-3333-444444444444"
gp = os.path.join(d, "grants.json")
with open(gp, "w", encoding="utf-8") as fh:
    json.dump({"grants": [{"repo": os.path.abspath(d), "sessionId": sid,
                           "sessionName": "s", "granted": "2020-01-01T00:00:00Z",
                           "action": "commit"}]}, fh)
os.environ["CAI_GRANTS"] = gp

from cai.grant import store as gstore  # noqa: E402
gstore.SESSIONS_GLOB = os.path.join(d, "sessions", "*.json")
os.makedirs(os.path.join(d, "sessions"), exist_ok=True)
with open(os.path.join(d, "sessions", "s.json"), "w", encoding="utf-8") as fh:
    json.dump({"name": "s", "sessionId": sid, "cwd": os.path.abspath(d)}, fh)

check("POSITIVE CONTROL -- allows the commit once a grant covers it",
      cli.main(["pre-commit", "--repo", d]) == 0)
check("but still refuses the push, which is a separate permission",
      cli.main(["pre-push", "--repo", d]) == 1)

os.environ["CAI_GRANTS"] = os.path.join(d, "does-not-exist.json")
check("fails closed when the grant source is missing",
      cli.main(["pre-commit", "--repo", d]) == 1)
del os.environ["CAI_GRANTS"]

check("bare invocation prints the reference", cli.main([]) == 0)
check("the help says an agent must report and stop",
      "MUST REPORT THE REFUSAL AND STOP" in cli.MAN_HELP)
check("and that the bypass is the operator's", "OPERATOR'S" in cli.MAN_HELP)
check("it names why a pre-commit hook cannot check a subject",
      "cannot check a subject" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
