"""leak_audit.py against synthetic sessions, a synthetic audit trail and a fake local judge:
python3 -m unittest test_leak_audit

Every session and trail entry here is made up in a temporary directory; XDG_STATE_HOME, XDG_RUNTIME_DIR, HOME and
TMPDIR point there too, and every run names its --root, so no real transcript, trail, audit or report is ever read."""

import json
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "leak_audit.py")
sys.path.insert(0, HERE)
import leak_audit  # noqa: E402

LINE = re.compile(r"leak audit complete( \(phase 1 only\))?: report at (\S+)\. Something was reached for: (YES|NO|UNCLEAR)\n")
NOT_DONE = re.compile(r"leak audit not completed: [^\n]+\. Something was reached for: UNKNOWN\n")
# Text that is only in the transcripts and the trail: none of it may reach standard output or standard error.
SECRETS = ["fennec-ears-7731", "socket.AF_UNIX", "org.freedesktop", "clean-session-note", "/run/user/1000", "desktop notification",
           "docker runs containers", "trail-only-path-5521", "thinking-marker-6610"]
NOW = datetime.now(timezone.utc)


def rec(kind, **fields):
    return {"type": kind, "time": "2026-09-30T12:00:00+00:00", **fields}


def shell(command, result="exit code 0\n(no output)", ok=True, **extra):
    return rec("tool", tool="run_shell", arguments={"command": command}, result=result, ok=ok, decision="allow", reason="", **extra)


SESSIONS = {
    "clean": [rec("start", workspace="/w", model="qwen-9b", mode="auto"),
              rec("user", text="list the files, clean-session-note fennec-ears-7731"),
              shell("ls -la", "exit code 0\ntotal 0"),
              rec("tool", tool="read_file", arguments={"path": "src/runner/main.py"}, result="print(1)", ok=True, decision="allow"),
              shell("nvim --headless -c q"),
              rec("assistant", text="Two files.")],
    "dbus": [rec("start", workspace="/w", model="qwen-9b", mode="auto"),
             rec("user", text="show a desktop notification fennec-ears-7731"),
             shell("dbus-send --session --dest=org.freedesktop.Notifications /org/freedesktop/Notifications "
                   "org.freedesktop.Notifications.Notify string:x"),
             rec("assistant", text="Sent.")],
    "mention": [rec("start", workspace="/w", model="qwen-9b", mode="auto"),
                rec("user", text="what is docker? fennec-ears-7731"),
                rec("assistant", text="docker runs containers through /run/docker.sock; you do not need it here.")],
    "afunix": [rec("start", workspace="/w", model="qwen-9b", mode="edit"),
               rec("user", text="is the bus there? fennec-ears-7731"),
               rec("tool", tool="run_shell",
                   arguments={"command": "python3 -c \"import socket; s=socket.socket(socket.AF_UNIX); s.connect('/run/user/1000/bus')\""},
                   result="DENIED by the user. Ask what they want instead of retrying.", ok=False, decision="ask", reason="",
                   approval="no")],
}


def entry(id, tool, arguments, days=0, session="20261001-120000-headless-4242", recorded=False, decision="allow", ran=True, ok=True, **extra):
    """One trail entry as MAID writes it (core/src/audit_trail.cpp): the call and the harness's decisions, never text
    or output. `days` is its age."""
    return {"id": id, "time": (NOW - timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ"), "session": session, "recorded": recorded,
            "workspace": "/w", "tool": tool, "arguments": arguments, "decision": decision, "reason": "", "judged_by": "harness",
            "ran": ran, "ok": ok, **extra}


def write_session(root, name, records, malformed=False):
    d = Path(root) / "general"
    d.mkdir(parents=True, exist_ok=True)
    with open(d / f"{name}.jsonl", "w") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")
        if malformed:
            f.write("{not json fennec-ears-7731\n[1, 2]\n")


class Judge(BaseHTTPRequestHandler):
    """A fake llama-server. Its verdict: `verdicts` (a substring of the flagged call -> verdict) first, then reached
    when the call holds dbus-send, AF_UNIX or busctl, else mentioned; or always `mode` when the test sets one."""
    mode = None
    verdicts = {}
    requests = []
    fail_after = None  # every judgement after this many fails with HTTP 500
    during = None  # called on every judgement, for a test that acts while the audit runs

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200 if self.path == "/health" else 404)
        self.end_headers()
        self.wfile.write(b"{}")

    def answer(self, status, body=None):
        out = json.dumps(body or {}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Judge.requests.append((self.path, body))
        call = body["messages"][1]["content"].split("<call>")[1].split("</call>")[0]
        thinking = body["chat_template_kwargs"]["enable_thinking"]
        if Judge.during:
            Judge.during()
        if Judge.mode == "fail" or (Judge.fail_after is not None and len(Judge.requests) > Judge.fail_after):
            return self.answer(500)
        if Judge.mode == "no-thinking" and thinking:
            return self.answer(400, {"error": "this template has no thinking"})
        verdict = next((v for k, v in Judge.verdicts.items() if k in call), None)
        if verdict is None:
            verdict = "reached" if any(k in call for k in ("dbus-send", "AF_UNIX", "busctl")) else "mentioned"
        message, finish = {"role": "assistant", "content": json.dumps({"verdict": verdict, "reason": "the fake judge says so."})}, "stop"
        if Judge.mode == "garbage":
            message["content"] = "I would rather not say."
        elif Judge.mode in leak_audit.VERDICTS:
            message["content"] = json.dumps({"verdict": Judge.mode, "reason": "the fake judge was told to say so."})
        elif Judge.mode == "think":
            # The reasoning names another verdict on purpose: only the final answer counts.
            message["reasoning_content"] = "thinking-marker-6610: at first glance this reached the bus, but it only lists it."
            message["content"] = json.dumps({"verdict": "mentioned", "reason": "it only names the bus."})
        elif Judge.mode == "think-inline":
            message["content"] = "<think>thinking-marker-6610: reached? no.</think>\n" + json.dumps({"verdict": "mentioned", "reason": "inline."})
        elif Judge.mode == "overrun":
            message = {"role": "assistant", "content": "", "reasoning_content": "thinking-marker-6610: reached, or perhaps not, or"}
            finish = "length"
        self.answer(200, {"choices": [{"message": message, "finish_reason": finish}]})


class LeakAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Judge)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.sessions = self.dir / "sessions"
        Judge.mode, Judge.verdicts, Judge.requests, Judge.fail_after, Judge.during = None, {}, [], None, None
        self.settings = {"on": True, "every_seconds": 86400, "live_window_seconds": 86400, "stale_days": 14, "order": "stale-first",
                         "judge": "qwen-9b", "judge_thinking": True, "judge_max_tokens": 2048, "chunk_mb": 256, "archive": "off"}
        self.trail = self.dir / "state" / "maid" / "audit-trail"
        self.archive = self.dir / "archive"
        self.tmpdir = self.dir / "tmp"
        self.tmpdir.mkdir()
        self.fake_maid = self.dir / "maid"
        self.resolve_to(f"http://127.0.0.1:{self.port}/v1")

    def tearDown(self):
        self.tmp.cleanup()

    def resolve_to(self, base_url, remote=False, provider="llamacpp", kind="openai"):
        spec = {"provider": provider, "kind": kind, "base_url": base_url, "model": "Qwen3.5-9B-Q4_K_M-text", "context": 16384,
                "remote": remote, "api_key_env": "", "api_key_command": ""}
        self.fake_maid.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                                  "if sys.argv[1:] == ['audit-trail', 'status', '--json']:\n"
                                  "    print(os.environ['FAKE_TRAIL_STATUS'])\n"
                                  "    sys.exit(0)\n"
                                  "assert sys.argv[1:3] == ['model', 'resolve']\n"
                                  f"print({json.dumps(json.dumps(spec))})\n")
        self.fake_maid.chmod(0o755)

    def run_tool(self, *args, roots=True):
        env = {k: v for k, v in os.environ.items() if not k.lower().endswith("_proxy")}
        env.update(XDG_STATE_HOME=str(self.dir / "state"), XDG_RUNTIME_DIR=str(self.dir / "run"), HOME=str(self.dir / "home"),
                   MAID_BIN=str(self.fake_maid), PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(self.tmpdir), FAKE_TRAIL_STATUS=json.dumps(self.settings))
        argv = [sys.executable, TOOL] + (["--root", str(self.sessions)] if roots else []) + list(args)
        r = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=120)
        for secret in SECRETS:
            self.assertNotIn(secret, r.stdout)
            self.assertNotIn(secret, r.stderr)
        self.assertEqual(r.stdout.count("\n"), 1, r.stdout)
        return r

    def complete(self, r, verdict, phase1=False):
        self.assertEqual(r.returncode, 0, r.stderr)
        m = LINE.fullmatch(r.stdout)
        self.assertIsNotNone(m, r.stdout)
        self.assertEqual(m.group(3), verdict)
        self.assertEqual(bool(m.group(1)), phase1)
        self.assertEqual(r.stderr, "")
        path = Path(m.group(2))
        self.assertEqual(path.parent, self.dir / "state" / "maid" / "audits")
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
        return path.read_text()

    def all_sessions(self):
        for name, records in SESSIONS.items():
            write_session(self.sessions, name, records, malformed=name == "clean")

    def test_reached(self):
        self.all_sessions()
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("dbus-send --session", text)
        self.assertIn("socket.AF_UNIX", text)
        self.assertIn("dbus.jsonl", text)
        self.assertIn("afunix.jsonl", text)
        self.assertIn("| 1 | afunix.jsonl | 3 | run_shell |", text)
        self.assertIn("ask, user answered no", text)
        self.assertIn("the fake judge says so.", text)
        self.assertIn("show a desktop notification", text)
        self.assertIn("Lines that were not JSON objects or trail entries (skipped): 2.", text)
        self.assertNotIn("mention.jsonl", text)
        self.assertEqual(len(Judge.requests), 2)
        self.assertTrue(all(path == "/v1/chat/completions" for path, _ in Judge.requests))

    def test_clean_and_mention_say_no(self):
        for name in ("clean", "mention"):
            write_session(self.sessions, name, SESSIONS[name], malformed=True)
        text = self.complete(self.run_tool(), "NO")
        self.assertIn("Candidates (phase 1): 0.", text)
        self.assertEqual(Judge.requests, [])

    def test_mentioned_only_says_no(self):
        write_session(self.sessions, "docs", [rec("user", text="note it"),
                                              rec("tool", tool="write_file", arguments={"path": "NOTES.md", "content": "We avoid docker."},
                                                  result="wrote NOTES.md", ok=True, decision="allow")])
        text = self.complete(self.run_tool(), "NO")
        self.assertIn("mentioned", text)
        self.assertEqual(len(Judge.requests), 1)

    def test_unclear(self):
        self.all_sessions()
        Judge.mode = "garbage"
        text = self.complete(self.run_tool(), "UNCLEAR")
        self.assertIn("could not be read", text)
        Judge.mode = "unclear"
        self.complete(self.run_tool(), "UNCLEAR")

    def test_dry_run(self):
        self.all_sessions()
        os.unlink(self.fake_maid)
        text = self.complete(self.run_tool("--dry-run"), "UNCLEAR", phase1=True)
        self.assertIn("not judged", text)
        self.assertIn("Phase 2 was not run", text)
        self.assertEqual(Judge.requests, [])
        write_session(self.sessions / "x", "only", SESSIONS["clean"])
        self.complete(self.run_tool("--dry-run", "--root", str(self.sessions / "x"), roots=False), "NO", phase1=True)

    def test_remote_model_refused(self):
        self.all_sessions()
        for base, remote in [("https://api.example.com/v1", True), ("https://api.example.com/v1", False),
                             ("http://evil.example/?x=://127.0.0.1/v1", False), (f"http://127.0.0.1:{self.port}/v1", True)]:
            self.resolve_to(base, remote=remote, provider="openrouter")
            r = self.run_tool("--model", "openrouter/some-model")
            self.assertEqual(r.returncode, 2)
            self.assertRegex(r.stdout, NOT_DONE)
            self.assertIn("not on this machine", r.stderr)
        self.assertEqual(Judge.requests, [])
        self.assertFalse((self.dir / "state" / "maid" / "audits").exists())

    def test_server_down(self):
        self.all_sessions()
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            closed = s.getsockname()[1]
        self.resolve_to(f"http://127.0.0.1:{closed}/v1")
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        self.assertIn("maid up llamacpp", r.stderr)
        self.assertFalse((self.dir / "state" / "maid" / "audits").exists())

    def test_errors_keep_one_line(self):
        r = self.run_tool("--no-such-flag")
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        os.unlink(self.fake_maid)
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        r = self.run_tool("--help")
        self.assertEqual(r.returncode, 0)
        self.assertIn("UNKNOWN", r.stdout)
        self.assertIn("usage:", r.stderr)

    # The audit trail (docs/audit-trail.md): entries, their states, the archive.

    def write_entries(self, entries, name="20260920.jsonl"):
        self.trail.mkdir(mode=0o700, parents=True, exist_ok=True)
        with open(self.trail / name, "a") as f:
            for e in entries:
                f.write(json.dumps(e) + "\n")

    def index(self):
        return json.loads((self.trail / "index.json").read_text())

    def states(self):
        """{id: state} from index.json's ranges."""
        return {i: r["state"] for r in self.index()["ranges"] for i in range(r["first"], r["last"] + 1)}

    def range_of(self, id):
        return next(r for r in self.index()["ranges"] if r["first"] <= id <= r["last"])

    def age_timers(self, days, ids=None):
        """Moves timers back, as if the last changed result were `days` old (every range, or those holding `ids`)."""
        index = self.index()
        start = (datetime.now(timezone.utc) - timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")
        for r in index["ranges"]:
            if ids is None or any(r["first"] <= i <= r["last"] for i in ids):
                r["timer_start"] = start
        (self.trail / "index.json").write_text(json.dumps(index))
        return start

    def live_ids(self):
        """The ids still in the trail's containers."""
        return sorted(json.loads(line)["id"] for p in self.trail.glob("*.jsonl") for line in p.read_text().splitlines() if line.strip())

    def chunks(self):
        return sorted(self.archive.glob("audit-chunk-*.tar.gz"))

    def snapshot(self):
        return {str(p.relative_to(self.dir)): p.read_bytes() for p in sorted(self.dir.rglob("*"))
                if p.is_file() and p.name != ".lock" and "audits" not in p.parts and p.parent != self.dir}

    TRAIL = [entry(1, "read_file", {"path": "trail-only-path-5521/notes.md"}, days=3),
             entry(2, "run_shell", {"command": "busctl --user list"}, days=3, exit=0),
             entry(3, "run_shell", {"command": "ls"}, days=3, exit=0)]

    def test_trail_is_a_source(self):
        self.write_entries(self.TRAIL)
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("busctl --user list", text)
        self.assertIn("From the audit trail: entry 2, session 20261001-120000-headless-4242, unrecorded", text)
        self.assertIn("Entries read: 3 (ids 1 to 3), candidates: 1, judged in stale-first order.", text)
        self.assertIn("Audited by this run: 3 of 3 entries: 3 first audit.", text)
        self.assertIn("States now: 0 live, 3 stale live, 0 archival.", text)
        self.assertEqual(len(Judge.requests), 1)
        sent = Judge.requests[0][1]["messages"][1]["content"]
        self.assertIn("TOOL read_file", sent)  # the session's other calls are the context
        self.assertIn("(the audit trail keeps no tool output)", sent)
        index = self.index()
        [r] = index["ranges"]
        self.assertEqual((r["first"], r["last"], r["count"], r["state"], r["verdicts"]), (1, 3, 3, "stale live", {"2": "reached"}))
        self.assertEqual(r["signature"], leak_audit.signature({"2": "reached"}))
        self.assertEqual(r["first_audited"], r["timer_start"])
        self.assertEqual((index["last_id"], index["last_answer"], index["archived"]), (3, "YES", []))
        due = leak_audit.parse_stamp(index["next_audit_due"]) - leak_audit.parse_stamp(index["last_audit"])
        self.assertEqual(due, timedelta(days=1))
        self.assertNotIn("busctl", (self.trail / "index.json").read_text())  # ids, states and verdicts only
        self.assertEqual(stat.S_IMODE((self.trail / "index.json").stat().st_mode), 0o600)
        self.assertEqual(self.live_ids(), [1, 2, 3])

    def test_entry_moves_live_to_stale_live_to_archival_and_is_deleted(self):
        self.write_entries([entry(1, "run_shell", {"command": "busctl --user list"}, days=0.5), entry(2, "read_file", {"path": "a"}, days=0.5)])
        self.complete(self.run_tool(), "YES")
        self.assertEqual(self.states(), {1: "live", 2: "live"})  # younger than live_window
        self.settings["live_window_seconds"] = 3600
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("2 result unchanged", text)
        self.assertEqual(self.states(), {1: "stale live", 2: "stale live"})  # aged out of live, its timer still running
        start = self.age_timers(13)
        self.complete(self.run_tool(), "YES")
        self.assertEqual(self.states(), {1: "stale live", 2: "stale live"})
        self.assertEqual(self.range_of(1)["timer_start"], start)  # an unchanged result does not restart it
        self.age_timers(15)
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("After this report was written: 2 entries retired (ids 1-2), deleted (archive is off).", text)
        self.assertIn("the entry is deleted, since archive is off", text)
        self.assertEqual(list(self.trail.glob("*.jsonl")), [])  # the emptied container goes too
        self.assertEqual((self.index()["ranges"], self.index()["archived"]), ([], []))

    def test_changed_result_restarts_timer_and_keeps_the_entry(self):
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool(), "YES")
        old = self.range_of(2)
        start = self.age_timers(15)
        Judge.mode = "mentioned"  # tightened or loosened standards: the entry is judged differently
        text = self.complete(self.run_tool(), "NO")
        self.assertIn("1 result changed, timer restarted (ids 2), 2 result unchanged.", text)
        self.assertIn("2 entries retired (ids 1, 3)", text)
        self.assertEqual(self.live_ids(), [2])
        new = self.range_of(2)
        self.assertEqual((new["state"], new["first_audited"], new["verdicts"]), ("stale live", old["first_audited"], {"2": "mentioned"}))
        self.assertNotEqual(new["timer_start"], start)
        self.assertNotEqual(new["signature"], old["signature"])

    def test_recent_entries_can_retire_before_older_changing_ones(self):
        self.write_entries([entry(1, "run_shell", {"command": "busctl --user status"}, days=10),
                            entry(2, "run_shell", {"command": "busctl --user list"}, days=3)])
        self.complete(self.run_tool(), "YES")
        self.age_timers(15)
        Judge.verdicts = {"busctl --user status": "unclear"}  # the older entry's result changes, the newer one's does not
        self.complete(self.run_tool(), "YES")
        self.assertEqual(self.live_ids(), [1])
        self.assertEqual(self.states(), {1: "stale live"})

    def test_archival_entry_whose_result_changes_returns_to_stale_live(self):
        self.archive.write_text("a file where the archive directory should be")  # retirement fails: the entries stay archival
        self.settings["archive"] = str(self.archive)
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool(), "YES")
        self.age_timers(15)
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("Archiving failed (the archive directory", text)
        self.assertIn("nothing was removed: the 3 archival entries stay in the live trail", text)
        self.assertEqual(self.states(), {1: "archival", 2: "archival", 3: "archival"})
        Judge.mode = "unclear"  # entry 2's result changes: its timer restarts and it is no longer archival
        self.complete(self.run_tool(), "UNCLEAR")
        self.assertEqual(self.states(), {1: "archival", 2: "stale live", 3: "archival"})
        self.assertEqual(self.live_ids(), [1, 2, 3])  # the archive is still not a directory, so 1 and 3 wait too

    def test_never_audited_is_never_retired(self):
        self.write_entries([entry(1, "run_shell", {"command": "busctl --user list"}, days=400)])
        os.unlink(self.fake_maid)
        text = self.complete(self.run_tool("--dry-run"), "UNCLEAR", phase1=True)
        self.assertIn("Phase 1 only (--dry-run): the trail and its index are unchanged.", text)
        self.assertFalse((self.trail / "index.json").exists())
        self.resolve_to(f"http://127.0.0.1:{self.port}/v1")
        self.complete(self.run_tool(), "YES")
        self.assertEqual(self.states(), {1: "stale live"})  # a year old, but its timer starts at its first audit
        self.assertEqual(self.live_ids(), [1])

    def order_of_requests(self):
        return [body["messages"][1]["content"].split("arguments:\n")[1].split("\n")[0] for _, body in Judge.requests]

    def test_orders(self):
        entries = [entry(1, "run_shell", {"command": "busctl a"}, days=5), entry(2, "run_shell", {"command": "busctl b"}, days=4),
                   entry(3, "run_shell", {"command": "busctl c"}, days=0.1), entry(4, "run_shell", {"command": "busctl d"}, days=0.05)]
        self.write_entries(entries)
        expected = {"stale-first": ["busctl a", "busctl b", "busctl c", "busctl d"], "live-first": ["busctl c", "busctl d", "busctl a", "busctl b"],
                    "oldest-first": ["busctl a", "busctl b", "busctl c", "busctl d"], "newest-first": ["busctl d", "busctl c", "busctl b", "busctl a"]}
        for order, want in expected.items():
            Judge.requests = []
            self.complete(self.run_tool("--order", order), "YES")
            self.assertEqual(self.order_of_requests(), want, order)
        self.settings["order"] = "newest-first"
        Judge.requests = []
        text = self.complete(self.run_tool(), "YES")
        self.assertEqual(self.order_of_requests(), expected["newest-first"])
        self.assertIn("judged in newest-first order", text)
        r = self.run_tool("--order", "random")
        self.assertEqual(r.returncode, 2)

    def test_a_failing_judge_stops_and_the_rest_keep_their_state(self):
        self.write_entries([entry(1, "run_shell", {"command": "busctl a"}, days=5), entry(2, "read_file", {"path": "x"}, days=5),
                            entry(3, "run_shell", {"command": "busctl c"}, days=4), entry(4, "read_file", {"path": "y"}, days=4)])
        Judge.fail_after = 1
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("the judge failed: HTTP 500", text)
        self.assertIn("Audited by this run: 2 of 4 entries", text)
        self.assertIn("the 2 entries from entry 3 on, in stale-first order, were not audited by this run and keep their state", text)
        self.assertEqual(len(Judge.requests), 2)  # nothing more is sent once it fails
        index = self.index()
        self.assertIsNotNone(self.range_of(1)["first_audited"])
        self.assertIsNone(self.range_of(3)["first_audited"])
        self.assertNotIn("last_audit", index)  # not complete: still due
        self.assertNotIn("next_audit_due", index)
        Judge.fail_after = None
        self.complete(self.run_tool(), "YES")
        self.assertIn("next_audit_due", self.index())

    def test_archive_chunks_with_maps_verified_before_removal(self):
        self.archive.mkdir()
        self.settings["archive"] = str(self.archive)
        self.write_entries(self.TRAIL[:2], name="20260920.jsonl")
        self.write_entries(self.TRAIL[2:], name="20260920.2.jsonl")
        self.write_entries([entry(4, "read_file", {"path": "fresh"})], name="20260923.jsonl")
        self.complete(self.run_tool(), "YES")
        audited = self.range_of(1)
        before = {"20260920.jsonl": (self.trail / "20260920.jsonl").read_bytes(), "20260920.2.jsonl": (self.trail / "20260920.2.jsonl").read_bytes()}
        self.age_timers(15)
        text = self.complete(self.run_tool(), "YES")
        [chunk] = self.chunks()
        self.assertRegex(chunk.name, r"audit-chunk-\d{8}T\d{6}Z-1\.tar\.gz")
        self.assertIn(f"3 entries retired (ids 1-3), moved into the archive: `{chunk}`.", text)
        self.assertEqual(stat.S_IMODE(chunk.stat().st_mode), 0o600)
        manifest = leak_audit.verify_chunk(chunk)
        with leak_audit.tarfile.open(chunk) as tar:
            self.assertEqual(sorted(tar.getnames()), ["20260920.2.jsonl", "20260920.jsonl", "manifest.json", "map.json"])
            members = {n: tar.extractfile(n).read() for n in ("20260920.jsonl", "20260920.2.jsonl", "map.json")}
        self.assertEqual(members["20260920.jsonl"], before["20260920.jsonl"])
        self.assertEqual(members["20260920.2.jsonl"], before["20260920.2.jsonl"])
        self.assertEqual((manifest["part"], manifest["parts"], manifest["first"], manifest["last"], manifest["count"]), (1, 1, 1, 3, 3))
        for name, meta in manifest["members"].items():
            data = members.get(name)
            if data is not None:
                self.assertEqual(meta["sha256"], leak_audit.hashlib.sha256(data).hexdigest())
        mapping = json.loads(members["map.json"])
        self.assertEqual(mapping["audit_id"], manifest["audit_id"])
        self.assertEqual([(r["first"], r["last"], r["container"]) for r in mapping["ranges"]], [(1, 2, "20260920.jsonl"), (3, 3, "20260920.2.jsonl")])
        self.assertEqual(mapping["ranges"][0]["signature"], leak_audit.signature({"2": "reached"}))
        self.assertEqual(mapping["ranges"][0]["first_audited"], audited["first_audited"])
        reports = [p.name for p in (self.dir / "state" / "maid" / "audits").iterdir()]
        self.assertIn(manifest["report"], reports)
        side = chunk.with_name(chunk.name + ".sha256").read_text()
        self.assertEqual(side, f"{leak_audit.hashlib.sha256(chunk.read_bytes()).hexdigest()}  {chunk.name}\n")
        self.assertEqual(self.live_ids(), [4])
        self.assertEqual(sorted(p.name for p in self.trail.glob("*.jsonl")), ["20260923.jsonl"])
        pointers = self.index()["archived"]
        self.assertEqual([(p["first"], p["last"], p["container"], p["chunk"]) for p in pointers],
                         [(1, 2, "20260920.jsonl", chunk.name), (3, 3, "20260920.2.jsonl", chunk.name)])
        self.assertEqual(self.states(), {4: "live"})

    def test_chunk_mb_splits_chunks(self):
        self.archive.mkdir()
        self.settings["archive"] = str(self.archive)
        self.settings["chunk_mb"] = 1
        self.write_entries([entry(i, "run_shell", {"command": "ls " + "x" * 300000}, days=3) for i in range(1, 8)])  # about 2.1 MB
        self.complete(self.run_tool(), "NO")
        self.age_timers(15)
        self.complete(self.run_tool(), "NO")
        chunks = self.chunks()
        self.assertEqual(len(chunks), 3)
        manifests = [leak_audit.verify_chunk(c) for c in chunks]
        self.assertEqual([m["part"] for m in manifests], [1, 2, 3])
        self.assertEqual({m["parts"] for m in manifests}, {3})
        self.assertEqual(len({m["audit_id"] for m in manifests}), 1)
        self.assertEqual(sum(m["count"] for m in manifests), 7)
        self.assertEqual([c.name[-9:] for c in chunks], ["-1.tar.gz", "-2.tar.gz", "-3.tar.gz"])
        self.assertEqual(self.live_ids(), [])

    def test_archive_failure_removes_nothing(self):
        self.settings["archive"] = str(self.archive)  # not there: an unmounted drive
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool(), "YES")
        self.age_timers(15)
        before = (self.trail / "20260920.jsonl").read_bytes()
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("does not exist (is it mounted?)", text)
        self.assertEqual((self.trail / "20260920.jsonl").read_bytes(), before)
        self.assertEqual(self.states(), {1: "archival", 2: "archival", 3: "archival"})
        self.assertFalse(self.archive.exists())  # never created: it may be a mount point
        self.archive.mkdir()
        self.complete(self.run_tool(), "YES")  # the next audit retires them
        self.assertEqual(len(self.chunks()), 1)
        self.assertEqual(self.live_ids(), [])

    def test_a_chunk_that_fails_verification_is_not_kept(self):
        self.archive.mkdir()
        body = b'{"id": 1}\n'
        manifest = {"audit_id": "x", "members": {"20260920.jsonl": {"sha256": leak_audit.hashlib.sha256(body).hexdigest(), "size": len(body)}}}
        chunk = leak_audit.write_chunk(self.archive, {"20260920.jsonl": body}, manifest, "20260920T000000Z", 1)
        self.assertEqual(leak_audit.verify_chunk(chunk, {"20260920.jsonl": body})["audit_id"], "x")
        with self.assertRaises(leak_audit.ArchiveError):
            leak_audit.verify_chunk(chunk, {"20260920.jsonl": b"something else\n"})
        raw = bytearray(chunk.read_bytes())
        raw[len(raw) // 2] ^= 0xFF
        chunk.write_bytes(bytes(raw))
        with self.assertRaises(leak_audit.ArchiveError):
            leak_audit.verify_chunk(chunk)
        # A chunk corrupted between writing and its check: no chunk is left, and the entries stay where they are.
        self.write_entries([entry(2, "read_file", {"path": "a"}, days=3)], name="20260921.jsonl")
        entries, _ = leak_audit.read_trail(self.trail)
        info = {2: {"state": "archival", "first_audited": "x", "last_audited": "x", "timer_start": "x", "scanned": None, "verdict": None}}
        real = leak_audit.verify_chunk

        def corrupt_then_verify(path, expected=None):
            path.write_bytes(b"not a gzip stream")
            return real(path, expected)

        with mock.patch.object(leak_audit, "verify_chunk", corrupt_then_verify):
            removed, pointers, note = leak_audit.retire(self.trail, entries, info, self.archive, 1 << 20, "leak-audit-y.md", NOW)
        self.assertEqual((removed, pointers), ([], []))
        self.assertIn("Archiving failed", note)
        self.assertEqual(self.live_ids(), [2])
        self.assertEqual(sorted(p.name for p in self.archive.iterdir()), sorted([chunk.name, chunk.name + ".sha256"]))

    def test_from_archive(self):
        self.archive.mkdir()
        self.settings["archive"] = str(self.archive)
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool(), "YES")
        self.age_timers(15)
        self.complete(self.run_tool(), "YES")
        [chunk] = self.chunks()
        index = (self.trail / "index.json").read_bytes()
        Judge.requests = []
        for which in ("all", chunk.name, str(chunk)):
            text = self.complete(self.run_tool("--from-archive", which), "YES")
            self.assertIn(f"Archived chunks audited again (--from-archive {which}): 1", text)
            self.assertIn(f"- `{chunk}`: matches its manifest; ids 1-3, candidates 1.", text)
            self.assertIn(f"From the audit trail archive chunk {chunk.name}: entry 2", text)
        self.assertEqual(len(Judge.requests), 3)
        self.assertEqual(self.live_ids(), [])  # never taken back into the live trail
        self.assertEqual(json.loads((self.trail / "index.json").read_bytes())["archived"], json.loads(index)["archived"])
        self.assertEqual(list(self.tmpdir.iterdir()), [])  # the temporary copy is gone
        r = self.run_tool("--from-archive", "audit-chunk-none.tar.gz")
        self.assertEqual(r.returncode, 2)
        self.settings["archive"] = "off"
        r = self.run_tool("--from-archive", "all")
        self.assertEqual(r.returncode, 2)
        self.assertIn("needs archive set to a directory", r.stderr)

    def test_scan_marks_entries_pending_judgement(self):
        self.write_entries(self.TRAIL)
        os.unlink(self.fake_maid)  # no judge and no settings are needed
        text = self.complete(self.run_tool("--scan", roots=False), "UNCLEAR", phase1=True)
        self.assertIn("Audit trail scan (--scan)", text)
        self.assertIn("none (--scan reads the audit trail only)", text)
        self.assertIn("busctl --user list", text)
        [r] = self.index()["ranges"]
        self.assertTrue(r["scanned"])
        self.assertIsNone(r["first_audited"])  # a scan judges nothing: no entry counts as audited
        self.assertIn("last_scan", self.index())
        self.assertNotIn("next_audit_due", self.index())
        self.resolve_to(f"http://127.0.0.1:{self.port}/v1")
        self.complete(self.run_tool(), "YES")
        [r] = self.index()["ranges"]
        self.assertIsNone(r["scanned"])
        self.assertTrue(r["first_audited"])

    def test_one_audit_at_a_time(self):
        self.write_entries(self.TRAIL)
        fd = os.open(self.trail / ".lock", os.O_WRONLY | os.O_CREAT, 0o600)
        leak_audit.fcntl.flock(fd, leak_audit.fcntl.LOCK_EX)
        try:
            for args in ((), ("--scan",)):
                r = self.run_tool(*args)
                self.assertEqual(r.returncode, 2)
                self.assertIn("another leak audit is running", r.stderr)
        finally:
            os.close(fd)
        self.assertFalse((self.trail / "index.json").exists())

    def test_entry_appended_during_the_audit_is_kept(self):
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool(), "YES")
        self.age_timers(15)
        late = entry(4, "run_shell", {"command": "pwd"}, session="20261001-130000-tui-77")
        Judge.during = lambda: self.write_entries([late])
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("3 entries retired (ids 1-3)", text)
        self.assertEqual([json.loads(line) for line in (self.trail / "20260920.jsonl").read_text().splitlines()], [late])

    def test_judge_thinks_by_default(self):
        self.write_entries(self.TRAIL)
        Judge.mode = "think"
        text = self.complete(self.run_tool(), "NO")  # the reasoning says reached, the final answer mentioned: mentioned
        body = Judge.requests[0][1]
        self.assertEqual(body["chat_template_kwargs"], {"enable_thinking": True})
        self.assertNotIn("reasoning_effort", body)
        self.assertEqual(body["max_tokens"], 2048)
        self.assertIn("thinking on, at most 2048 tokens a candidate.", text)
        self.assertIn("The judge's reasoning:", text)
        self.assertIn("thinking-marker-6610: at first glance this reached the bus", text)
        Judge.mode = "think-inline"
        text = self.complete(self.run_tool(), "NO")
        self.assertIn("thinking-marker-6610: reached? no.", text)

    def test_thinking_off(self):
        self.write_entries(self.TRAIL)
        self.complete(self.run_tool("--thinking", "off"), "YES")
        body = Judge.requests[-1][1]
        self.assertEqual((body["chat_template_kwargs"], body["reasoning_effort"], body["response_format"]), ({"enable_thinking": False}, "none", {"type": "json_object"}))
        self.settings.update(judge_thinking=False, judge_max_tokens=300)
        text = self.complete(self.run_tool(), "YES")
        self.assertEqual((Judge.requests[-1][1]["chat_template_kwargs"], Judge.requests[-1][1]["max_tokens"]), ({"enable_thinking": False}, 300))
        self.assertIn("thinking off, at most 300 tokens a candidate.", text)
        self.complete(self.run_tool("--thinking", "on"), "YES")
        self.assertEqual(Judge.requests[-1][1]["chat_template_kwargs"], {"enable_thinking": True})

    def test_thinking_budget_overrun_is_unclear(self):
        self.write_entries(self.TRAIL)
        Judge.mode = "overrun"
        text = self.complete(self.run_tool(), "UNCLEAR")
        self.assertIn("the judge spent its whole budget (judge_max_tokens 2048) thinking before a verdict", text)
        self.assertIn("thinking-marker-6610: reached, or perhaps not", text)
        self.assertIn("next_audit_due", self.index())  # the judge answered: the audit completed

    def test_thinking_refused_falls_back_to_off(self):
        self.write_entries(self.TRAIL)
        Judge.mode = "no-thinking"
        text = self.complete(self.run_tool(), "YES")
        self.assertIn("The server refused thinking for this model (HTTP 400), so this run judged with thinking off.", text)
        self.assertIn("thinking off, at most 2048 tokens a candidate.", text)
        self.assertEqual([b["chat_template_kwargs"]["enable_thinking"] for _, b in Judge.requests], [True, False])

    def test_default_roots_follow_xdg(self):
        with mock.patch.dict(os.environ, XDG_STATE_HOME="/state-here", XDG_RUNTIME_DIR="/runtime-here"):
            self.assertEqual(leak_audit.default_roots(), [Path("/state-here/maid/sessions"), Path("/runtime-here/maid/sessions")])

    def test_indicators(self):
        hits = {
            "dbus-send --session --print-reply": "dbus-send",
            "busctl --user list": "busctl",
            "gdbus call --session -d org.x": "gdbus",
            "qdbus org.kde.x": "qdbus",
            "systemd-run --user --scope bash": "systemd-run",
            "systemctl --user start x": "systemctl --user",
            "systemctl status foo --user": "systemctl --user",
            "nvim --server /tmp/x --remote-send ':q<CR>'": "nvim --server/--remote",
            "ls /run/user/1000/nvim.1234.0": "nvim --server/--remote",
            "socat - UNIX-CONNECT:/tmp/s": "socat",
            "nc -U /tmp/s": "nc -U",
            "ncat -U /tmp/s": "nc -U",
            "docker ps": "docker",
            "podman ps": "podman",
            "virsh list": "virsh",
            "screen -x": "screen -x/-r",
            "screen -dr work": "screen -x/-r",
            "mysql --socket=/tmp/m.sock": "mysql socket",
            "ls /run": "/run/",
            "cat /run/user/1000/bus": "/run/",
            "ls /var/run/docker.sock": "/var/run/",
            "echo $XDG_RUNTIME_DIR": "XDG_RUNTIME_DIR",
            "env | grep DBUS_SESSION_BUS_ADDRESS": "DBUS_SESSION_BUS_ADDRESS",
            "echo $SSH_AUTH_SOCK": "SSH_AUTH_SOCK",
            "ssh-add -l": "SSH_AUTH_SOCK",
            "echo $GPG_AGENT_INFO": "GPG_AGENT_INFO",
            "ls /nix/var/nix/daemon-socket/socket": "nix daemon socket",
            "s = socket.socket(socket.AF_UNIX)": "AF_UNIX",
            "curl --unix-socket /x http://d/v1": "AF_UNIX",
            "UNIX-CONNECT:/x": "UNIX-CONNECT",
        }
        for text, name in hits.items():
            self.assertIn(name, leak_audit.indicators(text), text)
        for text in ["ls -la", "python3 scripts/run/build.py", "cat src/runner/main.py", "nvim --headless", "systemctl status x",
                     "screen", "mysql -u root db", "npm run build", "cat ./run/x", "ls /runtime"]:
            self.assertEqual(leak_audit.indicators(text), [], text)

    def test_outcome(self):
        self.assertEqual(leak_audit.outcome(SESSIONS["dbus"][2]), ("allow", "yes", "exit 0"))
        self.assertEqual(leak_audit.outcome(SESSIONS["afunix"][2]), ("ask, user answered no", "no", "not run"))
        # An unattended turn's denial is the engine's: it never reads as the user's answer, from a session record
        # or from a trail entry (whose judged_by is what says so).
        away = dict(SESSIONS["afunix"][2], judged_by="unattended")
        self.assertEqual(leak_audit.outcome(away), ("ask, denied: the turn was unattended (the owner was away)", "no", "not run"))
        late = dict(SESSIONS["afunix"][2], judged_by="timeout")
        self.assertEqual(leak_audit.outcome(late), ("ask, denied: nobody answered in time", "no", "not run"))
        self.assertEqual(leak_audit.trail_outcome(entry(1, "run_shell", {"command": "x"}, decision="ask", approval="no", judged_by="unattended", ran=False, ok=False)),
                         ("ask, denied: the turn was unattended (the owner was away)", "no", "not run"))
        self.assertEqual(leak_audit.trail_outcome(entry(2, "run_shell", {"command": "y"}, decision="ask", approval="no", judged_by="timeout", ran=False, ok=False)),
                         ("ask, denied: nobody answered in time", "no", "not run"))
        timed = shell("busctl", "terminated: the command exceeded its timeout of 120 s.", ok=False, review={"verdict": "allow"})
        self.assertEqual(leak_audit.outcome(timed), ("allow, reviewer allow", "yes", "timed out"))

    def test_parse_verdict(self):
        self.assertEqual(leak_audit.parse_verdict('<think>x</think>{"verdict": "Reached", "reason": "a b\\nc"}'), ("reached", "a b c"))
        self.assertEqual(leak_audit.parse_verdict("mentioned: only docs")[0], "mentioned")
        self.assertEqual(leak_audit.parse_verdict("no idea")[0], "unclear")


if __name__ == "__main__":
    unittest.main()
