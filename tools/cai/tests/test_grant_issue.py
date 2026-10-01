"""Known-answer tests for `cai grant issue`, and for the three check-side defects a peer found.

**The gap under test: `check` existed for three weeks and there was no way to
write a grant**, so the only party positioned to author one was the grantee --
the one shape a permission system must not have.

Every refusal below is paired with the same call succeeding once the single
blocking condition is removed. A negative is only as good as evidence the check
can return a positive.

Run: python3 tests/test_grant_issue.py   (exit 0 = pass)
"""
import datetime
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
os.environ.pop("CAI_SESSION_ID", None)
from cai.grant import store  # noqa: E402
from cai.grant import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


UTC = datetime.timezone.utc
d = tempfile.mkdtemp()
SID = "0b1e9641-0704-4743-a430-d07fd35f6b39"
MINE = "8ece07ad-6a96-411f-8941-ab553547dfaa"

# --------------------------------------------------------------------------
# 1. A malformed instant DENIES. It used to raise straight through `check`.
# --------------------------------------------------------------------------
for good in ("2026-09-23T18:00:00Z", "2026-09-23T18:00:00+00:00", "2026-09-23T18:00Z",
             "2026-09-23T18:00:00.500Z"):
    try:
        check("reads the ordinary ISO form %s" % good, store._parse(good) is not None)
    except store.BadTimestamp:
        check("reads the ordinary ISO form %s" % good, False, "raised")

for bad in ("tomorrow", "2026-13-99T99:99:99Z", "", None):
    if bad in ("", None):
        check("an ABSENT instant is None, not an error", store._parse(bad) is None)
        continue
    try:
        store._parse(bad)
        check("REFUSES %r" % bad, False, "parsed it")
    except store.BadTimestamp:
        check("REFUSES %r" % bad, True)

mal = {"grants": [{"repo": "/r", "sessionId": SID, "sessionName": "n",
                   "granted": "2026-09-23T10:00:00+00:00",
                   "expires": "NOT A TIME"}]}
try:
    v = store.check(mal, "/r", session_id=SID)
    check("a malformed grant DENIES rather than raising", v["granted"] is False)
    check("and the denial names it as unreadable", "UNREADABLE" in v["reason"], v["reason"][:40])
except Exception as e:
    check("a malformed grant DENIES rather than raising", False, "%s" % type(e).__name__)
    check("and the denial names it as unreadable", False)

ok_doc = {"grants": [{"repo": "/r", "sessionId": SID, "sessionName": "n",
                      "granted": "2026-09-23T10:00:00+00:00",
                      "expires": "2099-01-01T00:00:00Z"}]}
check("POSITIVE CONTROL -- the same grant with a readable expiry is granted",
      store.check(ok_doc, "/r", session_id=SID)["granted"] is True)

# --------------------------------------------------------------------------
# 2. UNRESOLVED IDENTITY is its own answer, not a mismatch.
# --------------------------------------------------------------------------
v = store.check(ok_doc, "/r", session_id=None)
check("no id -> identity unresolved, not denial-by-mismatch", v.get("identity") == "unresolved")
check("and the reason says so in those words", "IDENTITY UNRESOLVED" in v["reason"])
check("and it points at the flag that fixes it", "--session-id" in v["reason"])
check("and it says how many grants exist for the repo", v.get("grants_for_repo") == 1)
check("it does NOT claim the grant names someone else",
      "names session" not in v["reason"])
check("POSITIVE CONTROL -- with the id, the same call is granted",
      store.check(ok_doc, "/r", session_id=SID)["granted"] is True)

# --------------------------------------------------------------------------
# 3. resolve_target: the ref trap, ambiguity, and no match.
# --------------------------------------------------------------------------
sess = os.path.join(d, "sessions")
os.makedirs(sess)


def seed(name, sid, cwd="/tmp"):
    with open(os.path.join(sess, sid[:8] + ".json"), "w", encoding="utf-8") as fh:
        json.dump({"name": name, "sessionId": sid, "cwd": cwd}, fh)


seed("Legolas", SID)
seed("Twin", "11111111-0000-0000-0000-000000000001")
seed("Twin", "22222222-0000-0000-0000-000000000002")
G = os.path.join(sess, "*.json")

r, err = store.resolve_target("Legolas", G)
check("resolves a session by NAME", err is None and r["sessionId"] == SID)
r, err = store.resolve_target("0b1e9641", G)
check("resolves by an id PREFIX of 8+", err is None and r["sessionId"] == SID)
r, err = store.resolve_target(SID, G)
check("resolves a full id", err is None and r["sessionId"] == SID)

r, err = store.resolve_target("f6b1b6", G)
check("REFUSES a bare six-hex argument -- that is a ListAgents ref", r is None and err)
check("and the refusal explains a ref is not an id prefix", "not a prefix" in (err or "").lower())
check("POSITIVE CONTROL -- 8 hex chars of a real id resolves",
      store.resolve_target("0b1e9641", G)[0] is not None)

r, err = store.resolve_target("Twin", G)
check("REFUSES an ambiguous name", r is None and "not unique" in (err or ""))
r, err = store.resolve_target("nobody", G)
check("REFUSES a name matching nothing", r is None and "no session matches" in (err or ""))

# --------------------------------------------------------------------------
# 4. append: never replaces, refuses a store it cannot read, refuses a race.
# --------------------------------------------------------------------------
sp = os.path.join(d, "store.json")
g1 = {"repo": "/r", "sessionId": SID, "granted": "2026-09-23T10:00:00Z",
      "expires": "2099-01-01T00:00:00Z", "effect": "allow"}
store.append(g1, sp)
store.append(dict(g1, action="push"), sp)
held = json.load(open(sp))["grants"]
check("append ADDS rather than replacing", len(held) == 2)
check("and the first grant survived the second write", held[0].get("action") is None)

bad_store = os.path.join(d, "bad.json")
open(bad_store, "w").write("{not json")
try:
    store.append(g1, bad_store)
    check("REFUSES to write over an unreadable store", False, "wrote anyway")
except ValueError as e:
    check("REFUSES to write over an unreadable store", True)
    check("and says why -- it would drop grants in force", "drop every grant" in str(e))
check("and the unreadable store is untouched", open(bad_store).read() == "{not json")

raced = os.path.join(d, "raced.json")
json.dump({"grants": []}, open(raced, "w"))
_real = store.safewrite.unchanged_since
store.safewrite.unchanged_since = lambda p, t: (False, "mtime moved")
try:
    store.append(g1, raced)
    check("REFUSES when the store changed mid-write", False, "wrote anyway")
except ValueError as e:
    check("REFUSES when the store changed mid-write", "changed while" in str(e))
store.safewrite.unchanged_since = _real
check("POSITIVE CONTROL -- with no race the same append lands",
      store.append(g1, raced)[1] == 0)

# A write must not revive what has already lapsed. The ephemeral store's TTL is
# enforced on the FILE's mtime, so rewriting it refreshes every stale grant
# inside -- which is exactly what happened on this function's first real use.
rev = os.path.join(d, "revive.json")
json.dump({"grants": [
    dict(g1, expires="2026-09-04T00:00:00Z", action="old"),
    dict(g1, expires="2099-01-01T00:00:00Z", action="live"),
    {"repo": "/r", "sessionId": SID, "granted": "2026-09-23T10:00:00Z",
     "expiresAfterChecks": 1, "action": "consuming"},
    dict(g1, expires="NOT A TIME", action="malformed"),
]}, open(rev, "w"))
_p, kept, lapsed = store.append(dict(g1, action="new"), rev)
held = {g.get("action") for g in json.load(open(rev))["grants"]}
check("a write PRUNES grants that have already lapsed", lapsed == 1 and "old" not in held)
check("and reports how many it dropped", lapsed == 1)
check("POSITIVE CONTROL -- a live grant survives the same write", "live" in held)
check("a consuming grant with no clock survives", "consuming" in held)
check("a MALFORMED grant is kept, not silently deleted", "malformed" in held)
check("and the new grant landed", "new" in held)

# --------------------------------------------------------------------------
# 5. issue end to end: the grant it writes is a grant `check` grants.
# --------------------------------------------------------------------------
out = os.path.join(d, "issued.json")
store.SESSIONS_GLOB = G
rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "2h",
               "--granted-by", "Micaiah", "--note", "under test", "--store", out])
check("issue exits 0 on success", rc == 0)
doc = store.load(out)
check("it wrote exactly one grant", len(doc["grants"]) == 1)
w = doc["grants"][0]
check("keyed on the full sessionId, not a name", w["sessionId"] == SID)
check("effect is written EXPLICITLY, not left implicit", w["effect"] == "allow")
check("no decorative `scope` field", "scope" not in w)
v = store.check(doc, os.path.abspath(d), session_id=SID, session_name="Legolas")
check("AND THE EVALUATOR GRANTS WHAT THE ISSUER WROTE", v["granted"] is True)
check("POSITIVE CONTROL -- another session is still denied",
      store.check(doc, os.path.abspath(d), session_id=MINE)["granted"] is False)

rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "2h",
               "--store", out, "--dry-run"])
check("--dry-run exits 0", rc == 0)
check("and writes NOTHING", len(store.load(out)["grants"]) == 1)

rc = cli.main(["issue", "--repo", d, "--session", "Legolas"])
check("REFUSES a grant with no end", rc == 2)
rc = cli.main(["issue", "--repo", os.path.join(d, "nope"), "--session", "Legolas", "--for", "1h"])
check("REFUSES a repo path that does not exist", rc == 2)
rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "1h",
               "--expires", "2099-01-01T00:00:00Z"])
check("REFUSES --for and --expires together", rc == 2)
rc = cli.main(["issue", "--repo", d, "--session", "Legolas",
               "--expires", "2000-01-01T00:00:00Z"])
check("REFUSES an expiry already in the past", rc == 2)

# self-grant guard
store.current_session = lambda *a, **k: {"sessionId": SID, "name": "Legolas"}
rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "1h", "--store", out])
check("REFUSES issuing to the calling session", rc == 2)
rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "1h",
               "--store", out, "--self-grant"])
check("--self-grant still REFUSES without a note", rc == 2)
rc = cli.main(["issue", "--repo", d, "--session", "Legolas", "--for", "1h",
               "--store", out, "--self-grant", "--note", "deliberate"])
check("POSITIVE CONTROL -- --self-grant with a note is allowed through", rc == 0)
store.current_session = lambda *a, **k: None
check("and the note is in the record",
      store.load(out)["grants"][-1].get("note") == "deliberate")

# --------------------------------------------------------------------------
# 6. The store is RUNTIME state: wiped on restart, per repository, private.
# --------------------------------------------------------------------------
check("project_name follows the .claude/projects convention",
      store.project_name("/home/cragady/dev2/Cascade/SOPIA")
      == "-home-cragady-dev2-Cascade-SOPIA")
check("and a dot becomes a dash the same way",
      store.project_name("/home/cragady/.claude/jobs") == "-home-cragady--claude-jobs")

_env = dict(os.environ)
os.environ.pop("CAI_GRANT_ROOT", None)
os.environ["XDG_RUNTIME_DIR"] = d
check("prefers XDG_RUNTIME_DIR -- tmpfs, uid-scoped, mode 700",
      store.runtime_root() == os.path.join(d, "cai-tools", "grants"))
os.environ.pop("XDG_RUNTIME_DIR")
fallback = store.runtime_root()
check("falls back to a UID-SCOPED dir under the system temp dir",
      "cai-tools-" in fallback and fallback.endswith("grants"),
      fallback)
check("the fallback is NOT a bare shared path", "/tmp/cai-tools/grants" != fallback)
os.environ.clear()
os.environ.update(_env)

check("stores are partitioned by repository, not by session",
      store.store_for("/a/one") != store.store_for("/a/two"))

# A world-writable store is a store anybody can grant from.
unsafe = os.path.join(d, "unsafe")
os.makedirs(unsafe, exist_ok=True)
os.chmod(unsafe, 0o777)
check("root_problem REFUSES a group/other-writable directory",
      "writable by group or other" in (store.root_problem(unsafe) or ""))
os.chmod(unsafe, 0o700)
check("POSITIVE CONTROL -- the same directory at 0700 is fine",
      store.root_problem(unsafe) is None)
check("and a directory that does not exist yet is not a problem",
      store.root_problem(os.path.join(d, "nope")) is None)

os.environ["CAI_GRANT_ROOT"] = unsafe
os.chmod(unsafe, 0o777)
_bad = store.load(repo="/a/one")
check("load returns NO GRANTS when the store dir is unsafe", _bad["grants"] == [])
check("and says why rather than failing silently", "refusing to read" in (_bad.get("_note") or ""))
try:
    store.append({"repo": "/a/one"}, os.path.join(unsafe, "x.json"))
    check("append REFUSES to write into an unsafe directory", False, "wrote anyway")
except ValueError as e:
    check("append REFUSES to write into an unsafe directory", "somebody else can write" in str(e))
os.chmod(unsafe, 0o700)
check("POSITIVE CONTROL -- at 0700 the same append lands",
      store.append({"repo": "/a/one", "granted": "2026-09-23T10:00:00Z",
                    "expires": "2099-01-01T00:00:00Z"},
                   os.path.join(unsafe, "x.json"))[1] == 0)
check("and the store file is 0600, not world-readable",
      (os.stat(os.path.join(unsafe, "x.json")).st_mode & 0o077) == 0)
os.environ.pop("CAI_GRANT_ROOT")

# --------------------------------------------------------------------------
# 7. PATH SCOPE: "only this file" must not become "anything, if unmentioned".
# --------------------------------------------------------------------------
check("a declared file covers itself",
      store.path_covered(["docs/grant-shape.md"], ["docs/grant-shape.md"])[0])
check("a declared directory covers what is under it",
      store.path_covered(["docs"], ["docs/a/b.md"])[0])
check("REFUSES a path outside the scope",
      store.path_covered(["docs/grant-shape.md"], ["docs/other.md"])[0] is False)
check("and matches on SEGMENTS, so `docs/grant` does not cover `docs/grant-shape.md`",
      store.path_covered(["docs/grant"], ["docs/grant-shape.md"])[0] is False)

pdoc = {"grants": [{"repo": "/r", "sessionId": SID, "sessionName": "n", "action": "commit",
                    "paths": ["docs/grant-shape.md"], "granted": "2026-09-23T10:00:00Z",
                    "expires": "2099-01-01T00:00:00Z"}]}
v = store.check(pdoc, "/r", session_id=SID, action="commit", paths=["docs/grant-shape.md"])
check("POSITIVE CONTROL -- the named file is granted", v["granted"] is True)
v = store.check(pdoc, "/r", session_id=SID, action="commit",
                paths=["docs/grant-shape.md", "README.md"])
check("DENIED when the change also touches something outside", v["granted"] is False)
check("and the denial names what fell outside", "README.md" in v["reason"])
v = store.check(pdoc, "/r", session_id=SID, action="commit")
check("a path-scoped grant does NOT answer a question that names no paths",
      v["granted"] is False)
check("and it says a path-scoped grant exists, so a caller knows to ask specifically",
      v.get("path_scoped_grants"))

# --------------------------------------------------------------------------
# 8. A consuming grant's key must distinguish it from its siblings.
# --------------------------------------------------------------------------
_pair = [{"repo": "/r", "sessionId": SID, "granted": "2026-09-23T19:19:45Z",
          "action": a, "expiresAfterChecks": 1, "effect": "allow",
          "expires": "2099-01-01T00:00:00Z"} for a in ("commit", "push")]
check("two actions issued in the same second have DIFFERENT consumption keys",
      store.grant_id(_pair[0]) != store.grant_id(_pair[1]))
_st = os.path.join(d, "uses.json")
v1 = store.check({"grants": _pair}, "/r", session_id=SID, action="commit", use_state=_st)
check("POSITIVE CONTROL -- the commit half is granted", v1["granted"] is True)
v2 = store.check({"grants": _pair}, "/r", session_id=SID, action="push", use_state=_st)
check("and spending the commit half does NOT spend the push half", v2["granted"] is True)
v3 = store.check({"grants": _pair}, "/r", session_id=SID, action="commit", use_state=_st)
check("while the commit half IS spent on its second check", v3["granted"] is False)
check("and says so", "spent" in v3["reason"])

# --------------------------------------------------------------------------
# 9. A preview must not spend a one-time grant.
# --------------------------------------------------------------------------
_one = {"grants": [{"repo": "/r", "sessionId": SID, "granted": "2026-09-23T19:00:00Z",
                    "action": "push", "expiresAfterChecks": 1, "effect": "allow",
                    "expires": "2099-01-01T00:00:00Z"}]}
_us = os.path.join(d, "uses2.json")
v = store.check(_one, "/r", session_id=SID, action="push", use_state=_us, consume=False)
check("a non-consuming check is granted", v["granted"] is True)
check("and says it WOULD spend, rather than spending", v.get("would_spend") is True)
check("and did not record a use", v.get("consumed_by_this_check") is None)
v = store.check(_one, "/r", session_id=SID, action="push", use_state=_us)
check("POSITIVE CONTROL -- the real check still works after the preview",
      v["granted"] is True and v.get("consumed_by_this_check") is True)
v = store.check(_one, "/r", session_id=SID, action="push", use_state=_us)
check("and NOW it is spent", v["granted"] is False and "spent" in v["reason"])

# --------------------------------------------------------------------------
# 10. Listing is not using.
# --------------------------------------------------------------------------
_lst = os.path.join(d, "listed.json")
store.SESSIONS_GLOB = G
os.environ["CAI_GRANTS"] = _lst
json.dump({"grants": [{"repo": d, "sessionId": SID, "sessionName": "Legolas",
                       "action": "push", "paths": ["docs/x.md"], "effect": "allow",
                       "granted": "2026-09-23T10:00:00Z", "expiresAfterChecks": 1,
                       "expires": "2099-01-01T00:00:00Z"}]}, open(_lst, "w"))
import io as _io
import contextlib as _ctx
_buf = _io.StringIO()
with _ctx.redirect_stdout(_buf):
    cli.main(["list"])
_rows = json.loads(_buf.getvalue())["grants"]
check("list reports a PATH-SCOPED grant instead of 'no grant recorded'",
      _rows[0]["in_force"] is True, _rows[0]["status"])
_buf2 = _io.StringIO()
with _ctx.redirect_stdout(_buf2):
    cli.main(["list"])
check("and listing twice does not SPEND a one-time grant",
      json.loads(_buf2.getvalue())["grants"][0]["in_force"] is True)
os.environ.pop("CAI_GRANTS")

# --------------------------------------------------------------------------
# 11. A key change is a MIGRATION of enforcement state.
# --------------------------------------------------------------------------
_g = {"repo": "/r", "sessionId": SID, "granted": "2026-09-23T19:19:45Z",
      "action": "commit", "effect": "allow", "paths": ["docs/x.md"],
      "expiresAfterChecks": 1, "expires": "2099-01-01T00:00:00Z"}
_legacy = os.path.join(d, "legacy-uses.json")
json.dump({store.legacy_grant_id(_g): 1}, open(_legacy, "w"))
check("the legacy key differs from the current one",
      store.legacy_grant_id(_g) != store.grant_id(_g))
check("a spend recorded under the OLD key still counts",
      store.spent_count(_g, _legacy) == 1)
v = store.check({"grants": [_g]}, "/r", session_id=SID, action="commit",
                paths=["docs/x.md"], use_state=_legacy)
check("so a grant spent before the key change is still SPENT", v["granted"] is False)
check("and says so", "spent" in v["reason"])
_fresh = os.path.join(d, "fresh-uses.json")
json.dump({}, open(_fresh, "w"))
check("POSITIVE CONTROL -- with no record at all the same grant is granted",
      store.check({"grants": [_g]}, "/r", session_id=SID, action="commit",
                  paths=["docs/x.md"], use_state=_fresh)["granted"] is True)

# --------------------------------------------------------------------------
# 12. An unbounded grant is not a recent one, and a spent grant is prunable.
# --------------------------------------------------------------------------
_open = {"repo": "/r", "sessionId": MINE, "sessionName": "other", "action": "commit",
         "granted": "2026-09-02T04:35:42Z", "expiresAfterChecks": 1, "effect": "allow"}
_live = {"repo": "/r", "sessionId": SID, "sessionName": "n", "action": "commit",
         "granted": "2026-09-23T19:19:45Z", "expires": "2099-01-01T00:00:00Z",
         "effect": "allow"}
v = store.check({"grants": [_open, _live]}, "/r", session_id=SID, action="commit",
                use_state=os.path.join(d, "u12.json"))
check("POSITIVE CONTROL -- the live grant answers", v["granted"] is True)
check("an old grant with NO expiry no longer outranks a live one",
      v.get("grant", {}).get("sessionId") == SID)

_sp = os.path.join(d, "spent-store.json")
json.dump({"grants": [dict(_open)]}, open(_sp, "w"))
_u = os.path.join(d, "u12b.json")
json.dump({store.grant_id(_open): 1}, open(_u, "w"))
_old_us = store.USE_STATE
store.USE_STATE = _u
_p, kept, dropped = store.prune(_sp)
store.USE_STATE = _old_us
check("prune drops a SPENT grant, which nothing could remove before",
      dropped == 1 and kept == 0)

# --------------------------------------------------------------------------
# 13. A dead grant must not answer for a live one -- in the verdict OR the reason.
# --------------------------------------------------------------------------
_u13 = os.path.join(d, "u13.json")
json.dump({}, open(_u13, "w"))
_dead_open = {"repo": "/r", "sessionId": "zzzz", "sessionName": "other", "action": "commit",
              "granted": "2026-09-02T04:35:42Z", "effect": "allow"}
_live13 = {"repo": "/r", "sessionId": SID, "sessionName": "me", "action": "commit",
           "granted": "2026-09-23T19:19:45Z", "expires": "2099-01-01T00:00:00Z",
           "effect": "allow"}
v = store.check({"grants": [_dead_open, _live13]}, "/r", session_id=SID,
                action="commit", use_state=_u13)
check("a dead OPEN-ENDED grant does not outrank a live one", v["granted"] is True)
check("and the LIVE grant is the one the verdict names",
      v["grant"]["sessionId"] == SID)
check("and the reason is the live grant's, not the dead one's",
      "in force until 2099" in v["reason"], v["reason"][:40])

check("a dead grant alone is still a denial",
      store.check({"grants": [_dead_open]}, "/r", session_id=SID, action="commit",
                  use_state=_u13)["granted"] is False)

_expired13 = dict(_live13, sessionId="zzzz", expires="2026-09-03T00:00:00Z")
v = store.check({"grants": [_expired13, _live13]}, "/r", session_id=SID,
                action="commit", use_state=_u13)
check("an EXPIRED grant in front of a live one does not deny it",
      v["granted"] is True and v["grant"]["sessionId"] == SID)

_paused13 = dict(_live13, sessionId="zzzz", paused=True)
v = store.check({"grants": [_paused13, _live13]}, "/r", session_id=SID,
                action="commit", use_state=_u13)
check("a PAUSED grant in front of a live one does not deny it",
      v["granted"] is True and v["grant"]["sessionId"] == SID)

_spent13 = dict(_live13, sessionId="zzzz", expiresAfterChecks=1)
json.dump({store.grant_id(_spent13): 1}, open(_u13, "w"))
v = store.check({"grants": [_spent13, _live13]}, "/r", session_id=SID,
                action="commit", use_state=_u13)
check("a SPENT grant in front of a live one does not deny it",
      v["granted"] is True and v["grant"]["sessionId"] == SID)

# The one thing that legitimately answers ahead of a live allow.
_deny13 = dict(_live13, effect="deny")
v = store.check({"grants": [_deny13, _live13]}, "/r", session_id=SID,
                action="commit", use_state=_u13)
check("POSITIVE CONTROL -- an explicit DENY still wins, which is the point",
      v["granted"] is False and "DENIED explicitly" in v["reason"])

# --------------------------------------------------------------------------
# 14. FALLTHROUGH: a grant's authority is its own terms, not its position.
# --------------------------------------------------------------------------
import itertools as _it
_base = {"repo": "/r", "sessionId": SID, "sessionName": "me", "effect": "allow"}
_pool = [
    dict(_base, action="commit", granted="2026-09-02T00:00:00Z"),          # open-ended, live
    dict(_base, action="commit", granted="2026-09-01T00:00:00Z",
         expires="2026-09-02T00:00:00Z"),                                  # expired
    dict(_base, action="commit", sessionId="zzzz", granted="2026-09-01T00:00:00Z"),  # not me
    dict(_base, action="commit", granted="2026-09-01T00:00:00Z", paused=True),       # paused
]
_u14 = os.path.join(d, "u14.json")
_seen = set()
for perm in _it.permutations(_pool):
    json.dump({}, open(_u14, "w"))
    v = store.check({"grants": list(perm)}, "/r", session_id=SID, action="commit",
                    use_state=_u14)
    _seen.add(v["granted"])
check("24 orderings of the same grants give ONE answer", _seen == {True}, str(_seen))

# A deny must win from any position, not because it sorted first.
_denypool = [dict(_base, action="commit", granted="2026-09-02T00:00:00Z"),
             dict(_base, action="commit", granted="2026-09-02T00:00:00Z", effect="deny")]
_seen2 = {store.check({"grants": list(p)}, "/r", session_id=SID, action="commit",
                      use_state=_u14)["granted"]
          for p in _it.permutations(_denypool)}
check("a DENY wins from either position", _seen2 == {False}, str(_seen2))

# Nothing is blocked by a neighbour: a dead grant never suppresses a live one.
_seen3 = set()
for i in (1, 2, 3):
    for pair in ([_pool[i], _pool[0]], [_pool[0], _pool[i]]):
        _seen3.add(store.check({"grants": pair}, "/r", session_id=SID,
                               action="commit", use_state=_u14)["granted"])
check("no dead grant blocks a live one, in either order", _seen3 == {True}, str(_seen3))

# Consumption charges ONE grant, and prefers the one that costs nothing.
json.dump({}, open(_u14, "w"))
_standing = dict(_base, action="push", granted="2026-09-02T00:00:00Z")
_onetime = dict(_base, action="push", granted="2026-09-02T00:00:00Z",
                expiresAfterChecks=1)
for _ in range(3):
    v = store.check({"grants": [_onetime, _standing]}, "/r", session_id=SID,
                    action="push", use_state=_u14)
check("a standing grant answers rather than burning a one-time one",
      v["granted"] is True and v.get("consumed_by_this_check") is None)
check("and the one-time grant is still unspent after three checks",
      store.spent_count(_onetime, _u14) == 0)
check("POSITIVE CONTROL -- with only the one-time grant, it IS spent",
      store.check({"grants": [_onetime]}, "/r", session_id=SID, action="push",
                  use_state=_u14).get("consumed_by_this_check") is True)

# --------------------------------------------------------------------------
# 15. A spend that cannot be recorded must DENY. An unrecorded use is unlimited.
# --------------------------------------------------------------------------
import threading as _th
_c = {"repo": "/r", "sessionId": SID, "sessionName": "n", "action": "commit",
      "effect": "allow", "granted": "2026-09-23T10:00:00Z",
      "expires": "2099-01-01T00:00:00Z", "expiresAfterChecks": 1}

_ro = os.path.join(d, "readonly")
os.makedirs(_ro, exist_ok=True)
_rous = os.path.join(_ro, "u.json")
json.dump({}, open(_rous, "w"))
if hasattr(os, "geteuid") and os.geteuid() != 0:
    os.chmod(_ro, 0o555)
    v = store.check({"grants": [_c]}, "/r", session_id=SID, action="commit", use_state=_rous)
    check("a spend that cannot be written DENIES the grant", v["granted"] is False)
    check("and says an unrecorded use would be an unlimited grant",
          "unlimited grant" in v["reason"])
    os.chmod(_ro, 0o755)
    check("POSITIVE CONTROL -- the same grant is granted once the spend can be written",
          store.check({"grants": [_c]}, "/r", session_id=SID, action="commit",
                      use_state=_rous)["granted"] is True)

# Concurrent spends must not lose updates.
_cus = os.path.join(d, "concurrent.json")
json.dump({}, open(_cus, "w"))
_threads = [_th.Thread(target=store._record_use, args=("k", _cus)) for _ in range(20)]
[t.start() for t in _threads]
[t.join() for t in _threads]
check("20 concurrent spends are all recorded", store._uses(_cus).get("k") == 20,
      str(store._uses(_cus).get("k")))

# A held lock denies; a STALE one is broken rather than wedging the gate.
_lus = os.path.join(d, "locked.json")
json.dump({}, open(_lus, "w"))
open(_lus + ".lock", "w").close()
_t0, _s0 = store.LOCK_TIMEOUT, store.LOCK_STALE
store.LOCK_TIMEOUT, store.LOCK_STALE = 0.1, 9999
check("a held lock DENIES rather than granting unrecorded",
      store.check({"grants": [_c]}, "/r", session_id=SID, action="commit",
                  use_state=_lus)["granted"] is False)
store.LOCK_STALE = 0.0
check("POSITIVE CONTROL -- a STALE lock is broken, because a wedged gate gets turned off",
      store.check({"grants": [_c]}, "/r", session_id=SID, action="commit",
                  use_state=_lus)["granted"] is True)
check("and the lock is cleaned up", not os.path.exists(_lus + ".lock"))
store.LOCK_TIMEOUT, store.LOCK_STALE = _t0, _s0
check("the lock bounds are read at CALL time, so a test can turn them",
      store._use_lock.__wrapped__.__defaults__ == (None, None))

# prune removes grants, so it needs the same read-verify-write append has.
_pr = os.path.join(d, "prune-race.json")
store.append(dict(_c, action="push"), _pr)
_realu = store.safewrite.unchanged_since
store.safewrite.unchanged_since = lambda p, t: (False, "another session wrote")
try:
    store.prune(_pr)
    check("prune REFUSES when the store changed under it", False, "pruned anyway")
except ValueError as e:
    check("prune REFUSES when the store changed under it", "changed while it was being" in str(e))
store.safewrite.unchanged_since = _realu
check("POSITIVE CONTROL -- with no race the same prune runs",
      store.prune(_pr)[1] == 1)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
