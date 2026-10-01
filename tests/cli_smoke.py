#!/usr/bin/env python3
"""Runs the built maic binary headless against a fake OpenAI-compatible server: the real link, the real HTTP
client, the real settings loader. Under the asan preset this catches memory errors the in-process suites
cannot see (they link httplib themselves). Usage: cli_smoke.py PATH_TO_MAIC"""
import http.server, json, os, subprocess, sys, tempfile, threading

MAIC = sys.argv[1]


class Fake(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"data":[{"id":"fake"}]}')

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        last = [m for m in body.get("messages", []) if m.get("role") == "user"][-1]["content"]
        for piece in ("echo: ", last.split("\n")[0]):
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": piece}}]}) + "\n\n").encode())
        self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 5, "completion_tokens": 2}}) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]
home = tempfile.mkdtemp()
cfg = os.path.join(home, "config", "maic")
os.makedirs(cfg)
with open(os.path.join(cfg, "settings.lua"), "w") as f:
    f.write("return { model = 'fake/fake', providers = { fake = { kind = 'openai', base_url = 'http://127.0.0.1:%d/v1' } }, harness = 'dumb', load_instructions = false }\n" % port)
env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(home, "config"), XDG_STATE_HOME=os.path.join(home, "state"), MAIC_TRIPWIRE_FILE=os.path.join(home, "none"), ASAN_OPTIONS="detect_leaks=0")
r = subprocess.run([MAIC, "-p", "ping", "--no-instructions"], capture_output=True, text=True, env=env, cwd=home, timeout=120)
ok = r.returncode == 0 and "echo: ping" in r.stdout and "AddressSanitizer" not in r.stderr
print(("ok" if ok else "FAIL") + ": exit %d, stdout %r" % (r.returncode, r.stdout.strip()[:80]))
if not ok:
    print(r.stderr[-3000:])
# maic setup off a terminal: the plan and exit 2, nothing done (the settings file exists, so that step is not on it).
s = subprocess.run([MAIC, "setup"], capture_output=True, text=True, env=env, cwd=home, timeout=120, stdin=subprocess.DEVNULL)
setup_ok = s.returncode == 2 and "plan (each a yes/no in a terminal)" in s.stdout and "Build llama.cpp" in s.stdout and "Write the global settings" not in s.stdout and "AddressSanitizer" not in s.stderr
print(("ok" if setup_ok else "FAIL") + ": setup off a terminal, exit %d" % s.returncode)
if not setup_ok:
    print(s.stdout[-2000:], s.stderr[-2000:])
sys.exit(0 if ok and setup_ok else 1)
