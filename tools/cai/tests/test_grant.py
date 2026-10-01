"""Known-answer tests for cai grant, with a positive control on every denial.

The rule this repository derived eleven times in one day: a negative is only as
good as evidence the check can return a positive. Every DENIED below is paired
with the same call succeeding when the one blocking condition is removed.

Run: python3 tests/test_grant.py   (exit 0 = pass)
"""
import datetime
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.grant import store  # noqa: E402

# **A suite that seeds sessions must own the environment.** CAI_SESSION_ID
# overrides cwd resolution, so an exported one makes every case resolve to the
# REAL session and the seeded ones stop mattering. Same isolation defect the
# hook suite hit; found again here because the gate ran the tests before the
# commit rather than beside it.
os.environ.pop("CAI_SESSION_ID", None)
from cai.grant import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


UTC = datetime.timezone.utc
REPO = "/repo/one"
SID = "aaaaaaaa-1111-2222-3333-444444444444"
OTHER = "bbbbbbbb-9999-9999-9999-999999999999"
T0 = datetime.datetime(2026, 9, 1, 12, 0, tzinfo=UTC)
d = tempfile.mkdtemp()


def doc_with(**over):
    g = {"repo": REPO, "sessionName": "sess", "sessionId": SID,
         "granted": "2026-09-01T10:00:00Z", "expires": "2026-09-01T14:00:00Z"}
    g.update(over)
    p = os.path.join(d, "grants.json")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump({"grants": [g]}, fh)
    return store.load(p)


live = doc_with()

# --- the affirmative case, so every denial below has a control ---
v = store.check(live, REPO, session_id=SID, session_name="sess", at=T0)
check("POSITIVE CONTROL -- an in-force grant is granted", v["granted"] is True)
check("and it says until when", "14:00" in v["reason"])

# --- expiry ---
late = store.check(live, REPO, session_id=SID, at=datetime.datetime(2026, 9, 1, 15, 0, tzinfo=UTC))
check("DENIED after expiry", late["granted"] is False)
check("and it reports how long ago, not merely that it lapsed",
      "EXPIRED" in late["reason"] and "minutes ago" in late["reason"])

early = store.check(live, REPO, session_id=SID, at=datetime.datetime(2026, 9, 1, 9, 0, tzinfo=UTC))
check("DENIED before the window opens", early["granted"] is False)

# --- the key: id authorises, name does not ---
wrong = store.check(live, REPO, session_id=OTHER, session_name="sess", at=T0)
check("DENIED for a different sessionId even with the right NAME", wrong["granted"] is False)
check("and says which session the grant actually names", "names session aaaaaaaa" in wrong["reason"])
check("POSITIVE CONTROL -- the same call with the right id is granted",
      store.check(live, REPO, session_id=SID, session_name="sess", at=T0)["granted"] is True)

renamed = store.check(live, REPO, session_id=SID, session_name="renamed-since", at=T0)
check("a changed NAME does not deny, because the id authorises", renamed["granted"] is True)
check("but the mismatch is surfaced rather than swallowed", "differs from" in renamed.get("note", ""))

# --- a re-root cannot inherit ---
check("DENIED to a re-rooted session, which carries a fresh id",
      store.check(live, REPO, session_id="cccccccc-0000-0000-0000-000000000000",
                  session_name="sess", at=T0)["granted"] is False)

# --- scope: another repo ---
check("DENIED for a repository the grant does not name",
      store.check(live, "/repo/other", session_id=SID, at=T0)["granted"] is False)
check("POSITIVE CONTROL -- the named repository is granted",
      store.check(live, REPO, session_id=SID, at=T0)["granted"] is True)

# --- fails closed on every source problem ---
missing = store.load(os.path.join(d, "nope.json"))
check("DENIED when the source is missing",
      store.check(missing, REPO, session_id=SID, at=T0)["granted"] is False)
bad = os.path.join(d, "bad.json")
open(bad, "w", encoding="utf-8").write("{ not json")
check("DENIED when the source is unparseable",
      store.check(store.load(bad), REPO, session_id=SID, at=T0)["granted"] is False)
check("and says it is treating the failure as no grant",
      "no grant" in store.load(bad)["_note"])
check("DENIED when no sessionId is supplied at all",
      store.check(live, REPO, session_id=None, at=T0)["granted"] is False)

# --- paused is not expired ---
paused_doc = doc_with(paused=True, pausedNote="testing the gate")
pv = store.check(paused_doc, REPO, session_id=SID, at=T0)
check("DENIED while paused", pv["granted"] is False and pv.get("paused") is True)
check("and it says paused rather than expired, since the two are different",
      "PAUSED" in pv["reason"] and "EXPIRED" not in pv["reason"])
check("the window and scope survive the pause, so lifting it re-decides nothing",
      pv["grant"]["expires"] == "2026-09-01T14:00:00Z")
check("POSITIVE CONTROL -- the same grant unpaused is granted",
      store.check(doc_with(), REPO, session_id=SID, at=T0)["granted"] is True)

# --- deny, and its precedence over a broader allow ---
def two_grants(tmp):
    p = os.path.join(tmp, "two.json")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump({"grants": [
            {"repo": REPO, "sessionId": SID, "sessionName": "sess",
             "granted": "2026-09-01T10:00:00Z", "expires": "2026-09-01T14:00:00Z"},
            {"repo": REPO, "sessionId": SID, "sessionName": "sess", "action": "risky",
             "effect": "deny", "granted": "2026-09-01T10:00:00Z"},
        ]}, fh)
    return store.load(p)


tg = two_grants(d)
us = os.path.join(d, "uses.json")
den = store.check(tg, REPO, session_id=SID, at=T0, action="risky", use_state=us)
check("an explicit deny denies", den["granted"] is False and den.get("effect") == "deny")
check("POSITIVE CONTROL -- the SAME repo-wide allow grants a different action",
      store.check(tg, REPO, session_id=SID, at=T0, action="other", use_state=us)["granted"] is True)
check("deny beats a broader allow -- the narrower statement answers",
      den["granted"] is False)
_un = store.check(tg, REPO, session_id=SID, at=T0, use_state=us)
check("an unscoped check is NOT denied by a deny on one specific action", _un["granted"] is True)
check("but it is told that action-scoped grants exist, so it knows to ask specifically",
      _un.get("action_scoped_grants") == ["deny:risky"])

# --- expiry on use ---
def consuming(tmp, n=1):
    p = os.path.join(tmp, "consume.json")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump({"grants": [
            {"repo": REPO, "sessionId": SID, "sessionName": "sess", "action": "once",
             "effect": "deny", "granted": "2026-09-01T10:00:00Z", "expiresAfterChecks": n},
        ]}, fh)
    return store.load(p)


us2 = os.path.join(d, "uses2.json")
cg = consuming(d)
first = store.check(cg, REPO, session_id=SID, at=T0, action="once", use_state=us2)
check("a consuming grant applies on its first check",
      first["granted"] is False and first.get("consumed_by_this_check") is True)
second = store.check(cg, REPO, session_id=SID, at=T0, action="once", use_state=us2)
check("POSITIVE CONTROL -- and is spent on the second, so it no longer applies",
      second["granted"] is False and "spent" in second["reason"])
check("the verdict reports how many times it was checked", second.get("spent") == 1)
check("consumption state lives OUTSIDE the grant file, which is unchanged",
      os.path.exists(us2) and "expiresAfterChecks" in open(
          os.path.join(d, "consume.json"), encoding="utf-8").read())

# --- the verdict headlines the most specific failure, and carries the rest ---
def spent_and_paused(tmp):
    p = os.path.join(tmp, "multi.json")
    with open(p, "w", encoding="utf-8") as fh:
        json.dump({"grants": [
            {"repo": REPO, "sessionId": SID, "sessionName": "sess",
             "granted": "2026-09-01T10:00:00Z", "expires": "2026-09-01T14:00:00Z",
             "paused": True},
            {"repo": REPO, "sessionId": SID, "sessionName": "sess", "action": "commit",
             "granted": "2026-09-01T10:00:00Z", "expiresAfterChecks": 1},
        ]}, fh)
    return store.load(p)


us3 = os.path.join(d, "uses3.json")
md = spent_and_paused(d)
store.check(md, REPO, session_id=SID, at=T0, action="commit", use_state=us3)   # spends it
mv = store.check(md, REPO, session_id=SID, at=T0, action="commit", use_state=us3)
check("the headline names the action-scoped grant, not whichever sorted last",
      "spent" in mv["reason"])
check("and the numbers come from the same grant the reason describes", mv.get("spent") == 1)
check("the other grant is still reported, not discarded",
      any("PAUSED" in c["reason"] for c in mv.get("also_considered", [])))

# --- the CLI ---
os.environ["CAI_GRANTS"] = os.path.join(d, "grants.json")
check("CLI exits 1 on denial",
      cli.main(["check", "--repo", REPO, "--session-id", OTHER, "--at", "2026-09-01T12:00:00Z"]) == 1)
check("POSITIVE CONTROL -- CLI exits 0 when granted",
      cli.main(["check", "--repo", REPO, "--session-id", SID, "--at", "2026-09-01T12:00:00Z"]) == 0)
check("bare invocation prints the reference", cli.main([]) == 0)
check("the help explains why it is a tool and not a heading",
      "WHY THIS IS A TOOL AND NOT A HEADING" in cli.MAN_HELP)
del os.environ["CAI_GRANTS"]

shutil.rmtree(d, ignore_errors=True)
# --- a session is not where the shell happens to be ----------------------------
_sd = tempfile.mkdtemp()
_sess = os.path.join(_sd, "s.json")
open(_sess, "w", encoding="utf-8").write(json.dumps(
    {"sessionId": "abcd1234-0000-0000-0000-000000000000", "name": "n", "cwd": "/somewhere/else"}))
_glob = os.path.join(_sd, "*.json")

_from_wrong_cwd = store.current_session(sessions_glob=_glob, cwd="/not/that/place")
check("cwd matching FAILS when the shell is elsewhere -- the original defect",
      _from_wrong_cwd is None)

_stated = store.current_session(sessions_glob=_glob, cwd="/not/that/place",
                                session_id="abcd1234")
check("a stated id identifies the session regardless of cwd",
      _stated is not None and _stated["sessionId"].startswith("abcd1234"))

os.environ["CAI_SESSION_ID"] = "abcd1234"
_env = store.current_session(sessions_glob=_glob, cwd="/not/that/place")
check("CAI_SESSION_ID does the same, for callers that cannot pass an argument",
      _env is not None)
os.environ["CAI_SESSION_ID"] = "nosuchid"
_bad = store.current_session(sessions_glob=_glob, cwd="/somewhere/else")
check("POSITIVE CONTROL -- a stated id naming no record resolves to NOTHING",
      _bad is None)
check("and it does not silently fall back to cwd matching, which would have matched here",
      store.current_session(sessions_glob=_glob, cwd="/somewhere/else") is None)
del os.environ["CAI_SESSION_ID"]
check("POSITIVE CONTROL -- with no id stated, cwd matching still works",
      store.current_session(sessions_glob=_glob, cwd="/somewhere/else") is not None)

# --- an extension appended beside the original: the RECENT window answers ------
_ext = {"grants": [
    {"repo": "/r", "sessionName": "n", "sessionId": "sid",
     "granted": "2026-09-02T05:00:00Z", "expires": "2026-09-02T07:00:00Z"},
    {"repo": "/r", "sessionName": "n", "sessionId": "sid",
     "granted": "2026-09-02T07:00:00Z", "expires": "2026-09-02T08:00:00Z"},
]}
_in = store.check(_ext, "/r", session_id="sid", at=store._parse("2026-09-02T07:30:00Z"))
check("a check inside the extension is granted", _in["granted"] is True)
check("and names the extension's window", "08:00:00Z" in _in["reason"])

_out = store.check(_ext, "/r", session_id="sid", at=store._parse("2026-09-02T08:30:00Z"))
check("REGRESSION -- after both lapse, the headline names the MOST RECENT expiry",
      "08:00:00Z" in _out["reason"] and "07:00:00Z" not in _out["reason"])
check("and it is denied", _out["granted"] is False)
check("POSITIVE CONTROL -- the older window still answers inside itself",
      store.check(_ext, "/r", session_id="sid",
                  at=store._parse("2026-09-02T06:00:00Z"))["granted"] is True)

# --- a prefix is not an id, and a denial must be readable ----------------------
_pfx = {"grants": [{"repo": "/r", "sessionName": "n",
                    "sessionId": "abcd1234-0000-0000-0000-000000000000",
                    "granted": "2026-09-03T00:00:00Z", "expires": "2026-09-03T23:00:00Z"}]}
_v = store.check(_pfx, "/r", session_id="abcd1234",
                 at=store._parse("2026-09-03T01:00:00Z"))
check("a PREFIX passed as an id is denied -- it is not the id", _v["granted"] is False)
check("REGRESSION -- and the denial says so, instead of printing two identical stubs",
      "same prefix, different values" in _v["reason"] and "A prefix is not an id" in _v["reason"])
check("POSITIVE CONTROL -- the full id is granted",
      store.check(_pfx, "/r", session_id="abcd1234-0000-0000-0000-000000000000",
                  at=store._parse("2026-09-03T01:00:00Z"))["granted"] is True)
_w = store.check(_pfx, "/r", session_id="ffff0000-0000-0000-0000-000000000000",
                 at=store._parse("2026-09-03T01:00:00Z"))
check("and a wholly different id keeps the short form, which is readable there",
      "same prefix" not in _w["reason"] and _w["granted"] is False)

# --- a full id is an id; only a prefix needs resolving ------------------------
# This session's own record vanished from ~/.claude/sessions while it was still
# running, so cwd-matching AND prefix-resolution both failed and a correctly
# stated full id became unusable. The client's session records are ITS
# bookkeeping; the grant is cai's to evaluate.
from cai.commit import cli as ccli  # noqa: E402

_full = "abcd1234-0000-0000-0000-000000000000"
check("a full id is taken as stated, with no record to consult",
      ccli._resolve_session(_full) == _full)
# REGRESSION: the rule lived in commit's CLI only, so `cai commit` and
# `cai grant check` gave opposite answers about the same session and grant.
check("REGRESSION -- the rule lives in the SHARED function, so both callers get it",
      (store.current_session(sessions_glob="/nonexistent/*.json",
                             session_id=_full) or {}).get("sessionId") == _full)
check("POSITIVE CONTROL -- a prefix still needs a record and finds none there",
      store.current_session(sessions_glob="/nonexistent/*.json",
                            session_id="abcd1234") is None)
check("POSITIVE CONTROL -- a PREFIX that resolves to nothing returns None, "
      "so the verdict says unknown rather than comparing a prefix as an id",
      ccli._resolve_session("zzzzzzzz") is None)
# **This assertion used to require the word `unknown`, and that was the bug.**
# An unresolvable session produced `names session b5d42ad9; this is unknown` --
# which reads as *the grant belongs to someone else* when the truth was *the
# question could not be answered*. A peer session acted on exactly that reading.
# Unresolved identity is now its own verdict, returned before any comparison.
_unres = store.check({"grants": [{"repo": "/r", "sessionId": _full,
                                  "sessionName": "n",
                                  "granted": "2026-01-01T00:00:00Z",
                                  "expires": "2099-01-01T00:00:00Z"}]},
                     "/r", session_id=None)
check("None resolves to IDENTITY UNRESOLVED, not a mismatch",
      _unres.get("identity") == "unresolved" and "IDENTITY UNRESOLVED" in _unres["reason"])
check("and it never claims the grant names another session",
      "names session" not in _unres["reason"])
check("POSITIVE CONTROL -- the full id is then granted",
      store.check({"grants": [{"repo": "/r", "sessionId": _full, "sessionName": "n",
                               "granted": "2026-01-01T00:00:00Z",
                               "expires": "2099-01-01T00:00:00Z"}]},
                  "/r", session_id=_full)["granted"] is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
