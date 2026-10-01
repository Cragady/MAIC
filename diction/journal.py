"""Session logs: the verbatim raw log, and the enriched turn-by-turn one."""

from __future__ import annotations

import collections
import threading
from datetime import datetime
from pathlib import Path

from diction.ui import MODIFIABLE_MARK

class Journal:
    """Two session files, both named for the document being dictated:

      <slug>-raw-<stamp>.log      verbatim, what was heard and nothing else
      <slug>-session-<stamp>.log  enriched -- operator turns, the agent's raw
                                  directives, and the effect each one had

    The scribe runs seconds behind the transcriber, so session entries are
    buffered and emitted a whole turn at a time, in the order spoken, rather
    than in the order the two threads happen to finish.
    """

    def __init__(self, logdir: Path, slug: str, stamp: str, meta: str,
                 expects_agent: bool = True, enrich: bool = False):
        self.raw_path = logdir / f"{slug}-raw-{stamp}.log"
        self.raw = self.raw_path.open("w", encoding="utf-8")
        self.expects_agent = expects_agent
        self.enrich = enrich
        self.session_path = (logdir / f"{slug}-session-{stamp}.log") if enrich else None
        self.session = self.session_path.open("w", encoding="utf-8") if enrich else None
        self.pending: collections.deque = collections.deque()
        self.lock = threading.Lock()
        if not enrich:
            return
        for line in (f"# diction session \u2014 {slug}",
                     f"# started {datetime.now():%Y-%m-%d %H:%M:%S}",
                     f"# {meta}", ""):
            self.session.write(line + "\n")
        self.session.flush()

    # -- emission -------------------------------------------------------
    def _emit(self, turn: dict):
        mark = turn.get("mark", " ")
        pad = " " * 9
        self.session.write(f"{mark}[{turn['ts']}] {turn['role']:<9} {turn['text']}\n")
        for label, body in turn["extra"]:
            for i, line in enumerate(body.splitlines() or [""]):
                self.session.write(f"{pad}{(label if i == 0 else ''):<9} {line.strip()}\n")
        self.session.flush()

    def _drain(self):
        while self.pending and self.pending[0]["done"]:
            self._emit(self.pending.popleft())

    def _raw(self, line: str):
        self.raw.write(line + "\n")
        self.raw.flush()

    # -- called by the transcriber --------------------------------------
    def heard(self, ts: str, text: str, mode: str = ""):
        with self.lock:
            mark = MODIFIABLE_MARK if mode == "normal" else " "
            self._raw(f"{mark}[{ts}] {text}")
            if not self.enrich:
                return
            turn = {"ts": ts, "role": "operator", "text": text, "mark": mark,
                    "extra": [], "done": not self.expects_agent}
            self.pending.append(turn)
            self._drain()

    def dropped(self, ts: str, text: str, why: str):
        with self.lock:
            self._raw(f" [{ts}] (dropped: {text})")
            if not self.enrich:
                return
            self.pending.append({"ts": ts, "role": "dropped", "text": f"{text!r} ({why})",
                                 "mark": " ", "extra": [], "done": True})

    def mode_switch(self, ts: str, text: str, new: str):
        mark = MODIFIABLE_MARK if new == "normal" else " "
        with self.lock:
            self._raw(f"{mark}[{ts}] (switched to {new} mode: {text})")
            if not self.enrich:
                return
            self.pending.append({"ts": ts, "role": "MODE", "text": f"\u21d2 {new} ({text})",
                                 "mark": mark, "extra": [], "done": True})
            self._drain()
            self._drain()

    # -- called by the scribe -------------------------------------------
    def agent(self, ts: str, reply: str):
        with self.lock:
            if not self.enrich:
                return
            for turn in self.pending:
                if not turn["done"] and not turn["extra"]:
                    turn["extra"].append(("agent", reply))
                    return

    def applied(self, ts: str, outcome: str):
        with self.lock:
            if not self.enrich:
                return
            for turn in self.pending:
                if not turn["done"]:
                    turn["extra"].append(("applied", outcome))
                    turn["done"] = True
                    break
            self._drain()

    def close(self):
        with self.lock:
            self.raw.close()
            if not self.enrich:
                return
            for turn in self.pending:
                turn["done"] = True
            self._drain()
            self.session.write(f"\n# ended {datetime.now():%Y-%m-%d %H:%M:%S}\n")
            self.session.close()

