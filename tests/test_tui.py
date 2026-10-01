#!/usr/bin/env python3
"""The TUI through a pty: maic as a person sees it, against the fake server from cli_smoke.py.

    python3 tests/test_tui.py [PATH_TO_MAIC] [-v]           (re-runs itself under `uv run --with pyte` if pyte is missing)

Exit 77 when neither pyte nor uv is available, which ctest reports as a skip. Every case starts its own maic in
a throwaway home (XDG_* under a temp dir, --no-record, the dumb harness, no instruction files), 120x40."""
import os, shutil, subprocess, sys, unittest

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

    def start(self, *args):
        tui = Tui([MAIC, "--no-record", "--harness", "dumb", "--no-instructions", *args], env=self.env, cwd=self.ws)
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
