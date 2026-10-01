"""The document: numbered passages or steps mirrored to markdown, and the directives that change it."""

from __future__ import annotations

import re
import threading
from datetime import datetime
from pathlib import Path

from diction.ui import CYAN, DIM, GREEN, OFF, YELLOW, print_recap, say

NARRATION_HEADING = "## Verbatim narration"


class Procedure:
    """The numbered step list, mirrored to disk after every change."""

    def __init__(self, path: Path, title: str, raw: bool = False, mode: str = "transcript"):
        self.path = path
        self.title = title
        self.mode = mode
        self.steps: list[str] = []
        self.kinds: list[str] = []          # "content" | "command", parallel to steps
        self.narration: list[tuple[str, str, str]] = []   # (ts, said, outcome)
        self.raw = raw
        self.lock = threading.Lock()
        if path.exists():
            self._load()

    def _load(self):
        text = self.path.read_text(encoding="utf-8")
        head, _, tail = text.partition(NARRATION_HEADING)
        body = [l for l in head.splitlines()
                if not l.startswith("# ") and not l.startswith("_Dictated")]

        # A document already written as a numbered procedure stays one, whatever
        # mode was requested -- otherwise resuming would mangle it into prose.
        if any(re.match(r"^\s*\d+\.\s+\S", l) for l in body):
            self.mode = "steps"

        if self.mode == "steps":
            for line in body:
                m = re.match(r"^\s*(\d+)\.\s+(.*\S)\s*$", line)
                if m:
                    self.steps.append(m.group(2))
                    self.kinds.append("content")
        else:
            para: list[str] = []
            kind = "content"

            def close():
                if para:
                    self.steps.append(" ".join(para))
                    self.kinds.append(kind)

            for line in body:
                stripped = line.strip()
                if stripped:
                    if stripped.startswith(">"):
                        if para and kind != "command":
                            close(); para = []
                        kind = "command"
                        para.append(stripped.lstrip("> ").strip())
                    else:
                        if para and kind != "content":
                            close(); para = []
                            kind = "content"
                        kind = "content"
                        para.append(stripped)
                elif para:
                    close(); para = []; kind = "content"
            close()
        for line in tail.splitlines():
            m = re.match(r"^-\s+\*\*(\d+:\d+)\*\*\s+(.*?)\s*(?:_\((.*)\)_)?$", line)
            if m:
                self.narration.append((m.group(1), m.group(2), m.group(3) or ""))
        if self.narration:
            self.raw = True     # never silently discard narration already on disk
        m = re.search(r"^#\s+(.*\S)\s*$", text, re.M)
        if m:
            self.title = m.group(1)

    def _flush(self):
        stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
        body = [f"# {self.title}", "", f"_Dictated with diction - last updated {stamp}_", ""]
        if self.mode == "steps":
            body += [f"{i}. {s}" for i, s in enumerate(self.steps, 1)]
        else:
            for passage, kind in zip(self.steps, self.kinds):
                body += [f"> {passage}" if kind == "command" else passage, ""]
            if self.steps:
                body.pop()
        body.append("")
        if self.raw:
            body += [NARRATION_HEADING.strip(), "",
                     "_What was actually said, and what it became._", ""]
            for ts, said, outcome in self.narration:
                suffix = f" _({outcome})_" if outcome.strip() else ""
                body.append(f"- **{ts}** {said}{suffix}")
            body.append("")
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text("\n".join(body), encoding="utf-8")
        tmp.replace(self.path)

    def append(self, step: str, kind: str = "content") -> int:
        with self.lock:
            self.steps.append(step)
            self.kinds.append(kind)
            self._flush()
            return len(self.steps)

    def revise(self, n: int, step: str) -> bool:
        with self.lock:
            if not 1 <= n <= len(self.steps):
                return False
            self.steps[n - 1] = step
            self._flush()
            return True

    def drop(self, n: int) -> bool:
        with self.lock:
            if not 1 <= n <= len(self.steps):
                return False
            del self.steps[n - 1]
            del self.kinds[n - 1]
            self._flush()
            return True

    def detail(self, n: int, clause: str) -> bool:
        with self.lock:
            if not 1 <= n <= len(self.steps):
                return False
            cur = self.steps[n - 1].rstrip()
            if cur.endswith("."):
                cur = cur[:-1]
            self.steps[n - 1] = f"{cur} ({clause.rstrip('.')})."
            self._flush()
            return True

    def recap(self, n: int = 0) -> list[tuple[int, str]]:
        """The last n steps with their real numbers; n=0 means all."""
        with self.lock:
            start = 0 if n <= 0 else max(0, len(self.steps) - n)
            return [(i, self.steps[i - 1]) for i in range(start + 1, len(self.steps) + 1)]

    def one(self, n: int) -> list[tuple[int, str]]:
        """Just passage n, so "print row 4" means row 4 and not the last four."""
        with self.lock:
            if not 1 <= n <= len(self.steps):
                return []
            return [(n, self.steps[n - 1])]

    def narrate(self, ts: str, said: str, outcome: str):
        with self.lock:
            self.narration.append((ts, said, outcome))
            if self.raw:
                self._flush()

    @property
    def unit(self) -> str:
        return "step" if self.mode == "steps" else "passage"


def apply_reply(proc_doc: "Procedure", reply: str, echo=True, ts: str = "  :  ",
                said: str = "", allow_commands: bool = True,
                record_commands: bool = False) -> str:
    """Parse the agent's directive lines and mutate the procedure.

    Returns a short description of what the utterance became, for the verbatim
    narration section.

    The scribe runs behind the transcriber, so each directive is labelled with
    the timestamp of the utterance that produced it -- otherwise it prints
    under whatever line happens to be current and reads as a response to it.
    """
    tag = f"{DIM}\u2502 [{ts}]{OFF}"
    unit = proc_doc.unit
    outcomes: list[str] = []
    drops = []
    was_command = False
    refused = False
    for line in reply.splitlines():
        line = line.strip().lstrip("-* ").strip()
        if not line or line.upper().startswith("SKIP"):
            continue
        if m := re.match(r"^DROP\s+(\d+)$", line, re.I):
            if not allow_commands:
                refused = True
                continue
            was_command = True
            drops.append(int(m.group(1)))
            continue
        if m := re.match(r"^(SHOW|RECAP)\s+(\d+)$", line, re.I):
            if not allow_commands:
                refused = True
                continue
            was_command = True
            kind, n = m.group(1).upper(), int(m.group(2))
            if kind == "SHOW":
                # "print row 4" means row 4, not the last four.
                rows = proc_doc.one(n)
                label = f"{unit} {n}:" if rows else f"there is no {unit} {n}"
            else:
                rows = proc_doc.recap(n)
                label = (f"all {unit}s:" if n <= 0
                         else f"last {min(n, len(rows))} {unit}"
                              f"{'' if min(n, len(rows)) == 1 else 's'}:")
            if echo:
                print_recap(rows, tag, label)
            outcomes.append(f"printed {len(rows)} {unit}{'' if len(rows) == 1 else 's'}")
            continue
        if m := re.match(r"^WRITE:\s*(.+)$", line, re.I):
            if not allow_commands:
                refused = True
                continue
            was_command = True
            composed = m.group(1).strip()
            n = proc_doc.append(composed)
            outcomes.append(f"composed {unit} {n}")
            if echo:
                say(f"  {tag}   {GREEN}\u270e {n}. {composed}{OFF}")
            continue
        if m := re.match(r"^APPEND:\s*(.+)$", line, re.I):
            step = m.group(1).strip()
            n = proc_doc.append(step)
            outcomes.append(f"became {unit} {n}")
            if echo:
                say(f"  {tag}   {GREEN}\u2192 {n}. {step}{OFF}")
        elif m := re.match(r"^REVISE\s+(\d+):\s*(.+)$", line, re.I):
            if not allow_commands:
                refused = True
                continue
            n, step = int(m.group(1)), m.group(2).strip()
            was_command = True
            if proc_doc.revise(n, step):
                outcomes.append(f"revised {unit} {n}")
                if echo:
                    say(f"  {tag}   {CYAN}\u21ba {n}. {step}{OFF}")
        elif m := re.match(r"^DETAIL\s+(\d+):\s*(.+)$", line, re.I):
            if not allow_commands:
                refused = True
                continue
            n, clause = int(m.group(1)), m.group(2).strip()
            was_command = True
            if proc_doc.detail(n, clause):
                outcomes.append(f"detail on {unit} {n}")
                if echo:
                    say(f"  {tag}   {CYAN}+ {n}: {clause}{OFF}")

    for n in sorted(drops, reverse=True):   # highest first keeps indices valid
        if proc_doc.drop(n):
            outcomes.append(f"dropped {unit} {n}")
            if echo:
                say(f"  {tag}   {YELLOW}\u2717 dropped {unit} {n}{OFF}")

    # Insert mode is lossless by construction: it cannot mutate earlier passages,
    # and anything the model declined to write down -- because it read dictation
    # as a command, or skipped it as an aside -- is appended verbatim instead.
    if not allow_commands and not outcomes and said:
        n = proc_doc.append(said)
        why = "command ignored" if refused else "would have been skipped"
        outcomes.append(f"became {unit} {n} ({why}; kept verbatim)")
        if echo:
            say(f"  {tag}   {GREEN}\u2192 {n}. {said}{OFF} {DIM}(verbatim){OFF}")

    # Normal mode keeps the spoken command in the transcript alongside its effect.
    # Appended last, so a DROP above operated on the indices the agent saw.
    if was_command and record_commands and said:
        n = proc_doc.append(said, kind="command")
        outcomes.append(f"recorded command as {unit} {n}")
        if echo:
            say(f"  {tag}   {DIM}\u275d {n}. {said}{OFF}")

    if not outcomes:
        if echo:
            # Otherwise a skip is indistinguishable from the scribe still working.
            say(f"  {tag}   {DIM}\u00b7 skipped{OFF}")
        return "skipped"
    return ", ".join(outcomes)

