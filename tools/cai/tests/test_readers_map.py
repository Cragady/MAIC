"""Known-answer tests for the reader's map -- four rulings, each with a control.

Run: python3 tests/test_readers_map.py
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.notation import readers_map as rmap  # noqa: E402
from cai.notation import store  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


DOC = {"definitions": [
    {"id": "live-one", "symbols": ["@live"], "name": "live", "definition": "a live meaning",
     "decided": "2026-01-01T00:00:00Z", "status": "in force", "retired": None,
     "lifetime_days": None, "scannable": True, "scope": "tools"},
]}

AUDIT = {"legend": {1: "agrees"}, "legend_taken_at": "2026-01-01T00:00:00Z",
         "entries": [{"symbol": "@gone", "resolution": None, "kind": None,
                      "occurrences": 3, "confidence": [], "inferred": False,
                      "sites": [{"file": "d.md", "line": 1}], "note": "unresolved"}]}

repo = tempfile.mkdtemp()
subprocess.run(["git", "init", "-q", repo], check=True)
subprocess.run(["git", "-C", repo, "config", "user.email", "a@b"], check=True)
subprocess.run(["git", "-C", repo, "config", "user.name", "t"], check=True)

m = rmap.new(repo, AUDIT)
rmap.save(repo, m)
check("a map is written, one per repository", os.path.exists(rmap.path(repo)))

# --- RULING: the stamp says where it can be validated, IF it can be ------------
m = rmap.load(repo)
check("an untracked map says its stamp CANNOT be validated",
      m["stamp_validation"]["validatable"] is False)
check("and says why rather than just refusing",
      "nothing outside the file witnesses" in m["stamp_validation"]["why"])

open(os.path.join(repo, "x"), "w").write("x")
subprocess.run(["git", "-C", repo, "add", "-A"], check=True)
subprocess.run(["git", "-C", repo, "commit", "-qm", "i"], check=True)
m = rmap.load(repo)
check("POSITIVE CONTROL -- once tracked, the stamp IS validatable",
      m["stamp_validation"]["validatable"] is True)
check("and it names the command that witnesses it",
      "git log -1" in m["stamp_validation"]["how"])
check("validation is recomputed at READ, not stored -- a stored one goes stale",
      json.loads(open(rmap.path(repo), encoding="utf-8").read())
      ["stamp_validation"]["validatable"] is False)

# --- RULING: symbol -> symbol alias, which follows its target ------------------
rmap.alias(m, "@gone", "@live")
check("an alias is symbol -> symbol, not a copied definition", m["entries"][0]["kind"] == "alias")
check("and is inferred by default -- a derived alias is a proposal",
      m["entries"][0]["inferred"] is True)
rmap.alias(m, "@gone", "@live", inferred=False)
check("a person can confirm it", m["entries"][0]["inferred"] is False)
try:
    rmap.alias(m, "@nothere", "@live")
    missing = False
except KeyError:
    missing = True
check("POSITIVE CONTROL -- aliasing a symbol with no entry is refused", missing)

# --- RULING: map first, then the tables ---------------------------------------
r = rmap.resolve(m, DOC, "@gone")
check("the map answers first", r["via"] == "map")
check("and it follows the target's LIVE definition rather than restating it",
      r["definition"] and r["definition"][0]["definition"] == "a live meaning")

DOC2 = {"definitions": [dict(DOC["definitions"][0], definition="a REFINED meaning")]}
r2 = rmap.resolve(m, DOC2, "@gone")
check("so refining the target reaches the alias for free",
      r2["definition"][0]["definition"] == "a REFINED meaning")

r3 = rmap.resolve(m, DOC, "@live")
check("a symbol not in the map falls through to the tables", r3["via"] == "tables")
r4 = rmap.resolve(m, DOC, "@unknown")
check("POSITIVE CONTROL -- an unknown symbol resolves to nothing, not to a present meaning",
      r4["via"] is None and r4["definition"] is None)

# --- RULING: one map, and a nulled pointer means the vocabulary moved ----------
v = rmap.verify(m, DOC)
check("a healthy map holds", v["ok"] is True and v["nulled_pointers"] == [])

rmap.alias(m, "@gone", "@vanished")
v2 = rmap.verify(m, DOC)
check("POSITIVE CONTROL -- an alias at a symbol that does not resolve FAILS the map",
      v2["ok"] is False and v2["nulled_pointers"] == ["@gone"])
check("and the verdict is discard-and-audit, not repair",
      "discard it and audit again" in v2["verdict"])
check("the reason says the vocabulary moved, not that one link broke",
      "vocabulary moved" in v2["why"])

# The bug this control exists for: `store.resolve` returns an EMPTY LIST for an
# absent symbol, never None. An `is None` test passed a nulled pointer as healthy.
check("REGRESSION -- store.resolve returns a falsy list, not None, for an absent symbol",
      store.resolve(DOC, "@vanished") == [])
r5 = rmap.resolve(m, DOC, "@gone")
check("REGRESSION -- resolving through a nulled alias reports the null",
      r5["nulled"] is True and r5["definition"] is None)
check("and tells the reader to re-derive rather than re-point",
      "re-pointing at whatever looks closest" in r5["note"])

# --- one map: writing replaces, and dissolution removes ------------------------
rmap.save(repo, rmap.new(repo, AUDIT))
check("writing again REPLACES -- there is one map, not a pile of stamps",
      len(rmap.load(repo)["entries"]) == 1)
gone = rmap.discard(repo)
check("discard removes it", gone and not os.path.exists(rmap.path(repo)))
check("POSITIVE CONTROL -- discarding twice is not an error", rmap.discard(repo) is None)
check("and loading a repo with no map returns nothing invented", rmap.load(repo) is None)

# --- RULING: the three resolutions of a finding -------------------------------
def _e(conf, occ=6, res="@x"):
    return {"symbol": "@s", "resolution": res, "kind": None, "occurrences": occ,
            "confidence": conf, "inferred": False, "sites": [], "note": ""}


check("no reading yet is an UNKNOWN SYMBOL -- a lookup miss, not a judgement",
      rmap.classify(_e([], res=None))["resolution"] == rmap.UNKNOWN_SYMBOL)
check("and it claims no meaning",
      "nothing here claims to know" in rmap.classify(_e([], res=None))["why"])

check("deviations that AGREE with each other indict the DEFINITION",
      rmap.classify(_e([[8, 2], [1, 1]]))["resolution"] == rmap.DEFINITION_AT_FAULT)
check("outright contradiction counts the same way",
      rmap.classify(_e([[4, 4], [1, 1]]))["resolution"] == rmap.DEFINITION_AT_FAULT)
check("the reason cites many applications outweighing one defining line",
      "outweigh the one line" in rmap.classify(_e([[8, 2], [1, 1]]))["why"])

check("deviations that DISAGREE with each other indict the MATERIAL",
      rmap.classify(_e([[7, 3], [1, 1]]))["resolution"] == rmap.USAGE_AT_FAULT)
check("and it leaves the definition alone",
      "definition left alone" in rmap.classify(_e([[7, 3], [1, 1]]))["needs"])

# REGRESSION: an earlier version compared deviations only against each other, so
# one stray occurrence in ten indicted the material.
check("REGRESSION -- 9 agreeing and 1 scattered is NO finding",
      rmap.classify(_e([[9, 1], [1, 3]]))["resolution"] is None)
check("a tie favours the standing definition -- the map proposes, it does not indict",
      rmap.classify(_e([[5, 1], [5, 2]]))["resolution"] is None)
check("POSITIVE CONTROL -- confirmation by a person is agreement, not a finding",
      rmap.classify(_e([[3, 5]]))["resolution"] is None)

# --- RULING: archive restores; live restores and then updates -----------------
_m = {"stance": rmap.LIVE, "entries": [_e([[8, 2]], occ=12)]}
_live = rmap.dissolution(_m)
check("a LIVE repository is told to update the symbols too",
      _live["may_edit_material"] is True and "update the symbols" in _live["action"])
check("and it counts the sites", _live["sites_to_update"] == 12)
check("the reason is that stale notation defers the same audit",
      "defers" in _live["why"])

_m["stance"] = rmap.ARCHIVE
_arch = rmap.dissolution(_m)
check("an ARCHIVE is restored for readability and LEFT ALONE",
      _arch["may_edit_material"] is False and _arch["action"] == "restore readability only")
check("because its symbols are part of what it records",
      "part of what it records" in _arch["why"])
check("POSITIVE CONTROL -- the two stances differ on the material, not on the findings",
      _arch["findings"] == _live["findings"] and
      _arch["may_edit_material"] != _live["may_edit_material"])
check("neither performs an edit -- both defer to the operator",
      "operator" in _arch["then"] and "operator" in _live["then"])

_m2 = rmap.new(repo, AUDIT, stance=rmap.ARCHIVE)
check("a map records its stance", _m2["stance"] == rmap.ARCHIVE)
check("and defaults to live, the case that needs the extra step",
      rmap.new(repo, AUDIT)["stance"] == rmap.LIVE)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
