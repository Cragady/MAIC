#!/usr/bin/env python3
"""Drives a terminal program through a pty and reads its screen back with pyte.

The program is forked under a pseudo-terminal of a fixed size; keys are written to it as the bytes a terminal
would send; the screen is a pyte emulator fed with everything the program writes, so a test can look at what a
person would see. Keys are given in a small notation, literal text plus these tokens:

    <esc> <cr> <tab> <s-tab> <bs> <space> <lt>        escape, Enter, Tab, Shift-Tab, Backspace, a space, a literal "<"
    <c-X>                                              Ctrl plus a letter (<c-w>, <c-p>, <c-z>, <c-c>)
    <m-X> <m-cr>                                       Alt plus a key (an ESC prefix): <m-cr> sends the input in maid
    <up> <down> <left> <right> <home> <end>            cursor keys
    <wheel-up> <wheel-down>                            SGR mouse wheel at the middle of the screen
    <wait>                                             let the screen settle before the next key

Standalone use (for poking at the UI by hand, each argument one key sequence):

    uv run --with pyte tests/tui_driver.py build/cli/maid -- 'ihello<esc>' ':help<cr>' ':q<cr>'
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

import pyte

KEYS = {
    "esc": b"\x1b", "cr": b"\r", "tab": b"\t", "s-tab": b"\x1b[Z", "bs": b"\x7f", "space": b" ", "lt": b"<",
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D", "home": b"\x1b[H", "end": b"\x1b[F",
}


def encode(sequence, cols=120, rows=40):
    """The bytes for one key sequence; a "<wait>" token becomes None in the list, meaning settle first."""
    out = []
    buf = b""
    i = 0
    while i < len(sequence):
        m = re.match(r"<([a-z0-9-]+)>", sequence[i:])
        if not m:
            buf += sequence[i].encode()
            i += 1
            continue
        name = m.group(1)
        i += m.end()
        if name == "wait":
            out.append(buf)
            out.append(None)
            buf = b""
        elif name in KEYS:
            buf += KEYS[name]
        elif re.fullmatch(r"c-[a-z]", name):
            buf += bytes([ord(name[2]) - 96])
        elif name.startswith("m-"):
            buf += b"\x1b" + KEYS.get(name[2:], name[2:].encode())
        elif name in ("wheel-up", "wheel-down"):
            buf += ("\x1b[<%d;%d;%dM" % (64 if name == "wheel-up" else 65, cols // 2, rows // 2)).encode()
        else:
            raise ValueError("unknown key <%s>" % name)
    out.append(buf)
    return out


class Tui:
    """The program runs under a tiny job-control shell: a stub in the pty (the session leader) forks it into its
    own process group as the foreground job, reports when it stops (Ctrl-Z) and resumes it on request, the way a
    shell's fg does. Forked straight into the pty the program would be an orphaned process group, and Linux
    discards the stop action of SIGTSTP for those, so Ctrl-Z could never be tested."""

    def __init__(self, argv, env=None, cwd=None, cols=120, rows=40):
        self.cols, self.rows = cols, rows
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = b""  # everything the program wrote, for text printed after the UI exits
        self.exit_status = None
        cmd_r, cmd_w = os.pipe()  # driver -> stub: b"c" resumes the job
        evt_r, evt_w = os.pipe()  # stub -> driver: b"S" stopped, b"X" exited
        e = dict(os.environ if env is None else env)
        e.setdefault("TERM", "xterm-256color")
        e["COLUMNS"], e["LINES"] = str(cols), str(rows)
        pid, fd = pty.fork()
        if pid == 0:
            os.close(cmd_w)
            os.close(evt_r)
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            self._stub(argv, e, cwd, cmd_r, evt_w)
        os.close(cmd_r)
        os.close(evt_w)
        self.pid, self.fd, self.cmd, self.evt = pid, fd, cmd_w, evt_r
        self.job = struct.unpack("i", os.read(evt_r, 4))[0]  # the program's pid, from the stub
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    @staticmethod
    def _stub(argv, env, cwd, cmd_r, evt_w):
        signal.signal(signal.SIGTTOU, signal.SIG_IGN)  # tcsetpgrp from the background would stop the stub itself
        child = os.fork()
        if child == 0:
            os.setpgid(0, 0)
            # A test runner often ignores job-control signals, and an ignored disposition survives exec.
            for sig in (signal.SIGTSTP, signal.SIGTTIN, signal.SIGTTOU, signal.SIGINT, signal.SIGQUIT, signal.SIGPIPE):
                signal.signal(sig, signal.SIG_DFL)
            if cwd:
                os.chdir(cwd)
            try:
                os.execvpe(argv[0], argv, env)
            finally:
                os._exit(127)
        try:
            os.setpgid(child, child)
        except OSError:
            pass  # the child got there first
        os.tcsetpgrp(0, child)
        os.write(evt_w, struct.pack("i", child))
        while True:
            _, status = os.waitpid(child, os.WUNTRACED)
            if os.WIFSTOPPED(status):
                os.write(evt_w, b"S")
                if os.read(cmd_r, 1) != b"c":
                    os.kill(child, signal.SIGKILL)
                    os._exit(1)
                os.tcsetpgrp(0, child)
                os.kill(child, signal.SIGCONT)
                continue
            os.write(evt_w, b"X")
            os._exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status))

    def pump(self, timeout):
        """Feeds the screen with what arrives within `timeout` seconds; True when something did."""
        got = False
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return got
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                return got
            try:
                data = os.read(self.fd, 65536)
            except OSError:  # the slave side closed: the program is gone
                return got
            if not data:
                return got
            got = True
            self.raw += data
            self.stream.feed(data)

    def settle(self, quiet=0.25, timeout=10.0):
        """Waits until nothing has arrived for `quiet` seconds (or `timeout` in all)."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if not self.pump(quiet):
                return
        raise TimeoutError("the screen kept changing for %.0f s" % timeout)

    def send(self, sequence, settle=True):
        for part in encode(sequence, self.cols, self.rows):
            if part is None:
                self.settle()
            elif part:
                os.write(self.fd, part)
        if settle:
            self.settle()

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display).rstrip("\n")

    def wait_for(self, needle, timeout=10.0):
        """Pumps until `needle` is on the screen; the screen text, or TimeoutError with the screen in it."""
        end = time.monotonic() + timeout
        while True:
            if needle in self.text():
                return self.text()
            if time.monotonic() >= end:
                raise TimeoutError("never saw %r on the screen:\n%s" % (needle, self.text()))
            self.pump(min(0.2, max(0.0, end - time.monotonic())))

    def suspend(self, timeout=5.0):
        """Ctrl-Z through the line discipline; returns once the stub reports the program stopped."""
        os.write(self.fd, b"\x1a")
        self._event(b"S", timeout, "the program did not stop on Ctrl-Z")

    def resume(self):
        """Like the shell's fg: the program gets the terminal back and SIGCONT; pumps its redraw."""
        self.screen.reset()
        os.write(self.cmd, b"c")
        self.settle()

    def wait_exit(self, timeout=10.0):
        """Waits for the program to exit, draining its last output; returns the exit status."""
        self._event(b"X", timeout, "the program did not exit")
        self.pump(0.3)
        _, status = os.waitpid(self.pid, 0)
        self.exit_status = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
        return self.exit_status

    def _event(self, want, timeout, what):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump(0.05)
            r, _, _ = select.select([self.evt], [], [], 0)
            if r:
                got = os.read(self.evt, 1)
                if got == want:
                    return
                raise RuntimeError("%s: the job %s instead" % (what, "exited" if got == b"X" else "stopped"))
        raise TimeoutError(what + ":\n" + self.text())

    def close(self):
        if self.exit_status is None:
            for target, kill in ((self.job, os.killpg), (self.pid, os.kill)):
                try:
                    kill(target, signal.SIGKILL)
                except OSError:
                    pass
            try:
                os.waitpid(self.pid, 0)
            except OSError:
                pass
        for fd in (self.fd, self.cmd, self.evt):
            os.close(fd)


def main():
    args = sys.argv[1:]
    if "--" not in args:
        sys.exit(__doc__)
    argv, keys = args[: args.index("--")], args[args.index("--") + 1 :]
    tui = Tui(argv)
    tui.settle()
    print(tui.text())
    for k in keys:
        print("\n=== %s" % k)
        tui.send(k)
        print(tui.text())
    tui.close()


if __name__ == "__main__":
    main()
