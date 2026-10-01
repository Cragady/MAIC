#!/usr/bin/env python3
"""The TUI through a pty: maic as a person sees it, against the fake server from cli_smoke.py.

    python3 tests/test_tui.py [PATH_TO_MAIC] [-v]           (re-runs itself under `uv run --with pyte` if pyte is missing)

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
    sys.exit(subprocess.call([uv, "run", "--quiet", "--with", "pyte", "python", __file__] + sys.argv[1:],
                             env=dict(os.environ, MAIC_TUI_REEXEC="1", MAIC_BIN=os.path.abspath(MAIC))))

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
