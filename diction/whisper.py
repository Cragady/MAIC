"""Speech to text through whisper-server (services/whisper.json): each utterance goes to it as a WAV, on loopback.

The route and fields are whisper.cpp's examples/server at the pinned tag: POST /inference, multipart, `file` plus
form fields; `response_format=json` answers {"text": ...} with one line per segment. GET /health is 200 once the
model is loaded and 503 while it loads. POST /load with a `model` path swaps the model in place.
"""

from __future__ import annotations

import functools
import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

DEFAULT_URL = "http://127.0.0.1:8083"


def server_url() -> str:
    return os.environ.get("DICTION_WHISPER_URL", DEFAULT_URL).rstrip("/")


def maic_bin() -> str | None:
    """The maic that started us (it exports MAIC_BIN), else the one on PATH."""
    return os.environ.get("MAIC_BIN") or shutil.which("maic")


@functools.cache
def models_root() -> Path:
    """<models_dir>/whisper: MAIC_MODELS_DIR (maic exports it), else `maic path models`, else MAIC's default."""
    d = os.environ.get("MAIC_MODELS_DIR")
    if not d and (exe := maic_bin()):
        r = subprocess.run([exe, "path", "models"], capture_output=True, text=True, timeout=20)
        d = r.stdout.strip() if r.returncode == 0 else ""
    if not d:
        state = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
        d = str(Path(state) / "maic" / "models")
    return Path(d) / "whisper"


def resolve_model(name: str) -> Path | None:
    """-m: None for "current" (whatever whisper-server loaded at start), else the ggml file it names: a path, or a
    name under <models_dir>/whisper/ as NAME, NAME.bin or ggml-NAME.bin ("distil-large-v3" finds
    ggml-distil-large-v3.bin)."""
    if name in ("", "current"):
        return None
    p = Path(name).expanduser()
    if p.is_file():
        return p.resolve()
    root = models_root()
    for cand in (root / name, root / f"{name}.bin", root / f"ggml-{name}.bin"):
        if cand.is_file():
            return cand.resolve()
    have = sorted(f.name for f in root.glob("*.bin") if f.name != "current.bin") if root.is_dir() else []
    raise RuntimeError(f"no whisper model '{name}' (looked in {root}; there: {', '.join(have) or 'nothing'}); "
                       f"{missing(name)}")


def missing(name: str) -> str:
    """What to say about a model that is not there: a catalog name says how to get it."""
    return "not installed" if "/" in name or name.endswith(".bin") else f"not installed: maic models install whisper-{name}"


def display_name(path: Path | None) -> str:
    """ggml-distil-large-v3.bin -> distil-large-v3; "current" resolves through the link first."""
    if path is None:
        link = models_root() / "current.bin"
        if not link.exists():
            return "current"
        path = link.resolve()
    stem = path.stem
    return stem[5:] if stem.startswith("ggml-") else stem


def multipart(fields: dict[str, str], files: dict[str, tuple[str, bytes, str]]) -> tuple[bytes, str]:
    """A multipart/form-data body by hand, and its content type."""
    boundary = uuid.uuid4().hex
    out = bytearray()
    for name, value in fields.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n").encode()
    for name, (filename, data, ctype) in files.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; filename=\"{filename}\"\r\n"
                f"Content-Type: {ctype}\r\n\r\n").encode()
        out += data + b"\r\n"
    out += f"--{boundary}--\r\n".encode()
    return bytes(out), f"multipart/form-data; boundary={boundary}"


class Whisper:
    def __init__(self, url: str, timeout: float = 60.0):
        self.url = url
        self.timeout = timeout

    def _post(self, path: str, fields: dict, files: dict | None = None) -> bytes:
        body, ctype = multipart(fields, files or {})
        req = urllib.request.Request(self.url + path, data=body, headers={"Content-Type": ctype})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"whisper-server: HTTP {e.code}: {e.read().decode(errors='replace')[:200]}") from None
        except (urllib.error.URLError, OSError) as e:
            raise RuntimeError(f"whisper-server is not answering at {self.url} ({e}): maic up whisper") from None

    def wait_ready(self, limit: float = 120.0):
        """Returns once /health says the model is loaded; 503 means still loading."""
        deadline = time.time() + limit
        while True:
            try:
                with urllib.request.urlopen(self.url + "/health", timeout=5) as r:
                    if r.status == 200:
                        return
            except urllib.error.HTTPError as e:
                if e.code != 503:
                    raise RuntimeError(f"whisper-server: /health answered HTTP {e.code}") from None
            except (urllib.error.URLError, OSError):
                raise RuntimeError(f"whisper-server is not answering at {self.url}: maic up whisper "
                                   f"(it needs a model: maic vendor use whisper FILE; docs/diction.md)") from None
            if time.time() > deadline:
                raise RuntimeError(f"whisper-server at {self.url} is still loading its model after {limit:.0f}s")
            time.sleep(0.5)

    def load(self, model: Path):
        """Swaps the server's model. whisper-server exits when a load fails, so only a ggml file is sent."""
        with model.open("rb") as f:
            if f.read(4) != b"lmgg":
                raise RuntimeError(f"{model} is not a whisper.cpp ggml model")
        self._post("/load", {"model": str(model)})
        self.wait_ready()

    def transcribe(self, wav: bytes) -> str:
        """One utterance: the segments' text joined by spaces, as faster-whisper's segments were."""
        raw = self._post("/inference",
                         {"temperature": "0.0", "response_format": "json", "language": "en", "beam_size": "5"},
                         {"file": ("utterance.wav", wav, "audio/wav")})
        return " ".join(json.loads(raw).get("text", "").splitlines())
