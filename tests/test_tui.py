#!/usr/bin/env python3
"""The TUI through a pty: maic as a person sees it, against the fake server from cli_smoke.py.

    python3 tests/test_tui.py [PATH_TO_MAIC] [-v]           (re-runs itself under `uv run --offline --with pyte` if pyte is missing)

Exit 77 when neither pyte nor uv is available, which ctest reports as a skip. Every case starts its own maic in
a throwaway home (XDG_* under a temp dir, --no-record, the dumb harness, no instruction files), 120x40."""
import os, shutil, subprocess, sys, tempfile, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIC = os.environ.get("MAIC_BIN", os.path.join(HERE, "..", "build", "cli", "maic"))

try:
    import pyte  # noqa: F401
except ImportError:
    if os.environ.get("MAIC_TUI_REEXEC"):
        print("skipped: pyte is not importable even under uv")
        sys.exit(77)
    uv = shutil.which("uv")
    if not uv:
        print("skipped: the TUI test needs pyte (pip install pyte, or uv on PATH to fetch it)")
        sys.exit(77)
    # uv asks PyPI about pyte on most runs; the gate stays offline unless MAIC_NETWORK_TESTS=1 (docs/testing.md).
    run = [uv, "run", "--quiet"] + ([] if os.environ.get("MAIC_NETWORK_TESTS") == "1" else ["--offline"]) + ["--with", "pyte", "python"]
    if subprocess.call(run + ["-c", "import pyte"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        print("skipped: pyte is not in uv's cache and the network is off (MAIC_NETWORK_TESTS=1 lets uv fetch it)")
        sys.exit(77)
    sys.exit(subprocess.call(run + [__file__] + sys.argv[1:], env=dict(os.environ, MAIC_TUI_REEXEC="1", MAIC_BIN=os.path.abspath(MAIC))))

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

    @classmethod
    def tearDownClass(cls):
        cls.fake.kill()
        cls.fake.wait()
        shutil.rmtree(cls.home, ignore_errors=True)

    def start(self, *args, env=None):
        tui = Tui([MAIC, "--no-record", "--harness", "dumb", "--no-instructions", *args], env=env or self.env, cwd=self.ws)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        return tui

    def test_welcome_shows_the_status_strip(self):
        tui = self.start()
        text = tui.text()
        self.assertIn("MAIC  ·  workspace " + self.ws, text)
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
        self.assertIn("settings  " + os.path.join(self.home, "config", "maic", "settings.lua"), text)

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
        # maic.nvim's fallback when MAIC is not connected: the text arrives as a bracketed paste, in normal mode too.
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
        sock = os.path.join(tempfile.mkdtemp(prefix="maic-tui-nvim-", dir=self.home), "nvim.sock")
        plugin = os.path.join(HERE, "..", "maic.nvim")
        host = subprocess.Popen([nvim, "--headless", "-u", "NONE", "-i", "NONE", "-n", "--listen", sock, "--cmd", "set rtp+=" + plugin],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (host.kill(), host.wait()))
        for _ in range(500):
            if os.path.exists(sock):
                break
            time.sleep(0.02)

        def remote(expr):
            return subprocess.run([nvim, "--headless", "--server", sock, "--remote-expr", expr], capture_output=True, text=True, timeout=10).stdout.strip()

        # Not an ancestor of maic: refused, with the reason on screen.
        refused = self.start(env=dict(self.env, NVIM=sock))
        self.assertIn("nvim: not connecting to $NVIM", refused.text())
        self.assertNotIn(" nvim ·", refused.text())
        # The test-only pair of variables lets it connect.
        tui = self.start(env=dict(self.env, NVIM=sock, MAIC_TESTING="1", MAIC_NVIM_TRUST_SOCKET="1"))
        tui.wait_for("nvim: connected to the nvim MAIC runs in")
        self.assertIn(" nvim ·", tui.text())  # the status strip
        remote("luaeval('require(\"maic\").send_text(\"from nvim\")')")
        tui.wait_for("│ from nvim")
        self.assertNotIn("echo: from nvim", tui.text())  # never sent by itself
        with open(os.path.join(self.ws, "notes.txt"), "w") as f:
            f.write("hello\n")
        tui.send("<esc>:e notes.txt<cr>")
        self.assertEqual(remote("expand('%:t')"), "notes.txt")
        remote("luaeval('require(\"maic\").command(\":rename from the plugin\")')")
        tui.wait_for("titled: from the plugin")
        # :MaicInterrupt does what the first Ctrl-C does to a running turn; idle, MAIC says so and keeps the draft.
        tui.send("<esc><c-c>")  # the first Ctrl-C of an idle MAIC clears the input
        tui.send("ihold on<m-cr>", settle=False)
        tui.wait_for("working… ctrl-c interrupts")
        self.assertEqual(remote("luaeval('require(\"maic\").interrupt()')"), "rpc")
        tui.wait_for("interrupted")
        tui.send("<esc>ia draft<esc>")
        remote("luaeval('require(\"maic\").interrupt()')")
        text = tui.wait_for("nvim: nothing to interrupt (MAIC is idle)")
        self.assertIn("│ a draft", text)

    def test_keymap_check_after_a_lazy_lock_change(self):
        # A changed lazy-lock.json makes MAIC run the keymap check once in the background (headless nvim with this
        # home's own nvim config) and report the collisions the last run did not have.
        if not shutil.which("nvim"):
            self.skipTest("nvim is not on PATH")
        cfg = tempfile.mkdtemp(prefix="maic-tui-keys-", dir=self.home)
        shutil.copytree(os.path.join(self.home, "config", "maic"), os.path.join(cfg, "maic"))
        os.makedirs(os.path.join(cfg, "nvim"))
        init, lock = os.path.join(cfg, "nvim", "init.lua"), os.path.join(cfg, "nvim", "lazy-lock.json")
        with open(init, "w") as f:
            f.write("vim.keymap.set('t', '<C-w>', '<C-\\\\><C-n><C-w>', { desc = 'window from a terminal' })\n")
        with open(lock, "w") as f:
            f.write('{ "lazy.nvim": { "branch": "main", "commit": "1111111" } }\n')
        env = dict(self.env, XDG_CONFIG_HOME=cfg, XDG_STATE_HOME=tempfile.mkdtemp(prefix="maic-tui-state-", dir=self.home))
        first = self.start(env=env)
        text = first.wait_for("nvim keymaps, checked for the first time: 1 collide: maic nvim keymaps", timeout=60)
        self.assertIn("MAIC never gets <C-w>", text)
        with open(init, "a") as f:
            f.write("vim.keymap.set('t', '<C-p>', '<C-\\\\><C-n>p', { desc = 'paste from a terminal' })\n")
        with open(lock, "w") as f:
            f.write('{ "lazy.nvim": { "branch": "main", "commit": "2222222" } }\n')
        second = self.start(env=env)
        text = second.wait_for("a plugin update added keymaps that collide: maic nvim keymaps", timeout=60)
        self.assertIn("MAIC never gets <C-p>", text)
        self.assertNotIn("MAIC never gets <C-w>", text)  # known since the last run
        third = self.start(env=env)  # the same lock file: no second run
        time.sleep(3)
        third.settle()
        self.assertNotIn("collide", third.text())

    def test_bare_shows_in_the_strip(self):
        tui = self.start("--bare")
        self.assertIn(" bare · harness armed", tui.text())
        tui.send(":theme nvim:habamax<cr>")
        self.assertIn("this MAIC is bare", tui.text())
        tui.send(":nvim<cr>")
        self.assertIn("nvim: bare (--bare, MAIC_BARE=1 or bare = true)", tui.text())
        plain = self.start(env=dict(self.env, MAIC_BARE="1"))
        self.assertIn(" bare · harness armed", plain.text())
        self.assertNotIn(" bare ·", self.start().text())

    def test_cd_moves_the_workspace(self):
        sub = os.path.join(self.ws, "cd-sub")
        os.makedirs(os.path.join(sub, ".maic"), exist_ok=True)
        with open(os.path.join(sub, ".maic", "settings.lua"), "w") as f:
            f.write("return { timestamps = true }\n")
        tui = self.start()
        tui.send(":cd cd-sub<cr>")
        text = tui.wait_for(" trust this directory? ")  # a project directory: :cd asks as a start there would
        self.assertIn("settings:     .maic/settings.lua", text)
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
        tui = Tui([MAIC, "--harness", "dumb", "--no-instructions"], env=self.env, cwd=proj)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        tui.send(":init<cr>", settle=False)
        encoded = proj.replace("/", "-").replace(" ", "-")
        text = tui.wait_for("this session moved to projects/")
        self.assertIn("(it worked here throughout)", text.replace("\n", " "))
        home = os.path.join(self.home, "state", "maic", "sessions", "projects", encoded)
        moved = [f for f in os.listdir(home) if f.endswith(".jsonl")]
        self.assertEqual(len(moved), 1)
        self.assertEqual([f for f in os.listdir(home) if not f.endswith(".jsonl")], [])  # no .pending or .moving left
        tui.wait_for("echo: Look over this project")  # the MAIC.md prompt still runs, into the moved file
        with open(os.path.join(home, moved[0])) as f:
            self.assertIn('"type":"rehomed"', f.read().replace(" ", ""))

    def test_init_leaves_an_unrecorded_session(self):
        ws = tempfile.mkdtemp(prefix="maic-tui-init-", dir=self.home)  # its own: :init makes it a project directory
        tui = Tui([MAIC, "--no-record", "--harness", "dumb", "--no-instructions"], env=self.env, cwd=ws)
        self.addCleanup(tui.close)
        tui.wait_for(STRIP)
        tui.send(":init<cr>")
        tui.wait_for("this session stays out of the project's home: it is not recorded")
        tui.wait_for("is not trusted, so its MAIC.md")

    def test_cd_into_a_project_directory_not_now(self):
        sub = os.path.join(self.ws, "cd-untrusted")
        os.makedirs(os.path.join(sub, ".maic"), exist_ok=True)
        with open(os.path.join(sub, ".maic", "settings.lua"), "w") as f:
            f.write("return { timestamps = true }\n")
        tui = self.start()
        tui.send(":cd cd-untrusted<cr>")
        tui.wait_for(" trust this directory? ")
        tui.send("n")
        text = tui.wait_for("workspace: " + sub)
        self.assertIn("not now: " + sub + " is untrusted for this session", text)
        self.assertIn("settings: unchanged", text)  # its settings.lua was not read


    def trust_case(self, settings):
        """A workspace with a project settings file, a state directory of its own, and maic started there (not
        waiting for the screen: the trust prompt comes first)."""
        ws = tempfile.mkdtemp(prefix="maic-tui-trust-", dir=self.home)
        os.makedirs(os.path.join(ws, ".maic"))
        with open(os.path.join(ws, ".maic", "settings.lua"), "w") as f:
            f.write(settings)
        env = dict(self.env, XDG_STATE_HOME=tempfile.mkdtemp(prefix="maic-tui-state-", dir=self.home))

        def start():
            tui = Tui([MAIC, "--no-record", "--harness", "dumb", "--no-instructions"], env=env, cwd=ws)
            self.addCleanup(tui.close)
            return tui
        return ws, start

    def test_trust_prompt_then_trusted(self):
        ws, start = self.trust_case("return { timestamps = true }\n")
        tui = start()
        text = tui.wait_for("[t] trust fully")
        self.assertIn("a project directory you have not trusted", text)
        self.assertIn(ws, text)
        self.assertIn("settings:     .maic/settings.lua", text)
        self.assertIn("tier standard;", text)
        self.assertIn("[s] trust sandboxed: its Lua runs in a child process that cannot reach the system", text)

        tui.send("t<cr>", settle=False)
        tui.wait_for(STRIP)
        tui.send(":settings<cr>")
        self.assertIn(os.path.join(ws, ".maic", "settings.lua"), tui.text())  # applied
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
        self.assertNotIn(os.path.join(ws, ".maic", "settings.lua"), tui.text())  # not applied
        tui.send(":trust<cr>")
        self.assertIn("trusted " + ws, tui.wait_for("trusted " + ws))
        again = start()
        self.assertNotIn("[t] trust fully", again.wait_for(STRIP))

    def test_own_import_from_outside_is_asked_once(self):
        # A config of its own: instruction files on, and a MAIC.md that imports a file outside every trusted directory.
        base = os.path.join(self.home, "own-import")
        cfg = os.path.join(base, "config", "maic")
        os.makedirs(cfg)
        os.makedirs(os.path.join(base, "outside"))
        target = os.path.join(base, "outside", "style.md")
        with open(target, "w") as f:
            f.write("write like a heron\n")
        with open(os.path.join(self.home, "config", "maic", "settings.lua")) as f:
            settings = f.read().replace("load_instructions = false", "load_instructions = true")
        with open(os.path.join(cfg, "settings.lua"), "w") as f:
            f.write(settings)
        with open(os.path.join(cfg, "MAIC.md"), "w") as f:
            f.write("Follow @%s please.\n" % target)
        env = dict(self.env, XDG_CONFIG_HOME=os.path.join(base, "config"), XDG_STATE_HOME=os.path.join(base, "state"))

        def start():
            tui = Tui([MAIC, "--no-record", "--harness", "dumb"], env=env, cwd=self.ws)
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
        self.assertEqual(os.stat(os.path.join(base, "state", "maic", "trust-imports.json")).st_mode & 0o777, 0o600)
        again = start()
        self.assertNotIn("import from outside?", again.wait_for(STRIP))  # remembered: never asked again

    def test_quit_prints_the_transcript_line(self):
        tui = self.start()
        tui.send(":q<cr>", settle=False)
        self.assertEqual(tui.wait_exit(), 0)
        tail = tui.raw.decode("utf-8", "replace")
        self.assertIn("transcript (temporary; gone at logout): " + os.path.join(self.home, "run", "maic", "sessions"), tail)
        self.assertIn("resume it with: maic -r ", tail)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        MAIC = os.path.abspath(sys.argv.pop(1))
    MAIC = os.path.abspath(MAIC)
    unittest.main()
