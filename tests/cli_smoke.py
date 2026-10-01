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

# `maic tools check` over the shipped script tool examples, a scaffolded tool and a broken manifest.
import shutil
examples = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "examples")
tools = os.path.join(home, ".maic", "tools")
for name in ("word_count", "json_pick"):
    shutil.copytree(os.path.join(examples, name), os.path.join(tools, name))
r = subprocess.run([MAIC, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
check_ok = r.returncode == 0 and "ok    " in r.stdout and "word_count (python)" in r.stdout and "json_pick (sh)" in r.stdout and "2 manifests, 0 problems" in r.stdout
print(("ok" if check_ok else "FAIL") + ": maic tools check on the examples, exit %d" % r.returncode + ("" if check_ok else "\n" + r.stdout.strip()))
r = subprocess.run([MAIC, "tools", "new", "greet", "--lang", "sh"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
new_ok = r.returncode == 0 and os.path.isfile(os.path.join(tools, "greet", "tool.json")) and os.path.isfile(os.path.join(tools, "greet", "main.sh"))
print(("ok" if new_ok else "FAIL") + ": maic tools new scaffolds a tool, exit %d" % r.returncode + ("" if new_ok else "\n" + r.stdout.strip()[:200]))
os.makedirs(os.path.join(tools, "needs_net"))
with open(os.path.join(tools, "needs_net", "tool.json"), "w") as f:
    f.write('{"name": "needs_net", "description": "x", "run": ["sh", "-c", "true"], "network": true}')
r = subprocess.run([MAIC, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
bad_ok = r.returncode == 1 and "FAIL  " in r.stdout and "per-tool network grants are not implemented yet" in r.stdout and "4 manifests, 1 problem" in r.stdout
print(("ok" if bad_ok else "FAIL") + ": maic tools check reports a manifest asking for the network, exit %d" % r.returncode + ("" if bad_ok else "\n" + r.stdout.strip()))
r = subprocess.run([MAIC, "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
list_ok = r.returncode == 0 and "word_count  (python)" in r.stdout and "reads **; writes nothing; timeout 10 s" in r.stdout and "tool skipped" in r.stdout and "maic-panel-check" in r.stdout
print(("ok" if list_ok else "FAIL") + ": maic tools lists script tools with language and declared reads/writes" + ("" if list_ok else "\n" + r.stdout.strip()))
sys.exit(0 if ok and setup_ok and check_ok and new_ok and bad_ok and list_ok else 1)
