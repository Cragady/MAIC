"""Known-answer tests for cai notation, with a positive control on every negative.

A negative result is only as good as evidence the check can return a positive --
the rule this repository derived nine times in one day and the owner had derived
independently elsewhere. Every "not found" below is paired with a case proving
the finder works.

Run: python3 tests/test_notation.py   (exit 0 = pass)
"""
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.notation import store  # noqa: E402
from cai.notation import audit as A  # noqa: E402
from cai.notation import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


UTC = datetime.timezone.utc
doc = store.load()

# --- the dictionary ---
check("dictionary loads with definitions", len(doc["definitions"]) >= 10)
check("one definition can carry several symbols -- the glyph and the long form",
      store.resolve(doc, "⚑")[0]["id"] == store.resolve(doc, "(╯°Д°)╯︵ ┻━┻")[0]["id"])
check("and the anchor pair resolves the same way",
      store.resolve(doc, "⚓")[0]["id"] == store.resolve(doc, "┬─┬ ノ( ゜-゜ノ)")[0]["id"])
check("POSITIVE CONTROL -- an unknown symbol resolves to nothing",
      store.resolve(doc, "☃") == [])
check("definitions hold their stated meaning",
      "fork" in store.resolve(doc, "⑂")[0]["definition"].lower())

# --- lifetimes: continuous, no versions ---
inner = store.resolve(doc, "---- Inner Source ----")
check("a retired definition is still served inside its fallout period", len(inner) == 1)
far = datetime.datetime(2030, 1, 1, tzinfo=UTC)
check("and stops being served after it", store.resolve(doc, "---- Inner Source ----", at=far) == [])
early = datetime.datetime(2020, 1, 1, tzinfo=UTC)
check("a definition is not served before it was decided",
      store.resolve(doc, "⑂", at=early) == [])
check("the retired entry names what superseded it",
      inner[0]["superseded_by"] == "fence-raw-source")
check("and the survivor records what it absorbed, without naming the dead symbol",
      "absorbed" in store.resolve(doc, "---- Raw Source ----")[0]
      and "Inner Source" not in " ".join(store.resolve(doc, "---- Raw Source ----")[0]["symbols"]))

# --- filter on read, compact on demand ---
_before = len(doc["definitions"])
_compacted, _dropped = store.compact(doc, at=far)
check("compact drops expired entries and reports them", "fence-inner-source" in _dropped)
check("and reading did not mutate the store", len(doc["definitions"]) == _before)

# --- not everything is a text marker ---
idx = store.symbols(doc)
check("the confidence digits are excluded from a scan", "1" not in idx)
check("POSITIVE CONTROL -- they are still real definitions",
      any(e["id"] == "confidence-class" for e in store.live(doc)))

# --- the audit ---
d = tempfile.mkdtemp()
p = os.path.join(d, "material.md")
with open(p, "w", encoding="utf-8") as fh:
    fh.write("---- Ghost Fence ----\nbody\n"
             "I mention ---- Ghost Fence ---- inside a sentence, which is not a use.\n"
             "a line with ⑂ in it\n")
rep = A.audit([p], doc)
check("audit finds an unresolved fence", rep["unresolved"] == 1)
check("and reports its NAME, not an empty marker -- the gap that let a shared "
      "fence definition silently change what was extracted",
      rep["entries"][0]["symbol"] == "---- Ghost Fence ----")
check("it reports where", rep["entries"][0]["sites"][0]["line"] == 1)
check("POSITIVE CONTROL -- the in-sentence mention is NOT counted",
      all(s["line"] != 3 for s in rep["entries"][0]["sites"]))
check("known glyphs are counted separately", rep["known"].get("⑂") == 1)
check("the audit states that it does not derive",
      "not performed here" in rep["entries"][0]["note"])
check("and carries its own legend, dated", "legend_taken_at" in rep and 1 in rep["legend"])

# --- confidence tuples ---
e = rep["entries"][0]
A.observe(e, 1, 9)
A.observe(e, 3, 1)
A.observe(e, 1, 2)
check("observations accumulate into the count", A.grade(e) == "11 1  1 3")
A.observe(e, 4, 5)
check("and a rating can fall as well as rise", "5 4" in A.grade(e))
try:
    A.observe(e, 9)
    _bad = False
except ValueError:
    _bad = True
check("POSITIVE CONTROL -- class 9 raises", _bad)

# --- purge keeps only pending decisions ---
m = A.purge(rep)
check("purge drops an entry with no decision pending", len(m["entries"]) == 0 and m["purged"] == 1)
e["slated"] = "update"
m2 = A.purge(rep)
check("and keeps one that is slated", len(m2["entries"]) == 1)

# --- status: declined is the one that stops re-litigation ---
_loc = store.resolve(doc, "LOC")
check("a DECLINED marker still resolves", len(_loc) == 1)
check("and says plainly that it was declined", store.status_of(_loc[0]) == "declined")
check("with the reason, so it is not re-proposed",
      "answered by request" in _loc[0]["definition"])
check("a candidate is distinguished from something in force",
      store.status_of(store.resolve(doc, "⊘")[0]) == "candidate"
      and store.status_of(store.resolve(doc, "⑂")[0]) == "in force")
check("retired is derived from the field, not restated",
      store.status_of(store.resolve(doc, "---- Inner Source ----")[0]) == "retired")

# --- the comms family, seeded from the conventions rather than invented ---
check("the comms fence carries transit, not origin",
      "TRANSIT" in store.resolve(doc, "---- comms source ----")[0]["definition"])
check("the r/w fence resolves under both its markers",
      store.resolve(doc, "---- comms r/w ----")[0]["id"]
      == store.resolve(doc, "---- comms draft r/w ----")[0]["id"])
check("the AT: header records what it fixes a claim to",
      "read at" in store.resolve(doc, "AT:")[0]["definition"])

# --- the two frozen contexts are judged differently ---
_sd = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q"], cwd=_sd, capture_output=True)
for _k in ("user.email", "user.name"):
    subprocess.run(["git", "config", _k, "t"], cwd=_sd, capture_output=True)
_sf = os.path.join(_sd, "sync-frozen")
os.makedirs(_sf)
_snap = os.path.join(_sf, "thing-20260902T000000Z.json")


def _write_snap(**over):
    body = {"frozen": "2026-09-02T00:00:00Z", "synced": "2026-09-02T00:00:00Z",
            "name": "thing-20260902T000000Z.json", "syncedFrom": "docs/thing.json"}
    body.update(over)
    with open(_snap, "w", encoding="utf-8") as fh:
        json.dump(body, fh, indent=2)


_write_snap(x=1)
subprocess.run(["git", "add", "-A"], cwd=_sd, capture_output=True)
subprocess.run(["git", "commit", "-qm", "snap"], cwd=_sd, capture_output=True)
_write_snap(x=2)
subprocess.run(["git", "add", "-A"], cwd=_sd, capture_output=True)
subprocess.run(["git", "commit", "-qm", "again"], cwd=_sd, capture_output=True)

_r = store.freeze_status(_snap, tolerance_seconds=10 ** 9)
check("a sync-frozen snapshot is not held to the single-commit rule",
      _r["commits"] == 2 and _r["single_commit_required"] is False and _r["kept"] is True)
check("because its identity comes from the content, not the history",
      "not checked here" in _r["note"])

_write_snap(x=2, syncedFrom=None)
_r2 = store.freeze_status(_snap, tolerance_seconds=10 ** 9)
check("POSITIVE CONTROL -- but it MUST say what it copied", _r2["kept"] is False)
check("and the finding names what is missing",
      _r2["missing_provenance"] == ["syncedFrom"])

check("a defaults-frozen file is still held to the single-commit rule",
      store.freeze_status(
          os.path.join(os.path.dirname(__file__), "defaults-frozen", "shapes.jsonl")
      )["single_commit_required"] is True)
shutil.rmtree(_sd, ignore_errors=True)

# --- staleness is measurable, not merely declared ---
# (the source-vs-fallback checks live in the fetch section below; these two
#  asserted the loaded doc was stale, which was right before the dictionary moved
#  into SOPIA and wrong afterwards -- the behaviour changed, so the test did)
st = store.is_stale(doc)
check("a doc read from the source is not stale, and says which path it came from",
      st["stale"] is False and "SOPIA" in st["path"])

# --- SOPIA defines, cai fetches ---
_src_path, _is_src = store.source_path()
check("the source is preferred over the embedded copy", _is_src is True)
check("and it is SOPIA's file, not one inside cai",
      "SOPIA" in _src_path and "src/cai" not in _src_path)
check("a loaded doc reports where it came from, whichever tier answered",
      doc.get("_path") in (_src_path, "SOPIA@remote:docs/guides/notation-dictionary.json"))
check("reading the source is not stale", store.is_stale(doc)["stale"] is False)

_saved = store.SOURCE_CANDIDATES
store.SOURCE_CANDIDATES = ()
os.environ["CAI_NO_REMOTE"] = "1"   # the remote sits above the tiers under test
_fb = store.load()
check("POSITIVE CONTROL -- with the source unreachable it falls back", _fb["_is_source"] is False)
check("what answers instead declares itself not-the-source",
      _fb["_is_source"] is False and "sync-frozen" in _fb["_path"])
check("and it is the SNAPSHOT, which now sits ahead of the embedded fallback",
      _fb["_path"].endswith(".json") and "notation-dictionary-" in _fb["_path"])
check("and reports how far behind it is, since stale without an age is not actionable",
      store.is_stale(_fb)["age_days"] is not None)
check("the fallback still resolves symbols", len(store.resolve(_fb, "⑂")) == 1)
store.SOURCE_CANDIDATES = _saved
os.environ.pop("CAI_NO_REMOTE", None)

# --- the freeze contract: two gates, each with a control on its failure ---

_fd = tempfile.mkdtemp()


def _git(*a):
    subprocess.run(["git"] + list(a), cwd=_fd, capture_output=True)


_git("init", "-q")
_git("config", "user.email", "t@t")
_git("config", "user.name", "t")
_frozen_dir = os.path.join(_fd, "defaults-frozen")
os.makedirs(_frozen_dir)
_stamp = datetime.datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

_good = os.path.join(_frozen_dir, "good.json")
with open(_good, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "name": "good.json", "x": 1}, fh)
_git("add", "-A")
_git("commit", "-qm", "freeze good")
_g = store.freeze_status(_good)
check("a properly frozen file keeps its contract", _g["kept"] is True)
check("one commit touches it -- the move that froze it", _g["commits"] == 1)
check("and it sits under the frozen directory", _g["in_frozen_dir"] is True)

with open(_good, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "x": 2}, fh)
_git("add", "-A")
_git("commit", "-qm", "edit after freeze")
_e = store.freeze_status(_good)
check("POSITIVE CONTROL -- an edit after freezing breaks it", _e["kept"] is False)
check("caught by the commit count, which needs no clock", _e["commits"] == 2)

_outside = os.path.join(_fd, "outside.json")
with open(_outside, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp}, fh)
_git("add", "-A")
_git("commit", "-qm", "outside")
_o = store.freeze_status(_outside)
check("POSITIVE CONTROL -- a freeze claim outside the frozen dir is refused",
      _o["kept"] is False and _o["in_frozen_dir"] is False)

# the name gate closes the rename bypass: a renamed file has a fresh path with
# exactly one commit, which the history gate alone would pass
_evade = os.path.join(_frozen_dir, "good-renamed.json")
with open(_evade, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "name": "good.json", "x": 999}, fh)
_git("add", "-A")
_git("commit", "-qm", "tampered under a new name")
_ev = store.freeze_status(_evade)
check("POSITIVE CONTROL -- the rename bypass passes the history gate", _ev["commits"] == 1)
check("but the name gate catches it", _ev["kept"] is False and _ev["name_ok"] is False)
check("and says what the bypass was", "bypass this gate closes" in _ev["note"])

# a legitimate re-freeze is a move plus a name change, and BOTH stay valid.
# a clean pair, so neither assertion leans on a file an earlier check edited.
_r1 = os.path.join(_frozen_dir, "ref-v1.json")
with open(_r1, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "name": "ref-v1.json", "x": 1}, fh)
_git("add", "-A")
_git("commit", "-qm", "freeze ref-v1")
_r2 = os.path.join(_frozen_dir, "ref-v2.json")
with open(_r2, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "name": "ref-v2.json", "x": 2}, fh)
_git("add", "-A")
_git("commit", "-qm", "re-freeze as ref-v2")
check("a re-freeze under a new name holds the contract",
      store.freeze_status(_r2)["kept"] is True)
check("and it is append rather than mutate -- the superseded file still holds",
      store.freeze_status(_r1)["kept"] is True)

_ns = os.path.join(_frozen_dir, "nameless.json")
with open(_ns, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp}, fh)
_git("add", "-A")
_git("commit", "-qm", "nameless")
_n = store.freeze_status(_ns)
check("a stamp with no name reports that gate unavailable rather than failing it",
      _n["name_ok"] is None and _n["kept"] is True)

# a file that cannot carry its own stamp needs a sidecar, and the sidecar is
# REQUIRED -- downgrading on its absence is what a rename exploits
_jl = os.path.join(_frozen_dir, "fixture.jsonl")
with open(_jl, "w", encoding="utf-8") as fh:
    fh.write('{"a":1}\n{"b":2}\n')  # multi-line, so it is JSONL and not valid JSON
_git("add", "-A")
_git("commit", "-qm", "add jsonl with no sidecar")
_j = store.freeze_status(_jl)
check("a JSONL file with no sidecar is BROKEN, not downgraded", _j["kept"] is False)
check("and says why the downgrade would be the bypass", "a rename exploits" in _j["note"])

with open(_jl + ".frozen", "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp, "name": "fixture.jsonl.frozen",
               "vouches_for": "fixture.jsonl"}, fh)
_git("add", "-A")
_git("commit", "-qm", "add its sidecar")
check("POSITIVE CONTROL -- with a sidecar vouching for it, it holds",
      store.freeze_status(_jl)["kept"] is True)

_renamed_jl = os.path.join(_frozen_dir, "fixture-v2.jsonl")
os.rename(_jl, _renamed_jl)
_git("add", "-A")
_git("commit", "-qm", "rename orphans the sidecar")
_rj = store.freeze_status(_renamed_jl)
check("POSITIVE CONTROL -- the rename dodge is closed for this class",
      _rj["kept"] is False and _rj["commits"] == 1)

shutil.rmtree(_fd, ignore_errors=True)

_sy = store.freeze_status(os.path.join(os.path.dirname(__file__), "..",
                                       "src", "cai", "notation", "dictionary.json"))
check("a synced copy makes no freeze claim, since it is updated on purpose",
      _sy["claims_frozen"] is False and "synced" in _sy["note"])

_nd = tempfile.mkdtemp()
_unstamped = os.path.join(_nd, "plain.json")
with open(_unstamped, "w", encoding="utf-8") as fh:
    json.dump({"definitions": []}, fh)
check("a file with no stamp claims nothing",
      store.freeze_status(_unstamped)["claims_frozen"] is False)
_uncommitted = os.path.join(_nd, "stamped.json")
with open(_uncommitted, "w", encoding="utf-8") as fh:
    json.dump({"frozen": _stamp}, fh)
_u = store.freeze_status(_uncommitted)
check("POSITIVE CONTROL -- a stamp no commit records is reported, not passed",
      _u["claims_frozen"] is True and _u["validated"] is False and "nothing" in _u["note"])
shutil.rmtree(_nd, ignore_errors=True)

# --- the CLI ---
check("bare invocation prints the reference", cli.main([]) == 0)
check("lookup exits 0 on a hit", cli.main(["lookup", "⑂"]) == 0)
check("POSITIVE CONTROL -- lookup exits 1 on a miss", cli.main(["lookup", "☃"]) == 1)
check("audit exits 1 when something is unresolved", cli.main(["audit", p]) == 1)
check("the help explains why, not only what", "WHY IT EXISTS" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
