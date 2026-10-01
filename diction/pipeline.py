"""The transcriber stage: utterance audio in, text out, with the gates between them.

Each utterance goes to whisper-server; stock hallucinations are dropped; a mode-switch phrase is handled here and
never reaches the scribe.
"""

from __future__ import annotations

import difflib
import queue
import re
import threading

from diction.audio import play_tone, wav_bytes
from diction.journal import Journal
from diction.ui import BOLD, DIM, GREEN, MODIFIABLE_MARK, OFF, RED, STATUS, YELLOW, esc, say
from diction.whisper import Whisper


# Stock phrases Whisper emits when handed non-speech audio. Only ever matched
# against a whole utterance, so a real step containing these words survives.
HALLUCINATIONS = {
    "thank you", "thanks", "thanks for watching", "thanks for watching!",
    "you", "bye", "okay", "oh", "so", "yeah", "mm", "hmm", "uh", "um",
    "please subscribe", "subscribe", "the end", "音", "♪",
}



SWITCH_VERBS = ("normal", "insert", "toggle")

# Strict form, for recognising a switch attempt that was NOT armed by a tap.
# Without the tap there is no established intent, so this has to be exact.
SWITCH_STRICT = re.compile(
    r"^\W*(?:diction|dictation|dictions|diktion|dixon|dixen)\W+(normal|insert|toggle)\W*$",
    re.I)
SWITCH_MAX_WORDS = 6


def match_switch(text: str, threshold: float = 0.6) -> str | None:
    """Recognise a mode-switch phrase inside an armed window.

    The tap has already established intent, so the wake word is not required --
    which matters, because Whisper mangles it badly: measured, one session
    produced "Dixon", "Dixen", "Dekchin", "Diction Tangle" and "Diction Togel"
    for the same spoken phrase. Only the verb has to survive, and it is matched
    fuzzily for the same reason ("Tago", "Tangle", "Togel" were all "toggle").

    Kept deliberately loose: this only ever runs on the utterance following a
    deliberate triple tap, and the word cap keeps a whole sentence from
    qualifying.
    """
    words = re.findall(r"[a-z]+", text.lower())
    if not words or len(words) > SWITCH_MAX_WORDS:
        return None
    best_score, best_verb = 0.0, None
    for w in words:
        for verb in SWITCH_VERBS:
            score = difflib.SequenceMatcher(None, w, verb).ratio()
            if score > best_score:
                best_score, best_verb = score, verb
    return best_verb if best_score >= threshold else None


class ModeState:
    """Which editing mode is live. Flipped on the transcriber thread the instant
    a switch is recognised, so utterances are tagged with the mode that was
    active when they were spoken -- not when the lagging scribe reached them."""

    def __init__(self, mode: str = "insert"):
        self.mode = mode
        self.lock = threading.Lock()

    def get(self) -> str:
        with self.lock:
            return self.mode

    def set(self, mode: str) -> str:
        with self.lock:
            self.mode = mode
            return self.mode

    def toggle(self) -> str:
        with self.lock:
            self.mode = "normal" if self.mode == "insert" else "insert"
            return self.mode



def transcriber(whisper: Whisper, in_q: queue.Queue, out_q: queue.Queue, journal: Journal,
                ready: threading.Event, mode_state: "ModeState | None" = None,
                tone: bool = False, arm: dict | None = None):
    """cli.main has waited for whisper-server to hold its model, so the mic opens now."""
    say(f"{GREEN}\N{STUDIO MICROPHONE}  listening\N{HORIZONTAL ELLIPSIS} (Ctrl-C to stop){OFF}\n")
    ready.set()

    while True:
        item = in_q.get()
        if item is None:
            out_q.put(None)
            return
        at, pcm, armed = item
        ts = f"{int(at)//60:02d}:{int(at)%60:02d}"
        # Silero VAD as a second gate, inside whisper-server (--vad in services/whisper.json).
        # WebRTC VAD opens utterances on breathing and room noise, and Whisper reliably
        # hallucinates stock phrases into that silence -- with no_speech_prob as low as
        # 0.10, so the usual confidence thresholds do not catch it. Silero does.
        try:
            text = esc(whisper.transcribe(wav_bytes(pcm)))
        except Exception as e:
            journal.dropped(ts, "(audio not transcribed)", str(e))
            say(f"  {DIM}\u2502 [{ts}]{OFF}   {RED}whisper: {e}{OFF}")
            continue
        if not text or not re.search(r"[A-Za-z]", text):
            continue
        if text.lower().strip(" .!?,") in HALLUCINATIONS:
            journal.dropped(ts, text, "stock phrase, likely hallucinated")
            continue
        # An armed window plus the phrase: two independent gates, so neither has
        # to be strict on its own.
        if armed and mode_state is not None and (verb := match_switch(text)):
            new = mode_state.toggle() if verb == "toggle" else mode_state.set(verb)
            if arm is not None:
                arm["taps"] = []      # consumed; a fresh tap is needed next time
            journal.mode_switch(ts, text, new)
            colour = YELLOW if new == "normal" else GREEN
            tapped = f"{int(armed)//60:02d}:{int(armed)%60:02d}"
            STATUS.clear()
            say(f"  {DIM}\u2502 [{ts}]{OFF}   {colour}{BOLD}\u21d2 {new.upper()} MODE{OFF}"
                f" {DIM}(tap at {tapped}){OFF}", typeset=0.012)
            if tone:
                play_tone(new)
            continue

        # A switch phrase that no tap armed. Handing it to the scribe is how a
        # failed switch ends up rewritten as a document edit -- the agent has no
        # directive for "change mode", so it improvises the nearest one it has.
        if not armed and mode_state is not None and SWITCH_STRICT.match(text):
            journal.mode_switch(ts, text, f"{mode_state.get()} (ignored: no tap)")
            say(f'  {DIM}\u2502 [{ts}]{OFF}   {YELLOW}\u26a0 "{esc(text)}" \u2014 '
                f'no tap, so no switch. Triple-tap, then say it again.{OFF}')
            continue

        journal.heard(ts, text, mode_state.get() if mode_state else "")
        live = mode_state.get() if mode_state else "normal"
        mark = MODIFIABLE_MARK if live == "normal" else " "
        say(f'  {DIM}{mark}├ [{ts}]{OFF} "{text}"')
        out_q.put((ts, text, live))

