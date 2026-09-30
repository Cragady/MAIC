"""Tests for the MAIC llama.cpp nodes, without ComfyUI: a fake OpenAI-compatible server on a loopback port.

    python3 -m unittest -v test_node      (ctest name: comfy_node)
"""

import base64
import http.server
import importlib.util
import json
import os
import socket
import struct
import threading
import unittest
import zlib

spec = importlib.util.spec_from_file_location("maic_llamacpp", os.path.join(os.path.dirname(os.path.abspath(__file__)), "__init__.py"))
node = importlib.util.module_from_spec(spec)
spec.loader.exec_module(node)


class FakeServer(http.server.HTTPServer):
    """Records every request body; answers with whatever `reply` holds (a message dict, or an int status)."""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.reply = {"role": "assistant", "content": "hello"}
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base_url(self):
        return "http://127.0.0.1:%d/v1" % self.server_address[1]

    def connection(self, model="current", timeout=5):
        return {"base_url": self.base_url, "model": model, "timeout": timeout}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, body))
        if isinstance(self.server.reply, int):
            self.send_response(self.server.reply)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error": {"message": "image input is not supported by this server"}}')
            return
        data = json.dumps({"choices": [{"index": 0, "message": self.server.reply, "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class Tensor:
    """The slice of torch.Tensor that png_data_url touches, over a nested list [H][W][C] of floats."""

    def __init__(self, rows):
        self.rows = rows
        self.shape = (len(rows), len(rows[0]), len(rows[0][0]))

    def _map(self, f):
        return Tensor([[[f(v) for v in px] for px in row] for row in self.rows])

    def mul(self, k):
        return self._map(lambda v: v * k)

    def round(self):
        return self._map(round)

    def clamp(self, lo, hi):
        return self._map(lambda v: min(max(v, lo), hi))

    def byte(self):
        return self._map(int)

    def cpu(self):
        return self

    def flatten(self):
        return self

    def tolist(self):
        return [v for row in self.rows for px in row for v in px]


def chat(server, **overrides):
    args = dict(connection=server.connection(), system="be brief", prompt="hi", think=False, format="text",
                temperature=0.8, top_k=40, top_p=0.95, min_p=0.05, seed=-1, extra_json="",
                keep_context=True, session_id="t", reset=False, unique_id="7")
    args.update(overrides)
    return node.MaicLlmChat().chat(**args)


class ChatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = FakeServer()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        node.SESSIONS.clear()
        self.server.requests.clear()
        self.server.reply = {"role": "assistant", "content": "hello"}

    def last_body(self):
        path, body = self.server.requests[-1]
        self.assertEqual(path, "/v1/chat/completions")
        return body

    def test_request_body(self):
        response, thinking, session = chat(self.server, prompt="one")
        self.assertEqual((response, thinking, session), ("hello", "", "t"))
        body = self.last_body()
        self.assertEqual(body["model"], "current")
        self.assertIs(body["stream"], False)
        self.assertEqual(body["messages"], [{"role": "system", "content": "be brief"}, {"role": "user", "content": "one"}])
        self.assertEqual(body["chat_template_kwargs"], {"enable_thinking": False})
        self.assertEqual(body["reasoning_format"], "deepseek")
        self.assertEqual(body["reasoning_effort"], "none")
        self.assertEqual((body["temperature"], body["top_k"], body["top_p"], body["min_p"], body["seed"]), (0.8, 40, 0.95, 0.05, -1))
        self.assertNotIn("response_format", body)

        chat(self.server, prompt="two", think=True)
        body = self.last_body()
        self.assertEqual(body["chat_template_kwargs"], {"enable_thinking": True})
        self.assertNotIn("reasoning_effort", body)

    def test_json_format(self):
        chat(self.server, format="json")
        self.assertEqual(self.last_body()["response_format"], {"type": "json_object"})

    def test_extra_json_merges_last(self):
        chat(self.server, extra_json='{"xtc_probability": 0.5, "dry_multiplier": 0.8, "temperature": 0.2, "logit_bias": [[15, false]]}')
        body = self.last_body()
        self.assertEqual(body["xtc_probability"], 0.5)
        self.assertEqual(body["dry_multiplier"], 0.8)
        self.assertEqual(body["temperature"], 0.2)
        self.assertEqual(body["logit_bias"], [[15, False]])
        with self.assertRaises(ValueError):
            chat(self.server, extra_json="[1, 2]")
        with self.assertRaises(json.JSONDecodeError):
            chat(self.server, extra_json="{not json")

    def test_history_per_session_and_reset(self):
        self.server.reply["content"] = "first reply"
        chat(self.server, prompt="first")
        self.server.reply["content"] = "second reply"
        chat(self.server, prompt="second")
        self.assertEqual(self.last_body()["messages"], [
            {"role": "system", "content": "be brief"},
            {"role": "user", "content": "first"},
            {"role": "assistant", "content": "first reply"},
            {"role": "user", "content": "second"},
        ])
        self.assertEqual(len(node.SESSIONS["t"]), 4)

        chat(self.server, prompt="other", session_id="u")
        self.assertEqual(len(self.last_body()["messages"]), 2)
        self.assertEqual(len(node.SESSIONS["t"]), 4)

        chat(self.server, prompt="again", reset=True)
        self.assertEqual(self.last_body()["messages"][1:], [{"role": "user", "content": "again"}])
        self.assertEqual(len(node.SESSIONS["t"]), 2)

    def test_no_context_stores_nothing(self):
        chat(self.server, prompt="a", keep_context=False)
        chat(self.server, prompt="b", keep_context=False)
        self.assertEqual(len(self.last_body()["messages"]), 2)
        self.assertEqual(node.SESSIONS, {})

    def test_empty_session_id_is_the_node_id(self):
        _, _, session = chat(self.server, session_id="", unique_id="42")
        self.assertEqual(session, "42")
        self.assertIn("42", node.SESSIONS)

    def test_thinking_is_split_out(self):
        self.server.reply = {"role": "assistant", "content": "the answer", "reasoning_content": "let me think"}
        response, thinking, _ = chat(self.server, think=True)
        self.assertEqual((response, thinking), ("the answer", "let me think"))
        self.assertEqual(node.SESSIONS["t"][-1], {"role": "assistant", "content": "the answer"})
        # Ollama's /v1 names the field `reasoning`.
        self.server.reply = {"role": "assistant", "content": "the answer", "reasoning": "hmm"}
        self.assertEqual(chat(self.server, think=True)[1], "hmm")

    def test_images_become_png_parts(self):
        image = Tensor([[[0.0, 0.5, 1.0], [1.0, 0.0, 0.0]], [[0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]])
        chat(self.server, prompt="what is this", images=[image, image])
        content = self.last_body()["messages"][-1]["content"]
        self.assertEqual(content[0], {"type": "text", "text": "what is this"})
        self.assertEqual([part["type"] for part in content[1:]], ["image_url", "image_url"])
        url = content[1]["image_url"]["url"]
        self.assertTrue(url.startswith("data:image/png;base64,"))
        png = base64.b64decode(url.split(",", 1)[1])
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        width, height, depth, color_type = struct.unpack(">IIBB", png[16:26])
        self.assertEqual((width, height, depth, color_type), (2, 2, 8, 2))
        idat_len = struct.unpack(">I", png[33:37])[0]
        raw = zlib.decompress(png[41:41 + idat_len])
        self.assertEqual(raw, b"\x00" + bytes([0, 128, 255, 255, 0, 0]) + b"\x00" + bytes([0, 255, 0, 0, 0, 255]))
        # The stored history keeps the text only.
        self.assertEqual(node.SESSIONS["t"][0], {"role": "user", "content": "what is this"})

    def test_http_error_text(self):
        self.server.reply = 400
        with self.assertRaises(RuntimeError) as cm:
            chat(self.server)
        self.assertIn("400", str(cm.exception))
        self.assertIn("image input is not supported", str(cm.exception))

    def test_unreachable_server(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        connection = {"base_url": "http://127.0.0.1:%d/v1" % port, "model": "current", "timeout": 5}
        with self.assertRaises(RuntimeError) as cm:
            chat(self.server, connection=connection)
        self.assertTrue(str(cm.exception).startswith("llama.cpp is not running: maic up llamacpp"))


class ServerTests(unittest.TestCase):
    def test_connection(self):
        (connection,) = node.MaicLlmServer().connect("http://127.0.0.1:8081/v1/", "current", 300)
        self.assertEqual(connection, {"base_url": "http://127.0.0.1:8081/v1", "model": "current", "timeout": 300})


if __name__ == "__main__":
    unittest.main()
