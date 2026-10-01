"""Terminal output: colours, the transient status line, and log lines printed above it."""

from __future__ import annotations

import sys
import threading
import time

DIM = "\033[2m"
BOLD = "\033[1m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
RED = "\033[31m"
OFF = "\033[0m"

# Marks a line whose utterance was processed in normal mode -- i.e. one that was
# able to modify the document. Normal mode then announces itself on every line,
# not only at the switch.
MODIFIABLE_MARK = "~"


class Status:
    """A transient one-line indicator that lives below the log.

    Printing the tap inline was the wrong shape: it arrived out of order against
    transcript lines, and during typing it filled the screen with taps that never
    mattered. A status line is the right shape -- it says "the tap landed, I am
    waiting for you" while that is true, and leaves nothing behind when it is not.
    """

    FRAMES = "\u280b\u2819\u2839\u2838\u283c\u2834\u2826\u2827\u2807\u280f"

    def __init__(self):
        self.text = ""
        self.until = 0.0
        self.on_expire = None
        self.lock = threading.RLock()
        self.tty = sys.stdout.isatty()
        threading.Thread(target=self._spin, daemon=True).start()

    def _render(self, frame: int = 0):
        if not self.tty or not self.text:
            return
        sys.stdout.write(f"\r\033[K  {CYAN}{self.FRAMES[frame % len(self.FRAMES)]}{OFF} "
                         f"{DIM}{self.text}{OFF}")
        sys.stdout.flush()

    def _spin(self):
        i = 0
        while True:
            time.sleep(0.09)
            with self.lock:
                if self.text and time.time() > self.until:
                    expired, self.on_expire = self.on_expire, None
                    self.clear()
                    if expired:
                        expired()
                elif self.text:
                    self._render(i)
            i += 1

    def set(self, text: str, seconds: float, on_expire=None):
        with self.lock:
            self.text, self.until = text, time.time() + seconds
            self.on_expire = on_expire
            self._render()

    def clear(self):
        with self.lock:
            self.on_expire = None
            if self.text and self.tty:
                sys.stdout.write("\r\033[K")
                sys.stdout.flush()
            self.text = ""

    def say(self, line: str, typeset: float = 0.0):
        """Print a log line above the status, restoring the status after."""
        with self.lock:
            # Hold on to the expiry callback too: clear() drops it, and a log
            # line arriving mid-wait would otherwise silently cancel the
            # failure state.
            held, until, expire = self.text, self.until, self.on_expire
            if held:
                self.clear()
            if typeset and self.tty:
                for ch in line:
                    sys.stdout.write(ch)
                    sys.stdout.flush()
                    time.sleep(typeset)
                sys.stdout.write("\n")
            else:
                sys.stdout.write(line + "\n")
            sys.stdout.flush()
            if held and time.time() < until:
                self.text, self.until, self.on_expire = held, until, expire
                self._render()


STATUS = Status()


def say(line: str, typeset: float = 0.0):
    STATUS.say(line, typeset)


def esc(s: str) -> str:
    return s.replace("\r", " ").strip()


def print_recap(rows: list[tuple[int, str]], tag: str, label: str):
    if not rows:
        say(f"  {tag}   {DIM}(nothing recorded yet){OFF}")
        return
    say(f"  {tag}   {BOLD}{label}{OFF}")
    width = len(str(rows[-1][0]))
    for i, step in rows:
        say(f"  {DIM}\u2502{OFF}     {CYAN}{i:>{width}}.{OFF} {step}")

