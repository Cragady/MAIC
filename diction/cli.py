#!/usr/bin/env python3
"""diction - narrate what you're doing, get it written down.

Captures the default microphone continuously, splits it into utterances on
natural pauses, has whisper-server (whisper.cpp, on loopback) transcribe each,
and hands the text to a scribe model that maintains the document: cleaned
prose, a numbered procedure, or the raw transcript. The scribe's backend, the
scribe and the whisper model come from a preset (diction/presets.py): by
default Claude Haiku through the claude CLI and distil-large-v3, as diction
always ran.

Three stages run concurrently so speech is never dropped while an earlier
utterance is still being transcribed or written up.

`maic diction ...` and `cai diction ...` both land in main() here.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tomllib
import wave
from datetime import datetime
from pathlib import Path

from diction.audio import RATE, TapDetector, capture, from_wavs, list_devices, pick_source, tap_test
from diction.document import Procedure
from diction.journal import Journal
from diction.pipeline import ModeState, transcriber
from diction.scribe import (SYSTEM_PROMPT_INSERT, SYSTEM_PROMPT_NORMAL, SYSTEM_PROMPT_STEPS, ClaudeScribe,
                            Scribe, resolve_agent, scribe)
from diction.ui import BOLD, DIM, OFF, RED, YELLOW, print_recap, say
from diction import presets
from diction import whisper as whisper_mod

HERE = Path(__file__).resolve().parent


def build_id() -> str:
    """Which build this is: the installed copy (a MAIC release, which writes VERSION), or the working tree.

    Worth printing, because they diverge -- a session was once debugged at
    length against fixes that were only in beta.
    """
    vf = HERE / "VERSION"
    if vf.exists():
        ref = vf.read_text(encoding="utf-8").splitlines()[0].strip() or "?"
        return f"diction {DIM}(stable {ref}){OFF}"
    try:
        r = subprocess.run(["git", "-C", str(HERE), "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True, timeout=3)
        ref = r.stdout.strip() or "?"
        d = subprocess.run(["git", "-C", str(HERE), "status", "--porcelain", "--", "."],
                           capture_output=True, text=True, timeout=3)
        dirty = "+" if d.stdout.strip() else ""
    except Exception:
        ref, dirty = "?", ""
    return f"diction-beta {DIM}(working tree {ref}{dirty}){OFF}"


LOG_DIR = "diction-logs"
HIDDEN_LOG_DIR = ".h-diction-logs"

CONFIG_HOME = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
CONFIG_PATH = CONFIG_HOME / "maic" / "diction.lua"
TOML_PATH = CONFIG_HOME / "diction" / "config.toml"


def config_file() -> Path | None:
    """The file diction's settings come from: diction.lua beside MAIC's settings.lua, else the config.toml diction
    read before it."""
    return CONFIG_PATH if CONFIG_PATH.exists() else TOML_PATH if TOML_PATH.exists() else None


def load_config() -> dict:
    """diction.lua as `maic settings read diction` evaluates it (diction never runs the Lua itself), else the old
    config.toml with a notice. Absent or broken is fine."""
    src = config_file()
    if src is None:
        return {}
    if src == TOML_PATH:
        print(f"{YELLOW}  diction reads {TOML_PATH}; `diction migrate-config` writes {CONFIG_PATH} from it{OFF}")
        try:
            return tomllib.loads(TOML_PATH.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError) as e:
            print(f"{YELLOW}  ignoring {TOML_PATH}: {e}{OFF}")
            return {}
    if TOML_PATH.exists():
        print(f"{YELLOW}  diction reads {CONFIG_PATH}; {TOML_PATH} is ignored (delete it){OFF}")
    exe = whisper_mod.maic_bin()
    if not exe:
        print(f"{YELLOW}  ignoring {CONFIG_PATH}: maic evaluates it, and no maic was found (MAIC_BIN or PATH){OFF}")
        return {}
    r = subprocess.run([exe, "settings", "read", "diction"], capture_output=True, text=True, timeout=20)
    if r.returncode != 0:
        print(f"{YELLOW}  ignoring {CONFIG_PATH}: {(r.stderr.strip() or r.stdout.strip()).removeprefix('maic: ')}{OFF}")
        return {}
    return json.loads(r.stdout)


LUA_KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local",
                "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"}


def lua_key(k: str) -> str:
    return k if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k) and k not in LUA_KEYWORDS else f"[{lua_value(k)}]"


def lua_value(v, indent: str = "") -> str:
    """A TOML value as Lua source: tables one key a line, strings quoted so Lua reads back the same bytes."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, list):
        return "{ " + ", ".join(lua_value(x, indent) for x in v) + " }"
    if isinstance(v, dict):
        inner = indent + "    "
        rows = [f"{inner}{lua_key(k)} = {lua_value(x, inner)}," for k, x in v.items()]
        return "{\n" + "\n".join(rows) + f"\n{indent}}}" if rows else "{}"
    escapes = {'"': '\\"', "\\": "\\\\", "\n": "\\n"}
    return '"' + "".join(escapes.get(c) or (f"\\{ord(c):03d}" if ord(c) < 32 or ord(c) == 127 else c) for c in str(v)) + '"'




def migrate_config(force: bool) -> int:
    """`diction migrate-config`: diction.lua from config.toml, which stays where it is."""
    if not TOML_PATH.exists():
        print(f"{RED}  no {TOML_PATH} to migrate{OFF}")
        return 1
    if CONFIG_PATH.exists() and not force:
        print(f"{RED}  {CONFIG_PATH} exists; --force overwrites it{OFF}")
        return 1
    try:
        cfg = tomllib.loads(TOML_PATH.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        print(f"{RED}  {TOML_PATH}: {e}{OFF}")
        return 1
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(f"-- diction's settings, written by `diction migrate-config` from {TOML_PATH} on "
                           f"{datetime.now():%Y-%m-%d}.\n-- maic evaluates it (`maic settings read diction`); "
                           f"the TOML is no longer read and can go.\nreturn {lua_value(cfg)}\n", encoding="utf-8")
    print(f"wrote {CONFIG_PATH} (from {TOML_PATH}, which is left in place and no longer read)")
    return 0


def resolve_logdir(args, cfg: dict, cwd: Path) -> tuple[Path, str]:
    """Where logs go, and why -- highest precedence first.

    An explicit flag always wins. --local-logs and --hidden-logs both mean
    "next to the document", so they override a configured global directory;
    that is the point of them once a global directory exists.
    """
    if args.log_dir:
        return cwd / Path(args.log_dir).expanduser(), "--log-dir"
    if args.hidden_logs:
        return cwd / HIDDEN_LOG_DIR, "--hidden-logs"
    if args.local_logs:
        return cwd / LOG_DIR, "--local-logs"
    g = os.environ.get("DICTION_LOG_DIR") or cfg.get("log_dir")
    if g:
        src = "DICTION_LOG_DIR" if os.environ.get("DICTION_LOG_DIR") else str(config_file())
        return Path(g).expanduser(), src
    return cwd / LOG_DIR, "default"


def slugify(text: str) -> str:
    out = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return out or "diction"


def run(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="diction", description="Narrate a task; get a written procedure.",
                                 epilog=f"Settings (presets, log_dir) live in {CONFIG_PATH}, a Lua file returning a "
                                        f"table, beside MAIC's settings.lua; maic evaluates it at the settings' "
                                        f"global_lua level (`maic settings read diction`). Without it, the old {TOML_PATH} is "

                                        f"still read, and `diction migrate-config` writes diction.lua from it.")
    ap.add_argument("command", nargs="?", default="start",
                    choices=["start", "devices", "taptest", "presets", "migrate-config"],
                    help="start (default); devices: list mics; taptest: tune the tap gesture; "
                         "presets: list the presets and whether their models are ready; "
                         "migrate-config: write diction.lua from the old config.toml (kept in place)")
    ap.add_argument("--force", action="store_true",
                    help="migrate-config: overwrite an existing diction.lua")
    ap.add_argument("-o", "--out", help="procedure markdown file (default: <cwd-name>.md)")
    ap.add_argument("-t", "--title", help="document title (default: derived from filename)")
    ap.add_argument("-d", "--device", help="pulse/pipewire source name (default: system default)")
    ap.add_argument("--preset", metavar="NAME",
                    help="a scribe backend, a scribe and a whisper model under one name: default "
                         "(Claude Haiku through the claude CLI and distil-large-v3, as diction always "
                         "ran; the text goes to Anthropic), api (the same Haiku through Anthropic's "
                         "API), local (the Qwen3.5 9B on llamacpp-2 and large-v3-turbo-q5_0), "
                         "local-small (the 4B), or a presets.NAME of diction.lua. Also "
                         "DICTION_PRESET. --backend, -m and --agent-model, and DICTION_BACKEND, "
                         "DICTION_MODEL and DICTION_AGENT_MODEL, override its parts")
    ap.add_argument("--backend", choices=list(presets.BACKENDS),
                    help="how the scribe is reached: claude-cli (a claude process on your own "
                         "login), api (a cloud provider's API, through maic model resolve), local "
                         "(an OpenAI-compatible MAIC server). Default: DICTION_BACKEND, else the "
                         "preset's (claude-cli)")
    ap.add_argument("-m", "--model",
                    help="whisper ggml model: a file, or a name under <models_dir>/whisper/ "
                         "(distil-large-v3 finds ggml-distil-large-v3.bin), or current, the model "
                         "whisper-server loaded; another one is loaded into the running server. "
                         "Default: DICTION_MODEL, else the preset's (distil-large-v3)")
    ap.add_argument("--agent-model", metavar="NAME",
                    help="the scribe. With backend claude-cli, what claude --model takes (haiku, "
                         "sonnet, opus or a full name). With api or local, a MAIC preset or "
                         "provider/model, as `maic model resolve` reads it, where haiku, sonnet and "
                         "opus mean haiku-4.5, sonnet-5 and opus-5.5; a preset name that lands on the "
                         "main llama server goes to the side server llamacpp-2 when it answers. "
                         "Default: DICTION_AGENT_MODEL, else the preset's (haiku)")
    ap.add_argument("--silence", type=int, default=700, help="ms of silence that ends an utterance")
    ap.add_argument("--aggressiveness", type=int, default=2, choices=[0, 1, 2, 3],
                    help="VAD strictness; raise it in a noisy room")
    ap.add_argument("--max-utterance", type=float, default=30.0, help="force-split after N seconds")
    ap.add_argument("--min-utterance", type=int, default=200, help="discard blips shorter than N ms")
    ap.add_argument("--agent-timeout", type=int, default=90)
    ap.add_argument("--tone", action="store_true",
                    help="play a short tone on a mode switch (rising into normal, "
                         "falling into insert)")
    ap.add_argument("--no-taps", action="store_true",
                    help="disable double-tap arming; mode can then only be set at startup")
    ap.add_argument("--tap-ratio", type=float, default=7.25,
                    help="how far above the rolling noise floor a transient must "
                         "jump to count as a tap (default: 7.25)")
    ap.add_argument("--tap-floor", type=int, default=220,
                    help="absolute RMS a transient must exceed (default: 220)")
    ap.add_argument("--clean", "--visual", dest="clean", action="store_true",
                    help="taptest: show only completed taps in the terminal, with "
                         "no onset noise (default: show every onset). --visual is "
                         "kept as an alias")
    ap.add_argument("--tap-gap", metavar="LO-HI", default="120-900",
                    help="ms between consecutive taps (default: 120-900)")
    ap.add_argument("--tap-count", type=int, default=3, metavar="N",
                    help="taps required to fire (default: 3; 2 is the old behaviour)")
    ap.add_argument("--arm-window", type=float, default=20.0, metavar="S",
                    help="how long after a tap the spoken switch is still accepted "
                         "(default: 20). It stays armed until used or expired, so a "
                         "misheard attempt can simply be repeated")
    ap.add_argument("--tap-tail", type=int, default=450, metavar="MS",
                    help="required quiet after the last tap before firing. This is "
                         "what separates a deliberate tap, which you stop after, "
                         "from a run that fell out of typing, which continues "
                         "(default: 450; 0 disables)")
    ap.add_argument("--tap-quiet", type=int, default=600, metavar="MS",
                    help="required quiet before the first tap, which is what "
                         "separates a deliberate pair from a run of keystrokes "
                         "(default: 600; 0 disables)")
    ap.add_argument("--mode", choices=["normal", "insert"], default="insert",
                    help="mode to start in (default: insert). normal parses commands "
                         "from the prose, acts on them and records them; insert treats "
                         "everything as dictation. Switch mid-session by double-tapping "
                         "and saying 'diction normal' / 'insert' / 'toggle'. "
                         "(transcript only; ignored with --steps)")
    ap.add_argument("--steps", action="store_true",
                    help="write a numbered procedure instead of a transcript: each "
                         "utterance becomes an imperative step")
    ap.add_argument("--no-agent", action="store_true",
                    help="raw transcription only; no scribe, no cleanup")
    ap.add_argument("--recap", type=int, default=3, metavar="N",
                    help="on resume, print the last N steps in full (default: 3; "
                         "0 to disable). Mid-session, just ask out loud.")
    ap.add_argument("--log-dir", metavar="DIR",
                    help=f"where session logs go (default: {LOG_DIR}/, or the "
                         f"global log_dir if one is configured)")
    ap.add_argument("--local-logs", action="store_true",
                    help=f"force {LOG_DIR}/ next to the document, ignoring any "
                         f"configured global log directory")
    ap.add_argument("--hidden-logs", action="store_true",
                    help=f"shorthand for --log-dir {HIDDEN_LOG_DIR} (also local)")
    ap.add_argument("--session-log", action="store_true",
                    help="also write an enriched turn-by-turn log: what you said, "
                         "the agent's raw directive, and the effect it had")
    ap.add_argument("--raw", action="store_true",
                    help="also write the verbatim narration into the document, "
                         "annotated with what each utterance became")
    # For tests: each file is one utterance, in place of the mic.
    ap.add_argument("--from-wav", action="append", metavar="FILE", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.command == "start" and not args.from_wav and importlib.util.find_spec("webrtcvad") is None:
        return run_under_uv(argv)

    if args.command == "devices":
        list_devices()
        return 0
    if args.command == "migrate-config":
        return migrate_config(args.force)

    cfg = load_config()
    if args.command == "presets":
        return presets.show(cfg, args.preset)

    try:
        gap_lo, gap_hi = (int(x) for x in args.tap_gap.split("-", 1))
    except ValueError:
        print(f"{RED}  --tap-gap wants LO-HI in ms, e.g. 350-900{OFF}")
        return 1

    if args.command == "taptest":
        logdir, _ = resolve_logdir(args, cfg, Path.cwd())
        logdir.mkdir(parents=True, exist_ok=True)
        tap_test(pick_source(args.device),
                 TapDetector(ratio=args.tap_ratio, floor=args.tap_floor,
                             quiet_ms=args.tap_quiet, count=args.tap_count,
                             gap_ms=(gap_lo, gap_hi), tail_ms=args.tap_tail),
                 logdir / f"taptest-{datetime.now():%Y%m%d-%H%M%S}.log",
                 visual=args.clean)
        return 0

    cwd = Path.cwd()
    out = Path(args.out) if args.out else cwd / f"{cwd.name}.md"
    title = args.title or out.stem.replace("-", " ").replace("_", " ").capitalize()
    mode = "steps" if args.steps else "transcript"
    proc_doc = Procedure(out, title, raw=args.raw, mode=mode)

    # An existing document's own shape wins over the requested one.
    if proc_doc.mode == "steps":
        prompts = {"normal": SYSTEM_PROMPT_STEPS, "insert": SYSTEM_PROMPT_STEPS}
        allow_commands, record_commands = True, False
        mode_state = ModeState("normal")
    else:
        prompts = {"normal": SYSTEM_PROMPT_NORMAL, "insert": SYSTEM_PROMPT_INSERT}
        allow_commands, record_commands = True, True
        mode_state = ModeState(args.mode)

    for f in args.from_wav or []:
        with wave.open(f, "rb") as w:
            if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (RATE, 1, 2):
                print(f"{RED}  {f}: --from-wav needs 16 kHz mono 16-bit PCM{OFF}")
                return 1

    try:
        preset, backend, scribe_name, whisper_arg = presets.choose(args.preset, args.backend, args.agent_model,
                                                                    args.model, cfg)
    except ValueError as e:
        print(f"{RED}  {e}{OFF}")
        return 1
    agent = None
    if args.no_agent:
        agent_label = "off"
    elif backend == "claude-cli":
        if not shutil.which("claude"):
            print(f"{RED}  {presets.CLAUDE_MISSING}{OFF}")
            return 1
        agent_label = scribe_name
    else:
        try:
            agent = resolve_agent(scribe_name)
        except Exception as e:
            print(f"{RED}  scribe: {e}{OFF}")
            return 1
        agent_label = f"{agent['provider']}/{agent['model']}"
        if backend == "local" and agent.get("remote"):
            print(f"{RED}  scribe: {agent_label} is a cloud model; the local backend keeps the text on this "
                  f"machine (--backend api to send it){OFF}")
            return 1
    try:
        whisper_path = whisper_mod.resolve_model(whisper_arg)
    except Exception as e:
        print(f"{RED}  {e}{OFF}")
        return 1
    whisper_name = whisper_mod.display_name(whisper_path)

    logdir, log_src = resolve_logdir(args, cfg, cwd)
    try:
        logdir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        print(f"{RED}  cannot create log directory {logdir}: {e}{OFF}")
        return 1
    log_notice = (f"{DIM}  logs: {logdir} ({log_src}){OFF}"
                  if log_src not in ("default", "--log-dir") else None)
    slug = slugify(title)
    stamp = f"{datetime.now():%Y%m%d-%H%M%S}"
    journal = Journal(logdir, slug, stamp,
                      f"whisper={whisper_name} scribe={agent_label}",
                      expects_agent=not args.no_agent, enrich=args.session_log)
    # stamped like the rest: a shared global log dir would otherwise clobber it
    errlog = (logdir / f"{slug}-scribe-{stamp}.log").open("w", encoding="utf-8")
    try:
        if args.no_agent:
            agent_scribe = None
        elif backend == "claude-cli":
            agent_scribe = ClaudeScribe(scribe_name, args.agent_timeout, errlog, prompts)
        else:
            agent_scribe = Scribe(agent, args.agent_timeout, errlog, prompts)
    except Exception as e:
        hint = " (or --preset local, which keeps it on this machine)" if agent and agent.get("remote") else ""
        print(f"{RED}  scribe: {e}{hint}{OFF}")
        return 1

    print(f"{BOLD}{build_id()}{OFF} {DIM}→{OFF} {out}")
    if proc_doc.steps:
        print(f"{DIM}  resuming at {proc_doc.unit} {len(proc_doc.steps)}{OFF}")
        if args.recap:
            print_recap(proc_doc.recap(args.recap), f"{DIM}\u2502{OFF}",
                        f"where you left off:")
    print(f"{DIM}  preset: {preset} · backend: {'off' if args.no_agent else backend} · whisper: {whisper_name} · scribe: "
          f"{agent_label} · mode: "
          f"{'raw transcript' if args.no_agent else mode_state.get()}{OFF}")
    if agent is not None and agent.get("remote"):
        print(f"{YELLOW}  scribe: {agent['provider']} is a cloud provider; the text of every utterance "
              f"goes to it (the audio stays on this machine){OFF}")
    elif agent_scribe is not None and backend == "claude-cli":
        print(f"{YELLOW}  scribe: the claude CLI sends the text of every utterance to Anthropic "
              f"(the audio stays on this machine){OFF}")
    if log_notice:
        print(log_notice)
    source = None if args.from_wav else pick_source(args.device)
    say(f"{DIM}  loading model\N{HORIZONTAL ELLIPSIS}{OFF}")
    whisper = whisper_mod.Whisper(whisper_mod.server_url(), timeout=args.agent_timeout)
    t_load = time.time()
    try:
        whisper.wait_ready()
        if whisper_path and whisper_path != (whisper_mod.models_root() / "current.bin").resolve():
            whisper.load(whisper_path)
    except Exception as e:
        print(f"{RED}  {e}{OFF}")
        return 1
    print(f"{DIM}  model ready in {time.time() - t_load:.1f}s{OFF}")

    audio_q: queue.Queue = queue.Queue()
    text_q: queue.Queue = queue.Queue()
    stop = threading.Event()
    done = threading.Event()
    ready = threading.Event()
    rec: dict = {}
    arm: dict = {"at": None}
    taps = None if args.no_taps else TapDetector(ratio=args.tap_ratio, floor=args.tap_floor,
                                                quiet_ms=args.tap_quiet, count=args.tap_count)

    if args.from_wav:
        source_thread = threading.Thread(target=from_wavs, args=(args.from_wav, audio_q, ready), daemon=True)
    else:
        source_thread = threading.Thread(target=capture, args=(source, audio_q, stop, args.aggressiveness,
                                                               args.silence, args.max_utterance, args.min_utterance, ready, rec,
                                                               taps, arm, args.arm_window),
                                         daemon=True)
    threads = [
        source_thread,
        threading.Thread(target=transcriber, args=(whisper, audio_q, text_q, journal, ready, mode_state, args.tone, arm),
                         daemon=True),
    ]
    if args.no_agent:
        def drain():
            while (item := text_q.get()) is not None:
                proc_doc.narrate(item[0], item[1], "")
            done.set()
        threads.append(threading.Thread(target=drain, daemon=True))
    else:
        threads.append(threading.Thread(target=scribe,
                                        args=(agent_scribe, text_q, proc_doc, done,
                                              journal, allow_commands,
                                              record_commands),
                                        daemon=True))
    for t in threads:
        t.start()

    try:
        while not done.is_set():
            time.sleep(0.2)
    except KeyboardInterrupt:
        say(f"\n{DIM}  stopping \u2014 draining {audio_q.qsize() + text_q.qsize()} pending\N{HORIZONTAL ELLIPSIS}{OFF}")
        stop.set()
        rp = rec.get("proc")
        if rp and rp.poll() is None:
            rp.terminate()          # unblocks the capture thread's read()
        if not done.wait(timeout=args.agent_timeout + 15):
            print(f"{YELLOW}  drain timed out; exiting anyway{OFF}")

    journal.close()
    errlog.close()
    n = len(proc_doc.steps)
    print(f"\n{BOLD}{n} {proc_doc.unit}{'' if n == 1 else 's'}{OFF} → {out}")
    print(f"{DIM}raw transcript → {journal.raw_path}{OFF}")
    if journal.session_path:
        print(f"{DIM}session log    → {journal.session_path}{OFF}")
    sys.stdout.flush()
    return 0


def run_under_uv(argv: list[str]) -> int:
    """The mic path needs webrtcvad, the one dependency outside the standard library. Rather than have it
    pip-installed anywhere, run diction again under `uv run` with diction's requirements.txt: offline when uv's
    cache already has the wheel, so a normal start makes no outbound connection; the first run fetches it once."""
    if os.environ.get("_DICTION_UV") or not shutil.which("uv"):
        print(f"{RED}  the mic needs webrtcvad: install uv (diction runs itself under `uv run` with "
              f"{HERE / 'requirements.txt'}), or pip install webrtcvad-wheels{OFF}")
        return 1
    paths = [p for p in os.environ.get("PYTHONPATH", "").split(os.pathsep) if p]
    env = dict(os.environ, _DICTION_UV="1", PYTHONPATH=os.pathsep.join(paths if str(HERE.parent) in paths else [str(HERE.parent), *paths]))
    # 3.13: webrtcvad-wheels 2.0.14 has wheels up to cp313, and building it for a newer Python needs its headers.
    uv = ["uv", "run", "--no-project", "--quiet", "--python", "3.13", "--with-requirements", str(HERE / "requirements.txt")]
    cached = subprocess.run(uv[:2] + ["--offline"] + uv[2:] + ["python", "-c", "import webrtcvad"],
                            env=env, capture_output=True).returncode == 0
    if not cached:
        print(f"{DIM}  fetching webrtcvad-wheels once, through uv{OFF}", flush=True)
    cmd = uv[:2] + (["--offline"] if cached else []) + uv[2:] + ["python", "-P", "-m", "diction.cli", *argv]
    os.execvpe("uv", cmd, env)


def main(argv: list[str] | None = None) -> int:
    """`maic diction` (through the maic-diction launcher) and `cai diction` both call this."""
    try:
        return run(sys.argv[1:] if argv is None else list(argv))
    except KeyboardInterrupt:          # Ctrl-C while probing mics or loading
        print(f"\n{DIM}  cancelled{OFF}")
        return 130


if __name__ == "__main__":
    sys.exit(main())
