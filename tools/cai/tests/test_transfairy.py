"""Known-answer tests for trans-fairy's deterministic core: pool + build.

Run: python3 tests/test_transfairy.py   (exit 0 = pass)
The stages that touch the filesystem/projects dir are exercised by the
end-to-end path, not here; this covers the logic that must never drift.
"""
import io
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.transfairy import build as buildmod  # noqa: E402
from cai.transfairy import pool as poolmod    # noqa: E402
from cai.transfairy import paths              # noqa: E402

fails = []


_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


CONV = {"uuid": "conv-fixture-0001", "name": "T", "chat_messages": [
    {"uuid": "m1", "parent_message_uuid": "ROOT", "sender": "human",
     "created_at": "2026-08-28T06:19:12.353380Z",
     "content": [{"type": "text", "text": "Remember FALCON."}]},
    {"uuid": "m2", "parent_message_uuid": "m1", "sender": "assistant",
     "created_at": "2026-08-28T06:19:20.000000Z",
     "content": [{"type": "text", "text": "Noted FALCON."}]},
    {"uuid": "x", "parent_message_uuid": "m1", "sender": "assistant",
     "created_at": "2026-08-28T06:19:21.000000Z",
     "content": [{"type": "text", "text": "off-branch, shorter"}]},
]}


def _pool(d):
    return poolmod.Pool(os.path.join(d, "uuid-pool.txt"))


# --- pool determinism ---
d = tempfile.mkdtemp()
p = _pool(d)
p.ensure(5)
ids = list(p._ids)
p2 = _pool(d)
_, add = p2.ensure(3)
check("pool: append-when-long touches nothing", add == 0 and p2._ids == ids)
_, add = p2.ensure(8)
check("pool: append-when-short preserves the prefix", p2._ids[:5] == ids and add == 3)
check("pool: first id (SID) is byte-stable across reopens", _pool(d)._ids[0] == ids[0])

# --- build determinism + verification ---
sid1, lines1, stats = buildmod.build(CONV, _pool(tempfile.mkdtemp() + "/x"), "/c", "2.1.236", "claude-opus-5")
# same pool -> same SID
dd = tempfile.mkdtemp()
sA, _, _ = buildmod.build(CONV, _pool(dd), "/c", "2.1.236", "claude-opus-5")
sB, _, _ = buildmod.build(CONV, _pool(dd), "/c", "2.1.236", "claude-opus-5")
check("build: same pool yields same SID (determinism)", sA == sB)
check("build: off-branch message dropped", stats["off_branch_dropped"] == 1)
checks = dict((n, ok) for n, ok, _ in buildmod.verify(lines1))
check("build: all six verification checks pass", all(checks.values()), str(checks))
_bb = [x for x in lines1 if x.get("subtype") == "build-boundary"]
check("build: records where the chain begins -- the source conversation",
      len(_bb) == 1 and _bb[0]["sourceConversationUuid"] is not None and _bb[0]["isMeta"] is True)
check("build: the provenance record is appended, not inserted (order untouched)",
      next(x["type"] for x in lines1 if x.get("type") in ("user", "assistant")) == "user")

# --- the empty-assistant skip is disclosed, and verify catches the adjacency it can cause ---
conv2 = {"name": "T2", "chat_messages": [
    {"uuid": "a", "parent_message_uuid": "ROOT", "sender": "human", "created_at": "2026-08-28T06:00:00.000000Z",
     "content": [{"type": "text", "text": "one"}]},
    {"uuid": "b", "parent_message_uuid": "a", "sender": "assistant", "created_at": "2026-08-28T06:00:01.000000Z",
     "content": [{"type": "thinking", "thinking": "hidden"}]},  # folds to empty
    {"uuid": "c", "parent_message_uuid": "b", "sender": "human", "created_at": "2026-08-28T06:00:02.000000Z",
     "content": [{"type": "text", "text": "two"}]},
]}
sid2, lines2, stats2 = buildmod.build(conv2, _pool(tempfile.mkdtemp() + "/x"), "/c", "2.1.236", "claude-opus-5")
check("build: empty assistant is counted, not silently dropped", stats2["skipped_empty_assistant"] == 1)
alt = dict((n, ok) for n, ok, _ in buildmod.verify(lines2))["no consecutive same-role turns"]
check("build: verify catches the adjacency the skip causes", alt is False)

# --- key-set conformance actually references the grammar ---
from cai.grammar import records  # noqa: E402
umsg = next(l for l in lines1 if l["type"] == "user")
check("build: user record keys == grammar USER_FIELDS", set(umsg.keys()) == set(records.USER_FIELDS))

# --- slug matches observed Claude Code behavior (/ and . -> -) ---
check("paths: slug replaces / and .", paths.slug("/home/x/.cfg/p") == "-home-x--cfg-p")

# --- graft + inject (pure functions) ---
from cai.transfairy import graft as G  # noqa: E402
import copy as _copy  # noqa: E402


def _built(name, msgs):
    return buildmod.build({"name": name, "chat_messages": msgs},
                          _pool(tempfile.mkdtemp()), "/c", "2.1.236", "claude-opus-5")[1]


_BASE = _built("base", [
    {"uuid": "b1", "parent_message_uuid": "R", "sender": "human", "created_at": "2026-08-28T06:00:00.000000Z",
     "content": [{"type": "text", "text": "base q"}]},
    {"uuid": "b2", "parent_message_uuid": "b1", "sender": "assistant", "created_at": "2026-08-28T06:00:01.000000Z",
     "content": [{"type": "text", "text": "base a"}]}])
_GRAFT = _built("graft", [
    {"uuid": "g1", "parent_message_uuid": "R", "sender": "human", "created_at": "2026-08-28T07:00:00.000000Z",
     "content": [{"type": "text", "text": "graft q"}]},
    {"uuid": "g2", "parent_message_uuid": "g1", "sender": "assistant", "created_at": "2026-08-28T07:00:01.000000Z",
     "content": [{"type": "text", "text": "graft a"}]}])
_bsid = _BASE[0]["sessionId"]
gsid, gout = G.graft(_copy.deepcopy(_BASE), _copy.deepcopy(_GRAFT), "2026-08-28T21:56:00Z", "after")
check("graft: new sessionId distinct from base", gsid != _bsid)
check("graft: base not modified in place", _BASE[0]["sessionId"] == _bsid)
check("graft: all sessionId rewritten to new", all(l.get("sessionId", gsid) == gsid for l in gout))
check("graft: seam carries lineage", any(l.get("subtype") == "graft-boundary" and l["baseSessionId"] == _bsid for l in gout))
check("graft: grafted rows marked origin.kind=graft", all(m.get("origin", {}).get("kind") == "graft" for m in G.messages(gout) if m["uuid"] in ("g1", "g2")))
check("graft: output passes verification", all(ok for _, ok, _ in buildmod.verify(gout)))

src = {"kind": "repo-sweep", "repo": "/p", "commit": "abc123"}
_, loud = G.mark_injected(_copy.deepcopy(_GRAFT), "2026-08-28T21:56:00Z", "loud", src)
check("inject loud: inject-boundary open+close isMeta", sum(1 for l in loud if l.get("subtype") == "inject-boundary" and l["isMeta"]) == 2)
check("inject loud: content marked origin.kind=inject", all(m.get("origin", {}).get("kind") == "inject" for m in G.messages(loud)))
_, silent = G.mark_injected(_copy.deepcopy(_GRAFT), "2026-08-28T21:56:00Z", "silent", src)
check("inject silent: no boundary, no origin marker",
      not any(l.get("subtype") == "inject-boundary" for l in silent)
      and not any(m.get("origin", {}).get("kind") == "inject" for m in G.messages(silent)))

# --- state --split (filesystem) ---
from cai.transfairy import stages as _S  # noqa: E402
from cai.transfairy.paths import Tree as _Tree  # noqa: E402
import shutil as _sh  # noqa: E402

_tc = tempfile.mkdtemp()
_wr = tempfile.mkdtemp()
_dr = tempfile.mkdtemp()
_tree = _Tree(_tc, work_root_override=_wr, data_root_override=_dr)
os.makedirs(os.path.join(_tree.projects_dir, "memory"))
io.open(os.path.join(_tree.projects_dir, "s.jsonl"), "w").write("{}\n")
io.open(os.path.join(_tree.projects_dir, "memory", "MEMORY.md"), "w").write("m\n")
io.open(os.path.join(_tree.projects_dir, "CLAUDE.md"), "w").write("c\n")
r1 = _S.state_split(_tree, "working-agent")
check("state --split: memory + CLAUDE.md copied, carry-forward seeded",
      r1["copied"]["memory"] == ["MEMORY.md"] and r1["copied"]["claude_md"]
      and os.path.exists(r1["copied"]["carry_forward"]))
check("state --split: transcripts excluded by default", r1["copied"]["transcripts"] == [])
r2 = _S.state_split(_tree, "working-agent", include=("transcripts",))
check("state --split: --include transcripts includes them", r2["copied"]["transcripts"] == ["s.jsonl"])
check("state --split: project dir intact (copied, not moved)", os.path.exists(os.path.join(_tree.projects_dir, "s.jsonl")))
try:
    _S.state_split(_tree, "bad-role")
    check("state --split: rejects an unknown role", False)
except ValueError:
    check("state --split: rejects an unknown role", True)

# --- the ledger is derived, not maintained ---
_lt = _Tree(tempfile.mkdtemp(), work_root_override=tempfile.mkdtemp(), data_root_override=tempfile.mkdtemp())
os.makedirs(_lt.projects_dir, exist_ok=True)
_lsid = "aaaaaaaa-1111-2222-3333-444444444444"
_lprev = "bbbbbbbb-1111-2222-3333-444444444444"
_lrecs = [
    {"type": "user", "uuid": "u1", "sessionId": _lsid, "message": {"role": "user", "content": "x"}},
    {"type": "assistant", "uuid": "a1", "sessionId": _lsid, "message": {"role": "assistant", "content": "y"},
     "previousSessionId": _lprev, "originSessionId": _lprev},
    {"type": "system", "subtype": "truncate-boundary", "isMeta": True, "sessionId": _lsid},
]
io.open(os.path.join(_lt.projects_dir, _lsid + ".jsonl"), "w").write(
    "\n".join(json.dumps(r) for r in _lrecs) + "\n")
_lr = _S.rebuild_ledger(_lt)
_ltext = io.open(_lr["ledger"], encoding="utf-8").read()
check("ledger: rebuilt FROM the transcripts, carrying their lineage",
      _lprev in _ltext and _lsid in _ltext and "truncate" in _ltext)
check("ledger: declares itself derived rather than maintained",
      "Derived, not maintained" in _ltext)
_first = _ltext
_S.rebuild_ledger(_lt)
check("ledger: idempotent -- a rebuild reproduces it exactly",
      io.open(_lr["ledger"], encoding="utf-8").read() == _first)
_sh.rmtree(_lt.projects_dir, ignore_errors=True)
for _d in (_tc, _wr, _dr, _tree.projects_dir):
    _sh.rmtree(_d, ignore_errors=True)

# --- tail truncation (split --truncate) ---
_tc2 = tempfile.mkdtemp(); _wr2 = tempfile.mkdtemp(); _dr2 = tempfile.mkdtemp()
_t2 = _Tree(_tc2, work_root_override=_wr2, data_root_override=_dr2)
_S.init(_t2)
_msgs = []
for _k in range(5):
    _msgs.append({"uuid": "u%d" % _k, "parent_message_uuid": ("a%d" % (_k - 1)) if _k else "R",
                  "sender": "human", "created_at": "2026-08-31T0%d:00:00.000000Z" % _k,
                  "content": [{"type": "text", "text": "q%d TOKEN%d" % (_k, _k)}]})
    _msgs.append({"uuid": "a%d" % _k, "parent_message_uuid": "u%d" % _k, "sender": "assistant",
                  "created_at": "2026-08-31T0%d:00:30.000000Z" % _k,
                  "content": [{"type": "text", "text": "a%d" % _k}]})
_sid0, _lines0, _ = buildmod.build({"name": "s", "chat_messages": _msgs},
                                   _pool(tempfile.mkdtemp()), _tc2, "2.1.236", "claude-opus-5")
_src = os.path.join(_t2.staged, _sid0 + ".jsonl")
io.open(_src, "w").write("\n".join(json.dumps(l) for l in _lines0) + "\n")
_r = _S.truncate(_t2, _src, before_text="TOKEN3")
_out = [json.loads(l) for l in io.open(_r["staged"])]
_txt = json.dumps(_out)
check("truncate: the prefix survives the cut", "TOKEN2" in _txt)
check("truncate: nothing past the cut is present (provable)", "TOKEN3" not in _txt and "TOKEN4" not in _txt)
check("truncate: mints a new sessionId (installs beside, never replaces)", _r["sessionId"] != _sid0)
check("truncate: the source is not modified", len(io.open(_src).readlines()) == len(_lines0))
check("truncate: the result still verifies", all(ok for _, ok, _ in buildmod.verify(_out)))
_bnd = [x for x in _out if x.get("subtype") == "truncate-boundary"]
_vis = [x for x in _out if x.get("origin", {}).get("kind") == "truncate"]
check("truncate default (silent): lineage present, agent left blind",
      len(_bnd) == 1 and _bnd[0]["isMeta"] is True and not _vis)
_r_ann = _S.truncate(_t2, _src, before_text="TOKEN3", mode="loud")
_o_ann = [json.loads(l) for l in io.open(_r_ann["staged"])]
check("truncate --loud: adds the visible notice too (the only half the model reads, P1)",
      any(x.get("origin", {}).get("kind") == "truncate" for x in _o_ann)
      and _r_ann["appended_records"] == 2)
_r_sil = _S.truncate(_t2, _src, before_text="TOKEN3", mode="true-silent")
_o_sil = [json.loads(l) for l in io.open(_r_sil["staged"])]
check("truncate --true-silent: appends nothing at all, not even lineage",
      _r_sil["appended_records"] == 0
      and not any(x.get("subtype") == "truncate-boundary" for x in _o_sil))

def _strip_sid(r):
    r = json.loads(json.dumps(r))
    for k in ("sessionId", "session_id"):
        r.pop(k, None)
    m = r.get("message")
    if isinstance(m, dict):
        m.pop("session_id", None)
    return r


_srcrecs = [json.loads(l) for l in io.open(_src) if l.strip()]
_cut_at = next(i for i, x in enumerate(_srcrecs) if "TOKEN3" in json.dumps(x))
def _strip_lin(r):
    r = _strip_sid(r)
    r.pop("previousSessionId", None)
    r.pop("originSessionId", None)
    r.pop("includedSessionIds", None)
    return r


_pure = all(_strip_lin(_o_ann[i]) == _strip_lin(_srcrecs[i]) for i in range(_cut_at))
check("truncate: the kept prefix is identical to the source, except the enumerated marker changes", _pure)
# --- lineage: recorded at the one place an id changes ---
_lin_src = [json.loads(l) for l in io.open(_src)]
_lin_orig = _lin_src[0]["sessionId"]
_r_lin = _S.truncate(_t2, _src, before_text="TOKEN3")
_o_lin = [json.loads(l) for l in io.open(_r_lin["staged"])]
_last = [x for x in _o_lin if x.get("type") in ("user", "assistant")][-1]
check("lineage: the final message records what this was before",
      _last.get("previousSessionId") == _lin_orig)
check("lineage: and the origin it ultimately came from", _last.get("originSessionId") == _lin_orig)
_sid_g, _o_g = G.graft(_copy.deepcopy(_o_lin), _copy.deepcopy(_GRAFT), "2026-08-31T12:00:00Z", "after")
_lg = [x for x in _o_g if x.get("type") in ("user", "assistant")][-1]
check("lineage: origin survives a second operation, predecessor moves",
      _lg.get("originSessionId") == _lin_orig and _lg.get("previousSessionId") != _lin_orig)
_r_ts = _S.truncate(_t2, _src, before_text="TOKEN3", mode="true-silent")
_o_ts = [json.loads(l) for l in io.open(_r_ts["staged"])]
_sid_g2, _o_g2 = G.graft(_copy.deepcopy(_o_g), _copy.deepcopy(_GRAFT), "2026-08-31T13:00:00Z", "after")
_marked = [i for i, x in enumerate(_o_g2) if "previousSessionId" in x]
_msgidx = [i for i, x in enumerate(_o_g2) if x.get("type") in ("user", "assistant")]
check("lineage: EXACTLY ONE marker, and it is the last message (no stranded copies)",
      _marked == [_msgidx[-1]])
check("lineage: origin still intact after three chained operations",
      _o_g2[_marked[0]].get("originSessionId") == _lin_orig)
_A2 = _built("A2", [
    {"uuid": "z0", "parent_message_uuid": "R", "sender": "human", "created_at": "2026-08-31T05:00:00.000000Z",
     "content": [{"type": "text", "text": "za"}]},
    {"uuid": "z1", "parent_message_uuid": "z0", "sender": "assistant", "created_at": "2026-08-31T05:00:30.000000Z",
     "content": [{"type": "text", "text": "zb"}]}])
_base_sid = _BASE[0]["sessionId"]
_graft_sid = _GRAFT[0]["sessionId"]
_sid_i, _o_i = G.graft(_copy.deepcopy(_BASE), _copy.deepcopy(_GRAFT), "2026-08-31T12:00:00Z", "after")
_li = [x for x in _o_i if x.get("type") in ("user", "assistant")][-1]
check("included: a graft records BOTH ancestors, not just one",
      set(_li.get("includedSessionIds", [])) == {_base_sid, _graft_sid})
_sid_j, _o_j = G.graft(_copy.deepcopy(_o_i), _copy.deepcopy(_A2), "2026-08-31T13:00:00Z", "after")
_lj = [x for x in _o_j if x.get("type") in ("user", "assistant")][-1]
_inc = set(_lj.get("includedSessionIds", []))
check("included: accumulates across a chain, losing no earlier contributor",
      {_base_sid, _graft_sid, _A2[0]["sessionId"]} <= _inc)
check("included: excludes the transcript's own id", _sid_j not in _inc)
check("included: still exactly one marker record",
      sum(1 for x in _o_j if "includedSessionIds" in x) == 1)
check("included: deterministic order (sorted)", _lj["includedSessionIds"] == sorted(_inc))
check("included: omitted for a single-ancestor operation (says nothing previous does not)",
      "includedSessionIds" not in [x for x in _o_lin if x.get("type") in ("user", "assistant")][-1])
check("lineage: true-silent records none, as its name promises",
      not any("previousSessionId" in x or "originSessionId" in x for x in _o_ts))
check("truncate: everything added is APPENDED after the cut, never inserted",
      all(i >= _cut_at for i, x in enumerate(_o_ann)
          if x.get("subtype") == "truncate-boundary" or x.get("origin", {}).get("kind") == "truncate"))
# --- head truncation (split --truncate --keep suffix) ---
_rs = _S.truncate(_t2, _src, before_text="TOKEN3", keep="suffix", mode="loud")
_os = [json.loads(l) for l in io.open(_rs["staged"])]
_ts = json.dumps(_os)
check("suffix: nothing before the cut is present (provable)", "TOKEN0" not in _ts and "TOKEN1" not in _ts)
check("suffix: the tail survives the cut", "TOKEN3" in _ts and "TOKEN4" in _ts)
_msgs_s = [l for l in _os if l.get("type") in ("user", "assistant")]
_uu = {l.get("uuid") for l in _os if l.get("uuid")}
check("suffix: exactly one record is re-parented to null",
      sum(1 for l in _msgs_s if l.get("parentUuid") is None) == 1)
check("suffix: no record has a missing parent after the repair",
      not [l for l in _os if l.get("parentUuid") and l["parentUuid"] not in _uu])
check("suffix: opening frames were re-emitted",
      _os[0].get("type") == "mode" and any(l.get("type") == "permission-mode" for l in _os[:4]))
check("suffix: boundary names the two modifications and the direction",
      any(l.get("subtype") == "truncate-boundary" and l.get("keep") == "suffix"
          and len(l.get("modifications", [])) == 2 for l in _os))
check("suffix: the visible notice says suffix, not prefix",
      any("**suffix**" in json.dumps(l.get("message", {})) for l in _os))
check("suffix: mints a new sessionId distinct from the prefix cut", _rs["sessionId"] != _r["sessionId"])
check("suffix: the source is still not modified", len(io.open(_src).readlines()) == len(_lines0))
try:
    _S.truncate(_t2, _src, before_text="TOKEN3", keep="middle")
    check("suffix: rejects an unknown keep value", False)
except ValueError:
    check("suffix: rejects an unknown keep value", True)

# --- compose: invisible root + visible notice + real suffix ---
_root = _built("root", [
    {"uuid": "r1", "parent_message_uuid": "R", "sender": "human", "created_at": "2026-08-31T05:00:00.000000Z",
     "content": [{"type": "text", "text": "read the quotes dir ROOTMARK"}]},
    {"uuid": "r2", "parent_message_uuid": "r1", "sender": "assistant", "created_at": "2026-08-31T05:00:01.000000Z",
     "content": [{"type": "text", "text": "the rules say ROOTBODY"}]}])
_sufx = [json.loads(l) for l in io.open(_rs["staged"])]
_csid, _cout = G.compose_rooted(_copy.deepcopy(_root), _copy.deepcopy(_sufx),
                                "2026-08-31T21:00:00Z", {"kind": "repo-sweep", "repo": "/p"})
_cmsgs = G.messages(_cout)
_ctxt = json.dumps(_cout)
check("compose: the root content is present (it must condition)", "ROOTBODY" in _ctxt)
check("compose: the root is marked inject, and only the root",
      sum(1 for m in _cmsgs if m.get("origin", {}).get("kind") == "inject") == 2)
check("compose: the root span is bracketed open and close in meta",
      sum(1 for l in _cout if l.get("subtype") == "inject-boundary") == 2)
check("compose: the visible notice exists and is a message the model reads",
      any(l.get("type") == "user" and "TRUNCATED" in json.dumps(l.get("message", {})) for l in _cout))
check("compose: the notice tells it not to infer the missing context",
      any("Do not infer" in json.dumps(l.get("message", {})) for l in _cout))
_first = _cmsgs[0]
check("compose: exactly one root, parented to null",
      sum(1 for m in _cmsgs if m.get("parentUuid") is None) == 1 and _first.get("parentUuid") is None)
_cu = {l.get("uuid") for l in _cout if l.get("uuid")}
check("compose: the chain is whole -- no record has a missing parent",
      not [l for l in _cout if l.get("parentUuid") and l["parentUuid"] not in _cu])
check("compose: the suffix survives verbatim", "TOKEN4" in _ctxt)
check("compose: sessionId rewritten everywhere",
      all(l.get("sessionId", _csid) == _csid for l in _cout))
check("compose: neither source was modified",
      _root[0].get("sessionId") != _csid and _sufx[0].get("sessionId") != _csid)
_, _tsil = G.compose_rooted(_copy.deepcopy(_root), _copy.deepcopy(_sufx),
                            "2026-08-31T21:00:00Z", {"kind": "x"}, mode="true-silent")
check("compose true-silent: no boundary and no origin marker on the root",
      not any(l.get("subtype") == "inject-boundary" for l in _tsil)
      and not any(m.get("origin", {}).get("kind") == "inject" for m in G.messages(_tsil)))

# --- compose verification: the checks the 2026-09-01 re-root ran by hand ---
def _vc(lines):
    return dict((n, ok) for n, ok, _ in G.verify_composed(lines))


_ok_pair = [{"type": "user", "uuid": "u", "sessionId": "s",
             "message": {"role": "user", "content": "hi"}},
            {"type": "assistant", "uuid": "a", "parentUuid": "u", "sessionId": "s",
             "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}}]
check("verify_composed: a clean transcript passes every check",
      all(ok for _, ok, _ in G.verify_composed(_ok_pair)))

_str_content = _copy.deepcopy(_ok_pair)
_str_content[1]["message"]["content"] = "STRING NOT LIST"
check("verify_composed: catches assistant content as a string (the defect that crashed a client)",
      not _vc(_str_content)["assistant content is a list of blocks"])

_user_str = _copy.deepcopy(_ok_pair)
check("verify_composed: a STRING on a user record is fine, so the check is role-aware",
      _vc(_user_str)["assistant content is a list of blocks"])

_orphan = [{"type": "user", "uuid": "u", "sessionId": "s",
            "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "gone"}]}}]
check("verify_composed: catches an orphan tool_result (an excision that took the use, not the result)",
      not _vc(_orphan)["no orphan tool_result"])

_twosid = _copy.deepcopy(_ok_pair)
_twosid[1]["sessionId"] = "other"
check("verify_composed: catches more than one sessionId",
      not _vc(_twosid)["exactly one sessionId"])

# --- the notice states only what the caller can know ---
_n_compose = G.truncate_notice("s", None, "/c", "2.1", "T", None, None, "u", "p",
                               keep="suffix", retained=921)["message"]["content"]
check("notice: with dropped unknown, it reports the retained count and says so",
      "921 records are present" in _n_compose and "not stated" in _n_compose)
check("notice: and never reports the retained count as though it were removed",
      "921 earlier records were removed" not in _n_compose)
_n_cut = G.truncate_notice("s", None, "/c", "2.1", "T", 8016, 8015, "u", "p",
                           keep="suffix", retained=921)["message"]["content"]
check("notice: with dropped known, it measures the past -- fixed, and unfalsifiable by any append",
      "8015 earlier records were removed" in _n_cut and "921 records are present" in _n_cut)

check("session_id_of: reads the id from a message record, not a frame",
      G.session_id_of([{"type": "system", "sessionId": "FRAME"}] + _ok_pair) == "s")

# --- compose at depth: notice leads, root sits inside the conversation ---
_dsid, _dout = G.compose_rooted(_copy.deepcopy(_root), _copy.deepcopy(_sufx),
                                "2026-08-31T21:00:00Z", {"kind": "repo-sweep"}, insert_at=2)
_dmsgs = G.messages(_dout)
_first_msg = _dmsgs[0]
check("depth: the notice leads and is the root record",
      "TRUNCATED" in json.dumps(_first_msg.get("message", {})) and _first_msg.get("parentUuid") is None)
check("depth: the leading notice says the conversation BELOW begins mid-thread",
      "conversation below" in json.dumps(_first_msg.get("message", {})))
_idx = [i for i, m in enumerate(_dmsgs) if m.get("origin", {}).get("kind") == "inject"]
check("depth: the root sits inside the conversation, not at either end",
      _idx and _idx[0] > 1 and _idx[-1] < len(_dmsgs) - 1, "root at %s of %s" % (_idx, len(_dmsgs)))
check("depth: exactly one record is parented to null",
      sum(1 for m in _dmsgs if m.get("parentUuid") is None) == 1)
_du = {l.get("uuid") for l in _dout if l.get("uuid")}
check("depth: the chain is whole across the insertion",
      not [l for l in _dout if l.get("parentUuid") and l["parentUuid"] not in _du])
check("depth: the root is still marked in meta only",
      sum(1 for l in _dout if l.get("subtype") == "inject-boundary") == 2)
try:
    G.compose_rooted(_copy.deepcopy(_root), _copy.deepcopy(_sufx), "t", {}, insert_at=999)
    check("depth: rejects an out-of-range insertion point", False)
except ValueError:
    check("depth: rejects an out-of-range insertion point", True)

_esid, _eout = G.compose_rooted(_copy.deepcopy(_root), _copy.deepcopy(_sufx),
                                "2026-08-31T21:00:00Z", {"kind": "x"}, insert_at=2,
                                notice_extra="INSTRUCTION-CANARY")
check("notice-extra: the instruction reaches the visible notice",
      any("INSTRUCTION-CANARY" in json.dumps(l.get("message", {})) for l in _eout))
_nd = G.truncate_notice("s", None, "/c", "2.1", "T", 5, 3, "u", "p")
_ne = G.truncate_notice("s", None, "/c", "2.1", "T", 5, 3, "u", "p", extra="XTRA")
_no = G.truncate_notice("s", None, "/c", "2.1", "T", 5, 3, "u", "p", only="ONLYTEXT")
_c = lambda r: r["message"]["content"]
check("notice: default describes the cut's nature", "TRUNCATED" in _c(_nd))
check("notice: extra keeps the default and appends", "TRUNCATED" in _c(_ne) and "XTRA" in _c(_ne))
check("notice: only replaces it entirely, but a notice still appears",
      _c(_no) == "ONLYTEXT" and "TRUNCATED" not in _c(_no))
check("notice-extra: it is not in any meta record",
      not any("INSTRUCTION-CANARY" in json.dumps(l) for l in _eout if l.get("isMeta")))

for _d in (_tc2, _wr2, _dr2, _t2.projects_dir):
    _sh.rmtree(_d, ignore_errors=True)

# --- compose asserts what its root IS, automatically -------------------------
def _rmsgs(sid, n, role="user"):
    out, prev = [], None
    for i in range(n):
        u = "%08x-0000-0000-0000-%012x" % (i + 1, i + 1)
        out.append({"type": role, "uuid": u, "parentUuid": prev, "sessionId": sid,
                    "timestamp": "2026-09-02T00:00:00Z", "cwd": "/x", "version": "2.1",
                    "message": {"role": role, "content": "m%d" % i}})
        prev = u
    return out


_root = _rmsgs("00000000", 2)
_suf = _rmsgs("cddb5f6c-7cb3-4a1d-a2e4-f7f58b3c060e", 3)


def _compose(mode="silent", kind="synth"):
    return G.compose_rooted(_root, _suf, "2026-09-02T00:00:00Z", {"kind": "t"},
                                mode=mode, root_kind=kind)[1]


def _rb(out):
    return [r for r in out if r.get("subtype") == "root-boundary"]


def _loud_text(out):
    return any("SYNTHESISED" in json.dumps(r.get("message", {})) for r in out)


check("silent writes the root marker automatically -- no flag required",
      len(_rb(_compose("silent"))) == 1)
check("and says nothing in message content, so the agent stays blind",
      _loud_text(_compose("silent")) is False)
check("loud writes the marker AND tells the agent",
      len(_rb(_compose("loud"))) == 1 and _loud_text(_compose("loud")) is True)
check("POSITIVE CONTROL -- true-silent skips the marker entirely",
      len(_rb(_compose("true-silent"))) == 0)
check("and true-silent says nothing either", _loud_text(_compose("true-silent")) is False)

check("the marker records the kind", _rb(_compose())[0]["rootKind"] == "synth")
check("synth-natural is a different kind",
      _rb(_compose(kind="synth-natural"))[0]["rootKind"] == "synth-natural")
check("and its loud text says the naturalness is DELIBERATE, which synth's does not",
      "deliberate" in json.dumps(_compose("loud", "synth-natural")).lower()
      and "deliberate" not in json.dumps(_compose("loud", "synth")).lower())

try:
    _compose(kind="natural")
    _nat = False
except ValueError as e:
    _nat = "build" in str(e)
check("POSITIVE CONTROL -- `natural` is refused, and says why it is build's case", _nat)

check("the marker names the placeholder id so lineage is not traced through it",
      _rb(_compose())[0]["syntheticRootSessionId"] == "00000000")
check("and says plainly not to trace through it",
      "do not trace through it" in _rb(_compose())[0]["note"])

# REGRESSION: the CLI parser referenced a module it had not imported, and every
# suite passed because none of them built the parser.
from cai.transfairy import cli as _tfcli  # noqa: E402
try:
    _tfcli.main(["compose", "--help"])
    _built = True
except (NameError, AttributeError):
    _built = False
except SystemExit:
    _built = True
check("REGRESSION -- the CLI parser actually builds, flags included", _built)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
