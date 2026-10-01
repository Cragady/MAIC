"""Behavioral tests for the drift-exposed CLI surface and the invariants.

These exercise cai.transfairy.cli.main(argv) -- the real entry path -- so a
change that breaks the help family, flag resolution, the --agent boundary, or
invariant 8 fails here rather than drifting silently away from DESIGN.md.

Run: python3 tests/test_cli.py   (exit 0 = pass)
"""
import contextlib
import io
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.transfairy import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


def run(argv, env=None):
    """Run main(argv); return (exit_code, stdout, stderr)."""
    old = dict(os.environ)
    if env:
        os.environ.update(env)
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = cli.main(argv)
            except SystemExit as e:
                code = e.code
    finally:
        os.environ.clear()
        os.environ.update(old)
    return code, out.getvalue(), err.getvalue()


def _export(d, uuid="c1"):
    p = os.path.join(d, "export.json")
    io.open(p, "w").write(json.dumps([{"uuid": uuid, "name": "t", "chat_messages": [
        {"uuid": "m1", "parent_message_uuid": "R", "sender": "human",
         "created_at": "2026-08-31T06:00:00.000000Z", "content": [{"type": "text", "text": "hi"}]},
        {"uuid": "m2", "parent_message_uuid": "m1", "sender": "assistant",
         "created_at": "2026-08-31T06:00:01.000000Z", "content": [{"type": "text", "text": "hello"}]}]}]))
    return p


_projdirs = []


def _dirs():
    tc = tempfile.mkdtemp(prefix="tc-")
    wr = tempfile.mkdtemp(prefix="wr-")
    dr = tempfile.mkdtemp(prefix="dr-")
    from cai.transfairy.paths import projects_dir
    _projdirs.append(projects_dir(tc))
    return tc, wr, dr


def _base(tc, wr, dr):
    return ["--target-cwd", tc, "--work-root", wr, "--data-root", dr]


# --- help family ---
code, out, _ = run(["--man-help"])
check("help: --man-help exits 0 and prints the reference", code == 0 and "full reference" in out)
code, out, _ = run(["--mahd"])
check("help: --mahd prints both (most-verbose-wins)", code == 0 and out.count("trans-fairy") >= 2)
code, _, err = run(["--help", "init"])
check("help: help flag + non-help token is a hard drop (exit 2)", code == 2)

# --- --agent / --yes mutual exclusion ---
code, _, err = run(["--agent", "--yes", "init"])
check("agent: --agent and --yes are mutually exclusive (exit 2)", code == 2 and "mutually exclusive" in err)

# --- flag resolution: env TRANSFAIRY_CWD used when --target-cwd absent ---
tc, wr, dr = _dirs()
code, out, _ = run(["--work-root", wr, "--data-root", dr, "init"], env={"TRANSFAIRY_CWD": tc})
check("flags: TRANSFAIRY_CWD resolves the target-cwd", code == 0 and tc in out)

# --- --agent boundary: exit 0, prints PIPELINE text + next command ---
tc, wr, dr = _dirs()
code, out, _ = run(["--agent"] + _base(tc, wr, dr) + ["init"])
check("agent: a completed stage exits 0 (a boundary is success)", code == 0)
check("agent: prints the PIPELINE stage text", "PIPELINE.md: stage 'init'" in out)
check("agent: prints the exact next command", "# next:" in out and "split --export" in out)

# --- --agent failure exits non-zero with a distinct message ---
tc, wr, dr = _dirs()
run(_base(tc, wr, dr) + ["init"])
code, _, err = run(["--agent"] + _base(tc, wr, dr) + ["split"])  # --export missing
check("agent: a failed stage exits non-zero", code != 0)

# --- invariant 8: install never overwrites (create-only) ---
tc, wr, dr = _dirs()
exp = _export(wr)
run(_base(tc, wr, dr) + ["init"])
run(_base(tc, wr, dr) + ["split", "--export", exp, "--target-uuid", "c1"])
run(_base(tc, wr, dr) + ["build"])
code1, out1, _ = run(_base(tc, wr, dr) + ["install"])
code2, _, err2 = run(_base(tc, wr, dr) + ["install"])
installed = json.loads(out1)["installed"]
check("invariant 8: first install succeeds", code1 == 0 and os.path.exists(installed))
check("invariant 8: second install refuses (exit 1, changes nothing)", code2 == 1 and "already installed" in err2 and "never overwrites" in err2)

# --- invariant 5/10: install copies (source survives) ---
staged_exists = os.path.exists(installed.replace(
    os.path.join(".claude", "projects", os.path.basename(os.path.dirname(installed))),
    ""))  # loose; real check below
sid = os.path.basename(installed)[:-6]
staged = os.path.join(wr, "trans-fairy-" + __import__("hashlib").sha256(os.path.abspath(tc).encode()).hexdigest()[:8], "staged", sid + ".jsonl")
check("invariant 5/10: install copies -- the staged source survives", os.path.exists(staged))

# --- inject silent is NEVER installed in a projects dir (owner's trust rule) ---
tc, wr, dr = _dirs()
exp = _export(wr)
run(_base(tc, wr, dr) + ["init"])
run(_base(tc, wr, dr) + ["split", "--export", exp, "--target-uuid", "c1"])
run(_base(tc, wr, dr) + ["build"])
code, out, _ = run(_base(tc, wr, dr) + ["install", "--inject", "--silent"])
res = json.loads(out)
check("inject silent: never installed in a projects dir", res.get("installed") is None and res.get("mode") == "silent")
check("inject silent: stays in staged for the harness", os.path.exists(res["staged"]))

# --- invariant 2: the tool never uses `cp -n` (fails open); it copies with shutil ---
srcdir = os.path.join(os.path.dirname(__file__), "..", "src", "cai", "transfairy")
uses_cpn = any("cp -n" in io.open(os.path.join(srcdir, f)).read()
               for f in os.listdir(srcdir) if f.endswith(".py"))
check("invariant 2: no `cp -n` anywhere in the transfairy package", not uses_cpn)

# --- invariant 6: install prints the resume command, never executes claude -r ---
tc, wr, dr = _dirs()
exp = _export(wr)
run(_base(tc, wr, dr) + ["init"])
run(_base(tc, wr, dr) + ["split", "--export", exp, "--target-uuid", "c1"])
run(_base(tc, wr, dr) + ["build"])
code, out, _ = run(_base(tc, wr, dr) + ["install"])
res = json.loads(out)
check("invariant 6: install returns the claude -r command as a string, does not run it",
      isinstance(res.get("resume"), str) and res["resume"].startswith("cd ") and "claude -r" in res["resume"])

# --- collision: refuse, then resolve additively with --reflow (never overwrite) ---
tc, wr, dr = _dirs()
exp = _export(wr)
run(_base(tc, wr, dr) + ["init"])
run(_base(tc, wr, dr) + ["split", "--export", exp, "--target-uuid", "c1"])
run(_base(tc, wr, dr) + ["build"])
c1, o1, _ = run(_base(tc, wr, dr) + ["install"])
run(_base(tc, wr, dr) + ["build"])                      # same pool -> same sid
c2, _, e2 = run(_base(tc, wr, dr) + ["install"])
check("collision: a second install of the same id is refused", c2 == 1 and "already installed" in e2)
check("collision: the refusal names --reflow as the additive way out", "--reflow" in e2)
c3, o3, _ = run(_base(tc, wr, dr) + ["install", "--reflow"])
r3 = json.loads(o3)
first = json.loads(o1)["sessionId"]
check("reflow: installs under a fresh id", c3 == 0 and r3["sessionId"] != first)
check("reflow: records what it was reflowed from", r3["reflowed_from"] == first)
import json as _j
_recs = [_j.loads(l) for l in io.open(r3["installed"], encoding="utf-8") if l.strip()]
_b = [x for x in _recs if x.get("subtype") == "reflow-boundary"]
check("reflow: emits a lineage record naming the original (an artificial fork)",
      len(_b) == 1 and _b[0]["reflowedFrom"] == first and _b[0]["isMeta"] is True)
check("reflow: the lineage record is meta, so it does not reach the model (P1)",
      _b[0].get("type") == "system")
check("reflow: writes a ledger row in the projects dir",
      r3.get("ledger") and os.path.exists(r3["ledger"])
      and first in io.open(r3["ledger"], encoding="utf-8").read())
check("reflow: the original install still exists (nothing overwritten)",
      os.path.exists(json.loads(o1)["installed"]) and os.path.exists(r3["installed"]))

# teardown: remove the real projects dirs the installs created (temp dirs the OS cleans)
import shutil
for pd in _projdirs:
    shutil.rmtree(pd, ignore_errors=True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
