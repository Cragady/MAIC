"""leak_audit.py against synthetic sessions and a fake local judge: python3 -m unittest test_leak_audit

Every session here is made up in a temporary directory; XDG_STATE_HOME, XDG_RUNTIME_DIR and HOME point there too,
and every run names its --root, so no real transcript is ever read."""

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
from unittest import mock
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "leak_audit.py")
sys.path.insert(0, HERE)
import leak_audit  # noqa: E402

LINE = re.compile(r"leak audit complete( \(phase 1 only\))?: report at (\S+)\. Something was reached for: (YES|NO|UNCLEAR)\n")
NOT_DONE = re.compile(r"leak audit not completed: [^\n]+\. Something was reached for: UNKNOWN\n")
# Text that is only in the transcripts: none of it may reach standard output or standard error.
SECRETS = ["fennec-ears-7731", "socket.AF_UNIX", "org.freedesktop", "clean-session-note", "/run/user/1000", "desktop notification",
           "docker runs containers"]


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


def write_session(root, name, records, malformed=False):
    d = Path(root) / "general"
    d.mkdir(parents=True, exist_ok=True)
    with open(d / f"{name}.jsonl", "w") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")
        if malformed:
            f.write("{not json fennec-ears-7731\n[1, 2]\n")


class Judge(BaseHTTPRequestHandler):
    """A fake llama-server: reached when the flagged call holds dbus-send or AF_UNIX, else mentioned, or always
    `mode` when the test sets one."""
    mode = None
    requests = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200 if self.path == "/health" else 404)
        self.end_headers()
        self.wfile.write(b"{}")

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Judge.requests.append((self.path, body))
        call = body["messages"][1]["content"].split("<call>")[1].split("</call>")[0]
        if Judge.mode == "garbage":
            content = "I would rather not say."
        elif Judge.mode:
            content = json.dumps({"verdict": Judge.mode, "reason": "the fake judge was told to say so."})
        elif "dbus-send" in call or "AF_UNIX" in call:
            content = json.dumps({"verdict": "reached", "reason": "it talks to the session bus."})
        else:
            content = json.dumps({"verdict": "mentioned", "reason": "only text."})
        out = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


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
        Judge.mode = None
        Judge.requests = []
        self.fake_maic = self.dir / "maic"
        self.resolve_to(f"http://127.0.0.1:{self.port}/v1")

    def tearDown(self):
        self.tmp.cleanup()

    def resolve_to(self, base_url, remote=False, provider="llamacpp", kind="openai"):
        spec = {"provider": provider, "kind": kind, "base_url": base_url, "model": "Qwen3.5-9B-Q4_K_M-text", "context": 16384,
                "remote": remote, "api_key_env": "", "api_key_command": ""}
        self.fake_maic.write_text(f"#!{sys.executable}\nimport sys\nassert sys.argv[1:3] == ['model', 'resolve']\n"
                                  f"print({json.dumps(json.dumps(spec))})\n")
        self.fake_maic.chmod(0o755)

    def run_tool(self, *args, roots=True):
        env = {k: v for k, v in os.environ.items() if not k.lower().endswith("_proxy")}
        env.update(XDG_STATE_HOME=str(self.dir / "state"), XDG_RUNTIME_DIR=str(self.dir / "run"), HOME=str(self.dir / "home"),
                   MAIC_BIN=str(self.fake_maic), PYTHONDONTWRITEBYTECODE="1")
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
        self.assertEqual(path.parent, self.dir / "state" / "maic" / "audits")
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
        self.assertIn("it talks to the session bus.", text)
        self.assertIn("show a desktop notification", text)
        self.assertIn("Lines that were not JSON objects (skipped): 2.", text)
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
        os.unlink(self.fake_maic)
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
        self.assertFalse((self.dir / "state" / "maic" / "audits").exists())

    def test_server_down(self):
        self.all_sessions()
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            closed = s.getsockname()[1]
        self.resolve_to(f"http://127.0.0.1:{closed}/v1")
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        self.assertIn("maic up llamacpp", r.stderr)
        self.assertFalse((self.dir / "state" / "maic" / "audits").exists())

    def test_errors_keep_one_line(self):
        r = self.run_tool("--no-such-flag")
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        os.unlink(self.fake_maic)
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertRegex(r.stdout, NOT_DONE)
        r = self.run_tool("--help")
        self.assertEqual(r.returncode, 0)
        self.assertIn("UNKNOWN", r.stdout)
        self.assertIn("usage:", r.stderr)

    def test_default_roots_follow_xdg(self):
        with mock.patch.dict(os.environ, XDG_STATE_HOME="/state-here", XDG_RUNTIME_DIR="/runtime-here"):
            self.assertEqual(leak_audit.default_roots(), [Path("/state-here/maic/sessions"), Path("/runtime-here/maic/sessions")])

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
        timed = shell("busctl", "terminated: the command exceeded its timeout of 120 s.", ok=False, review={"verdict": "allow"})
        self.assertEqual(leak_audit.outcome(timed), ("allow, reviewer allow", "yes", "timed out"))

    def test_parse_verdict(self):
        self.assertEqual(leak_audit.parse_verdict('<think>x</think>{"verdict": "Reached", "reason": "a b\\nc"}'), ("reached", "a b c"))
        self.assertEqual(leak_audit.parse_verdict("mentioned: only docs")[0], "mentioned")
        self.assertEqual(leak_audit.parse_verdict("no idea")[0], "unclear")


if __name__ == "__main__":
    unittest.main()
