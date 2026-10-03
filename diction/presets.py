"""Presets: a scribe backend, a scribe and a whisper model under one name, with a note on what they cost.

`default` is diction as it always ran: Claude Haiku through the `claude` CLI on the user's own login, and
distil-large-v3 for speech. `api` is the same Haiku through Anthropic's API. `local` and `local-small` keep the
narration on this machine. `presets.NAME` in diction.lua (beside MAID's settings.lua) adds a preset, or overrides a
built-in one field by field.
"""

from __future__ import annotations

import os
import shutil

from diction import whisper as whisper_mod
from diction.scribe import resolve_agent, server_answers
from diction.ui import BOLD, DIM, OFF, YELLOW

BUILTIN = {
    "default": {"backend": "claude-cli", "scribe": "haiku", "whisper": "distil-large-v3",
                "note": "narration goes to Anthropic through your claude login, as before"},
    "api": {"backend": "api", "scribe": "haiku-4.5", "whisper": "distil-large-v3",
            "note": "narration goes to Anthropic through its API; needs ANTHROPIC_API_KEY"},
    "local": {"backend": "local", "scribe": "llamacpp-2/Qwen3.5-9B-Q4_K_M-text", "whisper": "large-v3-turbo-q5_0",
              "note": "needs the card to itself (~7.3 GB); nothing leaves the machine"},
    "local-small": {"backend": "local", "scribe": "llamacpp-2/Qwen3.5-4B-Q4_K_M", "whisper": "large-v3-turbo-q5_0",
                    "note": "fits beside a parked ComfyUI (~4.7 GB); nothing leaves the machine"},
}
FIELDS = ("backend", "scribe", "whisper", "note")
# claude-cli: the `claude` process diction always ran. api: a cloud provider through `maid model resolve`.
# local: an OpenAI-compatible MAID server (llama-server), likewise resolved, never a cloud one.
BACKENDS = ("claude-cli", "api", "local")
CLAUDE_MISSING = ("the default scribe runs through the claude CLI, which is not installed; "
                  "--preset api (needs ANTHROPIC_API_KEY) or --preset local keeps working")

# `maid models install` ids for the scribe models the built-in presets name.
SCRIBE_CATALOG = {"Qwen3.5-9B-Q4_K_M-text": "qwen3.5-9b-text", "Qwen3.5-4B-Q4_K_M": "qwen3.5-4b"}


def table(cfg: dict) -> dict[str, dict]:
    """The built-in presets with diction.lua's presets.NAME laid over them, field by field."""
    out = {name: dict(p) for name, p in BUILTIN.items()}
    for name, p in (cfg.get("presets") or {}).items():
        if not isinstance(p, dict):
            raise ValueError(f"presets.{name} in diction's config is not a table")
        out.setdefault(name, {}).update({k: str(v) for k, v in p.items() if k in FIELDS})
    return out


def chosen_name(flag: str | None) -> str:
    return flag or os.environ.get("DICTION_PRESET") or "default"


def choose(flag: str | None, backend: str | None, scribe: str | None, whisper: str | None,
           cfg: dict) -> tuple[str, str, str, str]:
    """(preset, backend, scribe, whisper). Each field on its own: its flag, then its environment variable
    (DICTION_BACKEND, DICTION_AGENT_MODEL, DICTION_MODEL), then the chosen preset (--preset, then DICTION_PRESET),
    then `default`."""
    presets = table(cfg)
    name = chosen_name(flag)
    if name not in presets:
        raise ValueError(f"no preset '{name}' (there: {', '.join(presets)})")
    p, d = presets[name], presets["default"]
    out = (name,
           backend or os.environ.get("DICTION_BACKEND") or p.get("backend") or d["backend"],
           scribe or os.environ.get("DICTION_AGENT_MODEL") or p.get("scribe") or d["scribe"],
           whisper or os.environ.get("DICTION_MODEL") or p.get("whisper") or d["whisper"])
    if out[1] not in BACKENDS:
        raise ValueError(f"no scribe backend '{out[1]}' (there: {', '.join(BACKENDS)})")
    return out


def whisper_state(name: str) -> str:
    try:
        path = whisper_mod.resolve_model(name)
    except RuntimeError:
        return f"{YELLOW}{whisper_mod.missing(name)}{OFF}"
    return "the model whisper-server loaded" if path is None else f"installed ({path.name})"


def scribe_installed(model: str) -> bool:
    """Whether llama-server's router would list MODEL: NAME.gguf, or a folder NAME/ holding a model GGUF."""
    root = whisper_mod.models_root().parent / "llamacpp"
    if (root / f"{model}.gguf").is_file():
        return True
    return any(f.is_file() and not f.name.startswith("mmproj") for f in (root / model).glob("*.gguf"))


def scribe_state(backend: str, name: str) -> str:
    if backend == "claude-cli":
        exe = shutil.which("claude")
        if not exe:
            return f"{YELLOW}claude is not on PATH: {CLAUDE_MISSING}{OFF}"
        return f"claude --model {name}, cloud: the narration's text goes to Anthropic; claude is {exe}"
    try:
        agent = resolve_agent(name)
    except Exception as e:
        return f"{YELLOW}{e}{OFF}"
    where = f"{agent['provider']}/{agent['model']}"
    if backend == "local" and agent.get("remote"):
        return f"{YELLOW}{where} is a cloud model, which the local backend refuses{OFF}"
    if agent.get("remote"):
        if agent.get("api_key_env") and os.environ.get(agent["api_key_env"]):
            key = f"{agent['api_key_env']} is set"
        elif agent.get("api_key_command"):
            key = "api_key_command is configured"
        else:
            key = f"{YELLOW}no key: set {agent.get('api_key_env') or 'its api_key_env'}{OFF}"
        return f"{where}, cloud: the narration's text goes to {agent['provider']}; {key}"
    notes = []
    if agent["provider"].startswith("llamacpp") and not scribe_installed(agent["model"]):
        hint = (f"maid models install {SCRIBE_CATALOG[agent['model']]}" if agent["model"] in SCRIBE_CATALOG
                else f"no {agent['model']} under {whisper_mod.models_root().parent / 'llamacpp'}")
        notes.append(f"{YELLOW}not installed: {hint}{OFF}")
    if server_answers(agent["base_url"]):
        notes.append(f"server answers at {agent['base_url']}")
    else:
        notes.append(f"{YELLOW}server not answering at {agent['base_url']}: maid up {agent['provider']}{OFF}")
    return f"{where}, local; " + "; ".join(notes)


def show(cfg: dict, flag: str | None) -> int:
    """`diction presets`: each preset, its two models and whether they are ready. No call leaves the machine."""
    try:
        presets = table(cfg)
    except ValueError as e:
        print(f"{YELLOW}  {e}{OFF}")
        return 1
    active = chosen_name(flag)
    for name, p in presets.items():
        mark = f" {DIM}(chosen){OFF}" if name == active else ""
        print(f"{BOLD}{name}{OFF}{mark}")
        backend = p.get("backend") or presets["default"]["backend"]
        scribe = p.get("scribe") or presets["default"]["scribe"]
        whisper = p.get("whisper") or presets["default"]["whisper"]
        print(f"  backend  {backend}" if backend in BACKENDS
              else f"  backend  {YELLOW}{backend}: not one of {', '.join(BACKENDS)}{OFF}")
        print(f"  scribe   {scribe}: {scribe_state(backend, scribe)}")
        print(f"  whisper  {whisper}: {whisper_state(whisper)}")
        if p.get("note"):
            print(f"  {DIM}{p['note']}{OFF}")
    return 0

