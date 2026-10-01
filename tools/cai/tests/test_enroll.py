"""Known-answer tests for cai enroll, with a positive control on each refusal.

Run: python3 tests/test_enroll.py   (exit 0 = pass)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.enroll import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


d = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q"], cwd=d, capture_output=True)

# --- the levels are a notation, not a literal in this tool ---
check("the enrolment levels come from notation",
      sorted(cli.levels()) == ["off", "strong", "weak"])
check("POSITIVE CONTROL -- they resolve through the dictionary, not a local table",
      "enrollment-level" in open(
          os.path.join(os.path.dirname(__file__), "..", "src", "cai", "enroll", "cli.py"),
          encoding="utf-8").read())

# --- there is no default, on purpose ---
check("REFUSES with no level chosen -- enrollment is never implied", cli.main(["--repo", d]) == 2)
check("REFUSES when more than one level is chosen",
      cli.main(["--repo", d, "--weak", "--strong"]) == 2)

# --- status before anything ---
st = cli.status(d)
check("an untouched repository reports not enrolled", st["effective"] == "not enrolled")
check("and no hooks are installed", st["hooks_installed"] == [])

# --- weak installs nothing but is still a recorded decision ---
cli.enroll(d, "weak")
st = cli.status(d)
check("weak records the decision", st["declared"] == "weak")
check("and installs no hooks", st["hooks_installed"] == [])
check("POSITIVE CONTROL -- the marker file exists and is readable",
      os.path.exists(os.path.join(d, ".cai-enrollment")))

# --- strong installs both hooks ---
cli.enroll(d, "strong", force=True)
st = cli.status(d)
check("strong installs pre-commit and pre-push",
      sorted(st["hooks_installed"]) == ["pre-commit", "pre-push"])
check("and the effective level is strong", st["effective"] == "strong")
_hook_text = open(os.path.join(d, ".git", "hooks", "pre-commit"), encoding="utf-8").read()
check("the hook is a SHIM that delegates, rather than carrying the logic",
      "cai hook pre-commit" in _hook_text)
check("and it carries no check of its own, so improving cai improves it",
      "grant check" not in _hook_text and len(_hook_text.splitlines()) < 8)
check("and they are executable", os.access(os.path.join(d, ".git", "hooks", "pre-commit"), os.X_OK))

# --- off removes them, and is itself recorded ---
res = cli.enroll(d, "off")
st = cli.status(d)
check("off removes the hooks it installed", st["hooks_installed"] == [] and res["hooks_removed"])
check("and off is a RECORDED decision, not the absence of one", st["declared"] == "off")
check("which is how refusal is distinguishable from neglect",
      st["declared"] == "off" and st["effective"] == "off")

# --- strong is pre-flighted, because it could otherwise brick the repository ---
pf = cli.enroll(d, "strong")
check("REFUSES strong when the hooks could not succeed", pf.get("refused") is True)
check("and names what would have to be true first",
      any("no grant record" in p for p in pf["problems"]))
check("it calls it an impossible ask rather than a strict one",
      "impossible ask" in pf["note"])
check("and says the force flag is the operator's, never an agent's",
      "OPERATOR'S, never" in pf["note"])
check("POSITIVE CONTROL -- with --force the same call installs",
      cli.enroll(d, "strong", force=True).get("refused") is None)
cli.enroll(d, "off")

# --- no level re-applies the decision on record ---
cli.enroll(d, "strong", force=True)
os.remove(os.path.join(d, ".git", "hooks", "pre-commit"))
check("POSITIVE CONTROL -- a hook removed by hand is gone",
      cli.status(d)["hooks_installed"] == ["pre-push"])
cli.main(["--repo", d, "--force"])
check("re-running with no level restores the recorded decision",
      sorted(cli.status(d)["hooks_installed"]) == ["pre-commit", "pre-push"])

cli.enroll(d, "off")
cli.main(["--repo", d])
check("and re-applying `off` keeps it off rather than upgrading it",
      cli.status(d)["declared"] == "off" and cli.status(d)["hooks_installed"] == [])

fresh = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q"], cwd=fresh, capture_output=True)
check("REFUSES with no level and no decision on record -- there is still no default",
      cli.main(["--repo", fresh]) == 2)
shutil.rmtree(fresh, ignore_errors=True)

# --- dry run ---
r = cli.enroll(d, "strong", dry_run=True, force=True)
check("a dry run writes nothing", r["dry_run"] and cli.status(d)["hooks_installed"] == [])

# --- the help is an ASK, not a decision ---
check("bare invocation prints the reference", cli.main([]) == 0)
check("the help tells an agent to ask rather than choose",
      "Do not choose. Ask" in cli.MAN_HELP)
check("and states that the escape hatch is never the agent's",
      "AN AGENT MAY NEVER REACH FOR IT" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
# --- one step: enroll notices when /init has not been run ----------------------
# Owner: if cai enroll can detect that /init has not been run here, it asks
# whether the operator wants to do so now. It cannot run /init -- that is a client
# command -- so it notices and asks, the same contract as the level itself.
_ci = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q", _ci], check=True)
check("a repo with no CLAUDE.md is detected", cli.has_claude_md(_ci) is False)
open(os.path.join(_ci, "CLAUDE.md"), "w").write("# rules\n")
check("POSITIVE CONTROL -- and one with it is too", cli.has_claude_md(_ci) is True)
check("status carries it", cli.status(_ci)["claude_md"] is True)
os.remove(os.path.join(_ci, "CLAUDE.md"))
check("and flips back when it goes", cli.status(_ci)["claude_md"] is False)

check("the second ask names what enrolling would do without one",
      "enforced but not findable" in cli.SECOND_ASK)
check("it says cai cannot run /init", "cai cannot run `/init`" in cli.SECOND_ASK)
check("and that enrolment is not blocked by the answer",
      "not blocked by it" in cli.SECOND_ASK)
check("POSITIVE CONTROL -- enrolling still works with no CLAUDE.md",
      cli.enroll(_ci, "weak")["enrollment"] == "weak")
check("and the gates come in regardless of the missing document",
      cli.gates_in_force(_ci) is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
