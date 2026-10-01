"""MAIC sessions behind cai's contract: detection, the adapter, and the tools that take them.

Added in MAIC (tools/cai/PROVENANCE.md); cai's own suites beside this one are unchanged.
One check per MAIC record type, then each tool's MAIC path, then trans-fairy-write's
backup rule: a copy before any write, a `rewritten` record for a MAIC session, and the
`restore` and `list-backups` subcommands.

Run: python3 tests/test_maic.py   (exit 0 = pass)
"""
import contextlib
import io
import json
import os
import socket
import stat
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
_state = tempfile.mkdtemp()
os.environ["XDG_STATE_HOME"] = _state
from cai.grammar import maic  # noqa: E402
from cai.read import cli as readcli  # noqa: E402
from cai.read import reader  # noqa: E402
from cai.fabricate import cli as fabcli  # noqa: E402
from cai.transfairy import stages  # noqa: E402
from cai.transfairy.paths import Tree  # noqa: E402
from cai.transfairywrite import cli as tfwcli  # noqa: E402
from cai.transfairywrite import writer  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


def run(main, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = main(argv)
    return code, out.getvalue(), err.getvalue()


def write_jsonl(path, recs):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("".join(json.dumps(r) + "\n" for r in recs))
    os.utime(path, (time.time() - 9999, time.time() - 9999))
    return path


T = "2026-01-01T12:00:00-0600"
WS = tempfile.mkdtemp()
# One record of every type MAIC writes (docs/sessions.md), in the order a session could hold them.
BY_TYPE = {
    "start": {"workspace": WS, "model": "fake/fake", "mode": "manual", "host": "h", "pid": 1},
    "msg": {"role": "user", "content": "hello"},
    "user": {"text": "hello", "provider": "fake", "model": "fake/fake", "remote": False, "mode": "manual"},
    "assistant": {"text": "hi there"},
    "tool": {"tool": "read_file", "arguments": {"path": "a.txt"}, "result": "the file", "ok": True},
    "usage": {"input": 5, "output": 2, "context": 7},
    "context": {"text": "attached a.txt"},
    "title": {"text": "fennec ears"},
    "compact": {"stage": "head", "summary": "they talked about ears", "messages": 3},
    "reset": {"messages": 2},
    "clear": {},
    "undo": {"path": "a.txt", "summary": "restored"},
    "resumed_from": {"id": "parent", "path": "/nowhere/parent.jsonl", "records": 3},
    "imported_from": {"path": "/x.jsonl", "format": "claude-code", "model": "m", "messages": 1, "skipped": 0, "malformed": 0},
    "inject": {"role": "user"},
    "graft": {"id": "g", "path": "/g.jsonl", "messages": 1},
    "compose": {"id": "c", "path": "/c.jsonl", "from": 3, "records": 4},
}
check("the table covers every record type MAIC writes", set(BY_TYPE) == set(maic.TRANSCRIPT_TYPES))

# --- detection, one record type at a time, and its projection ---
for t, fields in BY_TYPE.items():
    rec = dict(fields, type=t, time=T)
    check("detected by content: a lone `%s` record is a MAIC session" % t, maic.is_maic([rec]))
    out = maic.to_claude([rec])
    check("projected: `%s` becomes exactly one record naming its line" % t,
          len(out) == 1 and out[0]["maic"] == {"index": 0, "type": t}, json.dumps(out)[:200])
    p = out[0]
    if t in ("user", "assistant"):
        ok = p["type"] == t and reader.messages(out) == [fields["text"]]
    elif t == "tool":
        blocks = p["message"]["content"]
        ok = (p["type"] == "assistant" and [b["type"] for b in blocks] == ["tool_use", "tool_result"]
              and blocks[0]["name"] == "read_file" and blocks[1]["content"] == "the file"
              and blocks[1]["is_error"] is False)
    elif t == "context":
        ok = p["type"] == "user" and p["origin"]["kind"] == "context" and p["message"]["content"] == "attached a.txt"
    elif t == "compact":
        ok = p.get("isCompactSummary") is True and p["message"]["content"] == "they talked about ears"
    else:
        ok = (p["type"] == "system" and p["isMeta"] is True and p["subtype"] == "maic-" + t
              and all(p[k] == v for k, v in fields.items() if k not in ("type",)))
    check("projected: `%s` keeps its meaning and its fields" % t, ok, json.dumps(p)[:300])

failed_tool = maic.to_claude([dict(BY_TYPE["tool"], type="tool", ok=False, time=T)])
check("a failed tool call projects as an error result", failed_tool[0]["message"]["content"][1]["is_error"] is True)
split = maic.to_claude([dict(BY_TYPE["tool"], type="tool", time=T)], resumable=True)
check("resumable: the result is its own user record after the call, as a resumed transcript needs",
      [r["type"] for r in split] == ["assistant", "user"]
      and split[1]["message"]["content"][0]["tool_use_id"] == split[0]["message"]["content"][0]["id"])

# --- detection refuses what is not a MAIC session ---
CLAUDE = [{"type": "user", "uuid": "u", "sessionId": "s", "message": {"role": "user", "content": "hi"}},
          {"type": "assistant", "uuid": "a", "parentUuid": "u", "sessionId": "s",
           "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}}]
check("POSITIVE CONTROL -- a Claude Code transcript is not a MAIC session", not maic.is_maic(CLAUDE))
check("nor are records of no known shape", not maic.is_maic([{"type": "summary", "summary": "x"}, {"a": 1}]))
marked = [dict(BY_TYPE["user"], type="user", time=T),
          maic.marker("truncate-boundary", sessionId="new", previousSessionId="old")]
check("cai's own marker carries lineage ids by name and does not hide the session",
      maic.is_maic(marked))

# --- forks: the pointer and its record count ---
home = os.path.join(maic.sessions_dir(), "general")
parent = write_jsonl(os.path.join(home, "20260101-120000-tui-1.jsonl"),
                     [dict(BY_TYPE["start"], type="start", time=T),
                      dict(BY_TYPE["user"], type="user", time=T),
                      dict(BY_TYPE["assistant"], type="assistant", time=T),
                      dict(BY_TYPE["user"], type="user", text="after the fork point", time=T)])
fork = write_jsonl(os.path.join(home, "20260102-120000-tui-1.jsonl"),
                   [{"type": "resumed_from", "id": "20260101-120000-tui-1", "path": "/rehomed/away.jsonl",
                     "records": 3, "time": T},
                    dict(BY_TYPE["user"], type="user", text="the fork's own turn", time=T)])
flat = maic.flatten(fork)
check("a fork loads its parent's first `records` lines, found by id when the path is gone, then its own",
      [r.get("text") for r in flat if r["type"] == "user"] == ["hello", "the fork's own turn"])
code, out, _ = run(readcli.main, [fork])
check("read follows the pointer as MAIC does",
      code == 0 and "hello" in out and "the fork's own turn" in out and "after the fork point" not in out, out)

# --- read: `Ln` names the session's own line ---
sess = write_jsonl(os.path.join(home, "20260103-120000-tui-1.jsonl"),
                   [dict(BY_TYPE[t], type=t, time=T) for t in
                    ("start", "msg", "user", "tool", "assistant", "usage", "compact", "reset", "user", "assistant")])
code, out, _ = run(readcli.main, [sess])
check("read: every turn, labelled with its line in the file",
      code == 0 and "L3 · user" in out and "L5 · assistant" in out and "L7 · user" in out, out)
code, out, _ = run(readcli.main, [sess, "--select", "tools"])
check("read --select tools: the call and its result under the tool record's line",
      code == 0 and "L4 · assistant" in out and "[tool_use read_file]" in out and "[tool_result] the file" in out, out)
code, out, _ = run(readcli.main, [sess, "--before-compaction"])
check("read --before-compaction: what the compact record replaced",
      code == 0 and "L3" in out and "L9" not in out, out)
code, out, _ = run(readcli.main, [sess, "--since-compaction"])
check("read --since-compaction: the summary and what followed", code == 0 and "L7" in out and "L3" not in out, out)

# --- fabricate: cai's turns and cai's marks, as MAIC writes a turn ---
fab = os.path.join(WS, "fab.jsonl")
code, out, _ = run(fabcli.main, [sess, "--to", fab, "--user", "q", "--assistant", "a", "--at", "2", "--timestamp", T])
made = maic.load(fab)[3:7]
check("fabricate: a user and an assistant turn, each a msg and its transcript record",
      code == 0 and [(r["type"], r.get("role")) for r in made] ==
      [("msg", "user"), ("user", None), ("msg", "assistant"), ("assistant", None)], out)
check("fabricate: marked exactly as cai marks a Claude Code record",
      all(r.get("fabricated") is True and r.get("origin") == {"kind": "fabricated"} for r in made))
code, out, _ = run(fabcli.main, [fab, "--audit"])
check("fabricate --audit finds them", code == 0 and json.loads(out)["fabricated"] == 4)

# --- trans-fairy: a cut of a MAIC session is a MAIC session ---
tree = Tree(WS, work_root_override=tempfile.mkdtemp(), data_root_override=tempfile.mkdtemp(),
            claude_projects_override=os.path.join(WS, "projects"))
stages.init(tree)
res = stages.truncate(tree, sess, at_line=5)
cut = maic.load(res["staged"])
check("trans-fairy truncate: the prefix verbatim, then a cai boundary marker",
      cut[:5] == maic.load(sess)[:5] and cut[5]["type"] == "cai" and cut[5]["subtype"] == "truncate-boundary")
check("and it is still a MAIC session by content", maic.is_maic(cut))
res = stages.install(tree, staged_path=res["staged"])
check("trans-fairy install: into the named projects dir, never into ~/.claude",
      res["installed"].startswith(os.path.join(WS, "projects")) and "maic -r" in res["resume"])

# --- trans-fairy-write: the backup rule ---
target = write_jsonl(os.path.join(home, "20260104-120000-tui-1.jsonl"),
                     [dict(BY_TYPE[t], type=t, time=T) for t in ("start", "msg", "user", "assistant")])
repl = write_jsonl(os.path.join(WS, "repl.jsonl"), maic.load(target)[:3])
original = open(target, "rb").read()
bk = tempfile.mkdtemp()
code, out, err = run(tfwcli.main, [target, "--from", repl, "--backup", bk])
rep = json.loads(out) if code == 0 else {}
check("write: the report keeps cai's keys and only those",
      code == 0 and sorted(rep) == sorted(["target", "source", "backup", "sessionId", "records",
                                           "liveness_overridden", "dry_run", "checks_passed"]), out + err)
copies = writer.list_backups(target)["backups"]
check("write: a copy under sessions/.backups/<id>/ holding the file as it was",
      len(copies) == 1 and os.path.dirname(copies[0]["backup"]) ==
      os.path.join(maic.backups_dir(), "20260104-120000-tui-1")
      and open(copies[0]["backup"], "rb").read() == original)
check("write: the copy is 0600 and its directory 0700",
      stat.S_IMODE(os.stat(copies[0]["backup"]).st_mode) == 0o600
      and stat.S_IMODE(os.stat(os.path.dirname(copies[0]["backup"])).st_mode) == 0o700)
check("write: stderr says where the copy is", copies[0]["backup"] in err, err)
after = maic.load(target)
check("write: a MAIC session gets a `rewritten` record naming the copy and the invocation",
      after[:-1] == maic.load(repl) and after[-1]["type"] == "rewritten"
      and after[-1]["backup"] == copies[0]["backup"] and "--from " + repl in after[-1]["invocation"]
      and after[-1]["records_before"] == 4 and after[-1]["records_after"] == 3)
check("write: no temp file is left beside the target",
      not [f for f in os.listdir(home) if f.endswith(".cai-new")])

claude_target = write_jsonl(os.path.join(WS, "cc", CLAUDE[0]["sessionId"] + ".jsonl"), CLAUDE)
claude_repl = write_jsonl(os.path.join(WS, "cc-repl.jsonl"),
                          CLAUDE + [{"type": "user", "uuid": "u2", "parentUuid": "a", "sessionId": "s",
                                     "message": {"role": "user", "content": "more"}}])
code, out, err = run(tfwcli.main, [claude_target, "--from", claude_repl, "--backup", tempfile.mkdtemp()])
check("write: a Claude Code transcript is byte for byte what --from gave, with no record added",
      code == 0 and open(claude_target, "rb").read() == open(claude_repl, "rb").read(), out + err)
check("write: and its copy was taken all the same", len(writer.list_backups(claude_target)["backups"]) == 1)

code, out, _ = run(tfwcli.main, ["list-backups", "20260104-120000-tui"])
check("list-backups ID: by a unique prefix", code == 0 and len(json.loads(out)["backups"]) == 1, out)
code, out, _ = run(tfwcli.main, ["list-backups", "nothing-like-it"])
check("list-backups of a session with none is an empty list, exit 0", code == 0 and json.loads(out)["backups"] == [])

os.utime(target, (time.time() - 9999, time.time() - 9999))
code, out, err = run(tfwcli.main, ["restore", "20260104-120000-tui-1"])
rep = json.loads(out) if code == 0 else {}
now = maic.load(target)
check("restore ID: the newest copy is back", code == 0 and now[:-1] == maic.load(rep["restored_from"]), out + err)
check("restore: what was there was copied first",
      rep.get("backup_of_previous") and maic.load(rep["backup_of_previous"])[-1]["type"] == "rewritten")
check("restore: a `rewritten` record names the copy restored and the one taken",
      now[-1]["type"] == "rewritten" and now[-1]["restored_from"] == rep["restored_from"]
      and now[-1]["backup"] == rep["backup_of_previous"] and now[-1]["invocation"].startswith("trans-fairy-write restore"))
rows = writer.list_backups(target)["backups"]
first = rows[0]["taken"]
os.utime(target, (time.time() - 9999, time.time() - 9999))
code, out, err = run(tfwcli.main, ["restore", "20260104-120000-tui-1", "--backup", first])
check("restore --backup TS: that copy, not the newest",
      code == 0 and maic.load(target)[:-1] == maic.load(rows[0]["backup"]), out + err)
code, _, err = run(tfwcli.main, ["restore", "20260104-120000-tui-1", "--backup", "19990101T000000Z"])
check("POSITIVE CONTROL -- restore refuses a stamp no copy has", code == 1 and "match" in err, err)
same = {"backup": "x", "bytes": 0}
ordered = sorted([dict(same, taken="20261001T120000Z-1"), dict(same, taken="20261001T120000Z"),
                  dict(same, taken="20261001T120000Z-10"), dict(same, taken="20261001T115959Z")],
                 key=lambda r: (r["taken"].partition("-")[0], int(r["taken"].partition("-")[2] or 0)))
check("copies within one second sort in the order they were taken",
      [r["taken"] for r in ordered] == ["20261001T115959Z", "20261001T120000Z", "20261001T120000Z-1", "20261001T120000Z-10"])
d = os.path.join(maic.backups_dir(), "order-test")
os.makedirs(d)
for name in ("20261001T120000Z-1", "20261001T120000Z", "20261001T120000Z-10"):
    open(os.path.join(d, name + ".jsonl"), "w").close()
check("and list-backups puts them in that order",
      [r["taken"] for r in writer.list_backups("order-test")["backups"]] ==
      ["20261001T120000Z", "20261001T120000Z-1", "20261001T120000Z-10"])

# --- liveness: the start record names a running maic ---
live = write_jsonl(os.path.join(home, "20260105-120000-tui-1.jsonl"),
                   [dict(BY_TYPE["start"], type="start", host=socket.gethostname(), pid=os.getpid(), time=T),
                    dict(BY_TYPE["user"], type="user", time=T)])
check("a session whose last opener is running here looks live",
      any("still running" in r for r in writer.liveness(live)))
check("and is a live session for grant's caller lookup",
      any(r["sessionId"] == "20260105-120000-tui-1" for r in maic.live_sessions()))
check("POSITIVE CONTROL -- a dead pid does not", not maic.live_reasons([dict(BY_TYPE["start"], type="start", pid=1 << 30)]))

# --- reflow: MAIC sessions are refused like a Claude projects file; --rewrite-session ---
from cai import reflow as reflowfam  # noqa: E402
from cai.reflow import cli as reflowcli  # noqa: E402


class _Stdin(io.StringIO):
    def __init__(self, text, tty):
        super().__init__(text)
        self._tty = tty

    def isatty(self):
        return self._tty


def _bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def _reflow(argv, typed=None, tty=False):
    real = sys.stdin
    sys.stdin = _Stdin(typed or "", tty)
    try:
        return run(reflowcli.main, argv)
    finally:
        sys.stdin = real


rf_recs = [dict(BY_TYPE["start"], type="start", time=T),
           dict(BY_TYPE["msg"], type="msg", time=T), dict(BY_TYPE["user"], type="user", time=T),
           {"type": "msg", "role": "assistant", "content": "hi there", "time": T},
           dict(BY_TYPE["assistant"], type="assistant", time=T)]
rf_home = os.path.join(maic.sessions_dir(), "reflowtest")
in_home = write_jsonl(os.path.join(rf_home, "20260201-120000-tui-7.jsonl"), rf_recs)
outside = write_jsonl(os.path.join(tempfile.mkdtemp(), "copied-session.jsonl"), rf_recs)
plain = os.path.join(tempfile.mkdtemp(), "notes.txt")
with open(plain, "w", encoding="utf-8") as fh:
    fh.write("one line.\ntwo line.\n\nnext paragraph.\n")
orig_home, orig_out = _bytes(in_home), _bytes(outside)

code, out, err = _reflow([in_home])
check("reflow refuses a MAIC session in MAIC's sessions directory", code == 1 and "MAIC's sessions directory" in out, out[-200:])
check("and leaves it byte-identical", _bytes(in_home) == orig_home)
code, out, err = _reflow([outside])
check("reflow refuses a MAIC session copied elsewhere, recognised by its records",
      code == 1 and "is a MAIC session" in out and _bytes(outside) == orig_out, out[-200:])
code, out, err = _reflow([plain])
check("a plain text file is reflowed exactly as before", code == 0 and json.loads(out)["refused"] == 0, out[-200:])

code, out, err = _reflow(["-n", in_home])
check("a dry run on a MAIC session reports that the result would not load",
      "would NOT load as a MAIC session" in err and _bytes(in_home) == orig_home, err[-200:])
code, out, err = _reflow(["--rewrite-session", "-n", in_home])
check("with --rewrite-session the invalid dry run exits 1 and writes nothing", code == 1 and _bytes(in_home) == orig_home)

code, out, err = _reflow(["--rewrite-session", in_home], typed="yes, rewrite it\n", tty=False)
check("--rewrite-session off a terminal refuses and writes nothing",
      code == 1 and "needs a person at a terminal" in out and _bytes(in_home) == orig_home, out[-200:])
check("and recommends the dry run with the exact command", ("cai reflow --rewrite-session %s --dry-run" % in_home) in err, err[-300:])
code, out, err = _reflow(["--rewrite-session", in_home], typed="yes\n", tty=True)
check("the wrong phrase aborts and writes nothing", code == 1 and "not confirmed" in out and _bytes(in_home) == orig_home)
check("the validation verdict is shown before the question", err.find("validation:") != -1 and err.find("validation:") < err.find("type 'yes, rewrite it'"))

before = len(writer.list_backups("20260201-120000-tui-7")["backups"])
code, out, err = _reflow(["--rewrite-session", in_home], typed="yes, rewrite it\n", tty=True)
backups = writer.list_backups("20260201-120000-tui-7")["backups"]
check("the phrase rewrites it, after a backup", code == 0 and _bytes(in_home) != orig_home and len(backups) == before + 1, out[-300:])
made = json.loads(out)["reports"][0]["backup"]
check("the backup is byte-identical to the original", _bytes(made) == orig_home)
check("the rewrite says the result does not load and how to restore",
      "does NOT load" in err and "cai trans-fairy-write restore 20260201-120000-tui-7 --backup" in err, err[-300:])
writer.restore("20260201-120000-tui-7", backup_ts=backups[-1]["taken"], ignore_live=True)
restored = _bytes(in_home)
tail = restored[len(orig_home):].decode().strip().splitlines()
check("restore puts the original back, though the rewritten file no longer parses",
      restored.startswith(orig_home) and len(tail) == 1 and json.loads(tail[0])["type"] == "rewritten", str(tail))

os.utime(in_home, (time.time() - 9999, time.time() - 9999))  # a just-restored file looks live
before_fault = _bytes(in_home)
real_write = reflowfam.write
reflowfam.write = lambda module, path, data: open(path, "w").write("torn\n")
try:
    code, out, err = _reflow(["--rewrite-session", in_home], typed="yes, rewrite it\n", tty=True)
finally:
    reflowfam.write = real_write
check("a write that differs from what was validated is undone from the backup",
      code == 1 and "was put back" in out and _bytes(in_home) == before_fault, "code %d; %s" % (code, out[out.find("refused\": \"") : out.find("refused\": \"") + 200] if "refused\": \"" in out else out[-120:]))

cc_dir = os.path.join(tempfile.mkdtemp(), ".claude", "projects", "-x")
cc = write_jsonl(os.path.join(cc_dir, "s.jsonl"), [
    {"type": "user", "uuid": "u1", "parentUuid": None, "sessionId": "s", "message": {"role": "user", "content": "hi"}},
    {"type": "assistant", "uuid": "a1", "parentUuid": "u1", "sessionId": "s",
     "message": {"role": "assistant", "content": [{"type": "text", "text": "hello"}]}}])
code, out, err = _reflow([cc])
check("a Claude projects file keeps its exact refusal",
      code == 1 and "this file is in a Claude projects directory. A live session appends to it" in out, out[-200:])
code, out, err = _reflow(["-n", cc])
check("a plain dry run on it adds nothing to stderr (its contract)", err == "", err[-200:])
code, out, err = _reflow(["--rewrite-session", "-n", cc])
check("--rewrite-session -n reports its validity", "Claude Code transcript" in err, err[-200:])

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
