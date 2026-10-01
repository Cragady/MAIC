"""diction against a fake whisper-server and a fake OpenAI-compatible scribe, never the microphone.

The pipeline runs as a user runs it, `python3 -m diction.cli`, with each utterance given as a WAV file (--from-wav)
instead of being cut from the mic. A fake `maic` answers `maic model resolve`. Two cases run the real maic from
MAIC_BIN (ctest sets it) and are skipped without it.

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
import unittest
import wave
from unittest import mock
from pathlib import Path

from diction.document import Procedure, apply_reply
from diction.scribe import HISTORY_EXCHANGES, SYSTEM_PROMPT_INSERT, SYSTEM_PROMPT_NORMAL, build_messages, resolve_agent

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


FAKE_MAIC = """#!/usr/bin/env python3
import json, os, sys
specs = json.loads(os.environ["FAKE_MAIC_SPECS"])
if sys.argv[1:3] == ["model", "resolve"] and sys.argv[3] in specs:
    print(json.dumps(specs[sys.argv[3]]))
    sys.exit(0)
print("maic: unknown model " + " ".join(sys.argv[1:]), file=sys.stderr)
sys.exit(1)
"""


def spec(provider, port, model="fake", context=16384):
    return {"provider": provider, "kind": "openai", "base_url": f"http://127.0.0.1:{port}/v1", "model": model,
            "context": context, "remote": False, "api_key_env": "", "api_key_command": ""}


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.whisper = serve(FakeWhisper)
        self.scribe = serve(FakeScribe)
        self.scribe.replies = {}
        self.whisper.texts = []
        self.tmp = Path(tempfile.mkdtemp(prefix="diction-test-"))
        self.work = self.tmp / "notes"
        self.work.mkdir()
        maic = self.tmp / "maic"
        maic.write_text(FAKE_MAIC)
        maic.chmod(0o755)
        # The side server is a closed port here, so the default stays on the main one.
        self.specs = {"qwen-4b": spec("llamacpp", self.scribe.server_address[1]), "llamacpp-2/fake": spec("llamacpp-2", 9)}
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("DICTION_")}
        self.env.update(PYTHONPATH=str(REPO), PYTHONDONTWRITEBYTECODE="1", MAIC_BIN=str(maic),
                        MAIC_MODELS_DIR=str(self.tmp / "models"), XDG_CONFIG_HOME=str(self.tmp / "config"),
                        DICTION_WHISPER_URL=f"http://127.0.0.1:{self.whisper.server_address[1]}")

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
        env = dict(self.env, FAKE_MAIC_SPECS=json.dumps(self.specs))
        p = subprocess.run([sys.executable, "-P", "-m", "diction.cli", *wavs, *flags], cwd=self.work, env=env,
                           capture_output=True, text=True, timeout=60)
        doc = self.work / "notes.md"
        return p, doc.read_text() if doc.exists() else ""

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

    def test_default_scribe_prefers_the_side_server_when_it_answers(self):
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
            with mock.patch.dict(os.environ, MAIC_BIN=self.env["MAIC_BIN"], FAKE_MAIC_SPECS=json.dumps(self.specs)):
                self.assertEqual(resolve_agent("llamacpp/fake")["provider"], "llamacpp")
                self.assertEqual(resolve_agent("qwen-4b")["provider"], "llamacpp-2")
        finally:
            side.shutdown()
            side.server_close()

    def test_whisper_server_down_says_how_to_start_it(self):
        self.env["DICTION_WHISPER_URL"] = "http://127.0.0.1:9"
        p, _ = self.run_diction(["anything"])
        self.assertEqual(p.returncode, 1)
        self.assertIn("maic up whisper", p.stdout)


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


@unittest.skipUnless(os.environ.get("MAIC_BIN"), "needs the built maic (MAIC_BIN; ctest sets it)")
class Maic(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="diction-maic-"))
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(self.tmp / "config"), XDG_STATE_HOME=str(self.tmp / "state"),
                        PYTHONDONTWRITEBYTECODE="1")
        self.env.pop("MAIC_MODELS_DIR", None)

    def maic(self, *args):
        return subprocess.run([os.environ["MAIC_BIN"], *args], cwd=self.tmp, env=self.env, capture_output=True, text=True, timeout=60)

    def test_model_resolve_qwen_4b(self):
        p = self.maic("model", "resolve", "qwen-4b")
        self.assertEqual(p.returncode, 0, p.stderr)
        got = json.loads(p.stdout)
        self.assertEqual({k: got[k] for k in ("provider", "kind", "base_url", "model", "context", "remote")},
                         {"provider": "llamacpp", "kind": "openai", "base_url": "http://127.0.0.1:8081/v1",
                          "model": "Qwen3.5-4B-Q4_K_M", "context": 16384, "remote": False})
        side = json.loads(self.maic("model", "resolve", "llamacpp-2/Qwen3.5-4B-Q4_K_M").stdout)
        self.assertEqual((side["provider"], side["base_url"], side["context"]), ("llamacpp-2", "http://127.0.0.1:8082/v1", 8192))
        cloud = json.loads(self.maic("model", "resolve", "haiku-4.5").stdout)
        self.assertEqual((cloud["provider"], cloud["kind"], cloud["remote"], cloud["api_key_env"]), ("anthropic", "anthropic", True, "ANTHROPIC_API_KEY"))
        self.assertNotEqual(self.maic("model", "resolve").returncode, 0)

    def test_maic_diction_passes_through(self):
        p = self.maic("diction", "--help")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(p.stdout.startswith("usage: diction"), p.stdout[:200])
        self.assertIn("--agent-model", p.stdout)
        self.assertEqual(self.maic("help", "diction").stdout, p.stdout)
        bad = self.maic("diction", "--no-such-flag")
        self.assertEqual(bad.returncode, 2, "diction's own exit code comes back through maic")
        self.assertIn("usage: diction", bad.stderr)


if __name__ == "__main__":
    unittest.main()
