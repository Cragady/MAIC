"""Known-answer tests for cai commit, with a positive control on every refusal.

Each refusal below reproduces a failure that actually happened on 2026-09-01 and
was found only afterwards, and each is paired with the same call succeeding once
the single blocking condition is removed.

Run: python3 tests/test_commit.py   (exit 0 = pass)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.commit import gate  # noqa: E402
from cai.commit import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


d = tempfile.mkdtemp()


def git(*a):
    subprocess.run(["git"] + list(a), cwd=d, capture_output=True)


git("init", "-q")
git("config", "user.email", "t@t")
git("config", "user.name", "t")
open(os.path.join(d, "seed.txt"), "w").write("seed\n")
git("add", "-A")
git("commit", "-qm", "seed")


def stage(name, body="x\n"):
    open(os.path.join(d, name), "w").write(body)
    git("add", "-A")


def levels(findings):
    return [f[0] for f in findings]


# --- an empty diff, which is what a silently failed edit looks like ---
ok, f = gate.check(d, "docs: something", require_grant=False)
check("REFUSES an empty staged diff", ok is False)
check("and names it as the shape of an edit that failed", "silently failed" in f[0][1])

stage("docs.txt")
ok, f = gate.check(d, "docs: something", require_grant=False)
check("POSITIVE CONTROL -- the same call passes once something is staged", ok is True)

# --- a failing verification ---
ok, f = gate.check(d, "docs: something", verify="exit 3", require_grant=False)
check("REFUSES when the verification command fails", ok is False)
check("and reports the exit code and the command", "exited 3" in f[0][1])
ok, f = gate.check(d, "docs: something", verify="exit 0", require_grant=False)
check("POSITIVE CONTROL -- a passing verification does not block", ok is True)

# --- a subject naming work the diff does not contain ---
ok, f = gate.check(d, "CLAUDE.md: the heading defers to cai grant", require_grant=False)
check("WARNS when the subject names something absent from the change",
      ok is True and "warn" in levels(f))
check("and it warns rather than refusing, since a scope may name a component",
      not any(k == "refuse" for k, _ in f))
ok2, f2 = gate.check(d, "docs: something", require_grant=False)
check("POSITIVE CONTROL -- a matching scope produces no warning", "warn" not in levels(f2))

# --- the grant, failing closed ---
ok, f = gate.check(d, "docs: something", require_grant=True, session_id="nobody")
check("REFUSES when no grant covers this repository and session", ok is False)

# --- it actually commits when everything holds ---
rep = gate.commit(d, "docs: a real commit", verify="exit 0", require_grant=False)
check("commits when every precondition holds", rep["ok"] and rep.get("committed") is True)
check("and reports the resulting commit", "docs: a real commit" in rep.get("commit", ""))
check("POSITIVE CONTROL -- the tree is now clean, so a second call refuses",
      gate.commit(d, "docs: again", require_grant=False)["ok"] is False)

# --- dry run ---
stage("another.txt")
rep = gate.commit(d, "docs: not yet", dry_run=True, require_grant=False)
check("a dry run reports without committing",
      rep["dry_run"] and "commit" not in rep)

# --- pushing is a separate permission ---
pr = gate.push(d, require_grant=True, session_id="nobody")
check("REFUSES a push when no grant covers the push action", pr["ok"] is False)
check("and says it was the PUSH action that was denied",
      "denied for push" in pr["findings"][0]["detail"])
check("POSITIVE CONTROL -- an ungated push is not blocked by the grant check",
      gate.push(d, require_grant=False, dry_run=True)["ok"] is True)

# --- the CLI ---
check("CLI exits 1 on refusal", cli.main(["--repo", d, "-m", "docs: x", "--verify", "exit 9", "--no-grant"]) == 1)
check("POSITIVE CONTROL -- CLI exits 0 when it commits",
      cli.main(["--repo", d, "-m", "docs: cli commit", "--no-grant"]) == 0)
check("bare invocation prints the reference", cli.main([]) == 0)
check("the help says why it is a tool rather than a habit",
      "WHY IT IS A TOOL" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
# --- INCIDENT 2026-09-03: the gate did not check the grant unless asked ---------
# require_grant defaulted to FALSE, the CLI passed an opt-in flag nobody used, and
# a full session of commits went ungated while the tool reported success. Every
# check the gate ran was correct; the grant was not among them.
_g = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q", _g], check=True)
subprocess.run(["git", "-C", _g, "config", "user.email", "a@b"], check=True)
subprocess.run(["git", "-C", _g, "config", "user.name", "t"], check=True)
open(os.path.join(_g, "f.md"), "w").write("x\n")
subprocess.run(["git", "-C", _g, "add", "-A"], check=True)

_ok, _f = gate.check(_g, "docs: x")
check("REGRESSION -- a bare gate.check now REQUIRES a grant",
      _ok is False and any("grant" in d for _, d in _f))
check("POSITIVE CONTROL -- the same call passes with require_grant=False",
      gate.check(_g, "docs: x", require_grant=False)[0] is True)
check("REGRESSION -- a bare CLI commit refuses in a repo with no grant",
      cli.main(["--repo", _g, "-m", "docs: x"]) == 1)
check("and nothing was committed",
      subprocess.run(["git", "-C", _g, "log", "--oneline"], capture_output=True,
                     text=True).stdout.strip() == "")
check("POSITIVE CONTROL -- --no-grant is the operator's bypass and it works",
      cli.main(["--repo", _g, "-m", "docs: x", "--no-grant"]) == 0)
check("--grant survives as a no-op so hooks do not break",
      cli.main(["--repo", _g, "-m", "docs: y", "--grant", "--no-grant"]) in (0, 1))
check("the help marks the bypass OPERATOR ONLY",
      "OPERATOR ONLY" in " ".join(str(a.help) for a in [])
      or "OPERATOR ONLY" in open("src/cai/commit/cli.py", encoding="utf-8").read())
check("and says an agent reports a refusal rather than reaching for it",
      "reports it and stops" in open("src/cai/commit/gate.py", encoding="utf-8").read())

# --- enrollment brings the gates in, in two steps -----------------------------
# Owner: enrollment should bring the gates regardless of strength, as two steps --
# whether they apply, and how they are enforced. And the base is ON: more open
# repos get the protections for the price of us following them. Only a recorded
# `off` turns them off.
from cai.enroll import cli as ecli  # noqa: E402


def _repo():
    r = tempfile.mkdtemp()
    subprocess.run(["git", "init", "-q", r], check=True)
    subprocess.run(["git", "-C", r, "config", "user.email", "a@b"], check=True)
    subprocess.run(["git", "-C", r, "config", "user.name", "t"], check=True)
    open(os.path.join(r, "f.md"), "w").write("x\n")
    subprocess.run(["git", "-C", r, "add", "-A"], check=True)
    return r


_never = _repo()
check("a repo never asked is GATED -- the protections come with the toolset",
      gate._gates_in_force(_never) is True)
check("and the CLI refuses there", cli.main(["--repo", _never, "-m", "f.md: x"]) == 1)

_off = _repo()
ecli.enroll(_off, "off")
check("an explicit `off` is the ONE thing that turns them off",
      gate._gates_in_force(_off) is False)
check("REGRESSION -- and the CLI honours it, which needs require_grant=None to "
      "actually reach the gate", cli.main(["--repo", _off, "-m", "f.md: x"]) == 0)

_weak = _repo()
ecli.enroll(_weak, "weak")
check("weak enrolment gates -- strength decides ENFORCEMENT, not scope",
      gate._gates_in_force(_weak) is True)
_strong_ok = gate._gates_in_force(_weak)
ecli.enroll(_weak, "off")
check("POSITIVE CONTROL -- the same repo flips when the decision changes",
      gate._gates_in_force(_weak) is False and _strong_ok is True)

check("an unreadable declaration FAILS CLOSED",
      gate._gates_in_force("/nonexistent/path/at/all") is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
