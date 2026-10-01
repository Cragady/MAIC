"""Known-answer tests for cai time. Run: python3 tests/test_time.py"""
import datetime
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.timekeeping import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


UTC = datetime.timezone.utc
fixed = datetime.datetime(2026, 9, 2, 5, 19, 0, tzinfo=UTC)

check("a stamp is UTC and Z-suffixed", cli.stamp(fixed) == "2026-09-02T05:19:00Z")
check("now() is timezone-aware, so it cannot be compared against a naive value",
      cli.now().tzinfo is not None)

w = cli.window("2h", start=fixed)
check("a window computes its end rather than being typed", w["expires"] == "2026-09-02T07:19:00Z")
check("and shows BOTH renderings, which is the whole point",
      "granted_local" in w and "expires_local" in w)
check("the note names the failure it prevents", "six hours wrong" in w["note"])

check("durations in minutes", cli.window("90m", start=fixed)["expires"] == "2026-09-02T06:49:00Z")
check("durations in days", cli.window("1d", start=fixed)["expires"] == "2026-09-03T05:19:00Z")

try:
    cli.parse_duration("soon")
    bad = False
except ValueError:
    bad = True
check("POSITIVE CONTROL -- an unreadable duration raises rather than guessing", bad)

check("bare invocation prints the reference", cli.main([]) == 0)
check("`now` exits 0", cli.main(["now"]) == 0)
check("`window` exits 0", cli.main(["window", "2h"]) == 0)
check("POSITIVE CONTROL -- a bad duration exits 2", cli.main(["window", "soon"]) == 2)
check("`until` reports a past instant as expired",
      cli.main(["until", "2020-01-01T00:00:00Z"]) == 0)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
