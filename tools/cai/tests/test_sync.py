"""Known-answer tests for cai sync. Run: python3 tests/test_sync.py"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai import source  # noqa: E402
from cai.sync import runner, spec, cli  # noqa: E402
from cai.document import store as dstore  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


doc = spec.load()
check("the jobs are data, not code", set(spec.jobs(doc)) >= {"shapes", "notation", "hooks"})
check("each job names where it reads from",
      all(j.get("from") for j in spec.jobs(doc).values()))

check("POSITIVE CONTROL -- an unknown job is refused, not invented",
      runner.run("no-such-job", doc)["ok"] is False)
check("and it lists what it does know",
      "Known:" in runner.run("no-such-job", doc)["note"])

r = runner.run("shapes", doc, dry_run=True)
check("a dry run reports without writing", r["ok"] and "written" not in r)
check("and names the file it would write", r["would_write"].endswith(".json"))

snaps = runner.snapshots("document", "shapes")
check("a snapshot exists for shapes", len(snaps) >= 1)
newest = runner.newest("document", "shapes")
payload = json.load(open(newest, encoding="utf-8"))
check("it is stamped FROZEN", bool(payload.get("frozen")))
check("and stamped SYNCED, which are usually opposites", bool(payload.get("synced")))
check("it records its own name, so a rename is detectable",
      payload["name"] == os.path.basename(newest))
check("and where it came from", payload["syncedFrom"] == "docs/shapes.json")
check("the note explains why frozen and synced can both hold",
      "writes a NEW dated file" in payload["note"])

check("a second sync writes beside the first rather than editing it",
      "beside this one" in payload["note"])

# --- the disaster case: the source is gone ---
saved_sopia, saved_cands = source.SOPIA, dstore.SOURCE_CANDIDATES
source.SOPIA = "/nonexistent"
dstore.SOURCE_CANDIDATES = ()
os.environ["CAI_NO_REMOTE"] = "1"   # the remote tier sits ABOVE these; this tests below it
d = dstore.load()
check("with the source unreachable, the SNAPSHOT answers",
      "sync-frozen" in d["_path"])
check("and the shapes are still usable", "session-name" in dstore.names(d))
check("it reports itself as not-the-source", d["_is_source"] is False)
check("with an age, since stale without an age is not actionable",
      source.stale(d).get("age_days") is not None)
r2 = runner.run("shapes", doc)
check("POSITIVE CONTROL -- and a sync with no source refuses rather than inventing",
      r2["ok"] is False and "nothing to snapshot" in r2["note"])
check("naming it as the case snapshots exist for", "exist FOR" in r2["note"])
source.SOPIA, dstore.SOURCE_CANDIDATES = saved_sopia, saved_cands
os.environ.pop("CAI_NO_REMOTE", None)

# --- the priority, asserted rather than assumed ---
import tempfile  # noqa: E402
_env = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
json.dump({"shapes": {"from-env": {"summary": "override"}}}, _env)
_env.close()
os.environ["CAI_DOCUMENTS"] = _env.name
check("an environment variable OVERRIDES even the source",
      "from-env" in dstore.names(dstore.load()))
del os.environ["CAI_DOCUMENTS"]
os.unlink(_env.name)

check("POSITIVE CONTROL -- without it, SOPIA answers (remote or local)",
      "SOPIA" in dstore.load()["_path"])

_s2, _c2 = source.SOPIA, dstore.SOURCE_CANDIDATES
source.SOPIA = "/nonexistent"
dstore.SOURCE_CANDIDATES = ()
os.environ["CAI_NO_REMOTE"] = "1"   # the remote tier sits ABOVE these; this tests below it
check("with SOPIA gone, the sync-frozen snapshot answers",
      "sync-frozen" in dstore.load()["_path"])
source.SOPIA, dstore.SOURCE_CANDIDATES = _s2, _c2
os.environ.pop("CAI_NO_REMOTE", None)

check("bare invocation prints the reference", cli.main([]) == 0)
check("list exits 0", cli.main(["list"]) == 0)
check("the help says why a snapshot is not a mirror",
      "SNAPSHOT rather than a mirror" in cli.MAN_HELP)

# --- a job may carry CODE, from this repo, to somewhere outside a cai package ---
# Owner: use the freeze-sync on the reflow implementation, so diction's internal
# tooling reflows without requiring cai. Three assumptions had to give: the source
# was SOPIA-relative, the destination was a cai package, and the payload was JSON.
_jobs = spec.jobs()
check("a text job exists and names its own root", 
      _jobs.get("reflow-lines", {}).get("root") == "self")
check("and declares its kind and extension",
      _jobs["reflow-lines"]["kind"] == "text" and _jobs["reflow-lines"]["ext"] == ".py")
check("and carries its reason, not just its mechanics",
      "hand-maintained second copy" in _jobs["reflow-lines"]["why"])

import glob as _glob  # noqa: E402
_snaps = sorted(_glob.glob(os.path.join(runner.REPO, "diction", "sync-frozen",
                                        "reflow-lines-*.py")))
check("a snapshot exists", bool(_snaps))
_head = open(_snaps[-1], encoding="utf-8").read().split("\n")[:6]
check("it carries provenance in a HEADER, since a .py holds no JSON keys",
      any(l.startswith("# syncedFrom:") for l in _head))
check("naming what it copied", any("src/cai/reflow/lines.py" in l for l in _head))
check("and when", any(l.startswith("# synced:") for l in _head))
check("POSITIVE CONTROL -- it is the real implementation, not a stub",
      "def apply" in open(_snaps[-1], encoding="utf-8").read())
check("POSITIVE CONTROL -- json jobs are unaffected",
      runner.run("notation", dry_run=True)["ok"] is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
