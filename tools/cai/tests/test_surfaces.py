"""Every tool's CLI must BUILD and RESPOND. **Two breaks got past a green suite.**

Run: python3 tests/test_surfaces.py

**Why this exists as its own suite.** Twice in two days a CLI shipped broken
while every other suite passed, because no test ever constructed the parser:

    2026-09-02  `trans-fairy` referenced `graftmod` without importing it
    2026-09-04  `redact` gained a SECOND `--force`, an argparse conflict

**Both were found by a human running the command**, which is the check no suite
was performing. Unit tests exercised the functions underneath and never the
surface a person actually touches. **A tool whose parser cannot be built is
totally broken and perfectly green.**

This suite is deliberately shallow and total: it builds every registered tool's
parser and asks for its reference. It asserts nothing about behaviour, which is
what keeps it cheap enough to cover everything.
"""
import io
import os
import sys
from contextlib import redirect_stdout, redirect_stderr
from importlib.metadata import entry_points

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
os.environ.pop("CAI_SESSION_ID", None)

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


tools = {ep.name: ep for ep in entry_points(group="cai.tools")}
check("tools are registered", len(tools) >= 15, "%d found" % len(tools))

for name in sorted(tools):
    loaded = err = None
    try:
        loaded = tools[name].load()
    except Exception as e:                                       # noqa: BLE001
        err = "%s: %s" % (type(e).__name__, e)
    check("%-18s entry point loads" % name, loaded is not None, err or "")
    if loaded is None:
        continue
    out, code, err2 = io.StringIO(), None, None
    try:
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            code = loaded(["--help"])
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else 0
    except Exception as e:                                       # noqa: BLE001
        err2 = "%s: %s" % (type(e).__name__, e)
    check("%-18s --help builds and returns" % name, err2 is None, err2 or "")
    check("%-18s --help prints something" % name, len(out.getvalue()) > 40)

# The two historical breaks, named, so the reason this suite exists survives it.
import argparse  # noqa: E402
_p = argparse.ArgumentParser()
_p.add_argument("--force", action="store_true")
_dup = False
try:
    _p.add_argument("--force", action="store_true")
except argparse.ArgumentError:
    _dup = True
check("POSITIVE CONTROL -- a duplicate flag IS an argparse error, so this can fail",
      _dup is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
