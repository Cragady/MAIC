"""diction against a fake whisper-server and a fake OpenAI-compatible scribe, never the microphone.

The pipeline runs as a user runs it, `python3 -m diction.cli`, with each utterance given as a WAV file (--from-wav)
instead of being cut from the mic. A fake `maid` answers `maid model resolve` and hands `maid settings` to the real
maid from MAID_BIN (ctest sets it); the cases that need the real one are skipped without it.

    python3 -m unittest -v diction.test_diction        from the repository root
"""

import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import tomllib
import unittest
import wave
from unittest import mock
from pathlib import Path

from diction import presets, scribe as scribe_mod
from diction.document import Procedure, apply_reply
from diction.scribe import (HISTORY_EXCHANGES, SYSTEM_PROMPT_INSERT, SYSTEM_PROMPT_NORMAL, ClaudeScribe, build_messages,
                            resolve_agent)

REPO = Path(__file__).resolve().parent.parent


class Recorder(http.server.BaseHTTPRequestHandler):
    """Base for both fakes: requests are recorded on the server object."""

    def log_message(self, *a):
        pass

    def reply(self, status, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ok"})
        else:
            self.reply(404, {"error": "no"})


class FakeWhisper(Recorder):
    """whisper-server's /inference: answers each request with the next text the test queued."""

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        boundary = self.headers["Content-Type"].split("boundary=")[1].encode()
        fields, files = {}, {}
        for part in body.split(b"--" + boundary):
            head, _, value = part.partition(b"\r\n\r\n")
            m = re.search(rb'name="([^"]+)"(; filename="([^"]+)")?', head)
            if not m:
                continue
            value = value[:-2] if value.endswith(b"\r\n") else value
            (files if m.group(3) else fields)[m.group(1).decode()] = value
        self.server.requests.append((self.path, fields, files))
        if self.path == "/inference":
            self.reply(200, {"text": self.server.texts.pop(0)})
        elif self.path == "/load":
            self.reply(200, {"status": "ok"})
        else:
            self.reply(404, {"error": "no"})


class FakeScribe(Recorder):
    """An OpenAI-compatible chat endpoint, streamed as SSE: the reply is looked up by the utterance it was sent."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        self.server.requests.append(body)
        said = re.search(r"<utterance>\n(.*)\n</utterance>", body["messages"][-1]["content"], re.S).group(1)
        text = self.server.replies.get(said, "SKIP")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for piece in (text[: len(text) // 2], text[len(text) // 2:]):
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": piece}}]}) + "\n\n").encode())
        self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}]}) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")


class FakeAnthropic(Recorder):
    """Anthropic's /v1/messages, unstreamed: the reply is looked up by the utterance it was sent."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        self.server.requests.append((self.path, {k.lower(): v for k, v in self.headers.items()}, body))
        said = re.search(r"<utterance>\n(.*)\n</utterance>", body["messages"][-1]["content"], re.S).group(1)
        self.reply(200, {"content": [{"type": "text", "text": self.server.replies.get(said, "SKIP")}]})


def serve(handler):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    srv.requests = []
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def silence(path: Path, seconds: float = 0.4):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\0\0" * int(16000 * seconds))


FAKE_MAID = """#!/usr/bin/env python3
import json, os, sys
if sys.argv[1:2] == ["settings"] and os.environ.get("REAL_MAID"):
    os.execv(os.environ["REAL_MAID"], [os.environ["REAL_MAID"], *sys.argv[1:]])
specs = json.loads(os.environ["FAKE_MAID_SPECS"])
if sys.argv[1:3] == ["model", "resolve"] and sys.argv[3] in specs:
    print(json.dumps(specs[sys.argv[3]]))
    sys.exit(0)
print("maid: unknown model " + " ".join(sys.argv[1:]), file=sys.stderr)
sys.exit(1)
"""


# `claude -p --input-format stream-json --output-format stream-json --verbose`, as diction reads it: one JSON user
# message a line in; an init line, the assistant's message and a `result` line out per message. Every start, message
# and end of input is noted in FAKE_CLAUDE_LOG.
FAKE_CLAUDE = """#!/usr/bin/env python3
import json, os, sys
replies = json.loads(os.environ.get("FAKE_CLAUDE_REPLIES", "{}"))
def note(**kw):
    with open(os.environ["FAKE_CLAUDE_LOG"], "a") as f:
        f.write(json.dumps(dict(pid=os.getpid(), **kw)) + "\\n")
note(event="start", argv=sys.argv[1:], cwd=os.getcwd())
print(json.dumps({"type": "system", "subtype": "init", "model": sys.argv[sys.argv.index("--model") + 1]}), flush=True)
for line in sys.stdin:
    payload = json.loads(line)["message"]["content"][0]["text"]
    said = payload.split("<utterance>\\n", 1)[1].rsplit("\\n</utterance>", 1)[0]
    note(event="message", said=said, payload=payload)
    reply = replies.get(said, "SKIP")
    print(json.dumps({"type": "assistant", "message": {"role": "assistant", "content": [{"type": "text", "text": reply}]}}), flush=True)
    if reply == "FAIL":
        print(json.dumps({"type": "result", "subtype": "error_during_execution", "is_error": True, "result": "overloaded"}), flush=True)
    else:
        print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": reply}), flush=True)
note(event="eof")
"""


def claude_log(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def legacy_argv(prompt: str, model: str) -> list[str]:
    return ["-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
            "--system-prompt", prompt, "--model", model, "--no-session-persistence"]


def spec(provider, port, model="fake", context=16384):
    return {"provider": provider, "kind": "openai", "base_url": f"http://127.0.0.1:{port}/v1", "model": model,
            "context": context, "remote": False, "api_key_env": "", "api_key_command": ""}


def cloud_spec(port, model="claude-haiku-4-5-20251001"):
    return {"provider": "anthropic", "kind": "anthropic", "base_url": f"http://127.0.0.1:{port}", "model": model,
            "context": 200000, "remote": True, "api_key_env": "ANTHROPIC_API_KEY", "api_key_command": ""}


def ggml(path: Path):
    """A stand-in whisper model: the ggml magic is all diction checks before sending it to /load."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"lmgg" + b"\0" * 60)


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.whisper = serve(FakeWhisper)
        self.scribe = serve(FakeScribe)
        self.scribe.replies = {}
        self.whisper.texts = []
        self.tmp = Path(tempfile.mkdtemp(prefix="diction-test-"))
        self.work = self.tmp / "notes"
        self.work.mkdir()
        maid = self.tmp / "maid"
        maid.write_text(FAKE_MAID)
        maid.chmod(0o755)
        # The fake claude is the only one on PATH; the real one is never reached.
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "claude").write_text(FAKE_CLAUDE)
        (self.bin / "claude").chmod(0o755)
        self.claude_log = self.tmp / "claude.log"
        # The side server is a closed port here, so the default stays on the main one.
        self.specs = {"qwen-4b": spec("llamacpp", self.scribe.server_address[1]), "llamacpp-2/fake": spec("llamacpp-2", 9)}
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("DICTION_") and k != "ANTHROPIC_API_KEY"}
        self.env.update(PYTHONPATH=str(REPO), PYTHONDONTWRITEBYTECODE="1", MAID_BIN=str(maid),
                        MAID_MODELS_DIR=str(self.tmp / "models"), XDG_CONFIG_HOME=str(self.tmp / "config"),
                        XDG_STATE_HOME=str(self.tmp / "state"), PATH=f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin",
                        FAKE_CLAUDE_LOG=str(self.claude_log), REAL_MAID=os.environ.get("MAID_BIN", ""),
                        DICTION_WHISPER_URL=f"http://127.0.0.1:{self.whisper.server_address[1]}")
        # Most cases exercise the pipeline on a local scribe and whatever whisper-server holds; the preset cases
        # below drop these to get the defaults.
        self.env.update(DICTION_BACKEND="local", DICTION_AGENT_MODEL="qwen-4b", DICTION_MODEL="current")

    def defaults(self):
        for k in ("DICTION_BACKEND", "DICTION_AGENT_MODEL", "DICTION_MODEL"):
            del self.env[k]

    def tearDown(self):
        for srv in (self.whisper, self.scribe):
            srv.shutdown()
            srv.server_close()

    def run_diction(self, said: list[str], *flags: str):
        """One run over len(said) utterances; returns (process, document text)."""
        self.whisper.texts = list(said)
        wavs = []
        for i in range(len(said)):
            silence(self.tmp / f"u{i}.wav")
            wavs += ["--from-wav", str(self.tmp / f"u{i}.wav")]
        p = self.diction(*wavs, *flags)
        doc = self.work / "notes.md"
        return p, doc.read_text() if doc.exists() else ""

    def diction(self, *args: str):
        env = dict(self.env, FAKE_MAID_SPECS=json.dumps(self.specs))
        return subprocess.run([sys.executable, "-P", "-m", "diction.cli", *args], cwd=self.work, env=env,
                              capture_output=True, text=True, timeout=60)

    def test_transcript_writes_the_scribes_cleaned_prose(self):
        self.scribe.replies = {"um so you uh open the shared inbox": "APPEND: So you open the shared inbox."}
        p, doc = self.run_diction(["um so you uh open the shared inbox"])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("So you open the shared inbox.", doc)
        self.assertNotIn("uh open", doc)
        path, fields, files = self.whisper.requests[0]
        self.assertEqual(path, "/inference")
        self.assertEqual((fields["temperature"], fields["response_format"]), (b"0.0", b"json"))
        self.assertEqual(files["file"][:4], b"RIFF")
        body = self.scribe.requests[0]
        self.assertEqual(body["messages"][0], {"role": "system", "content": SYSTEM_PROMPT_INSERT})
        self.assertTrue(body["stream"])
        self.assertEqual(body["chat_template_kwargs"], {"enable_thinking": False})
        self.assertEqual(body["reasoning_effort"], "none")
        self.assertIn("<steps>\n(empty - no steps recorded yet)\n</steps>", body["messages"][-1]["content"])
        self.assertIn("scribe: llamacpp/fake", p.stdout)

    def test_steps_numbers_each_step(self):
        self.scribe.replies = {"first you open the invoice screen": "APPEND: Open the invoice screen.",
                               "then copy the po number": "APPEND: Copy the PO number."}
        p, doc = self.run_diction(["first you open the invoice screen", "then copy the po number"], "--steps")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("1. Open the invoice screen.\n2. Copy the PO number.", doc)
        self.assertIn("1. Open the invoice screen.", self.scribe.requests[1]["messages"][-1]["content"])

    def test_no_agent_writes_raw_text_and_calls_no_scribe(self):
        p, doc = self.run_diction(["the raw words as heard", "Thank you."], "--no-agent", "--raw")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.scribe.requests, [])
        self.assertIn("## Verbatim narration", doc)
        self.assertIn("the raw words as heard", doc)
        raw = next((self.work / "diction-logs").glob("*-raw-*.log")).read_text()
        self.assertIn("] the raw words as heard", raw)
        self.assertIn("(dropped: Thank you.)", raw)

    def test_normal_mode_correction_revises_the_named_passage(self):
        first = "you filter to the last thirty days"
        fix = "correction, it's the last sixty days not thirty"
        self.scribe.replies = {first: "APPEND: You filter to the last thirty days.",
                               fix: "REVISE 1: You filter to the last sixty days."}
        p, doc = self.run_diction([first, fix], "--mode", "normal")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("You filter to the last sixty days.\n\n> correction, it's the last sixty days not thirty", doc)
        self.assertNotIn("thirty days.", doc)
        self.assertEqual(self.scribe.requests[1]["messages"][0]["content"], SYSTEM_PROMPT_NORMAL)

    def test_a_preset_name_prefers_the_side_server_when_it_answers(self):
        side = serve(FakeScribe)
        side.replies = {"hello there": "APPEND: Hello there."}
        self.specs["llamacpp-2/fake"] = spec("llamacpp-2", side.server_address[1])
        try:
            p, doc = self.run_diction(["hello there"])
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertEqual((len(side.requests), len(self.scribe.requests)), (1, 0))
            self.assertIn("scribe: llamacpp-2/fake", p.stdout)
            self.assertIn("Hello there.", doc)
            # Written out with its provider, the model stays on the main server.
            self.specs["llamacpp/fake"] = spec("llamacpp", self.scribe.server_address[1])
            with mock.patch.dict(os.environ, MAID_BIN=self.env["MAID_BIN"], FAKE_MAID_SPECS=json.dumps(self.specs)):
                self.assertEqual(resolve_agent("llamacpp/fake")["provider"], "llamacpp")
                self.assertEqual(resolve_agent("qwen-4b")["provider"], "llamacpp-2")
        finally:
            side.shutdown()
            side.server_close()

    def test_no_flags_is_legacy_claude_cli_haiku_and_distil_large_v3(self):
        self.defaults()
        said = ["open the shared inbox", "then filter to unbilled"]
        self.env["FAKE_CLAUDE_REPLIES"] = json.dumps({said[0]: "APPEND: Open the shared inbox.",
                                                      said[1]: "APPEND: Then filter to unbilled."})
        ggml(self.tmp / "models" / "whisper" / "ggml-distil-large-v3.bin")
        p, doc = self.run_diction(said)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("Open the shared inbox.\n\nThen filter to unbilled.", doc)
        self.assertEqual(self.scribe.requests, [])
        log = claude_log(self.claude_log)
        starts = [e for e in log if e["event"] == "start"]
        self.assertEqual(len(starts), 1, "one process for the session's scribe key, reused")
        self.assertEqual(starts[0]["argv"], legacy_argv(SYSTEM_PROMPT_INSERT, "haiku"))
        self.assertEqual(starts[0]["cwd"], str(self.tmp / "state" / "maid" / "diction" / "agent-cwd"))
        self.assertEqual([e["said"] for e in log if e["event"] == "message"], said)
        self.assertIn("<steps>\n1. Open the shared inbox.\n</steps>", log[-2]["payload"])
        self.assertEqual(log[-1]["event"], "eof", "stdin closed and the process ended on shutdown")
        load = [r for r in self.whisper.requests if r[0] == "/load"]
        self.assertTrue(load[0][1]["model"].endswith(b"/ggml-distil-large-v3.bin"), load)
        self.assertIn("preset: default · backend: claude-cli · whisper: distil-large-v3 · scribe: haiku", p.stdout)
        self.assertIn("the claude CLI sends the text of every utterance to Anthropic", p.stdout)

    def test_claude_missing_names_the_other_presets(self):
        self.defaults()
        (self.bin / "claude").unlink()
        p, _ = self.run_diction(["anything"])
        self.assertEqual(p.returncode, 1)
        self.assertIn("the default scribe runs through the claude CLI, which is not installed; "
                      "--preset api (needs ANTHROPIC_API_KEY) or --preset local keeps working", p.stdout)

    def anthropic_run(self, *flags: str):
        """One utterance through the Anthropic fake as haiku-4.5; returns (process, document, its requests)."""
        cloud = serve(FakeAnthropic)
        cloud.replies = {"open the shared inbox": "APPEND: Open the shared inbox."}
        self.specs["haiku-4.5"] = cloud_spec(cloud.server_address[1])
        self.env["ANTHROPIC_API_KEY"] = "test-key"
        try:
            p, doc = self.run_diction(["open the shared inbox"], *flags)
        finally:
            cloud.shutdown()
            cloud.server_close()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("Open the shared inbox.", doc)
        self.assertEqual((self.scribe.requests, claude_log(self.claude_log)), ([], []))
        path, headers, body = cloud.requests[0]
        self.assertEqual((path, headers["x-api-key"]), ("/v1/messages", "test-key"))
        self.assertEqual((body["model"], body["system"]), ("claude-haiku-4-5-20251001", SYSTEM_PROMPT_INSERT))
        self.assertIn("anthropic is a cloud provider", p.stdout)
        return p

    def test_preset_api_is_haiku_through_the_anthropic_api(self):
        self.defaults()
        ggml(self.tmp / "models" / "whisper" / "ggml-distil-large-v3.bin")
        p = self.anthropic_run("--preset", "api")
        self.assertIn("preset: api · backend: api · whisper: distil-large-v3 · scribe: anthropic/claude-haiku-4-5-20251001",
                      p.stdout)

    def test_backend_api_on_the_default_preset_maps_haiku(self):
        self.defaults()
        self.env["DICTION_MODEL"] = "current"
        p = self.anthropic_run("--backend", "api")
        self.assertIn("preset: default · backend: api", p.stdout)

    def test_api_without_a_key_names_the_variable_and_the_local_preset(self):
        self.defaults()
        self.env["DICTION_MODEL"] = "current"
        self.specs["haiku-4.5"] = cloud_spec(9)
        p, _ = self.run_diction(["anything"], "--preset", "api")
        self.assertEqual(p.returncode, 1)
        self.assertIn("set ANTHROPIC_API_KEY (or --preset local", p.stdout)

    def test_local_backend_refuses_a_cloud_model(self):
        self.specs["haiku-4.5"] = cloud_spec(9)
        p, _ = self.run_diction(["anything"], "--agent-model", "haiku")
        self.assertEqual(p.returncode, 1)
        self.assertIn("anthropic/claude-haiku-4-5-20251001 is a cloud model; the local backend keeps the text", p.stdout)

    def test_preset_local_is_the_9b_text_entry_on_llamacpp_2(self):
        self.defaults()
        self.scribe.replies = {"hello there": "APPEND: Hello there."}
        self.specs["llamacpp-2/Qwen3.5-9B-Q4_K_M-text"] = spec("llamacpp-2", self.scribe.server_address[1],
                                                               "Qwen3.5-9B-Q4_K_M-text", 8192)
        ggml(self.tmp / "models" / "whisper" / "ggml-large-v3-turbo-q5_0.bin")
        p, doc = self.run_diction(["hello there"], "--preset", "local")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("Hello there.", doc)
        self.assertEqual(self.scribe.requests[0]["model"], "Qwen3.5-9B-Q4_K_M-text")
        self.assertIn("preset: local · backend: local · whisper: large-v3-turbo-q5_0 · "
                      "scribe: llamacpp-2/Qwen3.5-9B-Q4_K_M-text", p.stdout)
        self.assertNotIn("cloud provider", p.stdout)
        self.assertNotIn("Anthropic", p.stdout)
        load = [r for r in self.whisper.requests if r[0] == "/load"]
        self.assertTrue(load[0][1]["model"].endswith(b"/ggml-large-v3-turbo-q5_0.bin"), load)

    def test_legacy_alias_haiku_on_the_api_backend_is_haiku_4_5(self):
        self.anthropic_run("--backend", "api", "--agent-model", "haiku")
        self.specs.update({"sonnet-5": cloud_spec(9, "claude-sonnet-5"), "opus-5.5": cloud_spec(9, "claude-opus-5-5")})
        with mock.patch.dict(os.environ, MAID_BIN=self.env["MAID_BIN"], FAKE_MAID_SPECS=json.dumps(self.specs)):
            self.assertEqual(resolve_agent("sonnet")["model"], "claude-sonnet-5")
            self.assertEqual(resolve_agent("opus")["model"], "claude-opus-5-5")

    def test_agent_model_on_claude_cli_goes_to_claude_model_unchanged(self):
        del self.env["DICTION_BACKEND"]
        p, _ = self.run_diction(["hello there"], "--agent-model", "sonnet", "--mode", "normal")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        start = claude_log(self.claude_log)[0]
        self.assertEqual(start["argv"], legacy_argv(SYSTEM_PROMPT_NORMAL, "sonnet"))

    @unittest.skipUnless(os.environ.get("MAID_BIN"), "needs the built maid (MAID_BIN; ctest sets it)")
    def test_diction_lua_through_maid_overrides_a_preset_and_sets_the_log_dir(self):
        lua = self.tmp / "config" / "maid" / "diction.lua"
        lua.parent.mkdir(parents=True)
        logs = self.tmp / "all-logs"
        lua.write_text('-- a comment, as Lua has them\n'
                       f'return {{ log_dir = "{logs}", presets = {{ ["local"] = {{ whisper = "distil-large-v3" }}, '
                       'mine = { scribe = "qwen-4b" } } }\n')
        self.defaults()
        self.specs["llamacpp-2/Qwen3.5-9B-Q4_K_M-text"] = spec("llamacpp-2", self.scribe.server_address[1])
        ggml(self.tmp / "models" / "whisper" / "ggml-distil-large-v3.bin")
        p, _ = self.run_diction(["hello there"], "--preset", "local")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("backend: local · whisper: distil-large-v3 · scribe: llamacpp-2/fake", p.stdout)
        self.assertIn(f"logs: {logs} ({lua})", p.stdout)
        self.assertTrue(list(logs.glob("*-raw-*.log")))
        self.assertNotIn("config.toml", p.stdout)
        cfg = {"presets": {"local": {"whisper": "distil-large-v3"}, "mine": {"scribe": "qwen-4b", "backend": "local"}}}
        self.assertEqual(presets.table(cfg)["local"], dict(presets.BUILTIN["local"], whisper="distil-large-v3"))
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(presets.choose("mine", None, None, None, cfg), ("mine", "local", "qwen-4b", "distil-large-v3"))
            with self.assertRaisesRegex(ValueError, "no scribe backend 'cloud'"):
                presets.choose("mine", None, None, None, {"presets": {"mine": {"backend": "cloud"}}})

    def test_config_toml_alone_is_still_read_with_one_notice(self):
        toml = self.tmp / "config" / "diction" / "config.toml"
        toml.parent.mkdir(parents=True)
        logs = self.tmp / "toml-logs"
        toml.write_text(f'log_dir = "{logs}"\n\n[presets.local]\nwhisper = "distil-large-v3"\n')
        self.defaults()
        self.specs["llamacpp-2/Qwen3.5-9B-Q4_K_M-text"] = spec("llamacpp-2", self.scribe.server_address[1])
        ggml(self.tmp / "models" / "whisper" / "ggml-distil-large-v3.bin")
        p, _ = self.run_diction(["hello there"], "--preset", "local")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("whisper: distil-large-v3", p.stdout)
        self.assertIn(f"logs: {logs} ({toml})", p.stdout)
        notice = (f"diction reads {toml}; `diction migrate-config` writes "
                  f"{self.tmp / 'config' / 'maid' / 'diction.lua'} from it")
        self.assertEqual(p.stdout.count(notice), 1, p.stdout)

    @unittest.skipUnless(os.environ.get("MAID_BIN"), "needs the built maid (MAID_BIN; ctest sets it)")
    def test_diction_lua_wins_over_config_toml_and_says_so(self):
        toml = self.tmp / "config" / "diction" / "config.toml"
        lua = self.tmp / "config" / "maid" / "diction.lua"
        for f in (toml, lua):
            f.parent.mkdir(parents=True)
        toml.write_text(f'log_dir = "{self.tmp / "toml-logs"}"\n')
        lua.write_text(f'return {{ log_dir = "{self.tmp / "lua-logs"}" }}\n')
        p, _ = self.run_diction(["hello there"])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn(f"logs: {self.tmp / 'lua-logs'} ({lua})", p.stdout)
        self.assertIn(f"diction reads {lua}; {toml} is ignored", p.stdout)
        self.assertNotIn("migrate-config", p.stdout)
        self.assertFalse((self.tmp / "toml-logs").exists())

    @unittest.skipUnless(os.environ.get("MAID_BIN"), "needs the built maid (MAID_BIN; ctest sets it)")
    def test_a_broken_diction_lua_is_named_and_ignored(self):
        lua = self.tmp / "config" / "maid" / "diction.lua"
        lua.parent.mkdir(parents=True)
        lua.write_text("return { log_dir = nil .. '/logs' }\n")

        p, _ = self.run_diction(["hello there"])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertRegex(p.stdout, rf"ignoring {re.escape(str(lua))}: {re.escape(str(lua))}:1: ")
        self.assertTrue((self.work / "diction-logs").is_dir())

    @unittest.skipUnless(os.environ.get("MAID_BIN"), "needs the built maid (MAID_BIN; ctest sets it)")
    def test_migrate_config_round_trips_through_maid(self):
        toml = self.tmp / "config" / "diction" / "config.toml"
        lua = self.tmp / "config" / "maid" / "diction.lua"
        toml.parent.mkdir(parents=True)
        text = ('# mine\nlog_dir = "~/diction-logs"\n\n[presets.mine]\nbackend = "local"\nscribe = "qwen-4b"\n'
                'note = "says \\"hi\\", a \\\\ and a tab\\t, caf\u00e9"\n\n[presets.local]\nwhisper = "distil-large-v3"\n\n'
                '[presets."two words"]\nend = "a keyword as a key"\n')
        toml.write_text(text)
        self.assertEqual(self.diction("migrate-config").returncode, 0)
        self.assertTrue(toml.exists(), "the TOML stays")
        written = lua.read_text()
        self.assertTrue(written.startswith(f"-- diction's settings, written by `diction migrate-config` from {toml}"), written)
        r = subprocess.run([os.environ["MAID_BIN"], "settings", "read", "diction"], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout), tomllib.loads(text))

        lua.write_text("return {}\n")
        again = self.diction("migrate-config")
        self.assertEqual(again.returncode, 1)
        self.assertIn("--force overwrites it", again.stdout)
        self.assertEqual(lua.read_text(), "return {}\n")
        self.assertEqual(self.diction("migrate-config", "--force").returncode, 0)
        self.assertEqual(lua.read_text(), written)

    def test_migrate_config_without_a_toml_says_so(self):
        p = self.diction("migrate-config")
        self.assertEqual(p.returncode, 1)
        self.assertIn("no " + str(self.tmp / "config" / "diction" / "config.toml"), p.stdout)
        self.assertFalse((self.tmp / "config" / "maid" / "diction.lua").exists())

    def test_presets_lists_each_with_what_is_missing(self):
        conf = self.tmp / "config" / "diction" / "config.toml"
        conf.parent.mkdir(parents=True)
        conf.write_text('[presets.mine]\nbackend = "local"\nscribe = "llamacpp-2/Qwen3.5-4B-Q4_K_M"\nnote = "my own"\n')
        self.specs.update({"haiku-4.5": cloud_spec(9),
                           "llamacpp-2/Qwen3.5-9B-Q4_K_M-text": spec("llamacpp-2", 9, "Qwen3.5-9B-Q4_K_M-text", 8192),
                           "llamacpp-2/Qwen3.5-4B-Q4_K_M": spec("llamacpp-2", 9, "Qwen3.5-4B-Q4_K_M", 8192)})
        self.defaults()
        p = self.diction("presets")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        out = re.sub(r"\x1b\[[0-9;]*m", "", p.stdout)
        self.assertEqual(re.findall(r"^(\S+)", out, re.M), ["default", "api", "local", "local-small", "mine"])
        default, api, local, small, mine = re.split(r"^\S.*\n", out, flags=re.M)[1:]
        self.assertIn("default (chosen)", out)
        self.assertIn("backend  claude-cli\n  scribe   haiku: claude --model haiku, cloud", default)
        self.assertIn(f"claude is {self.bin / 'claude'}", default)
        self.assertIn("narration goes to Anthropic through your claude login, as before", default)
        self.assertIn("backend  api\n  scribe   haiku-4.5: anthropic/claude-haiku-4-5-20251001, cloud", api)
        self.assertIn("no key: set ANTHROPIC_API_KEY", api)
        self.assertIn("not installed: maid models install whisper-distil-large-v3", default)
        self.assertIn("backend  local", local)
        self.assertIn("not installed: maid models install whisper-large-v3-turbo-q5_0", local)
        self.assertIn("not installed: maid models install qwen3.5-9b-text", local)
        self.assertIn("not installed: maid models install qwen3.5-4b", small)
        self.assertIn("server not answering at http://127.0.0.1:9/v1: maid up llamacpp-2", small)
        self.assertIn("my own", mine)
        self.assertIn("whisper  distil-large-v3", mine, "a config preset without whisper takes the default's")

        models = self.tmp / "models"
        ggml(models / "whisper" / "ggml-large-v3-turbo-q5_0.bin")
        (models / "llamacpp" / "Qwen3.5-4B-Q4_K_M").mkdir(parents=True)
        (models / "llamacpp" / "Qwen3.5-4B-Q4_K_M" / "Qwen3.5-4B-Q4_K_M.gguf").write_bytes(b"GGUF")
        (self.bin / "claude").unlink()
        self.env.update(DICTION_PRESET="local-small", ANTHROPIC_API_KEY="test-key")
        out = re.sub(r"\x1b\[[0-9;]*m", "", self.diction("presets").stdout)
        self.assertIn("local-small (chosen)", out)
        self.assertIn("claude is not on PATH: the default scribe runs through the claude CLI", out)
        self.assertIn("ANTHROPIC_API_KEY is set", out)
        self.assertIn("whisper  large-v3-turbo-q5_0: installed (ggml-large-v3-turbo-q5_0.bin)", out)
        self.assertNotIn("maid models install qwen3.5-4b", out)
        self.assertIn("maid models install qwen3.5-9b-text", out)

    def test_whisper_server_down_says_how_to_start_it(self):
        self.env["DICTION_WHISPER_URL"] = "http://127.0.0.1:9"
        p, _ = self.run_diction(["anything"])
        self.assertEqual(p.returncode, 1)
        self.assertIn("maid up whisper", p.stdout)


class Directives(unittest.TestCase):
    def setUp(self):
        self.doc = Procedure(Path(tempfile.mkdtemp(prefix="diction-doc-")) / "d.md", "D")
        self.doc.append("Open the shared inbox.")

    def test_insert_mode_refuses_mutating_directives(self):
        outcome = apply_reply(self.doc, "DROP 1\nREVISE 1: something else", echo=False,
                              said="delete that", allow_commands=False)
        self.assertEqual(self.doc.steps, ["Open the shared inbox.", "delete that"])
        self.assertIn("command ignored; kept verbatim", outcome)

    def test_insert_mode_keeps_a_skip_verbatim(self):
        apply_reply(self.doc, "SKIP", echo=False, said="uh hang on", allow_commands=False)
        self.assertEqual(self.doc.steps[-1], "uh hang on")


class Trimming(unittest.TestCase):
    rows = [(i, f"Passage number {i} of the document.") for i in range(1, 41)]

    def test_document_stays_and_the_oldest_exchanges_go(self):
        history = [(f"utterance {i} " + "x" * 300, f"APPEND: reply {i}") for i in range(20)]
        msgs = build_messages("SYSTEM", self.rows, "passage", "the new chunk", history, 2400)
        last = msgs[-1]["content"]
        self.assertTrue(all(f"{i}. Passage number {i} " in last for i, _ in self.rows), "every passage, with its number")
        self.assertIn("<utterance>\nthe new chunk\n</utterance>", last)
        kept = [m["content"] for m in msgs[1:-1] if m["role"] == "assistant"]
        self.assertTrue(0 < len(kept) < HISTORY_EXCHANGES, kept)
        self.assertEqual(kept[-1], "APPEND: reply 19", "the newest exchange is kept")
        self.assertNotIn("APPEND: reply 0", kept, "the oldest is dropped")
        self.assertEqual(msgs[0], {"role": "system", "content": "SYSTEM"})

    def test_history_is_capped_even_with_room(self):
        history = [(f"u{i}", f"APPEND: r{i}") for i in range(30)]
        msgs = build_messages("SYSTEM", self.rows, "passage", "now", history, 1_000_000)
        self.assertEqual(len(msgs), 2 + 2 * HISTORY_EXCHANGES)
        self.assertEqual(msgs[1]["content"], f"<utterance>\nu{30 - HISTORY_EXCHANGES}\n</utterance>")

    def test_a_document_larger_than_the_window_keeps_its_newest_passages_and_says_so(self):
        msgs = build_messages("SYSTEM", self.rows, "passage", "now", [], 1300)
        last = msgs[-1]["content"]
        self.assertIn("40. Passage number 40 ", last)
        self.assertNotIn("\n1. Passage number 1 ", last)
        self.assertRegex(last, r"\(passages 1-\d+ are not shown")


class Precedence(unittest.TestCase):
    def test_nothing_set_is_the_default_preset(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(presets.choose(None, None, None, None, {}), ("default", "claude-cli", "haiku", "distil-large-v3"))
            with self.assertRaisesRegex(ValueError, r"no preset 'nope' \(there: default, api, local, local-small\)"):
                presets.choose("nope", None, None, None, {})

    def test_flag_beats_environment_beats_preset(self):
        with mock.patch.dict(os.environ, {"DICTION_PRESET": "local"}, clear=True):
            self.assertEqual(presets.choose(None, None, None, None, {}),
                             ("local", "local", "llamacpp-2/Qwen3.5-9B-Q4_K_M-text", "large-v3-turbo-q5_0"))
            self.assertEqual(presets.choose("local-small", None, None, None, {}),
                             ("local-small", "local", "llamacpp-2/Qwen3.5-4B-Q4_K_M", "large-v3-turbo-q5_0"))
            os.environ.update(DICTION_BACKEND="api", DICTION_AGENT_MODEL="qwen-4b", DICTION_MODEL="distil-large-v3")
            self.assertEqual(presets.choose(None, None, None, None, {}), ("local", "api", "qwen-4b", "distil-large-v3"))
            self.assertEqual(presets.choose(None, "claude-cli", "sonnet", "current", {}),
                             ("local", "claude-cli", "sonnet", "current"))


class ClaudeCli(unittest.TestCase):
    """The claude-cli backend in process, against the fake claude: one process per scribe key, reaped on close."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="diction-claude-"))
        (self.tmp / "bin").mkdir()
        (self.tmp / "bin" / "claude").write_text(FAKE_CLAUDE)
        (self.tmp / "bin" / "claude").chmod(0o755)
        self.log = self.tmp / "claude.log"
        replies = {"one": "APPEND: One.", "two": "APPEND: Two.", "three": "APPEND: Three.", "boom": "FAIL"}
        patches = [mock.patch.dict(os.environ, PATH=f"{self.tmp / 'bin'}{os.pathsep}/usr/bin{os.pathsep}/bin",
                                   FAKE_CLAUDE_LOG=str(self.log), FAKE_CLAUDE_REPLIES=json.dumps(replies)),
                   mock.patch.object(scribe_mod, "AGENT_CWD", self.tmp / "agent-cwd")]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        self.doc = Procedure(self.tmp / "d.md", "D")

    def test_one_process_per_scribe_key_reused_and_reaped(self):
        agent = ClaudeScribe("haiku", 30)
        self.assertEqual(agent.ask(self.doc, "one", "insert"), "APPEND: One.")
        self.doc.append("One.")
        self.assertEqual(agent.ask(self.doc, "two", "insert"), "APPEND: Two.")
        self.assertEqual(agent.ask(self.doc, "three", "normal"), "APPEND: Three.")
        with self.assertRaisesRegex(RuntimeError, "overloaded"):
            agent.ask(self.doc, "boom", "normal")
        procs = list(agent.procs.values())
        agent.close()
        self.assertEqual([p.returncode for p in procs], [0, 0])
        for p in procs:
            p.stdout.close()
        log = claude_log(self.log)
        starts = [e for e in log if e["event"] == "start"]
        self.assertEqual([s["argv"] for s in starts],
                         [legacy_argv(SYSTEM_PROMPT_INSERT, "haiku"), legacy_argv(SYSTEM_PROMPT_NORMAL, "haiku")])
        self.assertEqual({s["cwd"] for s in starts}, {str(self.tmp / "agent-cwd")})
        self.assertEqual([e["said"] for e in log if e["event"] == "message"], ["one", "two", "three", "boom"])
        self.assertEqual(sum(e["event"] == "eof" for e in log), 2)
        second = next(e for e in log if e.get("said") == "two")
        self.assertIn("<steps>\n1. One.\n</steps>\n\n<utterance>\ntwo\n</utterance>", second["payload"])

    def test_a_dead_process_is_respawned(self):
        agent = ClaudeScribe("haiku", 30)
        agent.ask(self.doc, "one", "insert")
        first = agent.procs["insert"]
        first.kill()
        first.wait()
        self.assertEqual(agent.ask(self.doc, "two", "insert"), "APPEND: Two.")
        self.assertIsNot(agent.procs["insert"], first)
        agent.close()
        for p in (first, agent.procs["insert"]):
            p.stdin.close()
            p.stdout.close()


@unittest.skipUnless(os.environ.get("MAID_BIN"), "needs the built maid (MAID_BIN; ctest sets it)")
class Maid(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="diction-maid-"))
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(self.tmp / "config"), XDG_STATE_HOME=str(self.tmp / "state"),
                        PYTHONDONTWRITEBYTECODE="1")
        self.env.pop("MAID_MODELS_DIR", None)

    def maid(self, *args):
        return subprocess.run([os.environ["MAID_BIN"], *args], cwd=self.tmp, env=self.env, capture_output=True, text=True, timeout=60)

    def test_model_resolve_qwen_4b(self):
        p = self.maid("model", "resolve", "qwen-4b")
        self.assertEqual(p.returncode, 0, p.stderr)
        got = json.loads(p.stdout)
        self.assertEqual({k: got[k] for k in ("provider", "kind", "base_url", "model", "context", "remote")},
                         {"provider": "llamacpp", "kind": "openai", "base_url": "http://127.0.0.1:8081/v1",
                          "model": "Qwen3.5-4B-Q4_K_M", "context": 16384, "remote": False})
        side = json.loads(self.maid("model", "resolve", "llamacpp-2/Qwen3.5-4B-Q4_K_M").stdout)
        self.assertEqual((side["provider"], side["base_url"], side["context"]), ("llamacpp-2", "http://127.0.0.1:8082/v1", 8192))
        cloud = json.loads(self.maid("model", "resolve", "haiku-4.5").stdout)
        self.assertEqual((cloud["provider"], cloud["kind"], cloud["remote"], cloud["api_key_env"]), ("anthropic", "anthropic", True, "ANTHROPIC_API_KEY"))
        # What the presets and the legacy aliases name exists in maid.
        for name in ("sonnet-5", "opus-5.5"):
            self.assertEqual(json.loads(self.maid("model", "resolve", name).stdout)["provider"], "anthropic", name)
        for name in ("llamacpp-2/Qwen3.5-9B-Q4_K_M-text", "llamacpp-2/Qwen3.5-4B-Q4_K_M"):
            got = json.loads(self.maid("model", "resolve", name).stdout)
            self.assertEqual((got["base_url"], got["model"], got["context"]), ("http://127.0.0.1:8082/v1", name.split("/")[1], 8192))
        self.assertNotEqual(self.maid("model", "resolve").returncode, 0)

    def settings_read(self, lua: str | None):
        path = self.tmp / "config" / "maid" / "diction.lua"
        if lua is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(lua)
        return self.maid("settings", "read", "diction")

    def test_settings_read_diction_is_empty_without_the_file(self):
        p = self.settings_read(None)
        self.assertEqual((p.returncode, p.stdout), (0, "{}\n"), p.stderr)

    def test_settings_read_diction_evaluates_lua_as_json(self):
        p = self.settings_read('local home = os.getenv("HOME")\n'
                               'assert(os.time() and os.date("%Y") and os.clock())\n'
                               'local f = load("return 1 + 1")\n'
                               'print("to stderr")\n'
                               'return { log_dir = home .. "/logs", two = f(), presets = { mine = { scribe = "qwen-4b" } } }\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout), {"log_dir": os.environ["HOME"] + "/logs", "two": 2,
                                                "presets": {"mine": {"scribe": "qwen-4b"}}})
        self.assertEqual(p.stderr, "to stderr\n")

    def test_settings_read_diction_runs_at_global_lua(self):
        # diction.lua is the user's own file: full Lua by default, as settings.lua; global_lua = "sandbox" or
        # "restricted" in settings.lua runs it that way (docs/harness.md, Settings Lua runs at a level you choose).
        path = self.tmp / "config" / "maid" / "diction.lua"
        p = self.settings_read("os.execute('touch " + str(self.tmp / "full") + "')\nreturn { a = 1 }\n")
        self.assertEqual((p.returncode, json.loads(p.stdout)), (0, {"a": 1}), p.stderr)
        self.assertTrue((self.tmp / "full").exists(), "by default it runs as the user")
        for tier in ("sandbox", "restricted"):
            (self.tmp / "config" / "maid" / "settings.lua").write_text(f'return {{ global_lua = "{tier}" }}\n')
            for line in ("os.execute('touch " + str(self.tmp / "ran") + "')", "io.open('/etc/hostname')", "require('os')",
                         "local _ = ffi.C", "require('ffi')", "jit.off()", "debug.getinfo(1)", "package.loaded.os.exit(0)",
                         "dofile('/dev/null')", "loadfile('/dev/null')", "os.exit(0)", "os.remove('x')",
                         "local f, e = load(string.dump(function() end)); if not f then error(e) end",
                         "while true do end", "while true do pcall(function() while true do end end) end", "return {"):
                p = self.settings_read("local x = 1\n" + line + "\nreturn {}\n")
                self.assertEqual((p.returncode, p.stdout), (1, ""), tier + ": " + line)
                self.assertRegex(p.stderr, rf"^maid: {re.escape(str(path))}:[23]: ", tier + ": " + line)
            p = self.settings_read("return 1\n")
            self.assertEqual((p.returncode, p.stderr), (1, f"maid: {path}: must return a table\n"), tier)
            self.assertFalse((self.tmp / "ran").exists(), tier)


    def test_maid_diction_passes_through(self):
        p = self.maid("diction", "--help")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(p.stdout.startswith("usage: diction"), p.stdout[:200])
        self.assertIn("--agent-model", p.stdout)
        self.assertIn("--preset", p.stdout)
        self.assertEqual(self.maid("help", "diction").stdout, p.stdout)
        bad = self.maid("diction", "--no-such-flag")
        self.assertEqual(bad.returncode, 2, "diction's own exit code comes back through maid")
        self.assertIn("usage: diction", bad.stderr)


if __name__ == "__main__":
    unittest.main()
