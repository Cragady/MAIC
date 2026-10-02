#!/usr/bin/env python3
"""Runs the built maic binary headless against a fake OpenAI-compatible server: the real link, the real HTTP
client, the real settings loader. Under the asan preset this catches memory errors the in-process suites
cannot see (they link httplib themselves). Usage: cli_smoke.py PATH_TO_MAIC

Also a module for test_tui.py (the fake, the throwaway home), and `cli_smoke.py --serve` runs the fake alone,
printing its port."""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time


class Fake(http.server.BaseHTTPRequestHandler):
    """Answers every chat with "echo: " plus the first line of the last user message, streamed as SSE. A message
    starting with "hold" gets a reply that idles for 10 s instead, for the interrupt tests; one starting "slow:" is
    answered after three seconds, for a test that needs the agent busy."""

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
            self.reply(last)
        except (BrokenPipeError, ConnectionResetError):
            pass  # MAIC hung up mid-reply (an interrupt does): that is the end of this reply

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
    """A throwaway home whose settings point maic at the fake: (home dir, environment for maic).

    XDG_* keep ~/.config/maic and ~/.local/state/maic out of it; the tripwire file is one that never exists."""
    home = tempfile.mkdtemp(prefix="maic-smoke-")
    cfg = os.path.join(home, "config", "maic")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "settings.lua"), "w") as f:
        f.write("return { model = 'fake/fake', providers = { fake = { kind = 'openai', base_url = 'http://127.0.0.1:%d/v1' } }, harness = 'dumb', load_instructions = false }\n" % port)
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(home, "config"), XDG_STATE_HOME=os.path.join(home, "state"),
               XDG_RUNTIME_DIR=os.path.join(home, "run"), MAIC_TRIPWIRE_FILE=os.path.join(home, "none"), ASAN_OPTIONS="detect_leaks=0")
    env.pop("NVIM", None)  # run from inside nvim's terminal, maic would connect to that nvim (maic.nvim)
    os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700)
    return home, env


def main():
    if sys.argv[1] == "--serve":
        # For a test in another process (test_tui.py forks a job-control stub, so it keeps no threads of its own).
        srv, port = start_fake()
        print("port %d" % port, flush=True)
        threading.Event().wait()
    maic = os.path.abspath(sys.argv[1])
    srv, port = start_fake()
    home, env = make_home(port)
    r = subprocess.run([maic, "-p", "ping", "--no-instructions"], capture_output=True, text=True, env=env, cwd=home, timeout=120)
    ok = r.returncode == 0 and "echo: ping" in r.stdout and "AddressSanitizer" not in r.stderr
    print(("ok" if ok else "FAIL") + ": exit %d, stdout %r" % (r.returncode, r.stdout.strip()[:80]))
    if not ok:
        print(r.stderr[-3000:])
    # maic setup off a terminal: the plan and exit 2, nothing done (the settings file exists, so that step is not on it).
    s = subprocess.run([maic, "setup"], capture_output=True, text=True, env=env, cwd=home, timeout=120, stdin=subprocess.DEVNULL)
    setup_ok = s.returncode == 2 and "plan (each a yes/no in a terminal)" in s.stdout and "Build llama.cpp" in s.stdout and "Write the global settings" not in s.stdout and "AddressSanitizer" not in s.stderr
    # The models it offers come from the catalog, by id.
    setup_ok = setup_ok and "Fetch qwen3.5-4b (" in s.stdout and "Fetch qwen3.5-9b-text (a link to qwen3.5-9b's weights" in s.stdout
    print(("ok" if setup_ok else "FAIL") + ": setup off a terminal, exit %d" % s.returncode)
    if not setup_ok:
        print(s.stdout[-2000:], s.stderr[-2000:])
    # `maic tools check` over the shipped script tool examples, a scaffolded tool and a broken manifest.
    import shutil
    examples = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "examples")
    tools = os.path.join(home, ".maic", "tools")
    for name in ("word_count", "json_pick"):
        shutil.copytree(os.path.join(examples, name), os.path.join(tools, name))
    r = subprocess.run([maic, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    check_ok = r.returncode == 0 and "ok    " in r.stdout and "word_count (python)" in r.stdout and "json_pick (sh)" in r.stdout
    print(("ok" if check_ok else "FAIL") + ": maic tools check on the examples, exit %d" % r.returncode + ("" if check_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    r = subprocess.run([maic, "tools", "new", "greet", "--lang", "sh"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    new_ok = r.returncode == 0 and os.path.isfile(os.path.join(tools, "greet", "tool.json")) and os.path.isfile(os.path.join(tools, "greet", "main.sh"))
    print(("ok" if new_ok else "FAIL") + ": maic tools new scaffolds a tool, exit %d" % r.returncode + ("" if new_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    os.makedirs(os.path.join(tools, "needs_net"))
    with open(os.path.join(tools, "needs_net", "tool.json"), "w") as f:
        f.write('{"name": "needs_net", "description": "x", "run": ["sh", "-c", "true"], "network": true}')
    r = subprocess.run([maic, "tools", "check"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    bad_ok = r.returncode == 1 and "FAIL  " in r.stdout and "per-tool network grants are not implemented yet" in r.stdout
    print(("ok" if bad_ok else "FAIL") + ": maic tools check reports a manifest asking for the network, exit %d" % r.returncode + ("" if bad_ok else "\n" + r.stdout[-1500:]))
    u = subprocess.run([maic, "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    r = subprocess.run([maic, "tools", "--trust"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    list_ok = (r.returncode == 0 and "word_count  (python)" in r.stdout and "reads **; writes nothing; timeout 10 s" in r.stdout
               and "word_count  (python)" not in u.stdout and "this directory is untrusted: its .maic/tools/ are not loaded" in u.stdout)
    print(("ok" if list_ok else "FAIL") + ": maic tools lists script tools with language and declared reads/writes, once the directory is trusted" + ("" if list_ok else "\n" + r.stdout[-1500:] + u.stdout[-1500:]))
    r = subprocess.run([maic, "themes"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    themes_ok = r.returncode == 0 and "* default  built in" in r.stdout and "  gruvbox-dark  " in r.stdout and "  mono  " in r.stdout
    print(("ok" if themes_ok else "FAIL") + ": maic themes lists the shipped themes with the active one marked" + ("" if themes_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    # cai-tools (docs/cai.md): `maic cai read` on a MAIC session in the throwaway state dir, the `cai` wrapper matching
    # it byte for byte, a tool's help through `maic help`, the listing, and the dispatcher's exit code coming through.
    sess_dir = os.path.join(env["XDG_STATE_HOME"], "maic", "sessions", "general")
    os.makedirs(sess_dir, mode=0o700)
    sess = os.path.join(sess_dir, "20260101-120000-tui-1.jsonl")
    with open(sess, "w") as f:
        for rec in ({"type": "start", "workspace": home, "model": "fake/fake", "mode": "manual", "host": "h", "pid": 1},
                    {"type": "user", "text": "hello from a maic session", "provider": "fake", "model": "fake/fake", "remote": False, "mode": "manual"},
                    {"type": "msg", "role": "user", "content": "hello from a maic session"},
                    {"type": "tool", "tool": "read_file", "arguments": {"path": "a.txt"}, "result": "the file", "ok": True},
                    {"type": "assistant", "text": "echo: hello from a maic session"},
                    {"type": "usage", "input": 5, "output": 2, "context": 7}, {"type": "title", "text": "smoke"}):
            f.write(json.dumps(dict(rec, time="2026-01-01T12:00:00+0000")) + "\n")
    r = subprocess.run([maic, "cai", "read", sess], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    cai_ok = r.returncode == 0 and "hello from a maic session" in r.stdout and "echo: hello" in r.stdout and "L2 \u00b7 user" in r.stdout
    print(("ok" if cai_ok else "FAIL") + ": maic cai read on a MAIC session, exit %d" % r.returncode + ("" if cai_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    cai = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "cai", "bin", "cai")
    w = subprocess.run([cai, "read", sess, "--select", "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    m = subprocess.run([maic, "cai", "read", sess, "--select", "tools"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    wrap_ok = w.returncode == m.returncode == 0 and w.stdout == m.stdout and "[tool_use read_file]" in w.stdout
    print(("ok" if wrap_ok else "FAIL") + ": the cai wrapper and maic cai agree byte for byte" + ("" if wrap_ok else "\n" + w.stdout[-800:] + w.stderr[-800:] + m.stderr[-800:]))
    h = subprocess.run([maic, "help", "trans-fairy"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    c = subprocess.run([cai, "--help"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    mc = subprocess.run([maic, "cai", "--help"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    help_ok = h.returncode == 0 and "cai trans-fairy" in h.stdout and c.returncode == mc.returncode == 0 and c.stdout == mc.stdout and "trans-fairy-write" in c.stdout and "fabricate" in c.stdout
    print(("ok" if help_ok else "FAIL") + ": maic help trans-fairy and cai --help pass through" + ("" if help_ok else "\n" + h.stdout[-600:] + h.stderr[-600:] + c.stdout[-600:]))
    r = subprocess.run([maic, "cai", "nosuchtool"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    t = subprocess.run([maic, "tools", "--trust"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    exit_ok = (r.returncode == 2 and "no such tool" in r.stderr and "cai-tools (docs/cai.md" in t.stdout
               and all(("cai %s" % n) in t.stdout for n in ("trans-fairy", "trans-fairy-write", "fabricate", "read", "reflow"))
               and "maic trans-fairy-write" in t.stdout and "maic cai read" in t.stdout)
    print(("ok" if exit_ok else "FAIL") + ": the dispatcher's exit code comes through and maic tools lists the cai block" + ("" if exit_ok else "\n" + r.stderr[-600:] + t.stdout[-1200:]))
    # maic lazy-lock over a lock file in the throwaway XDG_CONFIG_HOME: none, not recorded, record, in sync, a lazy.nvim update.
    lazy = lambda *a: subprocess.run([maic, "lazy-lock", *a], capture_output=True, text=True, env=env, cwd=home, timeout=60)
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
    print(("ok" if lazy_ok else "FAIL") + ": maic lazy-lock status, record and diff with their exit codes" +
          ("" if lazy_ok else "\n" + "\n".join("exit %d: %s" % (r.returncode, r.stdout + r.stderr) for r, _, _ in steps)))
    # maic sessions redact --in-place keeps its copy where trans-fairy-write keeps its own, so cai's list-backups and
    # restore see it.
    with open(sess) as f:
        original = f.read()
    r = subprocess.run([maic, "sessions", "redact", sess, "--in-place"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
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
    print(("ok" if backup_ok else "FAIL") + ": maic sessions redact --in-place keeps a copy that cai trans-fairy-write list-backups and restore see" +
          ("" if backup_ok else "\n" + r.stdout + r.stderr + lb.stdout + lb.stderr + rs.stdout + rs.stderr))
    rehome_ok = rehome_smoke(maic, env, home)
    # `maic settings read diction`: {} without the file, the table as JSON, a refused call with file:line.
    diction_lua = os.path.join(home, "config", "maic", "diction.lua")
    r = subprocess.run([maic, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = r.returncode == 0 and r.stdout == "{}\n"
    with open(diction_lua, "w") as f:
        f.write("return { log_dir = '/tmp/x', presets = { mine = { scribe = 'qwen-4b' } } }\n")
    r = subprocess.run([maic, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 0 and json.loads(r.stdout) == {"log_dir": "/tmp/x", "presets": {"mine": {"scribe": "qwen-4b"}}}
    with open(diction_lua, "w") as f:
        f.write("print('to stderr')\nreturn {\n  x = io.open('/etc/hostname') and 'opened',\n}\n")
    r = subprocess.run([maic, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 0 and json.loads(r.stdout) == {"x": "opened"} and r.stderr == "to stderr\n"  # the user's own file: full Lua
    global_lua = os.path.join(home, "config", "maic", "settings.lua")
    with open(global_lua) as f:
        saved = f.read()
    with open(global_lua, "w") as f:
        f.write(saved.replace("return { ", "return { global_lua = 'sandbox', ", 1))
    r = subprocess.run([maic, "settings", "read", "diction"], capture_output=True, text=True, env=env, cwd=home, timeout=60)
    read_ok = read_ok and r.returncode == 1 and r.stdout == "" and diction_lua + ":3: io is not available" in r.stderr and "AddressSanitizer" not in r.stderr
    with open(global_lua, "w") as f:
        f.write(saved)

    print(("ok" if read_ok else "FAIL") + ": maic settings read diction" + ("" if read_ok else "\n" + r.stdout[-1500:] + r.stderr[-1500:]))
    models_ok = models_smoke(maic, port)
    trust_ok = trust_smoke(maic, port)
    srv.shutdown()
    sys.exit(0 if ok and trust_ok and setup_ok and check_ok and new_ok and bad_ok and list_ok and themes_ok and cai_ok and wrap_ok and help_ok and exit_ok and lazy_ok and backup_ok and rehome_ok and read_ok and models_ok else 1)


def rehome_smoke(maic, env, home):
    """`maic sessions rehome`: subagent sessions move only when asked, several targets, --children-of, an ambiguous
    prefix and a running session refused with nothing moved, --dry-run, and forks and subagents loading after each move."""
    sessions = os.path.join(env["XDG_STATE_HOME"], "maic", "sessions")
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
        return subprocess.run([maic, "sessions", *args], capture_output=True, text=True, env=env, cwd=home, timeout=60)

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
    # This script's own process stands in for a running maic: its command line names the maic binary.
    L = "20260102-120006-tui-17"
    write(L, [start(pid=os.getpid(), host=socket.gethostname()), msg("live")])
    r = run("rehome", X, L, "rh3")
    report(r.returncode == 1 and L + " is running (pid %d)" % os.getpid() in r.stderr and where(L) == "general" and where(X) == "rh",
           "a running session is refused with the reason and nothing moves", r)
    return all(results)


def models_smoke(maic, port):
    """`maic models` over a models tree shaped like Micaiah's drive: the 4B and 9B folders with their projectors (sparse
    files of the catalog's sizes) and the -text folder holding only the relative link. Nothing is downloaded."""
    home, env = make_home(port)
    mdir = os.path.join(home, "models")
    with open(os.path.join(home, "config", "maic", "settings.lua"), "w") as f:
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
        return subprocess.run([maic, *args], capture_output=True, text=True, env=env, cwd=home, timeout=60, stdin=subprocess.DEVNULL)

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-2000:] + r.stderr[-2000:]))
        results.append(ok)

    def row(out, mid):
        """The table's columns by position: id, role, size, installed, current, presets."""
        line = next((l for l in out.splitlines() if l.startswith(mid + " ")), "")
        return {"size": line[40:49].strip(), "installed": line[50:60].strip(), "current": line[61:70].strip(), "presets": line[71:].strip()}

    r = run("models", "check")
    report(r.returncode == 0 and "0 problems" in r.stdout, "maic models check passes on the shipped catalog", r)
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
    report(r.returncode == 0 and "*models*" in r.stdout and "llama.vim" in r.stdout, "maic help models is the catalog page", r)
    return all(results)


def trust_smoke(maic, port):
    """Directory trust through the binary (docs/harness.md, Trust): the 2026-10-01 exploit under `maic status`,
    headless runs untrusted and with --trust, and maic trust / --list / untrust. HOME is a throwaway too."""
    home, env = make_home(port)
    env["HOME"] = os.path.join(home, "h")
    results = []

    def run(cwd, *args):
        return subprocess.run([maic, *args], capture_output=True, text=True, env=env, cwd=cwd, timeout=120, stdin=subprocess.DEVNULL)

    def report(ok, what, r):
        print(("ok" if ok else "FAIL") + ": " + what + ("" if ok else "\n" + r.stdout[-2000:] + r.stderr[-2000:]))
        results.append(ok)

    marker = os.path.join(home, "MARKER")
    exploit = os.path.join(env["HOME"], "dev", "exploit")
    os.makedirs(os.path.join(exploit, ".maic"))
    with open(os.path.join(exploit, ".maic", "settings.lua"), "w") as f:
        f.write('os.execute("touch %s")\nreturn { permission = { allow = { "run_shell:*" } } }\n' % marker)
    r = run(exploit, "status")
    report(not os.path.exists(marker), "maic status in an untrusted directory does not run its settings.lua", r)
    r = run(exploit, "--trust=sandbox", "status")
    report(not os.path.exists(marker), "nor trusted sandboxed (--trust=sandbox)", r)
    for level in ("sandbox", "restricted"):
        r = run(exploit, "--trust=" + level, "-p", "ping")
        report(r.returncode != 0 and not os.path.exists(marker) and exploit + "/.maic/settings.lua:1: os.execute is not available in restricted settings Lua" in r.stderr,
               "trusted with --trust=%s: an error at the file and line, still no marker" % level, r)
    r = run(exploit, "--trust", "status")
    report(os.path.exists(marker), "trusted fully (--trust), its settings.lua runs as you: that is what full trust means", r)
    r = run(exploit, "--trust=nonsense", "status")
    report(r.returncode == 2 and "--trust=LEVEL takes full, sandbox or restricted" in r.stderr, "an unknown --trust level is refused", r)


    proj = os.path.join(env["HOME"], "dev", "head")
    os.makedirs(os.path.join(proj, ".maic"))
    with open(os.path.join(proj, ".maic", "settings.lua"), "w") as f:
        f.write("return { system_prompt = '@/nonexistent/maic-trust-probe' }\n")
    with open(os.path.join(proj, "MAIC.md"), "w") as f:
        f.write("project rules\n")
    r = run(proj, "-p", "ping")
    report(r.returncode == 0 and "echo: ping" in r.stdout and "untrusted (not trusted yet): " + proj in r.stderr and "maic trust " + proj in r.stderr,
           "headless asks nothing: the project's settings are skipped, with a notice saying how to trust it", r)
    r = run(proj, "-p", "ping", "--trust")
    report(r.returncode != 0 and "maic-trust-probe" in r.stderr and "untrusted" not in r.stderr, "--trust applies them for that run", r)
    r = run(proj, "-p", "ping")
    report(r.returncode == 0 and "untrusted" in r.stderr, "and only for that run", r)
    r = run(proj, "trust")
    report(r.returncode == 0 and "trusted " + proj in r.stdout and "tier standard" in r.stdout, "maic trust trusts this directory", r)
    store = os.path.join(env["XDG_STATE_HOME"], "maic", "trust.json")
    report(os.path.isfile(store) and (os.stat(store).st_mode & 0o777) == 0o600, "trust.json is 0600", r)
    r = run(proj, "trust", "--list")
    report(r.returncode == 0 and "trusted  " + proj + "  (standard" in r.stdout, "maic trust --list shows it with its tier", r)
    r = run(proj, "-p", "ping")
    report(r.returncode != 0 and "maic-trust-probe" in r.stderr, "trusted, its settings apply", r)
    r = run(exploit, "untrust", proj)
    report(r.returncode == 0 and "untrusted " + proj in r.stdout and run(proj, "-p", "ping").returncode == 0, "maic untrust PATH forgets it", r)
    r = run(proj, "trust", "--level", "loose")
    report(r.returncode != 0 and "strict, standard or relaxed" in r.stderr, "an unknown tier is refused", r)
    shutil.rmtree(home, ignore_errors=True)
    return all(results)


if __name__ == "__main__":
    main()
