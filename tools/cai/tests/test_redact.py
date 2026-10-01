"""Known-answer tests for redact, with positive controls on both known defects.

The two defects this tool produced are SOPIA CASES 15 and 16, and each has a
fixture here asserting the fix AND a control asserting the hazard was real:

  15 -- a blanker that handled a payload key as a string and skipped the same
        key where it held an object, reporting success over untouched data.
  16 -- a pattern that could not match its own target: the character before the
        colon was not a word character, so \\w+ returned a false zero.

Run: python3 tests/test_redact.py   (exit 0 = pass)
"""
import io
import json
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.redact import redactor  # noqa: E402
from cai.redact import cli  # noqa: E402
from cai.grammar import records  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


# A synthetic secret that matches no real credential; shape only.
SECRET = "DEADBEEFCAFE0123456789ABCDEF0123"          # 32 hex -> device-id shape
KEYNAME = "appv11+:authToken"                         # '+' ADJACENT to the colon: CASES 16
SID = "11111111-2222-3333-4444-555555555555"          # structural, must survive


def _transcript(d):
    """Two records: one with payload as a STRING, one as an OBJECT (CASES 15)."""
    p = os.path.join(d, "session.jsonl")
    rows = [
        {"type": "user", "uuid": "aaaa1111-2222-3333-4444-555555555555",
         "sessionId": SID, "promptId": "bbbb1111-2222-3333-4444-555555555555",
         "message": {"role": "user", "content": "look at the config"}},
        # payload as a bare string
        {"type": "assistant", "uuid": "cccc1111-2222-3333-4444-555555555555",
         "sessionId": SID, "requestId": "dddd1111-2222-3333-4444-555555555555",
         "toolUseResult": "salt=%s key=%s" % (SECRET, KEYNAME)},
        # SAME key holding an OBJECT -- the shape the old blanker skipped
        {"type": "assistant", "uuid": "eeee1111-2222-3333-4444-555555555555",
         "sessionId": SID, "requestId": "ffff1111-2222-3333-4444-555555555555",
         "toolUseResult": {"stdout": "salt=%s" % SECRET, "stderr": ""}},
    ]
    io.open(p, "w", encoding="utf-8").write("\n".join(json.dumps(r) for r in rows) + "\n")
    # **Aged, because redact refuses a live target.** A fixture written a moment
    # ago looks exactly like a session a client is holding open, which is the
    # point of the check -- so a test about anything else has to say it is not
    # that case rather than accidentally being it.
    os.utime(p, (0, 0))
    return p


# --- CASES 16: the pattern must match its own target, with a positive control ---
pat = next(p for p in redactor.CANDIDATE_PATTERNS if "oken" in p)
check("CASES 16: the credential-key pattern matches a key with '+' before the colon",
      bool(re.search(pat, KEYNAME)))
check("CASES 16 positive control: the old \\w+ form returns a false zero on it",
      not re.search(r"\b\w+:\w*[Tt]oken\w*", KEYNAME))

# --- CASES 15: both payload shapes are blanked ---
obj = {"toolUseResult": "secret-string", "nested": {"toolUseResult": {"stdout": "secret-object"}}}
blanked = redactor.blank_payloads(obj)
check("CASES 15: payload as a string is blanked", blanked["toolUseResult"] == redactor.NOTICE)
check("CASES 15: the same key holding an object is also blanked",
      blanked["nested"]["toolUseResult"]["stdout"] == redactor.NOTICE)
check("CASES 15: grammar owns the key sets (not re-declared here)",
      "toolUseResult" in records.PAYLOAD_KEYS and "promptId" in records.STRUCTURAL_KEYS)

# --- redact end to end ---
d = tempfile.mkdtemp()
t = _transcript(d)
pristine = os.path.join(d, "pristine.bak")
shutil.copy2(t, pristine)
rep = redactor.redact(t, os.path.join(d, "run1.bak"))
live = io.open(t, encoding="utf-8").read()
check("redact: the secret value is gone from the live file", SECRET not in live)
check("redact: structural identifiers survive (resume would break otherwise)",
      SID in live and "bbbb1111-2222-3333-4444-555555555555" in live)
check("redact: every line is still valid JSON", not redactor.check_json(redactor.load(t)))
check("redact: reports counts, never a value",
      SECRET not in json.dumps(rep) and rep["candidates"] >= 1)

# --- backup: fail-closed, and a copy rather than a move ---
check("backup: the original survives the copy", os.path.exists(t) and os.path.exists(os.path.join(d, "run1.bak")))
try:
    redactor.backup(t, os.path.join(d, "run1.bak"))
    check("backup: fail-closed -- refuses an existing destination", False)
except FileExistsError:
    check("backup: fail-closed -- refuses an existing destination", True)
check("backup: --force is required to overwrite",
      bool(redactor.backup(t, os.path.join(d, "run1.bak"), force=True)))
src = io.open(os.path.join(os.path.dirname(__file__), "..", "src", "cai", "redact", "redactor.py")).read()
check("backup: copies via shutil, shells out to no `cp` at all (which is how `cp -n` fails open)",
      "shutil.copy2" in src and "subprocess" not in src and "os.system" not in src)

# --- verify ---
v = redactor.verify(t, pristine)
check("verify: reports clean once the value is gone", v["clean"] and v["residual"] == 0)
check("verify: reports position and shape only, never the value",
      SECRET not in json.dumps(v))
# put the secret back to prove verify can fail
io.open(t, "a", encoding="utf-8").write(json.dumps({"type": "user", "message": {"content": SECRET}}) + "\n")
os.utime(t, (0, 0))
v2 = redactor.verify(t, pristine)
check("verify: catches a re-leak (residual, not clean)", not v2["clean"] and v2["residual"] >= 1)

# --- re-leak needs --candidates-from the pristine backup ---
d2 = tempfile.mkdtemp()
t2 = _transcript(d2)
p2 = os.path.join(d2, "pristine.bak")
shutil.copy2(t2, p2)
redactor.redact(t2, os.path.join(d2, "a.bak"))                       # clean it
io.open(t2, "a", encoding="utf-8").write(json.dumps({"x": SECRET}) + "\n")  # tooling re-leaks it
os.utime(t2, (0, 0))
r_default = redactor.redact(t2, os.path.join(d2, "b.bak"))
still = SECRET in io.open(t2, encoding="utf-8").read()
# A successful in-place run leaves its own target looking live, so a second pass
# is refused by the first one's success. Documented in redactor; aged here.
os.utime(t2, (0, 0))
r_pristine = redactor.redact(t2, os.path.join(d2, "c.bak"), candidates_from=p2)
gone = SECRET not in io.open(t2, encoding="utf-8").read()
check("re-leak: --candidates-from the pristine backup is what cleans it", gone)

# --- CLI surface ---
code = cli.main(["--man-help"])
check("cli: --man-help exits 0", code == 0)

# --- projection: removal pointed at structural categories instead of secrets ---
_SID = "pppppppp-1111-2222-3333-444444444444"
_recs = [
    {"type": "user", "uuid": "p1", "sessionId": _SID,
     "message": {"role": "user", "content": "first question"}},
    {"type": "assistant", "uuid": "p2", "parentUuid": "p1", "sessionId": _SID,
     "message": {"role": "assistant", "content": [
         {"type": "thinking", "thinking": "SECRETTHOUGHT should never be emitted"},
         {"type": "text", "text": "visible answer"},
         {"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": "ls"}}]}},
    {"type": "user", "uuid": "p3", "parentUuid": "p2", "sessionId": _SID,
     "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "t1",
                                              "content": "TOOLOUTPUT"}]}},
    {"type": "user", "uuid": "p4", "parentUuid": "p3", "sessionId": _SID,
     "isCompactSummary": True,
     "message": {"role": "user", "content": "AFTERBOUNDARY summary"}},
    {"type": "assistant", "uuid": "p5", "parentUuid": "p4", "sessionId": _SID,
     "message": {"role": "assistant", "content": [{"type": "text", "text": "post-compaction reply"}]}},
]

_conv = redactor.project(_recs)["text"]
check("project: emits conversation text", "first question" in _conv and "visible answer" in _conv)
check("project: POSITIVE CONTROL -- the thinking block exists in the input",
      any(b.get("type") == "thinking" for b in _recs[1]["message"]["content"]))
check("project: and thinking is never emitted", "SECRETTHOUGHT" not in _conv)
check("project: tool traffic excluded from the conversation selection", "TOOLOUTPUT" not in _conv)

_tools = redactor.project(_recs, select="tools")["text"]
check("project: the tools selection reaches what conversation cannot",
      "TOOLOUTPUT" in _tools and "tool_use Bash" in _tools)
check("project: and still never emits thinking", "SECRETTHOUGHT" not in _tools)
check("project: turns selection is user text only",
      "first question" in redactor.project(_recs, select="turns")["text"]
      and "visible answer" not in redactor.project(_recs, select="turns")["text"])

_since = redactor.project(_recs, since_compaction=True)
check("project: --since-compaction starts at the isCompactSummary record",
      _since["started_at_record"] == 4 and "post-compaction reply" in _since["text"])
check("project: and drops everything before it", "first question" not in _since["text"])
check("project: the boundary is a field, not a phrase",
      redactor.compaction_boundaries(_recs) == [3])

_capped = redactor.project(_recs, select="tools", max_block=4)
check("project: max_block is off by default", "truncated by --max-block" not in _tools)
check("project: and when it fires it leaves a visible mark",
      "truncated by --max-block" in _capped["text"])

# --- fence neutralisation: positional, so a mention survives and a fence does not ---
_txt = ("---- Raw Source ----\n"
        "body\n"
        "---- /Raw Source ----\n"
        "I am discussing ---- Raw Source ---- inside a sentence.\n")
_out, _n = redactor.neutralise_fences(_txt)
check("fences: a delimiter alone on a line is neutralised", _n == 2 and "neutralised: Raw Source" in _out)
check("fences: the closer is marked as a closer", "neutralised: /Raw Source" in _out)
check("fences: POSITIVE CONTROL -- a mention inside a sentence is untouched",
      "discussing ---- Raw Source ---- inside" in _out)

# --- --to makes the run non-destructive ---
_d3 = tempfile.mkdtemp()
_src = os.path.join(_d3, "src.jsonl")
with io.open(_src, "w", encoding="utf-8") as _fh:
    _fh.write("\n".join(json.dumps(r) for r in _recs) + "\n")
os.utime(_src, (0, 0))
_before = io.open(_src, encoding="utf-8").read()
_out3 = os.path.join(_d3, "out.jsonl")
_rep3 = redactor.redact(_src, None, to_path=_out3)
check("--to: writes elsewhere and reports it", _rep3["written_to"] == _out3 and not _rep3["in_place"])
check("--to: the source is byte-unchanged", io.open(_src, encoding="utf-8").read() == _before)
check("--to: and no backup is taken, because nothing was overwritten", _rep3["backup"] is None)
shutil.rmtree(_d3, ignore_errors=True)

for _d in (d, d2):
    shutil.rmtree(_d, ignore_errors=True)

def _fails(fn):
    try:
        fn()
        return False
    except Exception:
        return True


# --- the redaction map: numbered markers, and NO values -----------------------
# Owner asked for a map. The constraint is that a map naming what it replaced IS
# the secret -- a second file holding exactly what the first was cleaned of, and
# a more portable one. So it carries shape and position; the source resolves it.
_md = tempfile.mkdtemp()
_S = "7f3a9b2c-1122-3344-5566-778899aabbcc"
_T = "9999888877776666555544443333"
_src = os.path.join(_md, "t.jsonl")
open(_src, "w", encoding="utf-8").write("\n".join(json.dumps(r) for r in [
    {"type": "user", "uuid": "u1", "parentUuid": None, "sessionId": "s", "timestamp": "t",
     "message": {"role": "user", "content": "one " + _S}},
    {"type": "assistant", "uuid": "a1", "parentUuid": "u1", "sessionId": "s", "timestamp": "t",
     "message": {"role": "assistant", "content": [{"type": "text", "text": "two " + _S + " " + _T}]}},
    {"type": "user", "uuid": "u2", "parentUuid": "a1", "sessionId": "s", "timestamp": "t",
     "message": {"role": "user", "content": "three " + _S}},
]) + "\n")
_clean = os.path.join(_md, "clean.jsonl")
_map = os.path.join(_md, "map.json")
_rep = redactor.redact(_src, os.path.join(_md, "b.bak"), to_path=_clean, map_path=_map)

_ct = open(_clean, encoding="utf-8").read()
_mt = open(_map, encoding="utf-8").read()

check("THE SAFETY PROPERTY -- no redacted value appears in the map",
      _S not in _mt and _T not in _mt)
check("nor in the redacted output", _S not in _ct and _T not in _ct)
check("POSITIVE CONTROL -- the same check FINDS them in the source",
      _S in open(_src, encoding="utf-8").read())

_mj = json.loads(_mt)
check("markers are NUMBERED, so a reader can tell things apart",
      {e["marker"] for e in _mj["entries"]} == {"[REDACTED:1]", "[REDACTED:2]"})
check("the same value keeps ONE marker across the file",
      _ct.count("[REDACTED:1]") == 3)
check("and a different value gets a different one", _ct.count("[REDACTED:2]") == 1)

_uuid = [e for e in _mj["entries"] if e["kind"] == "uuid"][0]
check("the map names the KIND without naming the thing", _uuid["kind"] == "uuid")
check("carries its length, which is shape", _uuid["length"] == len(_S))
check("counts occurrences", _uuid["occurrences"] == 3)
check("and points at the lines, so the SOURCE resolves it", _uuid["lines"] == [1, 2, 3])
check("the note says plainly that no value is here",
      "No redacted VALUE appears here" in _mj["note"])

check("REGRESSION -- the map refuses to overwrite an existing file",
      _fails(lambda: redactor.redact(_src, os.path.join(_md, "b2.bak"),
                                     to_path=os.path.join(_md, "c2.jsonl"), map_path=_map)))

check("POSITIVE CONTROL -- verify still finds the output clean",
      redactor.verify(_clean, _src)["clean"] is True)

# --- REGRESSION: the original marker is the default again --------------------
# Numbering was made the default when the map was added, which silently broke
# anything matching the literal [REDACTED]. Nobody asked for that change.
_d1 = os.path.join(_md, "d1.jsonl")
redactor.redact(_src, os.path.join(_md, "b3.bak"), to_path=_d1)
check("REGRESSION -- the default marker is the ORIGINAL, unnumbered",
      "[REDACTED]" in open(_d1, encoding="utf-8").read())
check("and no numbered marker appears unless asked",
      "[REDACTED:" not in open(_d1, encoding="utf-8").read())

_d2 = os.path.join(_md, "d2.jsonl")
redactor.redact(_src, os.path.join(_md, "b4.bak"), to_path=_d2, numbered=True)
check("POSITIVE CONTROL -- --number numbers them", "[REDACTED:1]" in open(_d2, encoding="utf-8").read())

# --- the operator's substitution map: what to redact, and what it becomes -----
# Owner's actual ask. A marker erases WHAT KIND of thing was there; a chosen
# replacement keeps the sentence readable.
_sd = tempfile.mkdtemp()
_ssrc = os.path.join(_sd, "t.jsonl")
open(_ssrc, "w", encoding="utf-8").write("\n".join(json.dumps(r) for r in [
    {"type": "user", "uuid": "u1", "parentUuid": None, "sessionId": "s", "timestamp": "t",
     "message": {"role": "user", "content": "deploy to host.example.com and tell Jane Roe"}},
    {"type": "assistant", "uuid": "a1", "parentUuid": "u1", "sessionId": "s", "timestamp": "t",
     "message": {"role": "assistant",
                 "content": [{"type": "text", "text": "host.example.com up; Jane Roe told"}]}},
]) + "\n")
_sf = os.path.join(_sd, "subs.json")
open(_sf, "w", encoding="utf-8").write(
    json.dumps({"host.example.com": "<release-host>", "Jane Roe": "<person-a>"}))
_sout = os.path.join(_sd, "out.jsonl")
_srep = redactor.redact(_ssrc, os.path.join(_sd, "b.bak"), to_path=_sout,
                        substitutions_from=_sf)
_st = open(_sout, encoding="utf-8").read()

check("the named values are gone", "host.example.com" not in _st and "Jane Roe" not in _st)
check("and the CHOSEN replacements are there, so meaning survives",
      "<release-host>" in _st and "<person-a>" in _st)
check("POSITIVE CONTROL -- they were present in the source",
      "host.example.com" in open(_ssrc, encoding="utf-8").read())
check("the report counts pairs and applications, not values",
      _srep["operator_substitutions"] == {"pairs": 2, "applied": 4})
check("no substituted VALUE appears in the report",
      "Jane Roe" not in json.dumps(_srep))

_tsv = os.path.join(_sd, "subs.tsv")
open(_tsv, "w", encoding="utf-8").write("# a comment\nhost.example.com\t<h>\n\nJane Roe\t<p>\n")
check("a TAB-separated file works too, with comments and blanks skipped",
      redactor.load_substitutions(_tsv) == {"host.example.com": "<h>", "Jane Roe": "<p>"})

_ov = os.path.join(_sd, "overlap.json")
open(_ov, "w", encoding="utf-8").write(json.dumps({"host.example.com": "<long>", "host": "<short>"}))
_o2 = os.path.join(_sd, "o2.jsonl")
redactor.redact(_ssrc, os.path.join(_sd, "b2.bak"), to_path=_o2, substitutions_from=_ov)
check("longest find wins, so a shorter one cannot eat part of a longer",
      "<long>" in open(_o2, encoding="utf-8").read())

check("REGRESSION -- a map forces numbering at the LIBRARY level, not just the CLI",
      "[REDACTED:" in open(_map.replace("map.json", "clean.jsonl"), encoding="utf-8").read())

# --- redact refuses a LIVE transcript, which it only ever described ------------
# This module has carried the hazard in its comments since it was written -- an
# in-place rewrite truncates, so a concurrent append is destroyed and the backup
# cannot recover it -- and did not refuse. It described the wall without building
# it. The primitives come from trans-fairy-write rather than a third copy.
_ld = tempfile.mkdtemp()
_lsrc = os.path.join(_ld, "live.jsonl")
open(_lsrc, "w", encoding="utf-8").write(json.dumps({
    "type": "user", "uuid": "u1", "parentUuid": None,
    "sessionId": "9f9f9f9f-0000-0000-0000-000000000009", "timestamp": "t",
    "message": {"role": "user", "content": "x 7f3a9b2c-1122-3344-5566-778899aabbcc"}}) + "\n")

_live_refused = _fails(lambda: redactor.redact(_lsrc, os.path.join(_ld, "l.bak")))
check("a FRESH transcript is refused for an in-place rewrite", _live_refused)
check("and no backup was taken -- the refusal comes BEFORE the copy",
      not os.path.exists(os.path.join(_ld, "l.bak")))
check("the source is untouched", "7f3a9b2c" in open(_lsrc, encoding="utf-8").read())

check("POSITIVE CONTROL -- --to is unaffected, since it never touches the source",
      redactor.redact(_lsrc, os.path.join(_ld, "unused.bak"),
                      to_path=os.path.join(_ld, "out.jsonl"))["in_place"] is False)

os.utime(_lsrc, (0, 0))
check("POSITIVE CONTROL -- aged past the threshold, in place proceeds",
      redactor.redact(_lsrc, os.path.join(_ld, "l2.bak"))["in_place"] is True)

check("a successful in-place run leaves its own target looking live, "
      "so a second pass is refused by the first one's success",
      _fails(lambda: redactor.redact(_lsrc, os.path.join(_ld, "l3.bak"))))
check("POSITIVE CONTROL -- --to works for that second pass",
      redactor.redact(_lsrc, os.path.join(_ld, "u2.bak"),
                      to_path=os.path.join(_ld, "out2.jsonl"))["in_place"] is False)

# --- INCIDENT 2026-09-05: replacing in SERIALIZED text broke 18 real lines -----
# A candidate can BEGIN at the escaped character of a \\X pair, because the escape
# letter is inside the candidate character class. Removing it orphans the
# backslash and \\[REDACTED] is an invalid escape. check_json caught it and
# refused -- the only thing between this and a corrupted transcript.
_ed = tempfile.mkdtemp()
_esrc = os.path.join(_ed, "esc.jsonl")
_blob = "A" * 60
open(_esrc, "w", encoding="utf-8").write(json.dumps({
    "type": "user", "uuid": "u1", "parentUuid": None, "sessionId": "s", "timestamp": "t",
    "message": {"role": "user", "content": "\n" + _blob}}) + "\n")
os.utime(_esrc, (0, 0))

_eline = open(_esrc, encoding="utf-8").read()
check("the fixture really does put a candidate right after an escape",
      "\\n" + _blob[:8] in _eline)

_eout = os.path.join(_ed, "out.jsonl")
_erep = redactor.redact(_esrc, os.path.join(_ed, "e.bak"), to_path=_eout)
_written = open(_eout, encoding="utf-8").read()
_valid = True
try:
    json.loads(_written.strip())
except ValueError:
    _valid = False
check("REGRESSION -- the output is VALID JSON", _valid)
check("and the blob really was redacted", _blob not in _written)
check("while the escaped newline survived as an escape",
      json.loads(_written.strip())["message"]["content"].startswith("\n"))

# POSITIVE CONTROL: the old approach on the same line produces the failure.
import re as _re  # noqa: E402
_m = _re.search(r"[A-Za-z0-9+/]{48,}={0,2}", _eline)
_old = _eline[:_m.start()] + "[REDACTED]" + _eline[_m.end():]
_broke = False
try:
    json.loads(_old.strip())
except ValueError as e:
    _broke = "Invalid \\escape" in str(e)
check("POSITIVE CONTROL -- replacing in serialized text DOES break it", _broke)
check("and the match began at the escape letter, not after it",
      _eline[_m.start() - 1] == "\\")

# --- read-verify-write: DETECT the race, not just predict it ------------------
# liveness asks "does this look like a file a client is holding open" -- an mtime
# heuristic. A stat token taken before the read and checked before the write asks
# the answerable question: did anything change while I was working. It is the
# check a backup cannot substitute for, because a backup holds the pre-read state
# and therefore never contains the write it raced.
_rd = tempfile.mkdtemp()
_rsrc = os.path.join(_rd, "t.jsonl")


def _mk_race():
    open(_rsrc, "w", encoding="utf-8").write(json.dumps({
        "type": "user", "uuid": "11111111-1111-1111-1111-111111111111", "parentUuid": None,
        "sessionId": "9f9f9f9f-0000-0000-0000-00000000000e", "timestamp": "t",
        "message": {"role": "user", "content": "x 7f3a9b2c-1122-3344-5566-778899aabbcc"}}) + "\n")
    os.utime(_rsrc, (0, 0))


_mk_race()
_real_backup = redactor.backup


def _racing_backup(target, dest, force=False):
    out = _real_backup(target, dest, force=force)
    with open(target, "a", encoding="utf-8") as fh:
        fh.write(json.dumps({"type": "user", "message": {"content": "CLIENT WROTE"}}) + "\n")
    return out


redactor.backup = _racing_backup
_raced = _fails(lambda: redactor.redact(_rsrc, os.path.join(_rd, "r.bak")))
redactor.backup = _real_backup
check("a write that lands between the read and the write is REFUSED", _raced)
check("the racing write survives", "CLIENT WROTE" in open(_rsrc, encoding="utf-8").read())
check("and the BACKUP does not contain it -- which is why refusing is the only fix",
      "CLIENT WROTE" not in open(os.path.join(_rd, "r.bak"), encoding="utf-8").read())

os.utime(_rsrc, (0, 0))
check("POSITIVE CONTROL -- with no race it proceeds",
      redactor.redact(_rsrc, os.path.join(_rd, "r2.bak"))["in_place"] is True)

# The token catches replacement-by-rename too, not only appends.
from cai import safewrite as _sw  # noqa: E402
_tok = _sw.stat_token(_rsrc)
check("an unchanged file passes", _sw.unchanged_since(_rsrc, _tok)[0] is True)
import shutil as _sh  # noqa: E402
_other = os.path.join(_rd, "other.jsonl")
open(_other, "w", encoding="utf-8").write("{}\n")
_sh.move(_other, _rsrc)
check("REPLACEMENT by rename is caught, not just appending",
      _sw.unchanged_since(_rsrc, _tok)[0] is False)
check("a missing file refuses rather than guessing",
      _sw.unchanged_since(os.path.join(_rd, "nope"), _tok)[0] is False)

# --- liveness compares ids as IDS -------------------------------------------
# `sid in fh.read()` matched a one-character sessionId against every session file
# on the machine. Third instance of substring-versus-id in two days.
_sd2 = tempfile.mkdtemp()
_short = os.path.join(_sd2, "short.jsonl")
open(_short, "w", encoding="utf-8").write(json.dumps({
    "type": "user", "uuid": "u1", "sessionId": "s", "message": {"role": "user", "content": "x"}}) + "\n")
os.utime(_short, (0, 0))
_sess = os.path.join(_sd2, "sessions")
os.makedirs(_sess, exist_ok=True)
open(os.path.join(_sess, "a.json"), "w", encoding="utf-8").write(
    json.dumps({"sessionId": "totally-unrelated-session", "name": "n"}))
from cai.transfairywrite import writer as _tfw  # noqa: E402
check("REGRESSION -- a one-character id does not match an unrelated session",
      _tfw.liveness(_short, sessions_glob=os.path.join(_sess, "*.json")) == [])
open(os.path.join(_sess, "b.json"), "w", encoding="utf-8").write(
    json.dumps({"sessionId": "s", "name": "n"}))
check("POSITIVE CONTROL -- an exact id match still reports live",
      any("listed live" in r
          for r in _tfw.liveness(_short, sessions_glob=os.path.join(_sess, "*.json"))))

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
