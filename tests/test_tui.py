#!/usr/bin/env python3
"""The TUI through a pty: maid as a person sees it, against the fake server from cli_smoke.py.

    python3 tests/test_tui.py [PATH_TO_MAID] [-v]           (re-runs itself under `uv run --offline --with pyte` if pyte is missing)

Exit 77 when neither pyte nor uv is available, which ctest reports as a skip. Every case starts its own maid in
a throwaway home (XDG_* under a temp dir, --no-record, the dumb harness, no instruction files), 120x40.

The protocol is checked in every case: each maid records its engine connection (MAID_PROTOCOL_RECORD) and the case
fails when `maid protocol check` finds a violation in it, or when the engine's own guarded checks logged one
(protocol.log)."""
import json, os, shutil, subprocess, sys, tempfile, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAID = os.environ.get("MAID_BIN", os.path.join(HERE, "..", "build", "cli", "maid"))

try:
    import pyte  # noqa: F401
except ImportError:
    if os.environ.get("MAID_TUI_REEXEC"):
        print("skipped: pyte is not importable even under uv")
        sys.exit(77)
    uv = shutil.which("uv")
    if not uv:
        print("skipped: the TUI test needs pyte (pip install pyte, or uv on PATH to fetch it)")
        sys.exit(77)
    # uv asks PyPI about pyte on most runs; the gate stays offline unless MAID_NETWORK_TESTS=1 (docs/testing.md).
    run = [uv, "run", "--quiet"] + ([] if os.environ.get("MAID_NETWORK_TESTS") == "1" else ["--offline"]) + ["--with", "pyte", "python"]
    if subprocess.call(run + ["-c", "import pyte"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        print("skipped: pyte is not in uv's cache and the network is off (MAID_NETWORK_TESTS=1 lets uv fetch it)")
        sys.exit(77)
    sys.exit(subprocess.call(run + [__file__] + sys.argv[1:], env=dict(os.environ, MAID_TUI_REEXEC="1", MAID_BIN=os.path.abspath(MAID))))

sys.path.insert(0, HERE)
import cli_smoke  # noqa: E402
from tui_driver import Tui  # noqa: E402

STRIP = "(shift-tab to cycle)"


class TuiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The fake in its own process: this one forks the job-control stub and must stay single-threaded.
        cls.fake = subprocess.Popen([sys.executable, os.path.join(HERE, "cli_smoke.py"), "--serve"], stdout=subprocess.PIPE, text=True)
        port = int(cls.fake.stdout.readline().split()[1])
        cls.home, cls.env = cli_smoke.make_home(port)
        cls.ws = os.path.join(cls.home, "ws")
        os.makedirs(cls.ws)
        cls.streams = os.path.join(cls.home, "protocol-streams")
        os.makedirs(cls.streams)
        cls.env["MAID_PROTOCOL_RECORD"] = cls.streams

    def setUp(self):
        self.addCleanup(self.check_protocol)  # the first cleanup runs last, after every maid of the case is gone

    def check_protocol(self):
        """Turns what the guarded tier logs, and what the recorded streams break, into this case's failure."""
        logs = [os.path.join(d, "protocol.log") for d, _, files in os.walk(self.home) if "protocol.log" in files]
        faults = ""
        for log in logs:
            with open(log) as f:
                faults += f.read()
            os.remove(log)
        streams = [os.path.join(self.streams, f) for f in os.listdir(self.streams)]
        r = subprocess.run([MAID, "protocol", "check", self.streams], capture_output=True, text=True, env=self.env, timeout=60) if streams else None
        for f in streams:
            os.remove(f)
        self.assertEqual(faults, "", "the engine's guarded checks logged faults")
        if r is not None:
            self.assertEqual(r.returncode, 0, "a recorded stream breaks the protocol:\n" + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.fake.kill()
        cls.fake.wait()
        shutil.rmtree(cls.home, ignore_errors=True)

    def start(self, *args, env=None):
        tui = Tui([MAID, "--no-record", "--harness", "dumb", "--no-instructions", *args], env=env or self.env, cwd=self.ws)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        return tui

    def test_welcome_shows_the_status_strip(self):
        tui = self.start()
        text = tui.text()
        self.assertIn("MAID  ·  workspace " + self.ws, text)
        self.assertIn("model fake/fake", text)
        self.assertIn("⏵ manual " + STRIP, text)
        self.assertIn("harness armed", text)
        self.assertIn("DUMB HARNESS", text)
        self.assertIn(" NORMAL ", text)  # the input starts in normal mode

    def test_insert_type_and_send_gets_the_echo(self):
        tui = self.start()
        tui.send("ihello there")
        self.assertIn(" INSERT ", tui.text())
        self.assertIn("❯ hello there", tui.text())  # the insert-mode gutter
        tui.send("<esc>:w<cr>", settle=False)
        text = tui.wait_for("echo: hello there")
        self.assertIn("❯ hello there", text)
        self.assertIn(" NORMAL ", text)
        tui.settle()
        self.assertIn("ctx 5", tui.text())  # the usage report reached the strip

    def test_alt_enter_sends_too(self):
        tui = self.start()
        tui.send("iping<m-cr>", settle=False)
        tui.wait_for("echo: ping")

    def test_ctrl_u_scrolls_up_and_ctrl_d_down(self):
        tui = self.start()
        tui.send("i" + "<cr>".join("scroll%02d" % n for n in range(1, 61)))
        tui.send("<esc>:w<cr>", settle=False)
        tui.wait_for("echo: scroll01")
        tui.settle()
        self.assertNotIn("scroll05", tui.text())
        for _ in range(4):
            tui.send("<c-u>")
        self.assertIn("scroll05", tui.text(), "Ctrl-U scrolls up, toward older lines, as in vim")
        self.assertNotIn("echo: scroll01", tui.text())
        for _ in range(4):
            tui.send("<c-d>")
        self.assertIn("echo: scroll01", tui.text(), "Ctrl-D scrolls back down")

    def test_a_running_command_shows_its_output_live(self):
        tui = self.start()
        # The output's words are not in the command's text, so finding them on screen means the output is there.
        tui.send("ishell:printf 'live-%s\\n' one; sleep 4; printf 'live-%s\\n' two<esc>:w<cr>", settle=False)
        tui.wait_for("[y] yes")
        tui.send("y", settle=False)
        text = tui.wait_for("live-one", timeout=3)
        self.assertNotIn("live-two", text, "the first line shows while the command is still running")
        self.assertIn("▸ $ printf", text)
        text = tui.wait_for("ran it", timeout=15)
        self.assertIn("exit code 0", text, "the result took the live view's place")
        self.assertIn("live-two", text)

    def test_a_bang_command_says_it_starts_no_turn(self):
        tui = self.start()
        tui.send("i!printf 'paw-%s\\n' print<esc>:w<cr>", settle=False)
        text = tui.wait_for("result added; the agent sees it with your next message")
        self.assertIn("paw-print", text)
        self.assertNotIn("echo:", text, "a !cmd starts no turn")
        self.assertLess(text.index("paw-print"), text.index("result added"), "the line comes after the output")
        tui.send("ithen this<esc>:w<cr>", settle=False)
        text = tui.wait_for("echo: then this")
        self.assertLess(text.index("result added"), text.index("echo: then this"), "the agent answers the next message after the notice")

    def test_a_long_output_is_kept_whole_and_labelled(self):
        tui = self.start()
        tui.send("ishell:seq 1 20000<esc>:w<cr>", settle=False)
        tui.wait_for("[y] yes")
        tui.send("y", settle=False)
        tui.wait_for("ran it", timeout=15)
        tui.send(":set details on<cr>")
        tui.send("<c-w>kgg")
        text = tui.wait_for("[full output, display only: the model saw the capped result: maid sessions output ")
        self.assertIn("call_1]", text)
        # The same file from the shell: the label on stderr, the output itself on stdout.
        runtime = os.path.join(self.env["XDG_RUNTIME_DIR"], "maid", "sessions")
        newest = max((os.path.join(runtime, f) for f in os.listdir(runtime) if f.endswith(".jsonl")), key=os.path.getmtime)
        r = subprocess.run([MAID, "sessions", "output", newest, "call_1"], capture_output=True, text=True, env=self.env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "".join("%d\n" % i for i in range(1, 20001)))
        self.assertIn("full output, display only: the model saw the capped result", r.stderr)

    def test_ctrl_s_pauses_and_ctrl_q_resumes(self):
        tui = self.start()
        tui.send("ihold on<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts, ctrl-s pauses")
        tui.send("<c-s>", settle=False)
        text = tui.wait_for("PAUSED: ctrl-q resumes")
        self.assertIn("[Ctrl-Q] resume", text, "the pause menu is up")
        self.assertIn("↯ interrupt", text)
        self.assertNotIn("▣ ", text, "a pause is not the end of the turn: no footer yet")
        tui.send("<c-q>", settle=False)
        text = tui.wait_for("echo: Continue from where you stopped.")
        tui.wait_for("▣ ")
        self.assertNotIn("PAUSED", tui.text())

    def test_pause_menu_keeps_and_steer_drops(self):
        tui = self.start()
        tui.send("ihold on<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        tui.send("<c-s>", settle=False)
        tui.wait_for("[Ctrl-Q] resume")
        tui.send("k", settle=False)
        text = tui.wait_for("▣ ")
        self.assertIn("↯ keep", text)
        self.assertIn("holding", text, "keep leaves the partial reply as the answer")
        tui.send("ihold again<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        tui.send("<esc>:steer drop leave it out<cr>", settle=False)
        text = tui.wait_for("echo: The user dropped the topic")
        self.assertIn("↯ drop: leave it out", text)

    def test_help_opens(self):
        tui = self.start()
        tui.send(":help<cr>")
        text = tui.text()
        self.assertIn("shift-tab  cycle the mode", text)
        self.assertIn("ctrl-z  suspend to the shell; fg resumes", text)

    def test_path_lists_places(self):
        tui = self.start()
        tui.send(":path<cr>")
        text = tui.text()
        self.assertIn("places (:path NAME shows one", text)
        self.assertIn("workspace  " + self.ws, text)
        self.assertIn("settings  " + os.path.join(self.home, "config", "maid", "settings.lua"), text)

    def test_model_lists_presets(self):
        tui = self.start()
        tui.send(":model<cr>")
        text = tui.text()
        self.assertIn("model: fake/fake", text)
        self.assertIn("presets (:model NAME):", text)
        self.assertIn("qwen-4b", text)
        self.assertIn("fake/<model>  (openai, http://127.0.0.1:", text)

    def test_shift_tab_cycles_the_mode(self):
        tui = self.start()
        tui.send("<s-tab>")
        self.assertIn("⏵ auto-read " + STRIP, tui.text())
        tui.send("<s-tab>")
        self.assertIn("⏵ edit " + STRIP, tui.text())

    def test_ctrl_z_suspends_and_fg_redraws(self):
        tui = self.start()
        tui.send("isome draft")
        tui.suspend()
        self.assertNotIn(STRIP, tui.text())  # the screen was given back to the shell
        tui.resume()
        text = tui.wait_for(STRIP)
        self.assertIn("❯ some draft", text)  # the input survived, still in insert mode

    def test_theme_switches_the_colours_live(self):
        tui = self.start(env=dict(self.env, COLORTERM="truecolor"))

        def fg_of(needle):
            for y, line in enumerate(tui.screen.display):
                x = line.find(needle)
                if x >= 0:
                    return tui.screen.buffer[y][x].fg
            self.fail("%r is not on the screen:\n%s" % (needle, tui.text()))

        self.assertEqual(fg_of("harness armed"), "green")  # the default theme
        tui.send(":theme gruvbox-dark<cr>")
        self.assertIn("theme: gruvbox-dark (", tui.text())
        self.assertEqual(fg_of("harness armed"), "b8bb26")  # gruvbox green, in truecolor
        tui.send(":theme no-such-theme<cr>")
        self.assertIn("no theme no-such-theme", tui.text())
        self.assertEqual(fg_of("harness armed"), "b8bb26")  # a failed switch keeps the current theme

    def test_bracketed_paste_goes_into_the_input(self):
        # maid.nvim's fallback when MAID is not connected: the text arrives as a bracketed paste, in normal mode too.
        tui = self.start()
        tui.send("\x1b[200~fix this:\nx = 1\x1b[201~")
        text = tui.text()
        self.assertIn("│ fix this:", text)
        self.assertIn("  x = 1", text)
        self.assertIn("pasted 2 lines into the input", text)
        self.assertNotIn("echo:", text)

    def test_inside_a_host_nvim(self):
        nvim = shutil.which("nvim")
        if not nvim:
            self.skipTest("nvim is not on PATH")
        sock = os.path.join(tempfile.mkdtemp(prefix="maid-tui-nvim-", dir=self.home), "nvim.sock")
        plugin = os.path.join(HERE, "..", "maid.nvim")
        host = subprocess.Popen([nvim, "--headless", "-u", "NONE", "-i", "NONE", "-n", "--listen", sock, "--cmd", "set rtp+=" + plugin],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (host.kill(), host.wait()))
        for _ in range(500):
            if os.path.exists(sock):
                break
            time.sleep(0.02)

        def remote(expr):
            return subprocess.run([nvim, "--headless", "--server", sock, "--remote-expr", expr], capture_output=True, text=True, timeout=10).stdout.strip()

        # Not an ancestor of maid: refused, with the reason on screen.
        refused = self.start(env=dict(self.env, NVIM=sock))
        self.assertIn("nvim: not connecting to $NVIM", refused.text())
        self.assertNotIn(" nvim ·", refused.text())
        # The test-only pair of variables lets it connect.
        tui = self.start(env=dict(self.env, NVIM=sock, MAID_TESTING="1", MAID_NVIM_TRUST_SOCKET="1"))
        tui.wait_for("nvim: connected to the nvim MAID runs in")
        self.assertIn(" nvim ·", tui.text())  # the status strip
        remote("luaeval('require(\"maid\").send_text(\"from nvim\")')")
        tui.wait_for("│ from nvim")
        self.assertNotIn("echo: from nvim", tui.text())  # never sent by itself
        with open(os.path.join(self.ws, "notes.txt"), "w") as f:
            f.write("hello\n")
        tui.send("<esc>:e notes.txt<cr>")
        self.assertEqual(remote("expand('%:t')"), "notes.txt")
        remote("luaeval('require(\"maid\").command(\":rename from the plugin\")')")
        tui.wait_for("titled: from the plugin")
        # :MaidInterrupt does what the first Ctrl-C does to a running turn; idle, MAID says so and keeps the draft.
        tui.send("<esc><c-c>")  # the first Ctrl-C of an idle MAID clears the input
        tui.send("ihold on<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        self.assertEqual(remote("luaeval('require(\"maid\").interrupt()')"), "rpc")
        tui.wait_for("interrupted")
        tui.send("<esc>ia draft<esc>")
        remote("luaeval('require(\"maid\").interrupt()')")
        text = tui.wait_for("nvim: nothing to interrupt (MAID is idle)")
        self.assertIn("│ a draft", text)

    def test_keymap_check_after_a_lazy_lock_change(self):
        # A changed lazy-lock.json makes MAID run the keymap check once in the background (headless nvim with this
        # home's own nvim config) and report the collisions the last run did not have.
        if not shutil.which("nvim"):
            self.skipTest("nvim is not on PATH")
        cfg = tempfile.mkdtemp(prefix="maid-tui-keys-", dir=self.home)
        shutil.copytree(os.path.join(self.home, "config", "maid"), os.path.join(cfg, "maid"))
        os.makedirs(os.path.join(cfg, "nvim"))
        init, lock = os.path.join(cfg, "nvim", "init.lua"), os.path.join(cfg, "nvim", "lazy-lock.json")
        with open(init, "w") as f:
            f.write("vim.keymap.set('t', '<C-w>', '<C-\\\\><C-n><C-w>', { desc = 'window from a terminal' })\n")
        with open(lock, "w") as f:
            f.write('{ "lazy.nvim": { "branch": "main", "commit": "1111111" } }\n')
        env = dict(self.env, XDG_CONFIG_HOME=cfg, XDG_STATE_HOME=tempfile.mkdtemp(prefix="maid-tui-state-", dir=self.home))
        first = self.start(env=env)
        text = first.wait_for("nvim keymaps, checked for the first time: 1 collide: maid nvim keymaps", timeout=60)
        self.assertIn("MAID never gets <C-w>", text)
        with open(init, "a") as f:
            f.write("vim.keymap.set('t', '<C-p>', '<C-\\\\><C-n>p', { desc = 'paste from a terminal' })\n")
        with open(lock, "w") as f:
            f.write('{ "lazy.nvim": { "branch": "main", "commit": "2222222" } }\n')
        second = self.start(env=env)
        text = second.wait_for("a plugin update added keymaps that collide: maid nvim keymaps", timeout=60)
        self.assertIn("MAID never gets <C-p>", text)
        self.assertNotIn("MAID never gets <C-w>", text)  # known since the last run
        third = self.start(env=env)  # the same lock file: no second run
        time.sleep(3)
        third.settle()
        self.assertNotIn("collide", third.text())

    def test_bare_shows_in_the_strip(self):
        tui = self.start("--bare")
        self.assertIn(" bare · harness armed", tui.text())
        tui.send(":theme nvim:habamax<cr>")
        self.assertIn("this MAID is bare", tui.text())
        tui.send(":nvim<cr>")
        self.assertIn("nvim: bare (--bare, MAID_BARE=1 or bare = true)", tui.text())
        plain = self.start(env=dict(self.env, MAID_BARE="1"))
        self.assertIn(" bare · harness armed", plain.text())
        self.assertNotIn(" bare ·", self.start().text())

    def test_cd_moves_the_workspace(self):
        sub = os.path.join(self.ws, "cd-sub")
        os.makedirs(os.path.join(sub, ".maid"), exist_ok=True)
        with open(os.path.join(sub, ".maid", "settings.lua"), "w") as f:
            f.write("return { timestamps = true }\n")
        tui = self.start()
        tui.send(":cd cd-sub<cr>")
        text = tui.wait_for(" trust this directory? ")  # a project directory: :cd asks as a start there would
        self.assertIn("settings:     .maid/settings.lua", text)
        self.assertIn("[s] trust sandboxed", text)
        tui.send("s")
        text = tui.wait_for("settings changed: timestamps")
        self.assertIn("trusted sandboxed: " + sub, text)  # its settings.lua ran in the sandbox, and applied

        self.assertIn("workspace: " + sub, text)
        self.assertIn(" · in " + sub, text)  # the status strip
        tui.send(":pwd<cr>")
        tui.wait_for(":cd - returns to " + self.ws)
        tui.send(":cd -<cr>")
        tui.wait_for("workspace: " + self.ws + "  (was " + sub)
        tui.send(":cd no-such-dir<cr>")
        tui.wait_for("no directory " + os.path.join(self.ws, "no-such-dir"))

    def test_cd_waits_until_idle(self):
        os.makedirs(os.path.join(self.ws, "cd-busy"), exist_ok=True)
        tui = self.start()
        tui.send("islow: take your time<esc>:w<cr>", settle=False)
        tui.wait_for("working…")
        tui.send(":cd cd-busy<cr>", settle=False)
        tui.wait_for(":cd has to wait until the agent is idle")

    def test_init_moves_a_recorded_session(self):
        proj = os.path.realpath(os.path.join(self.home, "init-proj"))
        os.makedirs(proj)
        tui = Tui([MAID, "--harness", "dumb", "--no-instructions"], env=self.env, cwd=proj)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        tui.send(":init<cr>", settle=False)
        encoded = proj.replace("/", "-").replace(" ", "-")
        text = tui.wait_for("this session moved to projects/")
        self.assertIn("(it worked here throughout)", text.replace("\n", " "))
        home = os.path.join(self.home, "state", "maid", "sessions", "projects", encoded)
        moved = [f for f in os.listdir(home) if f.endswith(".jsonl")]
        self.assertEqual(len(moved), 1)
        self.assertEqual([f for f in os.listdir(home) if not f.endswith(".jsonl")], [])  # no .pending or .moving left
        tui.wait_for("echo: Look over this project")  # the MAID.md prompt still runs, into the moved file
        with open(os.path.join(home, moved[0])) as f:
            self.assertIn('"type":"rehomed"', f.read().replace(" ", ""))

    def test_init_leaves_an_unrecorded_session(self):
        ws = tempfile.mkdtemp(prefix="maid-tui-init-", dir=self.home)  # its own: :init makes it a project directory
        tui = Tui([MAID, "--no-record", "--harness", "dumb", "--no-instructions"], env=self.env, cwd=ws)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        tui.send(":init<cr>")
        tui.wait_for("this session stays out of the project's home: it is not recorded")
        tui.wait_for("is not trusted, so its MAID.md")

    def test_cd_into_a_project_directory_not_now(self):
        sub = os.path.join(self.ws, "cd-untrusted")
        os.makedirs(os.path.join(sub, ".maid"), exist_ok=True)
        with open(os.path.join(sub, ".maid", "settings.lua"), "w") as f:
            f.write("return { timestamps = true }\n")
        tui = self.start()
        tui.send(":cd cd-untrusted<cr>")
        tui.wait_for(" trust this directory? ")
        tui.send("n")
        text = tui.wait_for("workspace: " + sub)
        self.assertIn("not now: " + sub + " is untrusted for this session", text)
        self.assertIn("settings: unchanged", text)  # its settings.lua was not read


    def trust_case(self, settings):
        """A workspace with a project settings file, a state directory of its own, and maid started there (not
        waiting for the screen: the trust prompt comes first)."""
        ws = tempfile.mkdtemp(prefix="maid-tui-trust-", dir=self.home)
        os.makedirs(os.path.join(ws, ".maid"))
        with open(os.path.join(ws, ".maid", "settings.lua"), "w") as f:
            f.write(settings)
        env = dict(self.env, XDG_STATE_HOME=tempfile.mkdtemp(prefix="maid-tui-state-", dir=self.home))

        def start():
            tui = Tui([MAID, "--no-record", "--harness", "dumb", "--no-instructions"], env=env, cwd=ws)
            self.addCleanup(tui.close)
            return tui
        return ws, start

    def test_trust_prompt_then_trusted(self):
        ws, start = self.trust_case("return { timestamps = true }\n")
        tui = start()
        text = tui.wait_for("[t] trust fully")
        self.assertIn("a project directory you have not trusted", text)
        self.assertIn(ws, text)
        self.assertIn("settings:     .maid/settings.lua", text)
        self.assertIn("tier standard;", text)
        self.assertIn("[s] trust sandboxed: its Lua runs in a child process that cannot reach the system", text)

        tui.send("t<cr>", settle=False)
        tui.wait_for(STRIP)
        tui.send(":settings<cr>")
        self.assertIn(os.path.join(ws, ".maid", "settings.lua"), tui.text())  # applied
        again = start()
        text = again.wait_for(STRIP)
        self.assertNotIn("[t] trust fully", text)  # remembered: not asked twice

    def test_trust_prompt_not_now_then_trust_command(self):
        ws, start = self.trust_case("return { timestamps = true }\n")
        tui = start()
        tui.wait_for("[t] trust fully")
        tui.send("n<cr>", settle=False)
        text = tui.wait_for(STRIP)
        self.assertIn("untrusted (untrusted this session): " + ws, text)
        tui.send(":settings<cr>")
        self.assertNotIn(os.path.join(ws, ".maid", "settings.lua"), tui.text())  # not applied
        tui.send(":trust<cr>")
        self.assertIn("trusted " + ws, tui.wait_for("trusted " + ws))
        again = start()
        self.assertNotIn("[t] trust fully", again.wait_for(STRIP))

    def test_own_import_from_outside_is_asked_once(self):
        # A config of its own: instruction files on, and a MAID.md that imports a file outside every trusted directory.
        base = os.path.join(self.home, "own-import")
        cfg = os.path.join(base, "config", "maid")
        os.makedirs(cfg)
        os.makedirs(os.path.join(base, "outside"))
        target = os.path.join(base, "outside", "style.md")
        with open(target, "w") as f:
            f.write("write like a heron\n")
        with open(os.path.join(self.home, "config", "maid", "settings.lua")) as f:
            settings = f.read().replace("load_instructions = false", "load_instructions = true")
        with open(os.path.join(cfg, "settings.lua"), "w") as f:
            f.write(settings)
        with open(os.path.join(cfg, "MAID.md"), "w") as f:
            f.write("Follow @%s please.\n" % target)
        env = dict(self.env, XDG_CONFIG_HOME=os.path.join(base, "config"), XDG_STATE_HOME=os.path.join(base, "state"))

        def start():
            tui = Tui([MAID, "--no-record", "--harness", "dumb"], env=env, cwd=self.ws)
            self.addCleanup(tui.close)
            return tui

        tui = start()
        text = tui.wait_for("import from outside?")
        self.assertIn("target:         " + target, text)
        self.assertIn("size:           19 bytes", text)
        self.assertIn("it becomes standing instructions for every agent in every project", text)
        self.assertIn("its contents are sent to whatever model provider a session uses, cloud included", text)
        self.assertIn("every session pays its tokens", text)
        self.assertIn("if an agent or another tool can write that file, it can change future instructions", text)
        tui.send("y", settle=False)
        tui.wait_for("approved import: " + target)
        self.assertEqual(os.stat(os.path.join(base, "state", "maid", "trust-imports.json")).st_mode & 0o777, 0o600)
        again = start()
        self.assertNotIn("import from outside?", again.wait_for(STRIP))  # remembered: never asked again

    def test_the_session_stream_passes_the_protocol_check(self):
        # A turn with a tool call and its approval, a `!cmd`, the engine's `:` commands and a quit: the TUI's own
        # connection, recorded, passes `maid protocol check` (every case is checked the same way when it ends).
        tui = self.start()
        tui.send("ishell:printf 'whisker\\n'<esc>:w<cr>", settle=False)
        tui.wait_for("[y] yes")
        tui.send("y", settle=False)
        tui.wait_for("ran it", timeout=15)
        tui.send("i!printf 'paw-%s\\n' print<esc>:w<cr>", settle=False)
        tui.wait_for("paw-print")
        tui.send(":rename the fluffy tail<cr>")
        tui.wait_for("titled: the fluffy tail")
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        streams = [os.path.join(self.streams, f) for f in os.listdir(self.streams) if f.startswith("tui-")]
        self.assertEqual(len(streams), 1)
        r = subprocess.run([MAID, "protocol", "check", streams[0]], capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("events conform", r.stdout)
        with open(streams[0]) as f:
            # The recording also holds its header and skeleton lines (docs/design/engine-protocol.md section 17); those carry no msg.
            events = [m["msg"]["params"] for m in map(json.loads, f) if "msg" in m and m["msg"].get("method") == "maid.event"]
        types = [e.get("type") for e in events]
        for t in ("maid.input.added", "maid.approval.requested", "response.shell_call_output_content.delta", "maid.tool.output.delta",
                  "maid.session.title", "response.completed"):
            self.assertIn(t, types)
        self.assertEqual((types[-1], events[-1].get("state")), ("maid.session.state", "stopped"), "the quit stops the idle session (leave.quit.idle)")

    def recorded_states(self):
        """Each session's states in the index notifications the TUI recorded in this case ("stopped" for one removed),
        sessions in the order they were made (their ids sort so)."""
        states = {}
        for f in sorted(os.listdir(self.streams)):
            if not f.startswith("tui-"):
                continue
            with open(os.path.join(self.streams, f)) as fh:
                for m in map(json.loads, fh):
                    msg = m.get("msg", {})
                    if msg.get("method") != "maid.index":
                        continue
                    p = msg["params"]
                    if "removed" in p:
                        states.setdefault(p["removed"], []).append("stopped")
                    else:
                        states.setdefault(p["entry"]["id"], []).append(p["entry"]["state"])
        return [states[k] for k in sorted(states)]

    def test_flags_on_new_and_quit_override_the_leave_setting(self):
        tui = self.start()
        tui.send(":new --stop<cr>", settle=False)
        tui.wait_for("·  idle  ·")
        self.assertNotIn("other session", tui.text(), "--stop: the idle session left is stopped, not kept")
        tui.send(":new --bg<cr>", settle=False)
        tui.wait_for("1 other session (:switch)")
        tui.send(":q --park<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        first, second, third = self.recorded_states()
        self.assertEqual(first[-1], "stopped", "--stop on :new")
        self.assertIn("background", second, "--bg on :new keeps the idle session loaded")
        self.assertEqual(second[-1], "parked", "until the quit, which parks it (leave.no_daemon: no daemon keeps it)")
        self.assertEqual(third[-1], "parked", "--park on :q parks the idle session instead of stopping it")

    def test_a_leave_case_set_to_ask_asks(self):
        cfg = os.path.join(self.home, "config-ask")
        os.makedirs(os.path.join(cfg, "maid"), exist_ok=True)
        with open(os.path.join(cfg, "maid", "settings.lua"), "w") as f:
            f.write("local s = dofile(%r)\ns.leave = { switch = { idle = 'ask' }, quit = { idle = 'ask' } }\nreturn s\n" % os.path.join(self.env["XDG_CONFIG_HOME"], "maid", "settings.lua"))
        tui = self.start(env=dict(self.env, XDG_CONFIG_HOME=cfg))
        tui.send(":new<cr>", settle=False)
        tui.wait_for("leave this session?")
        tui.send("s", settle=False)
        tui.wait_for("·  idle  ·")
        self.assertNotIn("other session", tui.text(), "the answer, stop, ended the session left")
        tui.send(":q<cr>", settle=False)
        tui.wait_for("quit: this session?")
        tui.send("p", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        first, second = self.recorded_states()
        self.assertEqual(first[-1], "stopped")
        self.assertEqual(second[-1], "parked", "leave.quit.idle = 'ask': the answer, park, parks the idle session instead of stopping it")

    def test_new_parks_the_idle_session_and_switch_resumes_it(self):
        tui = self.start()
        tui.send("ifirst message<m-cr>", settle=False)
        tui.wait_for("▣ ")  # the turn is over: the session is idle when it is left
        tui.send("<esc>:new<cr>", settle=False)
        text = tui.wait_for("·  idle  ·")
        self.assertNotIn("echo: first message", text, "the view shows the new session")
        tui.send(":switch<cr>", settle=False)
        text = tui.wait_for("j/k move · Enter goes there · Esc stays")
        self.assertIn("+ a new session in " + self.ws, text)
        self.assertIn("·  parked  ·", text, "an idle session left without --bg is parked")
        tui.send("<cr>", settle=False)
        text = tui.wait_for("echo: first message")
        self.assertIn("❯ first message", text, "switching back resumes it with its history")
        tui.send(":fork<cr>", settle=False)
        tui.wait_for("echo: first message")
        tui.send("isecond<m-cr>", settle=False)
        tui.wait_for("echo: second")
        tui.wait_for("▣ ")
        tui.send("<esc>:switch<cr>", settle=False)
        text = tui.wait_for("j/k move · Enter goes there · Esc stays")
        self.assertEqual(text.count("·  parked  ·"), 2, "the fork left its parent parked, beside the first new session")
        tui.send("<esc>", settle=False)

    def test_a_working_session_goes_to_the_background_and_keeps_working(self):
        tui = self.start()
        tui.send("ihold on<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        tui.send("<esc>:new<cr>", settle=False)
        tui.wait_for("1 other session (:switch)")
        tui.send("iin parallel<m-cr>", settle=False)
        tui.wait_for("echo: in parallel")
        tui.send("<esc>:q<cr>", settle=False)
        tui.wait_for("1 other session is still working: quitting parks them")
        tui.send(":switch<cr>", settle=False)
        text = tui.wait_for("j/k move · Enter goes there · Esc stays")
        self.assertIn("·  working  ·", text)
        tui.send("<cr>", settle=False)
        text = tui.wait_for("holding")
        self.assertIn("❯ hold on", text, "back in the working session: its reply so far")
        tui.send("<c-c>", settle=False)
        tui.wait_for("interrupted")
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)

    def test_a_background_task_runs_beside_the_session_and_the_switcher_shows_it(self):
        tui = self.start()
        tui.send("ibg:explore:slow: look around<m-cr>", settle=False)
        tui.wait_for("the explore agent works in the background: slow: look around")
        tui.wait_for("ran it")  # the parent's turn ended while the task works
        tui.send("<esc>:switch<cr>", settle=False)
        text = tui.wait_for("j/k move · Enter goes there · Esc stays")
        self.assertIn("↳ explore: slow: look around  ·  working", text, "the task is a session of its own, under its parent")
        tui.send("<esc>", settle=False)
        tui.wait_for("the explore task finished", timeout=20)
        tui.send(":switch<cr>", settle=False)
        text = tui.wait_for("j/k move · Enter goes there · Esc stays")
        self.assertIn("↳ explore: slow: look around  ·  finished", text)
        tui.wait_for("↳ explore: slow: look around  ·  finished, parked")  # leave.task.after, once its job is done
        tui.send("<cr>", settle=False)  # the task is the first row after a new session
        text = tui.wait_for("echo: slow: look around")
        self.assertIn("❯ slow: look around", text, "switched into the task: its own conversation")
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)

    def test_a_session_in_the_daemon_keeps_working_after_quit(self):
        run = lambda *a: subprocess.run([MAID, *a], capture_output=True, text=True, env=self.env, cwd=self.ws, stdin=subprocess.DEVNULL, timeout=60)
        r = run("daemon", "start")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.addCleanup(lambda: run("daemon", "stop", "--yes"))
        tui = Tui([MAID], env=self.env, cwd=self.ws)  # no flag the daemon cannot take: the session opens there
        self.addCleanup(tui.close)
        text = tui.wait_for(STRIP)
        self.assertIn("in the daemon (maid daemon status)", text)
        self.assertIn("· daemon", text)
        tui.send("islow: still going<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        tui.send("<esc>:q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0, "quitting does not wait for the turn, and nothing is asked")
        sessions = lambda: json.loads(run("daemon", "status", "--json").stdout)["sessions"]
        end = time.time() + 30
        while time.time() < end and not any(e["state"] == "parked" and e["unseen"] for e in sessions()):
            time.sleep(0.2)
        mine = [e for e in sessions() if e["workspace"] == self.ws]
        self.assertTrue(mine and mine[0]["state"] == "parked" and mine[0]["unseen"], json.dumps(mine))  # leave.quit.after, once its work was done
        with open(mine[0]["transcript"]) as f:
            self.assertIn("echo: slow: still going", f.read(), "the turn finished in the daemon after the window closed")
        # The next maid in the same directory switches to it: the reply is there (wide: the strip counts the other one).
        tui = Tui([MAID], env=self.env, cwd=self.ws, cols=160)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        tui.send(":switch<cr>", settle=False)
        tui.wait_for("finished")
        tui.send("j<cr>", settle=False)
        tui.wait_for("echo: slow: still going")
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)

    def test_resume_by_id_opens_that_session_or_says_why_not(self):
        """maid -r ID of a session the daemon holds parked resumes exactly it, its conversation shown: from this
        directory, and from a directory deleted under the shell (in the session's workspace then, said so). An id
        that matches nothing is an error. None of them starts another transcript."""
        run = lambda *a: subprocess.run([MAID, *a], capture_output=True, text=True, env=self.env, cwd=self.ws, stdin=subprocess.DEVNULL, timeout=60)
        r = run("daemon", "start")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.addCleanup(lambda: run("daemon", "stop", "--yes"))
        c = cli_smoke.RpcClient(MAID, self.env, self.ws)
        c.call("maid.hello", {"protocol": 1, "client": {"name": "resume-test", "version": "0"}})
        sid = c.call("createConversation", {"maid": {"workspace": self.ws}})["result"]["id"]
        c.call("response.create", {"conversation": sid, "input": "ping"})
        c.wait(lambda m: m.get("method") == "maid.event" and m["params"].get("type") == "response.completed", 30)
        c.call("maid.session.leave", {"as": "park"})
        c.close()
        transcripts = lambda: {os.path.join(d, f) for top in (self.env["XDG_STATE_HOME"], self.env["XDG_RUNTIME_DIR"])
                               for d, _, fs in os.walk(os.path.join(top, "maid", "sessions")) for f in fs if f.endswith(".jsonl")}
        before = transcripts()

        tui = Tui([MAID, "-r", sid], env=self.env, cwd=self.ws)
        self.addCleanup(tui.close)
        text = tui.wait_for(STRIP)
        self.assertIn("resumed session " + sid + " in the daemon", text)
        self.assertIn("echo: ping", text, "the resumed session's conversation is on the screen")
        tui.send(":q --park<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        self.assertIn(sid + ".jsonl", tui.raw.decode("utf-8", "replace").split("resume it with:")[-1])

        # From a directory deleted under the shell: the session's own workspace, and a line that says so.
        gone = os.path.join(self.home, "gone")
        tui = Tui(["sh", "-c", 'mkdir "$1" && cd "$1" && rmdir "$1" && exec "$0" -r "$2"', MAID, gone, sid], env=self.env, cwd=self.home)
        self.addCleanup(tui.close)
        text = tui.wait_for(STRIP)
        self.assertIn("this shell's directory no longer exists", text)
        self.assertIn("resumed session " + sid + " in the daemon", text)
        self.assertIn("echo: ping", text)
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        self.assertIn(sid + ".jsonl", tui.raw.decode("utf-8", "replace").split("resume it with:")[-1])

        r = run("-r", "nosuchsession")
        self.assertTrue(r.returncode != 0 and "no session matching 'nosuchsession'" in r.stderr, r.stdout + r.stderr)
        self.assertEqual(transcripts(), before, "no resume started another transcript")

    def test_a_liaison_turn_shows_under_its_name_in_its_voice(self):
        run = lambda *a: subprocess.run([MAID, *a], capture_output=True, text=True, env=self.env, cwd=self.ws, stdin=subprocess.DEVNULL, timeout=60)
        r = run("daemon", "start")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.addCleanup(lambda: run("daemon", "stop", "--yes"))
        ws = os.path.join(self.home, "ws-voices")
        os.makedirs(ws, exist_ok=True)
        # The window's own settings give Claude a background; the daemon's stay as they are.
        cfg = os.path.join(self.home, "config-voices")
        shutil.copytree(self.env["XDG_CONFIG_HOME"], cfg, dirs_exist_ok=True)
        with open(os.path.join(cfg, "maid", "settings.lua")) as f:
            text = f.read()
        with open(os.path.join(cfg, "maid", "settings.lua"), "w") as f:
            f.write(text.replace("return {", "return { voices = { Claude = { bg = '#1e2a3a' } },", 1))
        tui = Tui([MAID], env=dict(self.env, XDG_CONFIG_HOME=cfg, COLORTERM="truecolor"), cwd=ws)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        sid = next(e["id"] for e in json.loads(run("daemon", "status", "--json").stdout)["sessions"] if e["workspace"] == ws)
        r = run("liaison", "send", sid, "--as", "Claude", "hello from claude")
        self.assertEqual(r.stdout, "echo: hello from claude\n", r.stderr)
        tui.wait_for("echo: hello from claude")
        lines = tui.screen.display
        y = next((i for i, l in enumerate(lines) if l.startswith("◆ Claude (liaison)")), -1)
        self.assertTrue(y >= 0 and lines[y + 1].startswith("  hello from claude"), tui.text())
        self.assertEqual(tui.screen.buffer[y + 1][2].bg, "1e2a3a", "Claude's voices entry paints its turn")
        self.assertNotIn("❯ hello from claude", tui.text())
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)

    def test_quit_prints_the_transcript_line(self):
        tui = self.start()
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        tail = tui.raw.decode("utf-8", "replace")
        self.assertIn("transcript (temporary; gone at logout): " + os.path.join(self.home, "run", "maid", "sessions"), tail)
        self.assertIn("resume it with: maid -r ", tail)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        MAID = os.path.abspath(sys.argv.pop(1))
    MAID = os.path.abspath(MAID)
    unittest.main()
