"""The scribe: a chat model that turns each utterance into directives for the document.

Three backends, one conversation per scribe key in each, with the same system prompts and per-utterance messages:

* claude-cli, ClaudeScribe: a persistent headless `claude` process per mode, on the user's own login, as diction
  always ran. Ported from the original diction.py.
* api and local, Scribe: a chat client (stdlib urllib) for a provider `maic model resolve NAME` names, Anthropic's
  Messages API or an OpenAI-compatible server such as llama-server on loopback. A local window is small, so each
  request carries the system prompt, the document, and only as many earlier exchanges as fit (see build_messages).
"""

from __future__ import annotations

import json
import os
import queue
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime

from pathlib import Path

from diction.document import Procedure, apply_reply
from diction.ui import DIM, OFF, RED, esc, say
from diction.whisper import maic_bin

# A neutral working directory for the claude process, so it picks up no project's instructions. The original kept
# it beside diction.py; an installed copy's directory may not be writable, so it lives in the state directory.
AGENT_CWD = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state") / "maic" / "diction" / "agent-cwd"
# How many of the newest passages each claude-cli message carries, as the original's Procedure.context().
CLAUDE_CONTEXT = 25

SYSTEM_PROMPT_INSERT = """You turn spoken narration into a clean written transcript.

You are given the transcript so far (numbered passages, possibly empty) and ONE
freshly transcribed chunk of the narrator's speech.

EVERY chunk is content. There are no commands, and you have no directive other
than APPEND. Even if a chunk sounds like an instruction -- "correction", "delete
that", "scratch that", "read that back" -- it is dictation and gets written down
as spoken. The narrator chose this mode precisely so that nothing they say is
acted on.

Write the chunk out as clean prose: drop disfluencies ("um", "uh", false starts,
repeated words), repair obvious mistranscriptions, add punctuation and
capitalisation. One thing is never a mistranscription: the word
"diction" is this tool's name, and must be written exactly as heard -- never
"dictation", never "diction" corrected to anything else. Keep the narrator's own words, meaning, register and domain
vocabulary. Never summarise, never rewrite into instructions, never add anything
they did not say.

Reply with exactly one line and nothing else:

APPEND: <cleaned prose>

Never SKIP anything intelligible; SKIP is only for a chunk that is not words at
all. Skipped speech is appended verbatim and uncleaned anyway, which is a worse
result than you writing it out properly.
"""

SYSTEM_PROMPT_NORMAL = """You turn spoken narration into a clean written transcript.

You are given the transcript so far (numbered passages, possibly empty) and ONE
freshly transcribed chunk of the narrator's speech.

By default a chunk is CONTENT. Write it out as clean prose: drop disfluencies
("um", "uh", false starts, repeated words), repair obvious mistranscriptions,
add punctuation and capitalisation. Keep the narrator's own words, meaning,
register and domain vocabulary. Never summarise, never rewrite into
instructions, never add anything they did not say.

A chunk is a COMMAND when either of these holds.

1. It opens with one of:

    "correction ..."     "diction, ..."      "scratch that"
    "strike that"        "delete that"       "edit that"
    "read that back"     "what do I have so far"

2. It names a passage by number -- "row 12", "line 12", "passage 12", "number
   twelve" -- and asks for something to be done to it. "Change row 22 to be
   verbatim", "delete row 24", "make row 24 say what row 22 asks for", "follow
   the instruction on row 33" are all commands. This form is how the narrator
   most often speaks, and it does not depend on any opening marker.

Reply with one directive per line and nothing else. No preamble, no markdown.

APPEND: <cleaned prose>
    Content. One passage. Usually you emit exactly one of these.

REVISE <n>: <replacement>
    A command to change passage <n>. Emit the complete replacement text. If no
    passage is named, they mean the most recent one.

DROP <n>
    A command to remove passage <n>.

SHOW <n>
    Print passage <n> itself -- "print row 4", "show me line 12", "what does
    row 4 say". Prints only; changes nothing.

RECAP <n>
    Print the last <n> passages -- "read me back the last three", "what do I
    have so far" (`RECAP 0` for the whole document). Prints only.

WRITE: <text>
    The narrator explicitly asked you to compose something -- "generate a fun
    response to line 4", "summarise rows 1 to 5", "draft a reply to that".
    Write it; it becomes a new passage. This is the ONLY case where words that
    are yours rather than theirs may enter the document, and it always takes an
    explicit request. Never volunteer it.

SKIP
    Transcription noise, or an aside to someone else in the room.

Rules:
- Classify every chunk on its own. You are in a running session and can see how
  you handled earlier chunks; ignore that. A chunk naming a passage number and
  an action is a command even if a garbled version of the same sentence came
  through earlier and you wrote it down as content. Do not follow your own
  precedent.
- When unsure whether a chunk is a command or content, treat it as CONTENT.
  A stray sentence is trivial to correct; silently eating dictation is not.
- A chunk that is only a mode-switch phrase -- "diction normal", "diction
  insert", "diction toggle", or a garbled version of one -- is not a document
  command. Mode switching is handled outside you. Emit SKIP for it. Never turn
  it into a REVISE or a DROP; there is nothing in the document to change.
- Never invent content unless explicitly asked to compose it, which is what
  WRITE is for. Never merge a command's wording into a passage.
- When a chunk IS a command, emit only its directive. The command's own words
  are recorded in the transcript separately and automatically -- do not also
  APPEND them yourself, or they will appear twice.
"""

SYSTEM_PROMPT_STEPS = """You turn spoken narration into a written procedure.

You are given the procedure so far (numbered, possibly empty) and ONE freshly
transcribed chunk of the narrator's speech. The narrator is performing a task
and describing it out loud as they go.

Reply with one directive per line and nothing else. No preamble, no markdown,
no commentary. Usually you emit exactly one line.

APPEND: <step>
    The chunk describes a new action. Write it as a terse imperative
    instruction, e.g. "Open the invoice screen." Strip filler, self-talk and
    disfluencies. One sentence, ending with a period. If the chunk plainly
    contains two distinct actions, emit two APPEND lines.

REVISE <n>: <replacement>
    The narrator is correcting or amending step <n> -- "no wait", "actually",
    "scratch that", "I meant". Emit the complete replacement text for it.

DETAIL <n>: <clause>
    The chunk qualifies step <n> without replacing it (a caveat, a value, a
    gotcha). Emit just the clause to attach.

DROP <n>
    The narrator wants step <n> removed -- "delete that step", "scratch step
    four", "that last one was not part of it". Emit one DROP line per step;
    they are applied highest-first so the numbering stays stable.

SHOW <n>
    The narrator wants the last <n> steps read back in full -- "read me back
    the last three steps", "what do I have so far", "recap the last two".
    Emit `SHOW <n>` on its own; it prints them and changes nothing. Use
    `SHOW 0` for the whole procedure.

SKIP
    Filler, throat-clearing, an aside to someone else, transcription noise, a
    remark about the recording itself, or anything that adds no procedural
    content.

Rules:
- Never invent an action the narrator did not describe.
- Keep the narrator's domain vocabulary; do not translate their terms.
- When genuinely unsure, SKIP.
"""


# A reply is one directive line, rarely a few; WRITE can be a paragraph.
REPLY_TOKENS = 1024
# Earlier exchanges kept per conversation, newest first, while they fit. The rules tell the model to classify each
# chunk on its own, so history is continuity ("scratch that" after a passage), not instruction.
HISTORY_EXCHANGES = 8


def tokens(text: str) -> int:
    """A deliberately high estimate: about three characters a token, plus a message's framing."""
    return len(text) // 3 + 4


def document_block(rows: list[tuple[int, str]], unit: str, budget: int) -> str:
    """The document as numbered lines, with their real numbers, so "row 12" means row 12. When it alone outgrows
    the window the oldest passages are left out, and the block says which."""
    if not rows:
        return "(empty - no steps recorded yet)"
    kept: list[str] = []
    used = 0
    for i, text in reversed(rows):
        line = f"{i}. {text}"
        if kept and used + tokens(line) > budget:
            break
        kept.append(line)
        used += tokens(line)
    kept.reverse()
    omitted = len(rows) - len(kept)
    if omitted:
        kept.insert(0, f"({unit}s 1-{omitted} are not shown: the document is longer than the scribe's window)")
    return "\n".join(kept)


def build_messages(system: str, rows: list[tuple[int, str]], unit: str, text: str,
                   history: list[tuple[str, str]], context: int) -> list[dict]:
    """The request for one utterance within `context` tokens.

    Always: the system prompt, and last the utterance with the whole document, in the message diction always sent.
    Then the most recent earlier exchanges, newest first, while they fit and up to HISTORY_EXCHANGES; the oldest go
    first. An earlier turn carries only its utterance: the document it saw is stale, and the current one is here."""
    budget = context - REPLY_TOKENS - tokens(system) - tokens(text) - 64
    doc = document_block(rows, unit, budget)
    payload = (f"<steps>\n{doc}\n</steps>\n\n"
               f"<utterance>\n{text}\n</utterance>")
    left = budget - tokens(doc)
    turns: list[dict] = []
    for said, reply in reversed(history[-HISTORY_EXCHANGES:]):
        cost = tokens(said) + tokens(reply) + 8
        if cost > left:
            break
        turns[:0] = [{"role": "user", "content": f"<utterance>\n{said}\n</utterance>"},
                     {"role": "assistant", "content": reply}]
        left -= cost
    return [{"role": "system", "content": system}, *turns, {"role": "user", "content": payload}]


def server_answers(base_url: str) -> bool:
    """Whether anything answers HTTP at the host of `base_url` (GET /health, any status)."""
    root = base_url.split("/v1")[0]
    try:
        urllib.request.urlopen(root + "/health", timeout=1).close()
    except urllib.error.HTTPError:
        return True
    except (urllib.error.URLError, OSError):
        return False
    return True


# The `claude` CLI's aliases, which --agent-model took when the scribe was a claude process.
LEGACY_SCRIBES = {"haiku": "haiku-4.5", "sonnet": "sonnet-5", "opus": "opus-5.5"}


def resolve_agent(name: str) -> dict:
    """--agent-model through `maic model resolve`: {provider, kind, base_url, model, context, remote, ...}.
    The legacy aliases haiku, sonnet and opus mean the MAIC presets in LEGACY_SCRIBES.

    A name without a provider (a preset such as the default qwen-4b) that lands on the main llama server goes to the
    side server instead whenever that one answers, so dictation never evicts the model a session is using.
    `llamacpp/MODEL` written out stays on the main server."""
    exe = maic_bin()
    if not exe:
        raise RuntimeError("--agent-model is resolved by `maic model resolve`, and no maic was found (MAIC_BIN or PATH)")

    def resolve(n: str) -> dict:
        r = subprocess.run([exe, "model", "resolve", n], capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            raise RuntimeError((r.stderr.strip() or r.stdout.strip()).removeprefix("maic: "))
        return json.loads(r.stdout)

    name = LEGACY_SCRIBES.get(name.lower(), name)
    spec = resolve(name)
    if "/" not in name and spec["provider"] == "llamacpp":
        side = resolve(f"llamacpp-2/{spec['model']}")
        if server_answers(side["base_url"]):
            return side
    return spec


def api_key(agent: dict) -> str:
    """The key a remote provider needs, from its environment variable or its command; "" for a local one."""
    if agent.get("api_key_env") and os.environ.get(agent["api_key_env"]):
        return os.environ[agent["api_key_env"]]
    if agent.get("api_key_command"):
        r = subprocess.run(agent["api_key_command"], shell=True, capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    if agent.get("remote"):
        raise RuntimeError(f"{agent['provider']} needs a key: set {agent.get('api_key_env') or 'its api_key_env'}")
    return ""


class ClaudeScribe:
    """A long-lived headless `claude` session that turns utterances into
    directives. Kept persistent so we pay CLI startup once, not per utterance."""

    def __init__(self, model: str, timeout: int, errlog=None,
                 prompts: dict | None = None):
        self.model = model
        # One session per mode, spawned on first use. A single session that knows
        # about both modes does not hold the line: with the command markers listed
        # in its own prompt, it emits commands during insert-mode turns no matter
        # how the mode is stated per turn. Measured, not assumed.
        self.prompts = prompts or {"normal": SYSTEM_PROMPT_NORMAL,
                                   "insert": SYSTEM_PROMPT_INSERT}
        self.timeout = timeout
        self.errlog = errlog
        self.procs: dict[str, subprocess.Popen] = {}
        AGENT_CWD.mkdir(parents=True, exist_ok=True)

    def _spawn(self, key: str) -> subprocess.Popen:
        return subprocess.Popen(
            ["claude", "-p", "--input-format", "stream-json",
             "--output-format", "stream-json", "--verbose",
             "--system-prompt", self.prompts[key], "--model", self.model,
             "--no-session-persistence"],
            cwd=AGENT_CWD, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errlog or subprocess.DEVNULL, text=True, bufsize=1,
            # Own process group: Ctrl-C hits the terminal's foreground group, and
            # we still want the scribe alive to finish draining the queue.
            start_new_session=True)

    def ask(self, proc_doc: Procedure, text: str, key: str) -> str:
        key = key if key in self.prompts else next(iter(self.prompts))
        proc = self.procs.get(key)
        if proc is None or proc.poll() is not None:
            proc = self.procs[key] = self._spawn(key)
        rows = proc_doc.recap(CLAUDE_CONTEXT)
        context = "\n".join(f"{i}. {s}" for i, s in rows) or "(empty - no steps recorded yet)"
        payload = (f"<steps>\n{context}\n</steps>\n\n"
                   f"<utterance>\n{text}\n</utterance>")
        msg = {"type": "user",
               "message": {"role": "user", "content": [{"type": "text", "text": payload}]}}
        try:
            proc.stdin.write(json.dumps(msg) + "\n")
            proc.stdin.flush()
        except (BrokenPipeError, OSError):
            proc = self.procs[key] = self._spawn(key)
            proc.stdin.write(json.dumps(msg) + "\n")
            proc.stdin.flush()

        # readline() rather than `for line in proc.stdout`: iterating a pipe
        # read-aheads in chunks and can block past the deadline.
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                raise RuntimeError("scribe session ended unexpectedly")
            try:
                m = json.loads(line)
            except json.JSONDecodeError:
                continue
            if m.get("type") == "result":
                if m.get("is_error"):
                    detail = esc(str(m.get("result") or m.get("subtype") or "unknown error"))
                    raise RuntimeError(detail[:200])
                return (m.get("result") or "").strip()
        raise TimeoutError(f"no reply in {self.timeout}s")

    def close(self):
        for proc in self.procs.values():
            if proc.poll() is not None:
                continue
            try:
                proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()


class ContextOverflow(RuntimeError):
    pass


class Scribe:
    """One conversation per scribe key, kept here and trimmed to the model's window on every request."""

    def __init__(self, agent: dict, timeout: int, errlog=None, prompts: dict | None = None):
        self.agent = agent
        # One conversation per mode. A single one that knows about both modes does not hold the line: with the
        # command markers listed in its own prompt, it emits commands during insert-mode turns no matter how the
        # mode is stated per turn. Measured, not assumed.
        self.prompts = prompts or {"normal": SYSTEM_PROMPT_NORMAL,
                                   "insert": SYSTEM_PROMPT_INSERT}
        self.timeout = timeout
        self.errlog = errlog
        self.key = api_key(agent)
        self.context = agent.get("context") or 32768
        self.history: dict[str, list[tuple[str, str]]] = {}

    def ask(self, proc_doc: Procedure, text: str, key: str) -> str:
        key = key if key in self.prompts else next(iter(self.prompts))
        history = self.history.setdefault(key, [])
        rows = proc_doc.recap(0)
        messages = build_messages(self.prompts[key], rows, proc_doc.unit, text, history, self.context)
        try:
            reply = self._chat(messages)
        except ContextOverflow:
            # The estimate was low for this text: once more with the document alone.
            reply = self._chat(build_messages(self.prompts[key], rows, proc_doc.unit, text, [], self.context))
        history.append((text, reply))
        del history[:-HISTORY_EXCHANGES]
        return reply

    def close(self):
        """Nothing to reap: each request is its own connection."""

    def _log(self, line: str):
        if self.errlog:
            self.errlog.write(f"[{datetime.now():%H:%M:%S}] {line}\n")
            self.errlog.flush()

    def _post(self, url: str, body: dict, headers: dict):
        req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json", **headers})
        try:
            return urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            self._log(f"HTTP {e.code} from {url}: {detail}")
            if e.code == 400 and "context" in detail.lower():
                raise ContextOverflow(detail[:200]) from None
            raise RuntimeError(f"HTTP {e.code}: {detail[:200]}") from None
        except (urllib.error.URLError, OSError) as e:
            self._log(f"{url}: {e}")
            hint = f": maic up {self.agent['provider']}" if not self.agent.get("remote") else ""
            raise RuntimeError(f"{self.agent['provider']} is not answering at {self.agent['base_url']}{hint}") from None

    def _chat(self, messages: list[dict]) -> str:
        if self.agent["kind"] == "anthropic":
            return self._anthropic(messages)
        body = {"model": self.agent["model"], "messages": messages, "stream": True,
                "max_tokens": REPLY_TOKENS, "temperature": 0.2}
        if not self.agent.get("remote"):
            # llama-server: thinking off, through the chat template and the reasoning budget alike.
            body["chat_template_kwargs"] = {"enable_thinking": False}
            body["reasoning_effort"] = "none"
        headers = {"Accept": "text/event-stream"}
        if self.key:
            headers["Authorization"] = f"Bearer {self.key}"
        deadline = time.time() + self.timeout
        out = []
        with self._post(self.agent["base_url"].rstrip("/") + "/chat/completions", body, headers) as r:
            for raw in r:
                if time.time() > deadline:
                    raise TimeoutError(f"no reply in {self.timeout}s")
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                for choice in json.loads(data).get("choices", []):
                    out.append((choice.get("delta") or {}).get("content") or "")
        return "".join(out).strip()

    def _anthropic(self, messages: list[dict]) -> str:
        body = {"model": self.agent["model"], "max_tokens": REPLY_TOKENS, "system": messages[0]["content"],
                "messages": messages[1:]}
        headers = {"x-api-key": self.key, "anthropic-version": "2023-06-01"}
        with self._post(self.agent["base_url"].rstrip("/") + "/v1/messages", body, headers) as r:
            reply = json.loads(r.read())
        return "".join(b.get("text", "") for b in reply.get("content", []) if b.get("type") == "text").strip()


def scribe(agent: Scribe | ClaudeScribe, in_q: queue.Queue, proc_doc: Procedure,
           done: threading.Event, journal=None, allow_commands: bool = True,
           record_commands: bool = False):
    try:
        while True:
            item = in_q.get()
            if item is None:
                return
            ts, text, live = item
            insert = (live == "insert") and proc_doc.mode != "steps"
            try:
                reply = agent.ask(proc_doc, text, key=live)
                if journal:
                    journal.agent(ts, reply)
                outcome = apply_reply(proc_doc, reply, ts=ts, said=text,
                                      allow_commands=allow_commands and not insert,
                                      record_commands=record_commands and not insert)
                if journal:
                    journal.applied(ts, outcome)
            except Exception as e:
                say(f"  {DIM}\u2502 [{ts}]{OFF}   {RED}scribe: {e}{OFF}")
                outcome = "scribe failed"
                if journal:
                    journal.applied(ts, f"scribe failed: {e}")
            proc_doc.narrate(ts, text, outcome)
    finally:
        agent.close()
        done.set()

