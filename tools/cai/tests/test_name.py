"""Known-answer tests for cai name. Run: python3 tests/test_name.py"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.name import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


p = cli.parse("SOPIA │ - b5d42ad9".replace("│", "⑂"))
check("parses the cwd", p["cwd"] == "SOPIA")
check("parses the slice", p["slice"] == "b5d42ad9")
check("resolves a marker through notation rather than a local table",
      p["markers"] and p["markers"][0]["id"] == "fork")

full = cli.parse("SOPIA ⑂ ⚑ - b5d42ad9")
check("parses several markers", [m["id"] for m in full["markers"]] == ["fork", "role-pivot"])

bare = cli.parse("plainname - abcd1234")
check("a name with no markers still parses", bare["cwd"] == "plainname" and not bare["markers"])
check("POSITIVE CONTROL -- an unrecognised token is reported, not absorbed",
      cli.parse("SOPIA ⑂ zzz - abcd1234")["unrecognised"] == ["zzz"])

r = cli.render("SOPIA", "b5d42ad9", fork=True, role="pivot")
check("renders from parts", r == "SOPIA ⑂ ⚑ - b5d42ad9")
check("and round-trips through parse",
      cli.parse(r)["cwd"] == "SOPIA" and cli.parse(r)["slice"] == "b5d42ad9")

check("bare invocation prints the reference", cli.main([]) == 0)
check("explain exits 0", cli.main(["explain"]) == 0)
check("markers exits 0", cli.main(["markers"]) == 0)
check("the help explains why a slice may not match the sessionId",
      "documentation, not an address" in cli.MAN_HELP)
check("and why the cwd leads", "truncates the tail" in cli.MAN_HELP)
check("the gated marker is named as gated", "not-eligible" in str(cli.GATED))

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
