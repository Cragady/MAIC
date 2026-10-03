#!/usr/bin/env python3
"""Runs the built maid binary headless against a fake OpenAI-compatible server: the real link, the real HTTP
client, the real settings loader. Under the asan preset this catches memory errors the in-process suites
cannot see (they link httplib themselves). Usage: cli_smoke.py PATH_TO_MAID

Also a module for test_tui.py (the fake, the throwaway home), and `cli_smoke.py --serve` runs the fake alone,
printing its port."""
import http.server, json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time


class Fake(http.server.BaseHTTPRequestHandler):
    """Answers every chat with "echo: " plus the first line of the last user message, streamed as SSE. A message
    starting with "hold" gets a reply that idles for 10 s instead, for the interrupt tests; one starting "slow:" is
    answered after three seconds, for a test that needs the agent busy; "shell:CMD" is a run_shell call of CMD,
    answered "ran it" once its result is in; "ask:QUESTION|OPTION|..." is a question call, answered "ran it" too;
    "bg:AGENT:JOB" is a task call that runs JOB as AGENT in the background, answered "ran it" too."""

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
        last = [m for m in body.get("messages", []) if m.get("role") == "user"][-1]["content"]
        if isinstance(last, list):  # text parts beside an image
            last = "".join(p.get("text", "") for p in last if p.get("type") == "text")
        try:
            answered = body["messages"][-1].get("role") == "tool"
            if last.startswith("shell:"):
                self.call("run_shell", {"command": last[6:]}, answered)
            elif last.startswith("ask:"):
                q = last[4:].split("|")
                self.call("question", {"question": q[0], "options": q[1:]}, answered)
            elif last.startswith("bg:"):
                agent, _, job = last[3:].partition(":")
                self.call("task", {"agent": agent, "prompt": job, "background": True}, answered)
            else:
                self.reply(last)
        except (BrokenPipeError, ConnectionResetError):
            pass  # MAID hung up mid-reply (an interrupt does): that is the end of this reply

    def call(self, name, args, answered):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if answered:
            chunks = [{"choices": [{"delta": {"content": "ran it"}}]}, {"choices": [{"delta": {}, "finish_reason": "stop"}]}]
        else:
            call = {"index": 0, "id": "call_1", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}
            chunks = [{"choices": [{"delta": {"tool_calls": [call]}}]}, {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]}]
        for c in chunks:
            self.wfile.write(("data: " + json.dumps(c) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")

    def reply(self, last):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if last.startswith("hold"):  # a reply that never ends: only an interrupt brings the turn back
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": "holding"}}]}) + "\n\n").encode())
            for _ in range(500):
                time.sleep(0.02)
                self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {}}]}) + "\n\n").encode())
            return
        if last.startswith("slow:"):
            time.sleep(3)
        for piece in ("echo: ", last.split("\n")[0]):
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": piece}}]}) + "\n\n").encode())
        self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 5, "completion_tokens": 2}}) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")


def start_fake():
    """The fake on a free loopback port, serving on a daemon thread; returns (server, port)."""
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def make_home(port):
    """A throwaway home whose settings point maid at the fake: (home dir, environment for maid).

    XDG_* keep ~/.config/maid and ~/.local/state/maid out of it; the tripwire file is one that never exists."""
    home = tempfile.mkdtemp(prefix="maid-smoke-")
    cfg = os.path.join(home, "config", "maid")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "settings.lua"), "w") as f:
        f.write("return { model = 'fake/fake', providers = { fake = { kind = 'openai', base_url = 'http://127.0.0.1:%d/v1' } }, harness = 'dumb', load_instructions = false }\n" % port)
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(home, "config"), XDG_STATE_HOME=os.path.join(home, "state"),
               XDG_RUNTIME_DIR=os.path.join(home, "run"), MAID_TRIPWIRE_FILE=os.path.join(home, "none"), ASAN_OPTIONS="detect_leaks=0")
    env.pop("NVIM", None)  # run from inside nvim's terminal, maid would connect to that nvim (maid.nvim)
    os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700)
    return home, env


def main():
    if sys.argv[1] == "--serve":
        # For a test in another process (test_tui.py forks a job-control stub, so it keeps no threads of its own).
        srv, port = start_fake()
        print("port %d" % port, flush=True)
        threading.Event().wait()
    maid = os.path.abspath(sys.argv[1])
    srv, port = start_fake()
    home, env = make_home(port)
    r = subprocess.run([maid, "-p", "ping", "--no-instructions"], capture_output=True, text=True, env=env, cwd=home, timeout=120)
    ok = r.returncode == 0 and "echo: ping" in r.stdout and "AddressSanitizer" not in r.stderr
    print(("ok" if ok else "FAIL") + ": exit %d, stdout %r" % (r.returncode, r.stdout.strip()[:80]))
    if not ok:
        print(r.stderr[-3000:])
    # The same turn as JSON lines, through the in-process engine, its connection recorded: `maid protocol check` passes it.
    rec = os.path.join(home, "headless-streams")
    os.makedirs(rec)
    j = subprocess.run([maid, "-p", "ping", "--no-instructions", "--json"], capture_output=True, text=True, env=dict(env, MAID_PROTOCOL_RECORD=rec), cwd=home, timeout=120)
    c = subprocess.run([maid, "protocol", "check", rec], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    lines = [json.loads(l) for l in j.stdout.splitlines() if l.strip()]
    stream_ok = (j.returncode == 0 and "".join(l["text"] for l in lines if l["type"] == "text") == "echo: ping" and lines[-1]["type"] == "usage"
                 and c.returncode == 0 and "1 stream, 0 with a violation" in c.stdout)
    print(("ok" if stream_ok else "FAIL") + ": maid -p --json through the engine, its recorded stream passes maid protocol check" +
          ("" if stream_ok else "\n" + j.stdout[-1500:] + j.stderr[-1500:] + c.stdout[-1500:]))
    # The recording describes itself: a header names the protocol hash, each event shape has a skeleton before it, and
    # `--blind` (no built-in schemas, only the file's own skeletons) still passes.
    recfile = os.path.join(rec, sorted(os.listdir(rec))[0])
    recs = [json.loads(l) for l in open(recfile) if l.strip()]
    phash = subprocess.run([maid, "protocol", "hash"], capture_output=True, text=True, env=env, cwd=home, timeout=30).stdout.strip()
    header = next((r for r in recs if r.get("dir") == "header"), None)
    skels = [r for r in recs if r.get("dir") == "skeleton"]
    blind = subprocess.run([maid, "protocol", "check", "--blind", recfile], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    self_ok = (header is not None and header.get("protocol") == phash and header.get("canonical") == "RFC 8785"
               and phash.startswith("sha256:") and any(k.get("of") == "maid.session.state" for k in skels)
               and all("hash" in k and "skeleton" in k for k in skels)
               and blind.returncode == 0 and "0 with a violation" in blind.stdout)
    print(("ok" if self_ok else "FAIL") + ": the recorded stream describes itself and passes maid protocol check --blind"
          + ("" if self_ok else "\n" + json.dumps(header) + "\n" + blind.stdout[-1500:] + blind.stderr[-1500:]))
    # maid setup off a terminal: the plan and exit 2, nothing done (the settings file exists, so that step is not on it).
    s = subprocess.run([maid, "setup"], capture_output=True, text=True, env=env, cwd=home, timeout=120, stdin=subprocess.DEVNULL)
    setup_ok = s.returncode == 2 and "plan (each a yes/no in a terminal)" in s.stdout and "Build llama.cpp" in s.stdout and "Write the global settings" not in s.stdout and "AddressSanitizer" not in s.stderr
    # The models it offers come from the catalog, by id.
    setup_ok = setup_ok and "Fetch qwen3.5-4b (" in s.stdout and "Fetch qwen3.5-9b-text (a link to qwen3.5-9b's weights" in s.stdout
    print(("ok" if setup_ok else "FAIL") + ": setup off a terminal, exit %d" % s.returncode)
    if not setup_ok:
        print(s.stdout[-2000:], s.stderr[-2000:])
    # `maid tools check` over the shipped script tool examples, a scaffolded tool and a broken manifest.
    import shutil
    examples = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "examples")
    tools = os.path.join(home, ".maid", "tools")
    for name in ("word_count", "json_pick"):
        shutil.copytree(os.path.join(examples, name), os.path.join(tools, name))
    r = subprocess.run([maid, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    check_ok = r.returncode == 0 and "ok    " in r.stdout and "word_count (python)" in r.stdout and "json_pick (sh)" in r.stdout
    print(("ok" if check_ok else "FAIL") + ": maid tools check on the examples, exit %d" % r.returncode + ("" if check_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    r = subprocess.run([maid, "tools", "new", "greet", "--lang", "sh"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    new_ok = r.returncode == 0 and os.path.isfile(os.path.join(tools, "greet", "tool.json")) and os.path.isfile(os.path.join(tools, "greet", "main.sh"))
    print(("ok" if new_ok else "FAIL") + ": maid tools new scaffolds a tool, exit %d" % r.returncode + ("" if new_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    os.makedirs(os.path.join(tools, "needs_net"))
    with open(os.path.join(tools, "needs_net", "tool.json"), "w") as f:
        f.write('{"name": "needs_net", "description": "x", "run": ["sh", "-c", "true"], "network": true}')
    r = subprocess.run([maid, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    bad_ok = r.returncode == 1 and "FAIL  " in r.stdout and "per-tool network grants are not implemented yet" in r.stdout
    print(("ok" if bad_ok else "FAIL") + ": maid tools check reports a manifest asking for the network, exit %d" % r.returncode + ("" if bad_ok else "\n" + r.stdout[-1500:]))
    # `maid protocol check` on a recorded stream that breaks a rule: the first event of a load must be number 0.
    stream = os.path.join(home, "bad-stream.jsonl")
    with open(stream, "w") as f:
        f.write('{"dir":"in","conn":"c1","msg":{"jsonrpc":"2.0","id":1,"method":"maid.session.subscribe","params":{"session":"s1"}}}\n')
        f.write('{"dir":"out","conn":"c1","msg":{"jsonrpc":"2.0","id":1,"result":{"epoch":"q7c2","sequence_number":0,"activity":"idle","replay_from":0}}}\n')
        f.write('{"dir":"out","conn":"c1","msg":{"jsonrpc":"2.0","method":"maid.event","params":{"type":"maid.notice","sequence_number":0,"stream_id":"s1","text":"hi","level":"info"}}}\n')
    r = subprocess.run([maid, "protocol", "check", stream], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    proto_ok = r.returncode == 1 and "FAIL  " + stream + ": #0 seq.start:" in r.stdout and "1 stream, 1 with a violation" in r.stdout
    print(("ok" if proto_ok else "FAIL") + ": maid protocol check names the first violation and its rule, exit %d" % r.returncode + ("" if proto_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    u = subprocess.run([maid, "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    r = subprocess.run([maid, "tools", "--trust"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    list_ok = (r.returncode == 0 and "word_count  (python)" in r.stdout and "reads **; writes nothing; timeout 10 s" in r.stdout
               and "word_count  (python)" not in u.stdout and "this directory is untrusted: its .maid/tools/ are not loaded" in u.stdout)
    print(("ok" if list_ok else "FAIL") + ": maid tools lists script tools with language and declared reads/writes, once the directory is trusted" + ("" if list_ok else "\n" + r.stdout[-1500:] + u.stdout[-1500:]))
    r = subprocess.run([maid, "themes"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    themes_ok = r.returncode == 0 and "* default  built in" in r.stdout and "  gruvbox-dark  " in r.stdout and "  mono  " in r.stdout
    print(("ok" if themes_ok else "FAIL") + ": maid themes lists the shipped themes with the active one marked" + ("" if themes_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    # cai-tools (docs/cai.md): `maid cai read` on a MAID session in the throwaway state dir, the `cai` wrapper matching
    # it byte for byte, a tool's help through `maid help`, the listing, and the dispatcher's exit code coming through.
    sess_dir = os.path.join(env["XDG_STATE_HOME"], "maid", "sessions", "general")
    os.makedirs(sess_dir, mode=0o700)
    sess = os.path.join(sess_dir, "20260101-120000-tui-1.jsonl")
    with open(sess, "w") as f:
        for rec in ({"type": "start", "workspace": home, "model": "fake/fake", "mode": "manual", "host": "h", "pid": 1},
                    {"type": "user", "text": "hello from a maid session", "provider": "fake", "model": "fake/fake", "remote": False, "mode": "manual"},
                    {"type": "msg", "role": "user", "content": "hello from a maid session"},
                    {"type": "tool", "tool": "read_file", "arguments": {"path": "a.txt"}, "result": "the file", "ok": True},
                    {"type": "assistant", "text": "echo: hello from a maid session"},
                    {"type": "usage", "input": 5, "output": 2, "context": 7}, {"type": "title", "text": "smoke"}):
            f.write(json.dumps(dict(rec, time="2026-01-01T12:00:00+0000")) + "\n")
    r = subprocess.run([maid, "cai", "read", sess], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    cai_ok = r.returncode == 0 and "hello from a maid session" in r.stdout and "echo: hello" in r.stdout and "L2 \u00b7 user" in r.stdout
    print(("ok" if cai_ok else "FAIL") + ": maid cai read on a MAID session, exit %d" % r.returncode + ("" if cai_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    cai = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "cai", "bin", "cai")
    w = subprocess.run([cai, "read", sess, "--select", "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    m = subprocess.run([maid, "cai", "read", sess, "--select", "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    wrap_ok = w.returncode == m.returncode == 0 and w.stdout == m.stdout and "[tool_use read_file]" in w.stdout
    print(("ok" if wrap_ok else "FAIL") + ": the cai wrapper and maid cai agree byte for byte" + ("" if wrap_ok else "\n" + w.stdout[-800:] + w.stderr[-800:] + m.stderr[-800:]))
    h = subprocess.run([maid, "help", "trans-fairy"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    c = subprocess.run([cai, "--help"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    mc = subprocess.run([maid, "cai", "--help"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    help_ok = h.returncode == 0 and "cai trans-fairy" in h.stdout and c.returncode == mc.returncode == 0 and c.stdout == mc.stdout and "trans-fairy-write" in c.stdout and "fabricate" in c.stdout
    print(("ok" if help_ok else "FAIL") + ": maid help trans-fairy and cai --help pass through" + ("" if help_ok else "\n" + h.stdout[-600:] + h.stderr[-600:] + c.stdout[-600:]))
    r = subprocess.run([maid, "cai", "nosuchtool"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    t = subprocess.run([maid, "tools", "--trust"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    exit_ok = (r.returncode == 2 and "no such tool" in r.stderr and "cai-tools (docs/cai.md" in t.stdout
               and all(("cai %s" % n) in t.stdout for n in ("trans-fairy", "trans-fairy-write", "fabricate", "read", "reflow"))
               and "maid trans-fairy-write" in t.stdout and "maid cai read" in t.stdout)
    print(("ok" if exit_ok else "FAIL") + ": the dispatcher's exit code comes through and maid tools lists the cai block" + ("" if exit_ok else "\n" + r.stderr[-600:] + t.stdout[-1200:]))
    # maid lazy-lock over a lock file in the throwaway XDG_CONFIG_HOME: none, not recorded, record, in sync, a lazy.nvim update.
    lazy = lambda *a: subprocess.run([maid, "lazy-lock", *a], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    lock = os.path.join(home, "config", "nvim", "lazy-lock.json")
    steps = [(lazy(), 2, "no lazy-lock.json at")]
    os.makedirs(os.path.dirname(lock))
    with open(lock, "w") as f:
        f.write('{\n  "lazy.nvim": { "branch": "main", "commit": "1111111aaaa" }\n}\n')
    steps.append((lazy(), 1, "not recorded yet"))
    steps.append((lazy("record"), 0, "old: none"))
    steps.append((lazy(), 0, "in sync"))
    with open(lock, "w") as f:
        f.write('{\n  "lazy.nvim": { "branch": "main", "commit": "2222222bbbb" }\n}\n')
    steps.append((lazy("diff"), 1, "updated  lazy.nvim  commit 1111111..2222222  (package manager updated)"))
    lazy_ok = all(r.returncode == rc and want in r.stdout and "AddressSanitizer" not in r.stderr for r, rc, want in steps)
    print(("ok" if lazy_ok else "FAIL") + ": maid lazy-lock status, record and diff with their exit codes" +
          ("" if lazy_ok else "\n" + "\n".join("exit %d: %s" % (r.returncode, r.stdout + r.stderr) for r, _, _ in steps)))
    # maid sessions redact --in-place keeps its copy where trans-fairy-write keeps its own, so cai's list-backups and
    # restore see it.
    with open(sess) as f:
        original = f.read()
    r = subprocess.run([maid, "sessions", "redact", sess, "--in-place"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    lb = subprocess.run([cai, "trans-fairy-write", "list-backups", "20260101-120000-tui-1"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    listed = json.loads(lb.stdout).get("backups", []) if lb.returncode == 0 else []
    with open(sess) as f:
        last = json.loads(f.read().splitlines()[-1])
    rs = subprocess.run([cai, "trans-fairy-write", "restore", "20260101-120000-tui-1", "--backup", listed[0]["taken"] if listed else "x", "--ignore-live"],
                        capture_output=True, text=True, env=env, cwd=home, timeout=60)
    with open(sess) as f:
        restored = f.read()
    backup_ok = (r.returncode == 0 and "the original is kept at" in r.stdout and len(listed) == 1 and last.get("type") == "rewritten"
                 and last.get("backup") == listed[0]["backup"] and rs.returncode == 0 and restored.startswith(original)
                 and json.loads(restored.splitlines()[-1]).get("type") == "rewritten")
    print(("ok" if backup_ok else "FAIL") + ": maid sessions redact --in-place keeps a copy that cai trans-fairy-write list-backups and restore see" +
          ("" if backup_ok else "\n" + r.stdout + r.stderr + lb.stdout + lb.stderr + rs.stdout + rs.stderr))
    output_ok = output_smoke(maid, env, sess_dir)
    rehome_ok = rehome_smoke(maid, env, home)
    # `maid settings read diction`: {} without the file, the table as JSON, a refused call with file:line.
    diction_lua = os.path.join(home, "config", "maid", "diction.lua")
    r = subprocess.run([maid, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = r.returncode == 0 and r.stdout == "{}\n"
    with open(diction_lua, "w") as f:
        f.write("return { log_dir = '/tmp/x', presets = { mine = { scribe = 'qwen-4b' } } }\n")
    r = subprocess.run([maid, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 0 and json.loads(r.stdout) == {"log_dir": "/tmp/x", "presets": {"mine": {"scribe": "qwen-4b"}}}
    with open(diction_lua, "w") as f:
        f.write("print('to stderr')\nreturn {\n  x = io.open('/etc/hostname') and 'opened',\n}\n")
    r = subprocess.run([maid, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 0 and json.loads(r.stdout) == {"x": "opened"} and r.stderr == "to stderr\n"  # the user's own file: full Lua
    global_lua = os.path.join(home, "config", "maid", "settings.lua")
    with open(global_lua) as f:
        saved = f.read()
    with open(global_lua, "w") as f:
        f.write(saved.replace("return { ", "return { global_lua = 'sandbox', ", 1))
    r = subprocess.run([maid, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 1 and r.stdout == "" and diction_lua + ":3: io is not available" in r.stderr and "AddressSanitizer" not in r.stderr
    with open(global_lua, "w") as f:
        f.write(saved)

    print(("ok" if read_ok else "FAIL") + ": maid settings read diction" + ("" if read_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    models_ok = models_smoke(maid, port)
    mcp_ok = mcp_smoke(maid, port)
    trust_ok = trust_smoke(maid, port)
    kit_ok = agent_kit_smoke(maid, port)
    color_ok = color_smoke(maid, port)
    trail_ok = audit_trail_smoke(maid, port)
    rpc_ok = rpc_smoke(maid, port)
    daemon_ok = daemon_smoke(maid, port)
    ui_ok = nvim_ui_smoke(maid, port)
    degrade_ok = degrade_smoke(maid, port)
    srv.shutdown()
    sys.exit(0 if degrade_ok and kit_ok and rpc_ok and daemon_ok and ui_ok and ok and stream_ok and trust_ok and color_ok and trail_ok and setup_ok and check_ok and new_ok and bad_ok and proto_ok and list_ok and themes_ok and cai_ok and wrap_ok and help_ok and exit_ok and lazy_ok and backup_ok and output_ok and rehome_ok and read_ok and models_ok and mcp_ok else 1)


class RpcClient:
    """`maid --rpc` driven as maid.nvim drives it: one JSON-RPC message per line each way. With `record`, every
    message is written as `maid protocol check` reads a connection, a request before its answer."""

    def __init__(self, maid, env, cwd, record=None):
        self.p = subprocess.Popen([maid, "--rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, cwd=cwd)
        self.rec = open(record, "w") if record else None
        self.lock = threading.Lock()
        self.cv = threading.Condition(self.lock)
        self.msgs, self.answers, self.next = [], {}, 1
        self.stderr = b""
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _record(self, d, msg):
        if self.rec:
            self.rec.write(json.dumps({"dir": d, "conn": "c1", "msg": msg}) + "\n")
            self.rec.flush()

    def _read(self):
        for line in self.p.stdout:
            msg = json.loads(line)
            with self.cv:
                if "id" in msg and "method" not in msg and msg["id"] is not None:
                    self._record("out", msg)
                    self.answers[msg["id"]] = msg
                elif "method" in msg:
                    self._record("out", msg)
                self.msgs.append(msg)
                self.cv.notify_all()

    def _read_stderr(self):
        self.stderr = self.p.stderr.read()

    def send(self, method, params=None, raw=None):
        with self.cv:
            if raw is not None:
                self.p.stdin.write(raw)
                self.p.stdin.flush()
                return None
            i, self.next = self.next, self.next + 1
            msg = {"jsonrpc": "2.0", "id": i, "method": method, "params": params or {}}
            self._record("in", msg)
            self.p.stdin.write((json.dumps(msg) + "\n").encode())
            self.p.stdin.flush()
            return i

    def wait(self, pred, timeout=30):
        with self.cv:
            ok = self.cv.wait_for(lambda: any(pred(m) for m in self.msgs), timeout)
            return next((m for m in self.msgs if pred(m)), None) if ok else None

    def answer(self, i, timeout=30):
        return self.wait(lambda m: m.get("id") == i and "method" not in m, timeout)

    def call(self, method, params=None, timeout=30):
        return self.answer(self.send(method, params), timeout)

    def events(self, session):
        with self.cv:
            return [m["params"] for m in self.msgs if m.get("method") == "maid.event" and m["params"].get("stream_id") == session]

    def close(self, timeout=30):
        self.p.stdin.close()
        try:
            code = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            code = None
        if self.rec:
            self.rec.close()
        return code


def rpc_smoke(maid, port):
    """`maid --rpc`: the handshake, a session created and followed, a turn, an approval answered over the pipe, a
    request answered while a long one runs, a resume with starting_after, the error codes, and the end at EOF and
    at a signal. Both ends' recordings pass `maid protocol check`."""
    import signal
    home, env = make_home(port)
    rec = os.path.join(home, "rpc-streams")
    os.makedirs(rec)
    results = []

    def report(ok, what, extra=""):
        results.append(ok)
        print(("ok" if ok else "FAIL") + ": maid --rpc " + what + ("" if ok else "\n" + extra[-3000:]))

    c = RpcClient(maid, dict(env, MAID_PROTOCOL_RECORD=rec), home, os.path.join(rec, "client.jsonl"))
    a = c.call("createConversation")
    early = a and a.get("error", {}).get("data", {}).get("code") == "maid_hello_required"
    h = c.call("maid.hello", {"protocol": 1, "client": {"name": "rpc-smoke", "version": "0"}, "capabilities": ["tool_output"]})
    hr = (h or {}).get("result", {})
    report(early and hr.get("protocol") == 1 and hr.get("path") == {"via": "stdio"} and hr.get("origin") == "local",
           "answers only maid.hello first, then the hello over stdio", json.dumps([a, h]))
    conv = (c.call("createConversation") or {}).get("result", {})
    sid = conv.get("id", "")
    sub = (c.call("maid.session.subscribe", {"session": sid}) or {}).get("result", {})
    first = c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("sequence_number") == 0)
    report(sid != "" and sub.get("replay_from") == 0 and first and first["params"]["by"]["name"] == "rpc-smoke",
           "creates a session and replays it from 0, the client named by its hello", json.dumps([conv, sub, first]))

    def turn(text):
        r = c.call("response.create", {"conversation": sid, "input": text})
        rid = (r or {}).get("result", {}).get("id")
        done = c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("type") in ("response.completed", "response.failed")
                      and m["params"]["response"]["id"] == rid, 60)
        return rid, done

    rid, done = turn("ping")
    text = "".join(e["delta"] for e in c.events(sid) if e["type"] == "response.output_text.delta")
    report(done and done["params"]["type"] == "response.completed" and "echo: ping" in text, "runs a turn and streams its reply", json.dumps(c.events(sid))[-2000:])
    mark = max(e["sequence_number"] for e in c.events(sid))

    # run_shell in manual mode asks first; the answer goes back over the pipe.
    r = c.send("response.create", {"conversation": sid, "input": "shell:echo rpc-$((40+2))"})
    ask = c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("type") == "maid.approval.requested", 60)
    yes = c.call("maid.approval.answer", {"session": sid, "approval": ask["params"]["id"], "choice": "yes"}) if ask else None
    rid2 = (c.answer(r) or {}).get("result", {}).get("id")
    done = c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("type") == "response.completed" and m["params"]["response"]["id"] == rid2, 60)
    out = json.dumps([e for e in c.events(sid) if e["type"].startswith("response.shell_call_output")])
    report(ask and yes and "result" in yes and done and "rpc-42" in out, "asks an approval and takes its answer", json.dumps([ask, yes]) + out)

    # A request that runs long (`!cmd` answers when the command ends) holds up nothing else.
    t0 = time.time()
    sh = c.send("maid.session.shell", {"session": sid, "command": "sleep 2; echo slept"})
    st = c.call("maid.engine.status")
    quick = time.time() - t0
    shr = c.answer(sh)
    report(st and "result" in st and quick < 1.5 and shr and shr.get("result", {}).get("exit_code") == 0 and time.time() - t0 >= 2,
           "answers a request while a long one runs (%.2f s)" % quick, json.dumps([st, shr]))

    # A client that lost its place resumes after the last event it saw.
    c.call("maid.session.unsubscribe", {"session": sid})
    with c.cv:
        c.msgs = [m for m in c.msgs if m.get("method") != "maid.event"]
    res = (c.call("maid.session.subscribe", {"session": sid, "starting_after": mark}) or {}).get("result", {})
    c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("sequence_number") == res.get("sequence_number"))
    seqs = [e["sequence_number"] for e in c.events(sid)]
    report(res.get("replay_from") == mark + 1 and seqs and seqs[0] == mark + 1 and seqs == list(range(mark + 1, res["sequence_number"] + 1)),
           "replays from starting_after", json.dumps([res, seqs]))
    gone = c.call("maid.nope")
    report(gone and gone["error"]["code"] == -32601, "answers an unknown method -32601", json.dumps(gone))
    code = c.close()
    transcripts = [f for d, _, fs in os.walk(os.path.join(home, "state")) for f in fs if f.startswith(sid) and f.endswith(".jsonl")]
    report(code == 0 and transcripts and "AddressSanitizer" not in c.stderr.decode(errors="replace"), "ends at EOF, exit %r, the session kept" % code, c.stderr.decode(errors="replace"))
    chk = subprocess.run([maid, "protocol", "check", rec], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    report(chk.returncode == 0 and "2 streams, 0 with a violation" in chk.stdout, "client's and engine's recordings pass maid protocol check", chk.stdout + chk.stderr)

    # One engine per transcript: without the daemon a second window is refused the session another window's engine
    # has open, until that one lets go of it; a killed holder's hold is taken over.
    def window():
        w = RpcClient(maid, env, home)
        w.call("maid.hello", {"protocol": 1, "client": {"name": "window", "version": "0"}})
        return w
    first, second = window(), window()
    got = first.call("maid.session.resume", {"session": sid})
    refused = second.call("maid.session.resume", {"session": sid})
    why = (refused or {}).get("error", {}).get("message", "")
    report(got and "result" in got and "is open in another MAID (pid %d): one engine per transcript" % first.p.pid in why,
           "refuses a second window the session another window's engine has open", json.dumps([got, refused]))
    first.close()
    got = second.call("maid.session.resume", {"session": sid})
    second.p.kill()
    second.close()
    third = window()
    again = third.call("maid.session.resume", {"session": sid})
    third.close()
    report(got and "result" in got and again and "result" in again, "opens it once the first window ends, and takes over a killed window's hold", json.dumps([got, again]))

    # A client that wants no text deltas: the done events carry the text, and maid.filtered_from accounts for the gaps.
    rec2 = os.path.join(home, "rpc-filtered")
    os.makedirs(rec2)
    c = RpcClient(maid, dict(env, MAID_PROTOCOL_RECORD=rec2), home, os.path.join(rec2, "client.jsonl"))
    h = (c.call("maid.hello", {"protocol": 1, "client": {"name": "rpc-filter", "version": "0"}, "exclude": ["response.output_text.delta"]}) or {}).get("result", {})
    sid = (c.call("createConversation") or {}).get("result", {}).get("id", "")
    c.call("maid.session.subscribe", {"session": sid})
    rid, done = turn("ping")
    evs = c.events(sid)
    text = "".join(e["text"] for e in evs if e["type"] == "response.output_text.done")
    marked = [e for e in evs if "filtered_from" in e.get("maid", {})]
    report(set(h.get("exclude", [])) == {"response.output_text.delta", "response.shell_call_output_content.delta", "maid.tool.output.delta"} and done
           and not any(e["type"] == "response.output_text.delta" for e in evs) and "echo: ping" in text and marked,
           "spares a filtered client its text deltas and marks the gap with maid.filtered_from", json.dumps([h, evs])[-3000:])
    code = c.close()
    chk = subprocess.run([maid, "protocol", "check", rec2], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    report(code == 0 and chk.returncode == 0 and "2 streams, 0 with a violation" in chk.stdout, "the filtered connection's recordings pass maid protocol check", chk.stdout + chk.stderr)

    # Faults, unrecorded: a line that is not JSON, then one over 1 MiB, which ends the connection.
    c = RpcClient(maid, env, home)
    c.send(None, raw=b"this is not json\n")
    bad = c.wait(lambda m: m.get("error", {}).get("code") == -32700)
    h = c.call("maid.hello", {"protocol": 1, "client": {"name": "x", "version": "0"}})
    old = c.call("maid.hello", {"protocol": 0})
    c.send(None, raw=b'{"jsonrpc":"2.0","id":99,"method":"maid.engine.status","params":{"pad":"' + b"x" * (1 << 20) + b'"}}\n')
    big = c.wait(lambda m: m.get("error", {}).get("data", {}).get("code") == "maid_too_large")
    try:
        code = c.p.wait(30)
    except subprocess.TimeoutExpired:
        code = None
    c.close()
    report(bad and bad["id"] is None and h and "result" in h and old and old["error"]["data"]["code"] == "maid_unsupported_protocol" and big and code == 1,
           "answers -32700, maid_unsupported_protocol and maid_too_large, and closes on the last (exit %r)" % code, json.dumps([bad, h, old, big]))
    c = RpcClient(maid, env, home)
    c.call("maid.hello", {"protocol": 1, "client": {"name": "x", "version": "0"}})
    c.p.send_signal(signal.SIGTERM)
    try:
        code = c.p.wait(30)
    except subprocess.TimeoutExpired:
        code = None
    c.close()
    report(code == 0, "ends cleanly at SIGTERM (exit %r)" % code, c.stderr.decode(errors="replace"))
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def daemon_smoke(maid, port):
    """`maid daemon`: start, status, a second start, `maid --rpc` carried to its socket, a session the closing
    client leaves (an idle one stopped, or parked when it says so first; a working one finishing in the background,
    parked then, and resumed by the next client), a
    daemon killed outright and started again over its stale socket, stop asking before it interrupts a turn, the
    systemd unit. The daemon's recorded connections pass `maid protocol check`. Every daemon started here is stopped."""
    import signal
    home, env = make_home(port)
    rec = os.path.join(home, "daemon-streams")
    os.makedirs(rec)
    env = dict(env, MAID_PROTOCOL_RECORD=rec)
    results = []

    def report(ok, what, extra=""):
        results.append(ok)
        print(("ok" if ok else "FAIL") + ": maid daemon " + what + ("" if ok else "\n" + extra[-3000:]))

    def run(*args, timeout=60):
        return subprocess.run([maid, *args], capture_output=True, text=True, env=env, cwd=home, stdin=subprocess.DEVNULL, timeout=timeout)

    def status():
        r = run("daemon", "status", "--json")
        return r.returncode, json.loads(r.stdout or "{}")

    def entry(sid):
        return next((e for e in status()[1].get("sessions", []) if e["id"] == sid), {})

    def until(pred, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.2)
        return False

    sock = os.path.join(env["XDG_RUNTIME_DIR"], "maid", "engine.sock")
    pid_file = os.path.join(env["XDG_RUNTIME_DIR"], "maid", "engine.pid")
    pids = []
    try:
        r = run("daemon", "status")
        report(r.returncode == 3 and "not running" in r.stdout, "status says it is not running (exit 3)", r.stdout + r.stderr)
        r = run("daemon", "start")
        with open(pid_file) as f:
            pids.append(int(f.read().split()[0]))
        mode = lambda p: oct(os.stat(p).st_mode & 0o777)
        report(r.returncode == 0 and "running (pid %d)" % pids[-1] in r.stdout and mode(sock) == "0o600" and mode(os.path.dirname(sock)) == "0o700",
               "starts, its socket 0600 in a 0700 directory", r.stdout + r.stderr)
        again, twice = run("daemon", "start"), run("daemon", "run")
        report(again.returncode == 0 and "already running" in again.stdout and twice.returncode == 1 and "already running" in twice.stderr,
               "a second start finds it, a second run is refused", again.stdout + twice.stdout + twice.stderr)

        # maid --rpc, as maid.nvim starts it, is carried to the daemon's socket.
        c = RpcClient(maid, env, home)
        h = (c.call("maid.hello", {"protocol": 1, "client": {"name": "daemon-smoke", "version": "0"}, "capabilities": ["tool_output"]}) or {}).get("result", {})
        sid = (c.call("createConversation", {"maid": {"workspace": home}}) or {}).get("result", {}).get("id", "")
        c.call("maid.session.subscribe", {"session": sid})
        r = c.call("response.create", {"conversation": sid, "input": "ping"})
        done = c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("type") == "response.completed", 60)
        text = "".join(e["delta"] for e in c.events(sid) if e["type"] == "response.output_text.delta")
        report(h.get("path") == {"via": "socket"} and sid.split("-")[2:3] == ["daemon"] and done and "echo: ping" in text,
               "takes maid --rpc as a client over its socket and runs a turn", json.dumps([h, sid, text]))
        left = (c.call("maid.session.leave", {"as": "park"}) or {}).get("result", {}).get("left") or {}
        code = c.close()
        report(code == 0 and left.get("state") == "parked" and entry(sid).get("state") == "parked",
               "parks an idle session its client quits with --park (maid.session.leave)", json.dumps([left, entry(sid)]))
        c = RpcClient(maid, env, home)
        c.call("maid.hello", {"protocol": 1, "client": {"name": "daemon-smoke", "version": "0"}})
        idle = (c.call("createConversation", {"maid": {"workspace": home}}) or {}).get("result", {}).get("id", "")
        listed = bool(entry(idle))
        code = c.close()
        report(code == 0 and listed and until(lambda: not entry(idle)), "stops an idle session its client left (leave.quit.idle)", json.dumps(entry(idle)))

        # A working session outlives its client: the turn ends in the background, and the next client resumes it.
        c = RpcClient(maid, env, home)
        c.call("maid.hello", {"protocol": 1, "client": {"name": "daemon-smoke", "version": "0"}})
        sid2 = (c.call("createConversation", {"maid": {"workspace": home}}) or {}).get("result", {}).get("id", "")
        c.call("response.create", {"conversation": sid2, "input": "slow: still here"})
        working = until(lambda: entry(sid2).get("activity") == "working", 10)
        c.close()
        left = entry(sid2)
        finished = until(lambda: entry(sid2).get("state") == "parked" and entry(sid2).get("unseen"), 30)
        c = RpcClient(maid, env, home)
        c.call("maid.hello", {"protocol": 1, "client": {"name": "daemon-smoke", "version": "0"}})
        c.call("maid.session.resume", {"session": sid2})
        snap = (c.call("maid.session.attach", {"session": sid2}) or {}).get("result", {})
        said = json.dumps(snap.get("items", []))
        report(working and left.get("state") == "background" and finished and "echo: slow: still here" in said,
               "lets a working session finish after its client left, parked then (leave.quit.after), and the next client sees the reply",
               json.dumps([left, entry(sid2), snap])[-2500:])

        # stop asks before it interrupts a turn: refused without a terminal unless --yes.
        c.call("response.create", {"conversation": sid2, "input": "hold on"})
        until(lambda: entry(sid2).get("activity") == "working", 10)
        r = run("daemon", "stop")
        report(r.returncode == 1 and "1 session is working" in r.stderr and "--yes" in r.stderr and status()[0] == 0,
               "stop refuses to interrupt a running turn unasked", r.stdout + r.stderr)
        c.close()

        # Killed outright, it leaves its socket and PID file; status says so, and start clears them.
        os.kill(pids[-1], signal.SIGKILL)
        until(lambda: not os.path.exists("/proc/%d" % pids[-1]) or open("/proc/%d/stat" % pids[-1]).read().split(")")[1].split()[0] == "Z", 10)
        r = run("daemon", "status")
        stale = r.returncode == 3 and "a socket was left" in r.stdout
        r = run("daemon", "start")
        with open(pid_file) as f:
            pids.append(int(f.read().split()[0]))
        e1, e2 = entry(sid), entry(sid2)
        report(stale and r.returncode == 0 and e1.get("state") == "parked" and e2.get("state") == "parked",
               "starts again over the socket of one that was killed, its sessions listed as parked", r.stdout + r.stderr + json.dumps([e1, e2]))

        # A client resumes a parked session in the new daemon; the epoch goes on.
        c = RpcClient(maid, env, home)
        c.call("maid.hello", {"protocol": 1, "client": {"name": "daemon-smoke", "version": "0"}})
        res = c.call("maid.session.resume", {"session": sid})
        c.close()
        report(res and "result" in res and res["result"].get("id") == sid, "resumes a parked session after a restart", json.dumps(res))

        r = run("daemon", "stop", "--yes")
        report(r.returncode == 0 and "stopped" in r.stdout and status()[0] == 3 and not os.path.exists(sock) and not os.path.exists(pid_file),
               "stops, taking its socket and PID file", r.stdout + r.stderr)
        chk = subprocess.run([maid, "protocol", "check", rec], capture_output=True, text=True, env=env, cwd=home, timeout=60)
        report(chk.returncode == 0 and " 0 with a violation" in chk.stdout, "its recorded connections pass maid protocol check", chk.stdout + chk.stderr)

        # The systemd unit: printed, written (systemctl a fake on PATH), taken away.
        bin_dir = os.path.join(home, "bin")
        os.makedirs(bin_dir)
        calls = os.path.join(home, "systemctl.log")
        with open(os.path.join(bin_dir, "systemctl"), "w") as f:
            f.write("#!/bin/sh\necho \"$*\" >> '%s'\n" % calls)
        os.chmod(os.path.join(bin_dir, "systemctl"), 0o755)
        env = dict(env, PATH=bin_dir + ":" + env["PATH"])
        unit = os.path.join(home, "config", "systemd", "user", "maid-daemon.service")
        printed, installed = run("daemon", "unit"), run("daemon", "unit", "install")
        with open(unit) as f:
            text = f.read()
        removed = run("daemon", "unit", "remove")
        with open(calls) as f:
            ctl = f.read()
        report("daemon run" in printed.stdout and "ExecStart=" in text and "daemon run" in text and "enable --now maid-daemon.service" in installed.stdout
               and removed.returncode == 0 and not os.path.exists(unit) and "enable" not in ctl.replace("disable", "") and "disable --now maid-daemon.service" in ctl,
               "unit prints, writes (never enabling it) and removes the systemd unit", printed.stdout + installed.stdout + removed.stdout + ctl)
    finally:
        for pid in pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def nvim_ui_smoke(maid, port):
    """nvim as MAID's interface: maid.nvim/tests/ui_test.lua drives a headless nvim against `maid --rpc` and the fake
    (the engine's recording passes `maid protocol check`), then `maid --ui nvim` with a stand-in nvim on PATH that
    records how it was started, and the runs it refuses. Skipped without nvim."""
    if not shutil.which("nvim"):
        print("skip: nvim interface (nvim is not installed)")
        return True
    home, env = make_home(port)
    rec = os.path.join(home, "ui-streams")
    os.makedirs(rec)
    test = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "maid.nvim", "tests", "ui_test.lua")
    r = subprocess.run(["nvim", "--headless", "-u", "NONE", "-i", "NONE", "-n", "-l", test], capture_output=True, text=True, cwd=home, timeout=300,
                       env=dict(env, MAID_UI_TEST_BIN=maid, MAID_PROTOCOL_RECORD=rec))
    chk = subprocess.run([maid, "protocol", "check", rec], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    ok = r.returncode == 0 and chk.returncode == 0 and "3 streams, 0 with a violation" in chk.stdout  # the first engine, the resumed one, the one that asks
    print(("ok" if ok else "FAIL") + ": the nvim interface (maid.nvim/tests/ui_test.lua), exit %d; its stream checked" % r.returncode
          + ("" if ok else "\n" + r.stdout[-6000:] + r.stderr[-2000:] + chk.stdout[-1500:]))

    # `maid --ui nvim` execs nvim with the plugin, this binary and the agent's flags; MAID_UI never reaches nvim's jobs.
    bin_dir = os.path.join(home, "bin")
    os.makedirs(bin_dir)
    seen = os.path.join(home, "nvim-argv.json")
    with open(os.path.join(bin_dir, "nvim"), "w") as f:
        f.write("#!/usr/bin/env python3\nimport json, os, sys\njson.dump({'argv': sys.argv[1:], 'ui': os.environ.get('MAID_UI')}, open(%r, 'w'))\n" % seen)
    os.chmod(os.path.join(bin_dir, "nvim"), 0o755)
    fake_env = dict(env, PATH=bin_dir + os.pathsep + env["PATH"])
    r = subprocess.run([maid, "--ui", "nvim", "--model", "fake/fake", "--no-instructions"], capture_output=True, text=True, env=fake_env, cwd=home, timeout=60)
    got = json.load(open(seen)) if os.path.exists(seen) else {}
    o = json.loads(got.get("ui") or "{}")
    exec_ok = (r.returncode == 0 and got.get("argv", [""])[0] == "-c" and "require('maid.ui').main(o)" in got["argv"][1]
               and o.get("args") == ["--model", "fake/fake", "--no-instructions"] and os.path.realpath(o.get("cmd", "")) == os.path.realpath(maid)
               and os.path.isfile(os.path.join(o.get("plugin", ""), "lua", "maid", "ui.lua")))
    print(("ok" if exec_ok else "FAIL") + ": maid --ui nvim starts nvim with maid.nvim and the engine's flags" + ("" if exec_ok else "\n%r %r %r" % (got, r.stdout, r.stderr)))
    refused = [subprocess.run([maid, "--ui", "nvim"] + extra, capture_output=True, text=True, env=dict(fake_env, **more), cwd=home, timeout=60)
               for extra, more in ((["--bare"], {}), ([], {"NVIM": "/nowhere"}), (["-p", "x"], {}))]
    refuse_ok = ([x.returncode for x in refused] == [2, 2, 1] and "with bare" in refused[0].stderr and "inside nvim" in refused[1].stderr
                 and "--ui chooses the interactive interface" in refused[2].stderr)
    print(("ok" if refuse_ok else "FAIL") + ": --ui nvim is refused with --bare, inside nvim and with -p" + ("" if refuse_ok else "\n" + "\n".join(x.stderr for x in refused)))
    shutil.rmtree(home, ignore_errors=True)
    return ok and exec_ok and refuse_ok


def output_smoke(maid, env, sess_dir):
    """`maid sessions output` on a kept output written by hand: the list, the plain print with its label on stderr,
    and --replay: stdout and stderr interleaved in the original order, each progress redraw its own write, on time."""
    import selectors
    sid = "20260101-130000-tui-2"
    side = os.path.join(sess_dir, sid + ".d")
    os.makedirs(side, mode=0o700)
    chunks = [("o", "building\n", 0), ("e", "warning: slow\n", 300), ("o", "progress 10%\r", 600), ("o", "progress 100%\r\n", 900), ("e", "done\n", 1200)]
    data, idx = "", ""
    for stream, text, ms in chunks:
        idx += "%s %d %d %d\n" % (stream, len(data), len(text), ms)
        data += text
    with open(os.path.join(side, "call_7.out"), "w") as f:
        f.write(data)
    with open(os.path.join(side, "call_7.idx"), "w") as f:
        f.write(idx)
    with open(os.path.join(sess_dir, sid + ".jsonl"), "w") as f:
        for rec in ({"type": "start", "workspace": sess_dir, "model": "fake/fake", "mode": "manual", "host": "h", "pid": 1},
                    {"type": "tool", "tool": "run_shell", "arguments": {"command": "make"}, "result": "exit code 0", "ok": True,
                     "full_output": {"path": sid + ".d/call_7.out", "bytes": len(data), "sha256": "", "delivered_to_model": False}}):
            f.write(json.dumps(dict(rec, time="2026-01-01T13:00:00+0000")) + "\n")
    ls = subprocess.run([maid, "sessions", "output", sid], capture_output=True, text=True, env=env, timeout=60)
    plain = subprocess.run([maid, "sessions", "output", sid, "call_7"], capture_output=True, env=env, timeout=60)  # bytes: text mode would turn \r into \n
    p = subprocess.Popen([maid, "sessions", "output", sid, "call_7", "--replay"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    sel = selectors.DefaultSelector()
    sel.register(p.stdout, selectors.EVENT_READ, "o")
    sel.register(p.stderr, selectors.EVENT_READ, "e")
    seen, start, label, label_done = [], None, b"", False
    while sel.get_map():
        for key, _ in sel.select(timeout=10):
            got = os.read(key.fileobj.fileno(), 65536)
            if not got:
                sel.unregister(key.fileobj)
                continue
            start = start or time.monotonic()  # the clock starts with the first thing out, the label or the first chunk
            if key.data == "e" and not label_done:
                # The label is stderr's first line; it may arrive over several reads, so gather it to its newline.
                label += got
                if b"\n" not in label:
                    continue
                label, _, got = label.partition(b"\n")
                label_done = True
                if not got:
                    continue
            seen.append((key.data, got.decode(), (time.monotonic() - start) * 1000))
    p.wait(timeout=30)
    order = [(s, t) for s, t, _ in seen] == [(s, t) for s, t, _ in chunks]
    timing = len(seen) == len(chunks) and all(abs(at - ms) < 200 for (_, _, at), (_, _, ms) in zip(seen, chunks))
    ok = (ls.returncode == 0 and "call_7  %d bytes  run_shell: make" % len(data) in ls.stdout
          and plain.returncode == 0 and plain.stdout == data.encode() and b"full output, display only: the model saw the capped result" in plain.stderr
          and p.returncode == 0 and b"full output, display only" in label and order and timing)
    print(("ok" if ok else "FAIL") + ": maid sessions output lists, prints with its label, and replays in order and on time" +
          ("" if ok else "\n" + ls.stdout + ls.stderr + repr(plain.stdout) + repr(plain.stderr) + repr(seen) + repr(label)))
    return ok


def rehome_smoke(maid, env, home):
    """`maid sessions rehome`: subagent sessions move only when asked, several targets, --children-of, an ambiguous
    prefix and a running session refused with nothing moved, --dry-run, and forks and subagents loading after each move."""
    sessions = os.path.join(env["XDG_STATE_HOME"], "maid", "sessions")
    general = os.path.join(sessions, "general")
    results = []

    def write(name, recs, where=general):
        os.makedirs(where, mode=0o700, exist_ok=True)
        with open(os.path.join(where, name + ".jsonl"), "w") as f:
            for rec in recs:
                f.write(json.dumps(dict(rec, time="2026-01-02T12:00:00+0000")) + "\n")

    def start(**kw):
        return dict({"type": "start", "workspace": home, "model": "fake/fake", "mode": "manual", "host": "h", "pid": 1}, **kw)

    def msg(text):
        return {"type": "msg", "role": "user", "content": text}

    def where(sid):
        found = [d for d, _, files in os.walk(sessions) if sid + ".jsonl" in files]
        return os.path.relpath(found[0], sessions) if len(found) == 1 else found

    def last(sid):
        with open(os.path.join(sessions, where(sid), sid + ".jsonl")) as f:
            return json.loads(f.read().splitlines()[-1])

    def run(*args):
        return subprocess.run([maid, "sessions", *args], capture_output=True, text=True, env=env, cwd=home, timeout=60)

    def report(ok, what, *rs):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + "\n".join(r.stdout[-1500:] + r.stderr[-1500:] for r in rs)))
        results.append(ok)

    def loads(*ids):
        return all(run("read", i).returncode == 0 for i in ids)

    P, C, G, F = "20260102-120000-tui-11", "20260102-120001-sub-12", "20260102-120002-sub-13", "20260102-120003-tui-14"
    write(P, [start(), msg("the fennec girl's ears"), {"type": "user", "text": "the fennec girl's ears"}])
    write(C, [start(parent=P, agent="explore"), msg("find a reference"), {"type": "user", "text": "find a reference"}])
    write(G, [start(parent=C, agent="explore"), msg("look closer"), {"type": "user", "text": "look closer"}])
    write(F, [{"type": "resumed_from", "id": P, "path": os.path.join(general, P + ".jsonl"), "records": 3}, msg("and her tail?")],
          os.path.join(sessions, "forks"))

    r = run("rehome", P, "rh", "-n")
    report(r.returncode == 0 and " -> " + os.path.join(sessions, "rh", P + ".jsonl") in r.stdout and "dry run: nothing moved (1 file would move)" in r.stdout
           and where(P) == "general", "rehome -n prints the plan and moves nothing", r)
    r = run("rehome", P[:-1], "rh")
    report(r.returncode == 0 and where(P) == "rh" and where(C) == "general" and where(G) == "general" and last(P).get("type") == "rehomed"
           and last(P).get("reason") == "rehome" and last(P).get("from") == os.path.join(general, P + ".jsonl") and loads(F, C, G)
           and "subagent of " + P in run("state", C).stdout, "rehome moves only the named session; its subagents stay and still name it", r)
    r = run("rehome", P, "rh2", "--subagent")
    report(r.returncode == 0 and r.stdout.count(" -> ") == 3 and where(P) == where(C) == where(G) == "rh2" and last(C).get("reason") == "rehome"
           and loads(F, C, G), "--subagent moves its subagents and theirs with it", r)
    r = run("rehome", "--children-of", P, "general")
    report(r.returncode == 0 and where(P) == "rh2" and where(C) == where(G) == "general" and loads(F, P, C, G), "--children-of moves the subagents, not the parent", r)
    r = run("rehome", P, "rh4", "--subagent-only")
    report(r.returncode == 0 and r.stdout.count(" -> ") == 2 and where(P) == "rh2" and where(C) == where(G) == "rh4" and loads(F, P, C, G),
           "--subagent-only moves the subagents and leaves the named session where it is", r)
    r = run("rehome", P, "rh4", "--subagent-only")
    report(r.returncode == 0 and r.stdout.count("already in rh4/") == 2 and "nothing to move" in r.stdout, "subagents already there are said to be", r)

    X, Y = "20260102-120004-tui-15", "20260102-120005-tui-16"
    write(X, [start(), msg("x")])
    write(Y, [start(), msg("y")])
    r = run("rehome", X, Y, "rh")
    report(r.returncode == 0 and where(X) == where(Y) == "rh", "several targets in one command", r)
    r = run("rehome", X, "20260102-12000", "general")
    report(r.returncode == 1 and "nothing moved:" in r.stderr and "'20260102-12000' matches 6 sessions" in r.stderr and P + "  [rh2]" in r.stderr
           and where(X) == "rh", "an ambiguous prefix lists the candidates and nothing moves", r)
    # This script's own process stands in for a running maid: its command line names the maid binary.
    L = "20260102-120006-tui-17"
    write(L, [start(pid=os.getpid(), host=socket.gethostname()), msg("live")])
    r = run("rehome", X, L, "rh3")
    report(r.returncode == 1 and L + " is running (pid %d)" % os.getpid() in r.stderr and where(L) == "general" and where(X) == "rh",
           "a running session is refused with the reason and nothing moves", r)
    return all(results)


def mcp_smoke(maid, port):
    """Claude Code as the agent (level 2) through the real binary: the fake `claude` from core/tests/fake_claude.hpp
    starts `maid mcp-bridge SOCKET` from its MCP config, as Claude Code would, and calls MAID's read_file through it.
    The real `claude` is never run: PATH holds only the fake and the system directories."""
    home, env = make_home(port)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "core", "tests", "fake_claude.hpp")) as f:
        header = f.read()
    script = header[header.index('R"PY(') + 5:header.index(')PY"')]
    bindir = os.path.join(home, "bin")
    os.makedirs(bindir)
    with open(os.path.join(bindir, "claude"), "w") as f:
        f.write(script)
    os.chmod(os.path.join(bindir, "claude"), 0o755)
    env.update(PATH=bindir + ":/usr/bin:/bin", FAKE_CLAUDE_DIR=home, FAKE_CLAUDE_BRIDGE="1")
    with open(os.path.join(home, "note.txt"), "w") as f:
        f.write("big fluffy fennec ears\n")
    r = subprocess.run([maid, "-p", 'CALL read_file {"path": "note.txt"}', "--model", "claude-cli/sonnet"], capture_output=True, text=True, env=env,
                       cwd=home, timeout=60, stdin=subprocess.DEVNULL)
    shakes = []
    if os.path.exists(os.path.join(home, "mcp.jsonl")):
        with open(os.path.join(home, "mcp.jsonl")) as f:
            shakes = [json.loads(l) for l in f if l.strip()]
    ok = (r.returncode == 0 and "results: " in r.stdout and "fennec ears" in r.stdout and len(shakes) == 1
          and shakes[0]["tools"]["result"]["tools"] and not os.listdir(os.path.join(env["XDG_RUNTIME_DIR"], "maid", "mcp")))
    print(("ok" if ok else "FAIL") + ": claude-cli as the agent: the bridge carries MCP to MAID's tools, read_file runs, the socket is gone after"
          + ("" if ok else "\n" + r.stdout[-2000:] + r.stderr[-2000:] + json.dumps(shakes)[:2000]))
    return ok


def models_smoke(maid, port):
    """`maid models` over a models tree shaped like Micaiah's drive: the 4B and 9B folders with their projectors (sparse
    files of the catalog's sizes) and the -text folder holding only the relative link. Nothing is downloaded."""
    home, env = make_home(port)
    mdir = os.path.join(home, "models")
    with open(os.path.join(home, "config", "maid", "settings.lua"), "w") as f:
        f.write("return { models_dir = '%s', load_instructions = false }\n" % mdir)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "models", "catalog.json")) as f:
        catalog = {m["id"]: m for m in json.load(f)["models"]}
    for mid in ("qwen3.5-4b", "qwen3.5-9b"):
        d = os.path.join(mdir, "llamacpp", catalog[mid]["install"]["dir"])
        os.makedirs(d)
        for fl in catalog[mid]["files"]:
            with open(os.path.join(d, fl["name"]), "wb") as out:
                out.truncate(fl["size"])
    os.makedirs(os.path.join(mdir, "llamacpp", "Qwen3.5-9B-Q4_K_M-text"))
    os.symlink("../Qwen3.5-9B-Q4_K_M/Qwen3.5-9B-Q4_K_M.gguf", os.path.join(mdir, "llamacpp", "Qwen3.5-9B-Q4_K_M-text", "Qwen3.5-9B-Q4_K_M-text.gguf"))
    results = []

    def run(*args):
        return subprocess.run([maid, *args], capture_output=True, text=True, env=env, cwd=home, timeout=60, stdin=subprocess.DEVNULL)

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-2000:] + r.stderr[-2000:]))
        results.append(ok)

    def row(out, mid):
        """The table's columns by position: id, role, size, installed, current, presets."""
        line = next((l for l in out.splitlines() if l.startswith(mid + " ")), "")
        return {"size": line[40:49].strip(), "installed": line[50:60].strip(), "current": line[61:70].strip(), "presets": line[71:].strip()}

    r = run("models", "check")
    report(r.returncode == 0 and "0 problems" in r.stdout, "maid models check passes on the shipped catalog", r)
    r = run("models", "install", "qwen3.5-9b-text", "--link")
    report(r.returncode == 0 and "present: " in r.stdout and "downloading" not in r.stdout and "qwen-9b" in r.stdout,
           "installing the -text entry on that tree downloads nothing, links it as current and names its preset", r)
    r = run("models")
    four, nine, text, coder = row(r.stdout, "qwen3.5-4b"), row(r.stdout, "qwen3.5-9b"), row(r.stdout, "qwen3.5-9b-text"), row(r.stdout, "qwen2.5-coder-7b")
    report(r.returncode == 0 and four["size"] == "3.2 GB" and four["installed"] == "yes" and nine["installed"] == "yes" and text["size"] == "link" and text["installed"] == "yes"
           and text["current"] == "llamacpp" and text["presets"] == "qwen-9b" and nine["current"] == "" and nine["presets"] == "qwen-9b-vision" and coder["installed"] == "no",
           "the table: the three folders installed, the -text link current, the coder not installed", r)
    r = run("models", "info", "qwen3.5-9b-text")
    report(r.returncode == 0 and r.stdout.splitlines()[2].startswith("The 9B without image processing") and "shares:" in r.stdout, "info prints the brief first", r)
    r = run("models", "remove", "qwen3.5-9b", "--yes")
    report(r.returncode == 1 and "qwen3.5-9b-text links qwen3.5-9b's weights" in r.stderr and os.path.exists(os.path.join(mdir, "llamacpp", "Qwen3.5-9B-Q4_K_M")),
           "remove refuses the 9B while the -text entry links it, and names it", r)
    r = run("models", "remove", "qwen3.5-4b")
    report(r.returncode == 2 and "--yes" in r.stderr and os.path.exists(os.path.join(mdir, "llamacpp", "Qwen3.5-4B-Q4_K_M")), "remove off a terminal needs --yes", r)
    r = run("models", "remove", "qwen3.5-4b", "--yes")
    report(r.returncode == 0 and not os.path.exists(os.path.join(mdir, "llamacpp", "Qwen3.5-4B-Q4_K_M")), "with --yes it removes the files and the folder", r)
    r = run("help", "models")
    report(r.returncode == 0 and r.stdout.startswith("NAME\nmodels - ") and "llama.vim" in r.stdout, "maid help models is the catalog page", r)
    return all(results)


def audit_trail_smoke(maid, port):
    """The audit trail through the binary (docs/audit-trail.md), over a synthetic trail in a throwaway HOME: init,
    status (counts, never contents; --json kept from maid's own parser), purge after a yes, offsite printing and
    never running anything, the systemd schedule, and the start-up check at maid -p, maid status and the TUI with a
    fake maid-leak-audit. start_services stays off: no real service is ever started."""
    import pty, select, signal
    home, env = make_home(port)
    env["HOME"] = os.path.join(home, "h")
    os.makedirs(env["HOME"])
    work = os.path.join(home, "work")  # no project marker: no trust prompt
    os.makedirs(work)
    fakebin = os.path.join(home, "fakebin")
    os.makedirs(fakebin)
    ran = os.path.join(home, "ran.log")  # every fake tool appends its name and arguments: nothing may be run that should not
    env["PATH"] = fakebin + ":/usr/bin:/bin"
    results = []
    audit_lua = os.path.join(home, "config", "maid", "audit.lua")
    trail = os.path.join(env["XDG_STATE_HOME"], "maid", "audit-trail")

    def run(*args, stdin=subprocess.DEVNULL, path=None, extra=None):
        e = dict(env, **(extra or {}))
        if path is not None:
            e["PATH"] = path
        return subprocess.run([maid, *args], capture_output=True, text=True, env=e, cwd=work, timeout=60, stdin=stdin)

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-2500:] + r.stderr[-2500:]))
        results.append(ok)

    def fake(name, body=""):
        p = os.path.join(fakebin, name)
        with open(p, "w") as f:
            f.write("#!/bin/sh\necho \"%s $*\" >> '%s'\n%s" % (name, ran, body))
        os.chmod(p, 0o755)
        return p

    def ran_lines():
        if not os.path.exists(ran):
            return []
        with open(ran) as f:
            return f.read().splitlines()

    def audit_settings(text):
        with open(audit_lua, "w") as f:
            f.write("return { " + text + " }\n")

    r = run("audit-trail")
    report(r.returncode == 0 and "audit trail: off" in r.stdout and "not written yet: maid audit-trail init" in r.stdout and "0 files" in r.stdout,
           "maid audit-trail: off by default, no trail", r)
    r = run("audit-trail", "status", "--json")
    j = json.loads(r.stdout) if r.returncode == 0 and r.stdout.startswith("{") else {}
    report(j.get("on") is False and j.get("every_seconds") == 86400 and j.get("archive") == "off" and j.get("judge_thinking") is True,
           "status --json reaches the command (not maid's own --json)", r)
    r = run("audit-trail", "init")
    mode = oct(os.stat(audit_lua).st_mode & 0o777) if os.path.exists(audit_lua) else ""
    report(r.returncode == 0 and "wrote " + audit_lua in r.stdout and mode == "0o600", "init writes audit.lua, 0600", r)
    r = run("audit-trail", "init")
    report(r.returncode == 0 and "already there, left as it is" in r.stdout, "init never overwrites it", r)

    # A synthetic trail: the containers hold markers that must never be shown.
    os.makedirs(trail, mode=0o700)
    for name, ids in (("20200101.jsonl", (1, 2, 3)), ("20200102.jsonl", (4, 5))):
        with open(os.path.join(trail, name), "w") as f:
            for i in ids:
                f.write(json.dumps({"id": i, "time": "2020-01-01T00:00:00Z", "tool": "run_shell", "arguments": {"command": "cat trail-marker-9931"},
                                    "session": "trail-session-4417"}) + "\n")
    with open(os.path.join(trail, "seq"), "w") as f:
        f.write("5\n")
    with open(os.path.join(trail, "index.json"), "w") as f:
        json.dump({"version": 1, "last_id": 3, "archived": [], "ranges": [
            {"first": 1, "last": 2, "count": 2, "state": "live", "verdicts": {"2": "reached"}, "signature": "trail-signature-2290"},
            {"first": 3, "last": 3, "count": 1, "state": "stale live", "verdicts": {}, "signature": "x"}]}, f)
    audit_settings("enabled = true, enforce = 'notify', every = '6h'")
    r = run("audit-trail", "status")
    secret = any(m in r.stdout + r.stderr for m in ("trail-marker-9931", "trail-session-4417", "trail-signature-2290", "run_shell"))
    report(r.returncode == 0 and "audit trail: on" in r.stdout and "2 files" in r.stdout and "2 live, 1 stale live, 0 archival; 2 not yet audited (last id 5)" in r.stdout
           and "(every 6h)" in r.stdout and "schedule: none" in r.stdout and "archive: off (an entry that gets the retirement signal is deleted)" in r.stdout
           and "the next start holds for an audit (notify)" in r.stdout and not secret,
           "status: on, counts by state, the schedule and the archive, nothing of what the entries hold", r)

    # off-site: printed, never run.
    archive = os.path.join(home, "archive")
    os.makedirs(archive)
    old, new = "audit-chunk-20200301T000000Z-1.tar.gz", "audit-chunk-%s-1.tar.gz" % time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    for name, size in ((old, 2048), ("audit-chunk-20200301T000000Z-2.tar.gz", 1024), (new, 512)):
        with open(os.path.join(archive, name), "wb") as f:
            f.write(b"x" * size)
        with open(os.path.join(archive, name + ".sha256"), "w") as f:
            f.write("0" * 64 + "  " + name + "\n")
    audit_settings("enabled = true, enforce = 'notify', archive = '%s'" % archive)
    empty = os.path.join(home, "empty")
    os.makedirs(empty)
    r = run("audit-trail", "offsite", "/mnt/cold", path=empty)
    report(r.returncode == 0 and "off-site: 2 chunks older than 90d in %s, 3.0 KB in all" % archive in r.stdout and old in r.stdout and new not in r.stdout
           and "by hand, if none of these fits" in r.stdout and "rsync -a" not in r.stdout and "cp -a" not in r.stdout, "offsite with nothing on PATH: the chunks, their size and plain steps", r)
    tools = os.path.join(home, "tools")
    os.makedirs(tools)
    for name in ("rsync", "rclone", "restic", "kopia", "syncthing", "cp", "sha256sum", "rm"):
        p = os.path.join(tools, name)
        with open(p, "w") as f:
            f.write("#!/bin/sh\necho \"%s $*\" >> '%s'\n" % (name, ran))
        os.chmod(p, 0o755)
    r = run("audit-trail", "offsite", "/mnt/cold", path=tools)
    q = "'%s/%s'" % (archive, old)
    report(r.returncode == 0 and "rsync -a --checksum --remove-source-files " + q in r.stdout and "'/mnt/cold/'" in r.stdout
           and "rclone move --checksum --include '%s'" % old in r.stdout and "restic -r '/mnt/cold' backup " + q in r.stdout
           and "kopia snapshot create '%s'" % archive in r.stdout and "ignoreDelete" in r.stdout
           and "cp -a " + q in r.stdout and "sha256sum -c '%s.sha256'" % old in r.stdout and "MAID runs none of this" in r.stdout
           and new not in r.stdout and ran_lines() == [], "offsite with rsync, rclone, restic, kopia, syncthing and coreutils: a command for each, none run", r)
    for keep in ("rsync", "rclone", "restic", "kopia", "syncthing"):
        os.remove(os.path.join(tools, keep))
    r = run("audit-trail", "offsite", "/mnt/cold", path=tools)
    report(r.returncode == 0 and "coreutils (cp, sha256sum, rm" in r.stdout and "rsync" not in r.stdout and ran_lines() == [],
           "offsite with coreutils only: the cp, sha256sum -c, rm fallback", r)
    r = run("audit-trail", "offsite", "/mnt/cold", "--older-than", "10000d", path=tools)
    report(r.returncode == 0 and "nothing to move" in r.stdout, "offsite --older-than past every chunk: nothing to move", r)
    audit_settings("enabled = true, enforce = 'notify'")
    r = run("audit-trail", "offsite", "/mnt/cold")
    report(r.returncode == 1 and "there are no chunks to move" in r.stderr, "offsite with archive off: refused", r)

    # The schedule: units from contrib/systemd/ in $XDG_CONFIG_HOME/systemd/user, systemctl a fake on PATH.
    units = os.path.join(home, "config", "systemd", "user")
    leak = fake("maid-leak-audit", "exit 0\n")
    audit_settings("enabled = true, every = '6h'")
    r = run("audit-trail", "schedule", "install", path=fakebin)
    report(r.returncode == 0 and "no systemctl here" in r.stdout and os.path.exists(os.path.join(units, "maid-leak-audit.timer")),
           "schedule install with no systemctl: written, the due check stands in", r)
    fake("systemctl")
    r = run("audit-trail", "schedule", "install")
    with open(os.path.join(units, "maid-leak-audit.service")) as f:
        service = f.read()
    with open(os.path.join(units, "maid-leak-audit.timer")) as f:
        timer = f.read()
    report(r.returncode == 0 and "\nExecStart=%s\n" % leak in service and "\nEnvironment=MAID_BIN=%s\n" % os.path.realpath(maid) in service
           and "\nOnUnitActiveSec=6h\n" in timer and "systemctl --user daemon-reload" in ran_lines()
           and "systemctl --user enable --now maid-leak-audit.timer" in ran_lines(), "schedule install: ExecStart, OnUnitActiveSec, enabled", r)
    r = run("audit-trail", "status")
    report("schedule: the systemd timer" in r.stdout, "status sees the timer", r)
    r = run("audit-trail", "schedule", "remove")
    report(r.returncode == 0 and not os.path.exists(os.path.join(units, "maid-leak-audit.timer")) and "systemctl --user disable --now maid-leak-audit.timer" in ran_lines(),
           "schedule remove: disabled and gone", r)
    os.remove(os.path.join(fakebin, "systemctl"))
    os.remove(ran)

    # The start-up check. The fake audit records the transcripts that exist while it runs: the session's comes after.
    fake("maid-leak-audit",
         "find \"$XDG_RUNTIME_DIR\" -name '*.jsonl' | sed 's/^/during: /' >> '%s'\n" % ran +
         "sleep 1\n"
         "if [ \"$1\" = --scan ]; then echo 'leak audit complete (phase 1 only): report at /fake/r.md. Something was reached for: UNCLEAR'; exit 0; fi\n"
         "if [ \"$FAKE_AUDIT\" = fail ]; then echo 'maid-leak-audit: the local model server llamacpp is not answering' >&2; "
         "echo 'leak audit not completed: see standard error. Something was reached for: UNKNOWN'; exit 2; fi\n"
         "echo 'leak audit complete: report at /fake/r.md. Something was reached for: NO'\n")

    def transcripts():
        return {os.path.join(d, f) for d, _, fs in os.walk(env["XDG_RUNTIME_DIR"]) for f in fs if f.endswith(".jsonl")}

    audit_settings("enabled = true, start_services = false")
    before = transcripts()
    r = run("-p", "ping")
    during = [line[len("during: "):] for line in ran_lines() if line.startswith("during: ")]
    hold = r.stderr.find("auditing with the local judge (qwen-9b) before the session opens, everything else on hold")
    done = r.stderr.find("audit trail: leak audit complete: report at /fake/r.md. Something was reached for: NO")
    report(r.returncode == 0 and "echo: ping" in r.stdout and 0 <= hold < done and "maid-leak-audit --model qwen-9b" in ran_lines()
           and "has never been audited" in r.stderr and set(during) <= before and transcripts() - before,
           "maid -p, judge-and-hold: the audit runs and ends before the session opens", r)
    os.remove(ran)
    r = run("-p", "ping", extra={"FAKE_AUDIT": "fail"})
    report(r.returncode == 0 and "echo: ping" in r.stdout and "the judge could not run, so the phase-1 scan stands in" in r.stderr
           and "maid-leak-audit: the local model server llamacpp is not answering" in r.stderr and "Something was reached for: UNCLEAR" in r.stderr
           and [l for l in ran_lines() if l.startswith("maid-leak-audit")] == ["maid-leak-audit --model qwen-9b", "maid-leak-audit --scan"],
           "judge-and-hold when the judge cannot run: the scan stands in and says so", r)
    os.remove(ran)
    audit_settings("enabled = true, enforce = 'scan-and-continue', start_services = false")
    r = run("-p", "ping")
    report(r.returncode == 0 and "scanning (phase 1) and continuing" in r.stderr and [l for l in ran_lines() if l.startswith("maid-leak-audit")] == ["maid-leak-audit --scan"],
           "scan-and-continue: the scan only", r)
    os.remove(ran)
    audit_settings("enabled = true, enforce = 'notify'")
    r = run("-p", "ping")
    report(r.returncode == 0 and "run maid-leak-audit (maid audit-trail schedule install runs it for you)" in r.stderr and ran_lines() == [],
           "notify: one line, nothing run", r)
    audit_settings("enabled = true, start_services = false")
    r = run("status")
    report("maid-leak-audit --model qwen-9b" in ran_lines() and "audit trail: leak audit complete" in r.stderr, "maid status checks too", r)
    os.remove(ran)
    # The TUI: the result line comes before the screen is drawn.
    master, slave = pty.openpty()
    p = subprocess.Popen([maid], stdin=slave, stdout=slave, stderr=slave, env=dict(env, TERM="xterm-256color"), cwd=work, start_new_session=True)
    os.close(slave)
    seen, deadline = b"", time.time() + 30
    while b"\x1b[?1049h" not in seen and time.time() < deadline:
        if select.select([master], [], [], 0.5)[0]:
            try:
                seen += os.read(master, 65536)
            except OSError:
                break
    os.killpg(p.pid, signal.SIGKILL)
    p.wait()
    os.close(master)
    text = seen.decode(errors="replace")
    at = text.find("Something was reached for: NO")
    report(0 <= at < text.find("\x1b[?1049h") and "maid-leak-audit --model qwen-9b" in ran_lines(), "the TUI holds until the audit is done, then opens",
           subprocess.CompletedProcess([], 0, text[-1500:], ""))
    os.remove(ran)
    # Nothing happens when a scheduler ran it, when the trail is off, and the size cap triggers by itself.
    with open(os.path.join(trail, "index.json"), "w") as f:
        json.dump({"last_audit": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "next_audit_due": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + 3600))}, f)
    r = run("-p", "ping")
    report(r.returncode == 0 and "audit trail:" not in r.stderr and ran_lines() == [], "a recent audit (a scheduler ran it): nothing happens", r)
    audit_settings("enabled = false, start_services = false")
    with open(os.path.join(trail, "20200101.jsonl"), "w") as f:
        f.write("x" * (2 << 20))
    r = run("-p", "ping")
    report(r.returncode == 0 and "audit trail:" not in r.stderr and ran_lines() == [], "off: never a hold, whatever the trail", r)
    audit_settings("enabled = true, start_services = false, live_mb = 1")
    r = run("-p", "ping")
    report(r.returncode == 0 and "past live_mb (1 MB)" in r.stderr and "maid-leak-audit --model qwen-9b" in ran_lines(), "past live_mb the next start holds", r)

    # purge: only after a yes at a terminal; the ids go on.
    r = run("audit-trail", "purge")
    report(r.returncode == 2 and "asks at a terminal" in r.stderr and os.path.exists(os.path.join(trail, "20200102.jsonl")), "purge off a terminal deletes nothing", r)
    master, slave = pty.openpty()
    os.write(master, b"n\n")
    r = run("audit-trail", "purge", stdin=slave)
    report(r.returncode == 0 and "nothing was deleted" in r.stdout and os.path.exists(os.path.join(trail, "20200102.jsonl")), "purge answered no deletes nothing", r)
    os.write(master, b"y\n")
    r = run("audit-trail", "purge", stdin=slave)
    os.close(master)
    os.close(slave)
    left = sorted(os.listdir(trail))
    report(r.returncode == 0 and "deleted 2 files of audit trail and its index (ids go on from 5)" in r.stdout and ".jsonl" not in "".join(left) and "seq" in left
           and "index.json" not in left and os.path.exists(os.path.join(archive, old)), "purge answered yes: the containers and the index go, seq and the archive stay", r)
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def color_smoke(maid, port):
    """Colour and --text-base in the informational commands. MAID_HOME is a throwaway tree with two fake services (a
    python http.server marked needs_gpu that `maid up` starts on a free loopback port, and one never started), so no
    real service runs and no GPU is touched (nvidia-smi is a stub on PATH). Colour is checked on a pty."""
    import pty
    home, env = make_home(port)
    results = []

    def free_port():
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            return sk.getsockname()[1]

    root = os.path.join(home, "root")
    os.makedirs(os.path.join(root, "services"))
    gpu_port, cpu_port = free_port(), free_port()
    for name, p, gpu in (("fake-gpu", gpu_port, True), ("fake-cpu", cpu_port, False)):
        with open(os.path.join(root, "services", name + ".json"), "w") as f:
            json.dump({"name": name, "description": "a fake", "command": ["python3", "-m", "http.server", str(p), "--bind", "127.0.0.1"],
                       "cwd": home, "port": p, "ready_timeout": 20, "needs_gpu": gpu}, f)
    stub = os.path.join(home, "bin")
    os.makedirs(stub)
    with open(os.path.join(stub, "nvidia-smi"), "w") as f:
        f.write("#!/bin/sh\necho 8192\n")
    os.chmod(os.path.join(stub, "nvidia-smi"), 0o755)
    env = dict(env, MAID_HOME=root, PATH=stub + ":" + env["PATH"], TERM="xterm-256color")
    env.pop("NO_COLOR", None)

    class Result:
        def __init__(self, returncode, stdout):
            self.returncode, self.stdout, self.stderr = returncode, stdout, ""

    def run(*args, tty=False, extra=None):
        e = dict(env, **(extra or {}))
        if not tty:
            return subprocess.run([maid, *args], capture_output=True, text=True, env=e, cwd=home, timeout=60, stdin=subprocess.DEVNULL)
        master, slave = pty.openpty()
        p = subprocess.Popen([maid, *args], stdout=slave, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, env=e, cwd=home)
        os.close(slave)
        out = b""
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
        os.close(master)
        return Result(p.wait(timeout=60), out.decode(errors="replace").replace("\r\n", "\n"))

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + repr(r.stdout[-2000:]) + r.stderr[-2000:]))
        results.append(ok)

    ESC = "\x1b["
    try:
        r = run("up", "fake-gpu")
        report(r.returncode == 0, "the fake GPU service starts", r)
        r = run("status", tty=True)
        gpu_line = next((l for l in r.stdout.splitlines() if l.startswith("fake-gpu:")), "")
        cpu_line = next((l for l in r.stdout.splitlines() if l.startswith("fake-cpu:")), "")
        report(ESC + "32mrunning" in gpu_line and ESC + "36mGPU" in gpu_line and ESC + "2mstopped" in cpu_line and "GPU" not in cpu_line,
               "on a terminal: running green with a GPU tag in its own colour, stopped dim and untagged", r)
        r = run("status", "--text-base", tty=True)
        report("\x1b" not in r.stdout and re.search(r"^service\tfake-gpu\trunning\thost\tgpu\tpid \d+\thttp://127.0.0.1:%d\t-$" % gpu_port, r.stdout, re.M)
               and "service\tfake-cpu\tstopped\thost\tcpu\t-\thttp://127.0.0.1:%d\t-\n" % cpu_port in r.stdout and r.stdout.startswith("harness\tarmed\n"),
               "--text-base on a terminal: plain records in a fixed order", r)
        r = run("status", tty=True, extra={"NO_COLOR": "1"})
        report("\x1b" not in r.stdout and "fake-gpu: running [host]" in r.stdout and "GPU" in r.stdout, "NO_COLOR on a terminal: the same lines without colour", r)
        r = run("status", tty=True, extra={"NO_COLOR": ""})
        report(ESC + "32mrunning" in r.stdout, "an empty NO_COLOR does not turn colour off", r)
        r = run("status")
        report("\x1b" not in r.stdout and "fake-gpu: running [host]" in r.stdout and "fake-cpu: stopped [host]" in r.stdout, "piped: no colour", r)
        r = run("status", "--text-base")
        report("\x1b" not in r.stdout and r.stdout.count("service\t") == 2, "piped with --text-base: records", r)
        r = run("gpu", "--text-base", tty=True)
        report(r.returncode == 0 and r.stdout == "comfyui\tstopped\t-1\t-1\ncard\t%d\n" % (8192 << 20), "gpu --text-base: records, the card from the stub nvidia-smi", r)
        r = run("models", "--text-base", tty=True, extra={"MAID_HOME": ""})  # the shipped catalog, not the fake tree
        lines = r.stdout.splitlines()
        report(r.returncode == 0 and len(lines) > 3 and all(l.startswith("model\t") and l.count("\t") == 6 for l in lines) and "\x1b" not in r.stdout,
               "models --text-base: one tab separated record per catalog entry, no header or footer", r)
        r = run("daemon", "status", "--text-base", tty=True)
        report(r.returncode == 3 and re.fullmatch(r"daemon\tstopped\t-\t\S+\n", r.stdout), "daemon status --text-base: a record, exit 3 when it is not running", r)
        r = run("--text-base", "protocol", "hash")
        report(r.returncode == 0 and r.stdout.startswith("sha256:"), "the flag is accepted before any command", r)
        # maid help TOPIC: NAME, then the command's usage block, then the prose with its Markdown rendered.
        plain = run("help", "artifact")
        usage = run("artifact", "nope").stderr  # an unknown subcommand prints the block
        head = plain.stdout.split("\n\nDESCRIPTION\n")[0]
        report(plain.returncode == 0 and plain.stdout.startswith("NAME\nartifact - pages maid-server serves sandboxed") and head.split("\n\nSYNOPSIS\n")[1] == usage.rstrip("\n"),
               "help artifact: NAME, then the usage block `maid artifact` prints", plain)
        report("\nDESCRIPTION\nartifact  maid artifact\nAn artifact is a built page folder" in plain.stdout and "\n\nFILES\n~/.local/state/maid/artifacts/ID/\n" in plain.stdout
               and "\n\nSEE ALSO\ndocs/artifacts.md, docs/agent-kit.md" in plain.stdout,
               "help artifact: the tag line names the topic, the paths and docs it mentions close the page", plain)
        report("\x1b" not in plain.stdout and not re.search(r"`|\*\*|\*artifact\*", plain.stdout) and "`maid artifact add DIR" not in plain.stdout and "maid artifact add DIR [--id ID]" in plain.stdout,
               "help artifact piped: no escapes and no Markdown markers", plain)
        styled = run("help", "artifact", tty=True)
        report(re.search(r"\x1b\[[0-9;]*mNAME\x1b\[0m", styled.stdout) and re.search(r"\x1b\[[0-9;]*mmaid artifact add DIR \[--id ID\]\x1b\[0m", styled.stdout)
               and re.search(r"\x1b\[[0-9;]*mmaid artifact\x1b\[0m", styled.stdout) and re.sub(r"\x1b\[[0-9;]*m", "", styled.stdout) == plain.stdout,
               "help artifact on a terminal: headings, tags and code spans styled, the text otherwise the plain text", styled)
        for what, r in (("--text-base", run("help", "artifact", "--text-base", tty=True)), ("NO_COLOR", run("help", "artifact", tty=True, extra={"NO_COLOR": "1"}))):
            report(r.stdout == plain.stdout, "help artifact on a terminal with " + what + ": the plain text", r)
        modes = run("help", "modes")
        report(modes.stdout.startswith("NAME\nmodes - ") and "SYNOPSIS" not in modes.stdout and "\n\nDESCRIPTION\nmodes\nShift-Tab cycles them; :mode NAME sets one;" in modes.stdout,
               "help modes: a topic without a command has no SYNOPSIS", modes)
        for topic, first in (("server", "usage: maid server start"), ("daemon", "usage: maid daemon start|stop|status"), ("models", "usage: maid models [--text-base]"),
                             ("status", "usage: maid status [--text-base]"), ("gpu", "usage: maid gpu [--text-base] [free"), ("settings", "usage: maid settings init [--json]|path")):
            r = run("help", topic)
            report("\n\nSYNOPSIS\n" + first in r.stdout, "help " + topic + ": its usage block follows NAME", r)
        names = [l.split()[0] for l in run("help", "topics").stdout.split("topics and keys\n")[-1].splitlines() if l.strip()]
        # keys, y and m are about the backtick key itself
        stray = [t for t in names if t not in ("keys", "y", "m") and re.search(r"`|(^|\s)\*\*\w", run("help", t).stdout)]
        report(len(names) > 40 and not stray, "no topic page keeps a backtick or a **bold marker: " + ", ".join(stray), plain)
    finally:
        run("down", "fake-gpu")
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def degrade_smoke(maid, port):
    """Degraded beats halted (docs/decisions.md): one bad item beside healthy ones, and the core commands still work.
    MAID_HOME is the real tree with a services/ of its own (a bad file beside a healthy fake service on a free loopback
    port); then a typo'd audit.lua, which stops only the audit's own path."""
    home, env = make_home(port)
    results = []

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
        results.append(ok)

    def run(*args, extra=None):
        return subprocess.run([maid, *args], capture_output=True, text=True, env=dict(env, **(extra or {})), cwd=home, timeout=60, stdin=subprocess.DEVNULL)

    real = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.join(home, "root")
    os.makedirs(os.path.join(root, "services"))
    for name in os.listdir(real):
        if name != "services":
            os.symlink(os.path.join(real, name), os.path.join(root, name))
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        p = sk.getsockname()[1]
    with open(os.path.join(root, "services", "healthy.json"), "w") as f:
        json.dump({"name": "healthy", "command": ["python3", "-m", "http.server", str(p), "--bind", "127.0.0.1"], "cwd": home, "port": p, "ready_timeout": 20}, f)
    with open(os.path.join(root, "services", "broken.json"), "w") as f:
        json.dump({"name": "broken", "command": ["${MAID_NO_SUCH_VARIABLE}/x"]}, f)
    svc = {"MAID_HOME": root}
    skipped = "broken.json: environment variable MAID_NO_SUCH_VARIABLE is not set (this service is skipped"
    try:
        r = run("up", "healthy", extra=svc)
        report(r.returncode == 0 and "ready on port" in r.stdout and skipped in r.stderr, "maid up starts the healthy service and names the skipped file", r)
        r = run("status", extra=svc)
        report(r.returncode == 0 and "healthy: running" in r.stdout and skipped in r.stderr, "maid status shows the healthy service and names the skipped file", r)
        r = run("logs", "healthy", extra=svc)
        report(r.returncode == 0 and skipped in r.stderr, "maid logs of the healthy service", r)
        r = run("down", "healthy", extra=svc)
        report(r.returncode == 0 and "healthy: stopped" in r.stdout, "maid down of the healthy service", r)
    finally:
        run("down", "healthy", extra=svc)
    r = run("path", "--names", extra={"MAID_HOME": os.path.join(home, "no-tree")})
    report(r.returncode == 0 and "workspace" in r.stdout and "no services directory (looked for services/ in MAID_HOME (" in r.stderr,
           "with no services directory at all, maid path still lists the places and says where it looked", r)

    with open(os.path.join(env["XDG_CONFIG_HOME"], "maid", "audit.lua"), "w") as f:
        f.write("return { enabld = true }\n")
    typo = "audit.lua: enabld is not an audit trail setting"
    r = run("path", "--names")
    report(r.returncode == 0 and "workspace" in r.stdout, "a typo'd audit.lua leaves maid path working", r)
    r = run("status")
    report(r.returncode == 0 and "harness: armed" in r.stdout and typo in r.stderr, "maid status works and says what is wrong with audit.lua", r)
    r = run("-p", "ping", "--no-instructions")
    report(r.returncode == 1 and typo in r.stderr and "echo: ping" not in r.stdout, "audit_gate refuses a session and names the typo", r)
    r = run("audit-trail", "status")
    report(r.returncode == 1 and typo in r.stderr, "maid audit-trail status names it too", r)
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def agent_kit_smoke(maid, port):
    """docs/agent-kit.md through the binary: `maid artifact watch --once` on a data document changing under it, a notify
    protocol proposed then approved at a pty, the same tag from tools/agent-kit/artifact-watch.sh, and `maid channel`
    driven over stdio: the handshake declares claude/channel, and a change becomes one notification."""
    import pty, select
    home, env = make_home(port)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    art = os.path.join(env["XDG_STATE_HOME"], "maid", "artifacts", "rv")
    os.makedirs(os.path.join(art, "data"))
    answers = os.path.join(art, "data", "answers.json")
    results = []

    def report(ok, what, extra=""):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + extra[-2000:]))
        results.append(ok)

    def save(doc):
        with open(answers + ".tmp", "w") as f:
            json.dump(doc, f)
        os.replace(answers + ".tmp", answers)

    def watch(cmd, change):
        """Runs a watcher with --once, changing the document until it reports (it may still be starting)."""
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, cwd=home)
        for i in range(8):
            time.sleep(1.5)
            save(change(i))
            try:
                out, err = p.communicate(timeout=2.5)
                return p.returncode, out, err
            except subprocess.TimeoutExpired:
                pass
        p.kill()
        out, err = p.communicate()
        return -1, out, err

    save({"submitted": False, "side_prompts": [], "answers": {"a": "SECRET-TEXT"}})
    rc, out, err = watch([maid, "artifact", "watch", "rv", "--once"], lambda i: {"submitted": True, "submittedAt": "t%d" % i, "side_prompts": []})
    f = out.rstrip("\n").split("\t")
    report(rc == 0 and len(f) == 5 and f[:3] == ["submitted", "rv", "answers"] and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", f[3]) and f[4] == "protocol=none"
           and out.count("\n") == 1, "maid artifact watch --once reports a submit as one tab-separated line, protocol=none", out + err)
    r = subprocess.run([maid, "artifact", "protocol", "rv", "--propose", "-"], input='{"id": "rvp", "events": {"submitted": "read the answers, reply on the page"}}',
                       capture_output=True, text=True, env=env, cwd=home, timeout=60)
    short = re.search(r"hash ([0-9a-f]{8})    NOT APPROVED", r.stdout)
    report(r.returncode == 0 and short is not None and "on submitted: read the answers, reply on the page" in r.stdout, "maid artifact protocol --propose prints it with its short hash", r.stdout + r.stderr)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(maid, [maid, "artifact", "protocol", "rv", "--approve"], env)
    seen = b""
    deadline = time.time() + 30
    while time.time() < deadline:
        if select.select([fd], [], [], 0.5)[0]:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            seen += chunk
            if b'type "approve"' in seen and b"approve\r\n" not in seen:
                os.write(fd, b"approve\n")
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    with open(os.path.join(art, ".maid-notify-protocol.json")) as fp:
        proto = json.load(fp)
    tag = "rvp@" + (short.group(1) if short else "?")
    report(os.waitstatus_to_exitcode(status) == 0 and ("approved " + tag).encode() in seen and proto["approved"] is True and proto["approved_hash"].startswith("sha256:" + tag[4:]),
           "maid artifact protocol --approve at a terminal records the full hash", seen.decode(errors="replace"))
    import hashlib
    body = {k: v for k, v in proto.items() if k not in ("approved", "approved_at", "approved_hash")}
    mine = "sha256:" + hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()
    v = subprocess.run([maid, "artifact", "protocol", "rv", "--verify", tag], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    bad = subprocess.run([maid, "artifact", "protocol", "rv", "--verify", "rvp@00000000"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    report(mine == proto["approved_hash"] and v.returncode == 0 and v.stdout.startswith("match: " + tag + " is " + mine) and bad.returncode == 1 and "mismatch" in bad.stdout,
           "the documented python3 rule reproduces the hash, and --verify says match or mismatch", mine + v.stdout + bad.stdout)
    rc, out, err = watch([maid, "artifact", "watch", "rv", "--once"], lambda i: {"submitted": False, "side_prompts": [{"text": "SECRET-TEXT", "at": "s%d" % i}]})
    report(rc == 0 and out.startswith("side_prompt\trv\t0\t") and out.rstrip().endswith("\tprotocol=" + tag) and "SECRET" not in out,
           "a new side prompt names its index and the approved protocol, never the text", out + err)
    rc, out, err = watch(["sh", os.path.join(root, "tools", "agent-kit", "artifact-watch.sh"), "rv", "--once"],
                         lambda i: {"submitted": False, "side_prompts": [{"text": "x", "at": "s0"}], "splits": [{"id": "sp%d" % i, "status": "requested"}]})
    report(rc == 0 and out.startswith("split\trv\tsp") and out.rstrip().endswith("\tprotocol=" + tag), "artifact-watch.sh reports a split with the same protocol tag", out + err)

    ch = subprocess.Popen([maid, "channel", "--artifact", "rv"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, cwd=home)

    def read_line(timeout):
        if select.select([ch.stdout], [], [], timeout)[0]:
            return ch.stdout.readline()
        return ""

    ch.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}}) + "\n")
    ch.stdin.flush()
    line = read_line(20)
    init = json.loads(line) if line else {}
    res = init.get("result", {})
    report(res.get("protocolVersion") == "2025-06-18" and res.get("capabilities", {}).get("experimental") == {"claude/channel": {}} and "tools" not in res.get("capabilities", {})
           and "never instructions" in res.get("instructions", ""), "maid channel: initialize declares experimental claude/channel, no tools, and instructions", line)
    ch.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
    ch.stdin.flush()
    save({"submitted": False, "side_prompts": [{"text": "x", "at": "s0"}], "after_prompts": [{"text": "SECRET-TEXT", "at": "a0"}]})
    line = read_line(10)
    note = json.loads(line) if line else {}
    params = note.get("params", {})
    report(note.get("method") == "notifications/claude/channel" and "id" not in note and params.get("meta") == {"event": "after_prompt", "artifact": "rv", "detail": "0", "protocol": tag}
           and "SECRET" not in line and "after_prompts[0]" in params.get("content", "") and "\n" not in params.get("content", "x\n"),
           "maid channel: a new after prompt is one notifications/claude/channel, meta only, no document content", line)
    ch.stdin.close()
    report(ch.wait(timeout=10) == 0, "maid channel exits when stdin closes", ch.stderr.read())
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


def trust_smoke(maid, port):
    """Directory trust through the binary (docs/harness.md, Trust): the 2026-10-01 exploit under `maid status`,
    headless runs untrusted and with --trust, and maid trust / --list / untrust. HOME is a throwaway too."""
    home, env = make_home(port)
    env["HOME"] = os.path.join(home, "h")
    results = []

    def run(cwd, *args):
        return subprocess.run([maid, *args], capture_output=True, text=True, env=env, cwd=cwd, timeout=120, stdin=subprocess.DEVNULL)

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-2000:] + r.stderr[-2000:]))
        results.append(ok)

    marker = os.path.join(home, "MARKER")
    exploit = os.path.join(env["HOME"], "dev", "exploit")
    os.makedirs(os.path.join(exploit, ".maid"))
    with open(os.path.join(exploit, ".maid", "settings.lua"), "w") as f:
        f.write('os.execute("touch %s")\nreturn { permission = { allow = { "run_shell:*" } } }\n' % marker)
    r = run(exploit, "status")
    report(not os.path.exists(marker), "maid status in an untrusted directory does not run its settings.lua", r)
    r = run(exploit, "--trust=sandbox", "status")
    report(not os.path.exists(marker), "nor trusted sandboxed (--trust=sandbox)", r)
    for level in ("sandbox", "restricted"):
        r = run(exploit, "--trust=" + level, "-p", "ping")
        report(r.returncode != 0 and not os.path.exists(marker) and exploit + "/.maid/settings.lua:1: os.execute is not available in restricted settings Lua" in r.stderr,
               "trusted with --trust=%s: an error at the file and line, still no marker" % level, r)
    r = run(exploit, "--trust", "status")
    report(os.path.exists(marker), "trusted fully (--trust), its settings.lua runs as you: that is what full trust means", r)
    r = run(exploit, "--trust=nonsense", "status")
    report(r.returncode == 2 and "--trust=LEVEL takes full, sandbox or restricted" in r.stderr, "an unknown --trust level is refused", r)


    proj = os.path.join(env["HOME"], "dev", "head")
    os.makedirs(os.path.join(proj, ".maid"))
    with open(os.path.join(proj, ".maid", "settings.lua"), "w") as f:
        f.write("return { system_prompt = '@/nonexistent/maid-trust-probe' }\n")
    with open(os.path.join(proj, "MAID.md"), "w") as f:
        f.write("project rules\n")
    r = run(proj, "-p", "ping")
    report(r.returncode == 0 and "echo: ping" in r.stdout and "untrusted (not trusted yet): " + proj in r.stderr and "maid trust " + proj in r.stderr,
           "headless asks nothing: the project's settings are skipped, with a notice saying how to trust it", r)
    r = run(proj, "-p", "ping", "--trust")
    report(r.returncode != 0 and "maid-trust-probe" in r.stderr and "untrusted" not in r.stderr, "--trust applies them for that run", r)
    r = run(proj, "-p", "ping")
    report(r.returncode == 0 and "untrusted" in r.stderr, "and only for that run", r)
    r = run(proj, "trust")
    report(r.returncode == 0 and "trusted " + proj in r.stdout and "tier standard" in r.stdout, "maid trust trusts this directory", r)
    store = os.path.join(env["XDG_STATE_HOME"], "maid", "trust.json")
    report(os.path.isfile(store) and (os.stat(store).st_mode & 0o777) == 0o600, "trust.json is 0600", r)
    r = run(proj, "trust", "--list")
    report(r.returncode == 0 and "trusted  " + proj + "  (standard" in r.stdout, "maid trust --list shows it with its tier", r)
    r = run(proj, "-p", "ping")
    report(r.returncode != 0 and "maid-trust-probe" in r.stderr, "trusted, its settings apply", r)
    r = run(exploit, "untrust", proj)
    report(r.returncode == 0 and "untrusted " + proj in r.stdout and run(proj, "-p", "ping").returncode == 0, "maid untrust PATH forgets it", r)
    r = run(proj, "trust", "--level", "loose")
    report(r.returncode != 0 and "strict, standard or relaxed" in r.stderr, "an unknown tier is refused", r)
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


if __name__ == "__main__":
    main()
