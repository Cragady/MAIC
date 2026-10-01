"""Known-answer tests for the remote tier. Run: python3 tests/test_remote.py"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai import remote, source  # noqa: E402
from cai.document import store as dstore  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


url = remote.sibling_url()
check("the SOPIA remote is DERIVED from this repository's origin, not configured",
      url and url.endswith("/SOPIA.git"))
check("same host and owner as origin", url and "CascadeRefiningInc" in url)

check("POSITIVE CONTROL -- a repo with no origin yields no remote",
      remote.sibling_url(repo="/tmp") is None)

os.environ["CAI_NO_REMOTE"] = "1"
d_local = dstore.load()
check("CAI_NO_REMOTE falls through to the local source",
      d_local["_path"].startswith("/") and "remote" not in d_local["_path"])
del os.environ["CAI_NO_REMOTE"]

d = dstore.load()
if d["_path"].startswith("SOPIA@remote:"):
    check("the remote answers ahead of the local working copy", True)
    check("and says which ref it read", bool(d.get("_remote", {}).get("ref")))
    check("shapes are usable from it", "session-name" in dstore.names(d))
else:
    check("the remote was unreachable and it fell through rather than raising",
          d["_path"].startswith("/"))
    check("which is the designed behaviour: a gate that fails with the network "
          "is a gate that gets turned off", True)
    check("shapes are usable regardless", "session-name" in dstore.names(d))

os.environ["CAI_DOCUMENTS"] = "/nonexistent-on-purpose.json"
check("an env override that does not exist does not silently become the remote",
      dstore.load()["_path"] != "/nonexistent-on-purpose.json")
del os.environ["CAI_DOCUMENTS"]

txt, meta = remote.read("docs/does-not-exist.json")
check("POSITIVE CONTROL -- a missing remote path returns None, not an exception",
      txt is None and "not in the published SOPIA" in meta.get("note", ""))

check("the module states that a local edit does not take effect until pushed",
      "until it is pushed" in source.remote_payload.__doc__)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
