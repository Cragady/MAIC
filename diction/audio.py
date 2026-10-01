"""Sound in and out: mic capture with VAD splitting, the tap detector, device probing, the mode tone.

Everything here is PCM as parecord delivers it: 16 kHz, signed 16-bit little-endian, mono.
"""

from __future__ import annotations

import array
import collections
import io
import math
import queue
import subprocess
import sys
import threading
import wave
from datetime import datetime
from pathlib import Path

from diction.ui import BOLD, DIM, GREEN, OFF, RED, STATUS, YELLOW, say

RATE = 16000
FRAME_MS = 20
FRAME_BYTES = RATE // 1000 * FRAME_MS * 2  # 16-bit mono


def samples(pcm: bytes) -> array.array:
    """s16le bytes as signed integers, whatever the machine's byte order."""
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def wav_bytes(pcm: bytes) -> bytes:
    """One utterance as the 16 kHz mono 16-bit WAV whisper-server reads without --convert."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def from_wavs(paths: list[str], out_q: queue.Queue, ready: threading.Event):
    """Each WAV file as one utterance, in order, stamped by where it falls in the run: the pipeline without a mic.

    For tests; cli.main has checked that each is 16 kHz mono 16-bit, as parecord would deliver."""
    ready.wait()
    at = 0.0
    for p in paths:
        with wave.open(p, "rb") as w:
            pcm = w.readframes(w.getnframes())
        out_q.put((at, pcm, None))
        at += len(pcm) / 2 / RATE
    out_q.put(None)


def capture(src: str | None, out_q: queue.Queue, stop: threading.Event,
            aggressiveness: int, silence_ms: int, max_utt_s: float, min_utt_ms: int,
            ready: threading.Event, handle: dict,
            taps: "TapDetector | None" = None, arm: dict | None = None,
            arm_window: float = 20.0):
    """Read PCM from parecord, emit one bytes blob per detected utterance.

    Waits for the transcriber to reach a ready whisper-server before opening the
    mic, so the clock starts when we are genuinely listening and nothing is
    captured while the server is still loading its model."""
    while not ready.wait(0.1):
        if stop.is_set():        # Ctrl-C during model load
            out_q.put(None)
            return
    cmd = ["parecord", "--raw", f"--rate={RATE}", "--format=s16le", "--channels=1",
           "--latency-msec=20"]
    if src:
        cmd.append(f"--device={src}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    handle["proc"] = proc      # so shutdown can unblock our read()

    import webrtcvad     # only the mic path needs it; cli.main runs diction under uv when it is missing
    vad = webrtcvad.Vad(aggressiveness)
    preroll = collections.deque(maxlen=int(300 / FRAME_MS))
    voiced: list[bytes] = []
    trailing = 0
    onset = 0
    silence_frames = silence_ms // FRAME_MS
    max_frames = int(max_utt_s * 1000 / FRAME_MS)
    min_frames = min_utt_ms // FRAME_MS
    keep_tail = int(200 / FRAME_MS)  # leave a little silence so words aren't clipped
    speech_count = 0
    start_at = 0.0
    n_frames = 0        # the audio stream is the clock, not the wall -- a stalled
                        # reader must not shift timestamps or split a double tap

    def emit(buf, n_speech, tail, at):
        """Drop blips, and trim the long trailing silence before transcribing."""
        if n_speech < min_frames:
            return
        cut = len(buf) - max(0, tail - keep_tail)
        ends_at = at + cut * FRAME_MS / 1000.0

        # A tap arms this utterance if it lands anywhere from `arm_window` before
        # it starts through to its end. Including the utterance's own span
        # matters: tapping and speaking straight away merges both into a single
        # utterance, so the taps sit inside it rather than before it.
        armed = None
        if arm is not None:
            for t in arm.get("taps", []):
                if at - arm_window <= t <= ends_at:
                    armed = t
        out_q.put((at, b"".join(buf[:cut]), armed))

    try:
        while not stop.is_set():
            frame = proc.stdout.read(FRAME_BYTES)
            if len(frame) < FRAME_BYTES:
                break
            n_frames += 1
            now = n_frames * FRAME_MS / 1000.0
            speech = vad.is_speech(frame, RATE)
            if taps is not None and taps.feed(frame, now, speech) == "fire" and arm is not None:
                # Recorded, not announced. Printing every candidate live meant
                # typing filled the screen with taps that never mattered, and the
                # line landed out of order against transcript lines anyway. The
                # switch reports which tap armed it instead.
                arm.setdefault("taps", []).append(now)
                del arm["taps"][:-12]
                at = f"{int(now)//60:02d}:{int(now)%60:02d}"
                STATUS.set(f"tap at {at} \u2014 say normal / insert / toggle", arm_window,
                           on_expire=lambda a=at: say(
                               f"  {DIM}\u2502 [{a}]{OFF}   {DIM}\u231b tap expired \u2014 "
                               f"no switch phrase heard{OFF}"))

            if not voiced:
                preroll.append(frame)
                if speech:
                    onset += 1
                    if onset >= 3:  # 60ms of speech opens an utterance
                        voiced = list(preroll)
                        preroll.clear()
                        speech_count = onset
                        trailing = 0
                        start_at = now
                else:
                    onset = 0
                continue

            voiced.append(frame)
            if speech:
                speech_count += 1
                trailing = 0
            else:
                trailing += 1

            if trailing >= silence_frames or len(voiced) >= max_frames:
                emit(voiced, speech_count, trailing, start_at)
                voiced = []
                trailing = 0
                onset = 0
                speech_count = 0
    finally:
        if voiced:
            emit(voiced, speech_count, trailing, start_at)
        proc.terminate()
        out_q.put(None)



def play_tone(mode: str):
    """A short distinct tone per mode, so a switch is audible when you are not
    looking at the terminal. Rising into normal, falling back into insert."""
    rate, dur = 16000, 0.11
    pairs = [(660, 990)] if mode == "normal" else [(880, 550)]
    frames = bytearray()
    for lo, hi in pairs:
        for half, (a, b) in enumerate(((lo, lo), (hi, hi))):
            for i in range(int(rate * dur / 2)):
                t = i / rate
                env = min(1.0, t * 60, (dur / 2 - t) * 60)
                frames += int(7000 * env * math.sin(2 * math.pi * a * t)).to_bytes(2, "little", signed=True)
    try:
        subprocess.Popen(["paplay", "--raw", f"--rate={rate}", "--format=s16le",
                          "--channels=1"], stdin=subprocess.PIPE,
                         stderr=subprocess.DEVNULL).communicate(bytes(frames), timeout=3)
    except Exception:
        pass



class TapDetector:
    """Detects a double tap/click/pop on the raw PCM, before VAD or Whisper.

    This is the out-of-band half of the mode switch: an acoustic event, not
    language, so it can never collide with dictation. It only arms a window --
    a spoken phrase has to follow -- so it is deliberately tuned for recall.
    A stray onset costs nothing; a missed tap costs a retry.
    """

    def __init__(self, ratio: float = 7.25, floor: int = 220, refractory_ms: int = 70,
                 gap_ms: tuple[int, int] = (120, 900), quiet_ms: int = 600,
                 count: int = 3, tail_ms: int = 450):
        self.ratio = ratio
        self.floor = floor
        self.refractory = refractory_ms / 1000.0
        self.gap = (gap_ms[0] / 1000.0, gap_ms[1] / 1000.0)
        # A deliberate tap happens in a pause; keystrokes arrive in a stream. Typing
        # cadence (100-250ms) sits inside the gap window, so timing alone cannot
        # separate them -- requiring quiet before the first tap can.
        self.quiet = quiet_ms / 1000.0
        # The window is wide because a comfortable tapping rhythm varies a lot
        # between sessions -- measured at 0.4-0.8s once and 0.18-0.30s the next,
        # the latter squarely on top of typing. So the gap window does not try to
        # separate them; the trailing quiet below does.
        # Three, not two. Typing and speech produce pairs at exactly the spacings
        # a deliberate double tap does -- measured, the distributions overlap
        # completely -- so pair timing cannot separate them. A third tap in the
        # same rhythm is far rarer by accident.
        self.count = max(2, count)
        # No trailing guard. It was there to reject triples that fall out of
        # typing, but a false arm is already inert -- it is a window that expires
        # unless a spoken phrase lands in it, and nobody says "diction normal"
        # mid-sentence. The guard was duplicating the second gate, and it cost
        # the natural gesture: speaking within 450ms of tapping cancelled the run.
        self.tail = 0.0
        self.pending = -1.0
        # The tap that completed the run, so a fire can report itself rather than
        # the silent frame that happened to confirm it.
        self.fire_at = -1.0
        self.fire_rms = 0.0
        self.recent: collections.deque = collections.deque(maxlen=50)  # ~1s of RMS
        self.last_onset = -1.0
        self.run: list[float] = []
        self.quiet_before = -1.0
        self.last_rms = 0.0
        self.last_onset_prev = -1.0

    def _rms(self, frame: bytes) -> float:
        a = samples(frame)
        return math.sqrt(math.sumprod(a, a) / len(a)) if a else 0.0

    def feed(self, frame: bytes, now: float, is_speech: bool = False):
        """Returns 'onset', 'fire' (a completed run of `count` taps), or None.

        `is_speech` keeps the trailing guard from eating the natural gesture.
        The guard is there to reject typing, which continues after a run; but
        speaking right after tapping also continues after a run, and that is
        exactly what a tap is for. Speech does not cancel a pending fire.
        """
        if self.pending > 0 and now - self.pending >= self.tail:
            self.pending = -1.0
            return "fire"


        rms = self.last_rms = self._rms(frame)
        baseline = (sorted(self.recent)[len(self.recent) // 2] if self.recent else 0.0)
        self.recent.append(rms)

        is_onset = (rms > self.floor and rms > baseline * self.ratio
                    and now - self.last_onset > self.refractory)
        if not is_onset:
            return None
        prev_onset = self.last_onset_prev = self.last_onset
        self.last_onset = now


        # Extend the current run only if this onset lands at a deliberate tap
        # spacing from the previous one; otherwise it starts a fresh run.
        if self.run and self.gap[0] <= now - self.run[-1] <= self.gap[1]:
            self.run.append(now)
        else:
            self.quiet_before = prev_onset
            self.run = [now]

        if len(self.run) >= self.count:
            first = self.run[0]
            self.run = []
            if self.quiet_before < 0 or first - self.quiet_before >= self.quiet:
                self.fire_at, self.fire_rms = now, self.last_rms
                if self.tail <= 0:
                    return "fire"
                self.pending = now      # confirmed only if nothing follows
        return "onset"


def probe(src: str | None, seconds: float = 0.8):
    """Record briefly and report (peak, rms). peak==0 means a dead device."""
    cmd = ["parecord", "--raw", f"--rate={RATE}", "--format=s16le", "--channels=1",
           "--latency-msec=20"]
    if src:
        cmd.append(f"--device={src}")
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=seconds + 2.5)
        data = r.stdout
    except subprocess.TimeoutExpired as e:
        data = e.stdout or b""
    if len(data) < 2:
        return 0, 0
    a = samples(data[: len(data) // 2 * 2])
    return max(abs(min(a)), max(a)), int(math.sqrt(math.sumprod(a, a) / len(a)))


def inputs() -> list[str]:
    r = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and not parts[1].endswith(".monitor"):
            out.append(parts[1])
    return out


def default_source() -> str:
    r = subprocess.run(["pactl", "info"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith("Default Source:"):
            return line.split(":", 1)[1].strip()
    return ""


def pick_source(requested: str | None) -> str | None:
    """Return a source that actually delivers audio, or None to use the default."""
    if requested:
        peak, _ = probe(requested)
        if peak == 0:
            print(f"{RED}  '{requested}' is delivering silence.{OFF} Try: diction devices")
            sys.exit(1)
        return requested

    dflt = default_source()
    peak, rms = probe(None)
    if peak > 0:
        if rms < 3:
            print(f"{YELLOW}  warning: default mic is very quiet (rms {rms}) \u2014 check it is unmuted.{OFF}")
        return None

    print(f"{YELLOW}  default source is silent: {dflt or '(unknown)'}{OFF}")
    for cand in inputs():
        if cand == dflt:
            continue
        cpeak, crms = probe(cand)
        if cpeak > 0 and crms >= 3:
            print(f"{YELLOW}  falling back to: {cand}{OFF}")
            return cand
    print(f"{RED}  no live microphone found.{OFF} Power on your headset, or run: diction devices")
    sys.exit(1)


def tap_test(src: str | None, det: "TapDetector", logpath: Path, visual: bool = False):
    """Calibration. Every onset, with amplitude and inter-onset gap, always goes
    to the log.

    By default every onset also prints, which is what makes the shell's own
    echo of your keystrokes useful -- it interleaves with the onsets and labels
    them, and the log has no equivalent. --visual narrows the terminal to
    completed taps only, for gauging the fire rate at a glance."""
    cmd = ["parecord", "--raw", f"--rate={RATE}", "--format=s16le", "--channels=1",
           "--latency-msec=20"]
    if src:
        cmd.append(f"--device={src}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    word = {2: "DOUBLE", 3: "TRIPLE"}.get(det.count, f"{det.count}x")
    header = (f"ratio={det.ratio} floor={det.floor} count={det.count} "
              f"gap={int(det.gap[0]*1000)}-{int(det.gap[1]*1000)}ms "
              f"quiet={int(det.quiet*1000)}ms tail={int(det.tail*1000)}ms")
    print(f"{GREEN}tap test{OFF} {DIM}{header}{OFF}")
    print(f"{DIM}  {'completed ' + word.lower() + ' taps only' if visual else 'every onset'}"
          f". Ctrl-C when done.{OFF}")
    print(f"{DIM}  full onset log \u2192 {logpath}{OFF}\n")

    log = logpath.open("w", encoding="utf-8")
    log.write(f"# diction tap test\n# {header}\n# started {datetime.now():%Y-%m-%d %H:%M:%S}\n\n")

    n_frames = 0
    onsets = fires = 0
    try:
        while True:
            frame = proc.stdout.read(FRAME_BYTES)
            if len(frame) < FRAME_BYTES:
                break
            n_frames += 1
            now = n_frames * FRAME_MS / 1000.0
            hit = det.feed(frame, now)
            if hit is None:
                continue
            onsets += 1
            gap = now - det.last_onset_prev if det.last_onset_prev > 0 else -1.0
            if hit == "fire":
                fires += 1
                # Report the tap that completed the run, not the silence that
                # confirmed it -- the two are `tail` apart.
                at, arms = det.fire_at, det.fire_rms
                print(f"  {GREEN}[{at:7.2f}s] {word} TAP{OFF}  {DIM}#{fires} "
                      f"(confirmed +{now - at:.2f}s){OFF}", flush=True)
                log.write(f"[{at:8.2f}] FIRE   rms={arms:7.0f} "
                          f"confirmed_at={now:8.2f}  #{fires}\n")
            else:
                if not visual:
                    print(f"  {DIM}[{now:7.2f}s] onset  rms={det.last_rms:6.0f} "
                          f"gap={gap:5.2f}{OFF}", flush=True)
                log.write(f"[{now:8.2f}] onset  rms={det.last_rms:7.0f} gap={gap:6.3f}\n")
            log.flush()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()

    dur = n_frames * FRAME_MS / 1000.0
    summary = f"{onsets} onsets, {fires} completed {word.lower()} taps in {dur:.0f}s"
    log.write(f"\n# {summary}\n")
    log.close()
    print(f"\n{BOLD}{summary}{OFF}")
    print(f"{DIM}  full log \u2192 {logpath}{OFF}")


def list_devices():
    r = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True)
    default = subprocess.run(["pactl", "info"], capture_output=True, text=True).stdout
    dflt = ""
    for line in default.splitlines():
        if line.startswith("Default Source:"):
            dflt = line.split(":", 1)[1].strip()
    print(f"{DIM}  probing each input\u2026{OFF}\n")
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or parts[1].endswith(".monitor"):
            continue
        peak, rms = probe(parts[1])
        if peak == 0:
            status = f"{RED}dead    {OFF}"
        elif rms < 3:
            status = f"{YELLOW}v.quiet {OFF}"
        else:
            status = f"{GREEN}live    {OFF}"
        mark = f"{BOLD} (default){OFF}" if parts[1] == dflt else ""
        print(f"  {status} rms {rms:5d}  {parts[1]}{mark}")

