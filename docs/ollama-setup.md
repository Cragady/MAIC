# Ollama Setup (local-only)

Ollama runs quantized LLMs on the RTX 2080. It is the LLM backend for everything else here, including ComfyUI (through the `comfyui-ollama` nodes). Measurements are in [local-llm-benchmarks.md](local-llm-benchmarks.md).

## Layout

| What | Where | Drive |
| :--- | :--- | :--- |
| Program (binary + bundled CUDA libs, 2.1 GB) | `~/program-files/ollama/v0.35.0/` | main |
| Command | `~/bin/ollama` (symlink) | main |
| Service definition (env, port, drive check) | `MAIC/services/ollama.json`, started with `maic up ollama` | main |
| Models | `/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/ollama/` | external |
| Machine key (created on first run, a few hundred bytes) | `~/.ollama/id_ed25519{,.pub}` | main |
| Source clone (for auditing) | `~/dev2/tools-and-things/ollama` | main |

## Install

Uses the official prebuilt release: it bundles its own CUDA runtime, so no system CUDA toolkit or root access is needed. (Building from source would need `nvcc`, which isn't installed.)

```sh
VER=v0.35.0
mkdir -p ~/program-files/ollama/$VER
cd ~/program-files/ollama

# Download and verify against the published checksum
curl -fL -o ollama-linux-amd64-$VER.tar.zst \
  https://github.com/ollama/ollama/releases/download/$VER/ollama-linux-amd64.tar.zst
curl -fsSL https://github.com/ollama/ollama/releases/download/$VER/sha256sum.txt | grep 'ollama-linux-amd64.tar.zst'
sha256sum ollama-linux-amd64-$VER.tar.zst   # must match the line above
# v0.35.0: 1c114a6b220c5efca2ef2b1e5f01d1e535e26f6cd6d1678c8489325d2835e525

# Extract, drop the archive, link the binary
zstd -dc ollama-linux-amd64-$VER.tar.zst | tar -x -C $VER
rm ollama-linux-amd64-$VER.tar.zst
ln -s ~/program-files/ollama/$VER/bin/ollama ~/bin/ollama

# Model store on the external drive
mkdir -p "/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/ollama"
```

Then build MAIC (see the [README](../README.md#structure-and-build)); `services/ollama.json` is already in the repo.

### Upgrading

Repeat the install with the new `VER`, then repoint the symlink: `ln -sfn ~/program-files/ollama/$VER/bin/ollama ~/bin/ollama`. Models on the external drive are kept. Delete the old version folder once the new one works.

### Uninstall

```sh
rm ~/bin/ollama
rm -rf ~/program-files/ollama ~/.ollama
# Models (external drive), only if you want them gone:
rm -rf "/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/ollama"
```

## The service definition: `services/ollama.json`

MAIC starts Ollama with these environment variables, refuses to start it if the external drive isn't mounted, and keeps its PID in `~/.local/state/maic/run/`. Logs go to `~/.local/state/maic/logs/ollama.log`.

```json
"env": {
  "OLLAMA_MODELS": "/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/ollama",
  "OLLAMA_HOST": "127.0.0.1:11434",
  "OLLAMA_NO_CLOUD": "1",
  "OLLAMA_REMOTES": "none.invalid",
  "OLLAMA_CLOUD_BASE_URL": "http://127.0.0.1:9"
}
```

| Variable | Why |
| :--- | :--- |
| `OLLAMA_MODELS` | Keeps all model blobs on the external drive. The `requires` check stops Ollama from starting without it. |
| `OLLAMA_HOST=127.0.0.1:11434` | Only this machine can reach the server (this is the default; set explicitly so it can't drift). |
| `OLLAMA_NO_CLOUD=1` | **The important one.** Without it, `ollama serve` contacts ollama.com in the background at startup and ~every 4 h (model recommendations + cloud model info), signed with the machine key. Also blocks cloud models, remote model stubs and web search/fetch. |
| `OLLAMA_REMOTES=none.invalid` | Refuses remote-host model stubs even if cloud were re-enabled (empty means `ollama.com`). |
| `OLLAMA_CLOUD_BASE_URL=http://127.0.0.1:9` | Undocumented. Sends any cloud call that gets past `NO_CLOUD` (sign-in/whoami, which it doesn't cover) to a dead local port. Release builds only accept loopback values here. |

Check it took effect: `maic logs ollama` shows `Ollama cloud disabled: true` and `Listening on 127.0.0.1:11434`.

## Daily use

```sh
maic up ollama               # start (maic down ollama to stop, maic status to check)
ollama run qwen3.5:4b        # chat in the terminal
ollama list                  # installed models
ollama ps                    # what's loaded, and how much is on GPU vs CPU
ollama pull <model>          # download (goes to the external drive)
ollama rm <model>            # delete a model
```

Installed models:

| Model | Size | Speed on 2080 | Use |
| :--- | ---: | ---: | :--- |
| `qwen3.5:4b` | 3.4 GB | 80.7 tok/s (100% GPU) | quick back-and-forth |
| `qwen3.5:9b` | 6.6 GB | 21.4 tok/s (79% GPU) | deeper passes |

Qwen3.5 reasons ("thinks") before answering unless told not to. For fast replies send `"think": false` in the API, or `/set nothink` in `ollama run`.

## Privacy audit (v0.35.0)

Read-only review of the source at commit `1abe35e6` (the audited files are identical in `v0.35.0`). Summary:

* **No telemetry, analytics, crash reporting or update checks** in the Linux server/CLI. The update checker exists only in the desktop app (`app/updater`), which isn't in the Linux binary.
* **Background traffic to ollama.com by default** (`server/model_recommendations.go`, `server/model_show_cache.go`): at startup and ~every 4 h. **Disabled by `OLLAMA_NO_CLOUD=1`.**
* **Registry traffic** (`registry.ollama.ai`, redirects to ollama.com / Hugging Face) happens only on `pull`, `push`, `run` of a missing model, or `create` from a missing model. `NO_CLOUD` doesn't block this; it's the explicit download path.
* **Not covered by `NO_CLOUD`:** `ollama signin` / `signout` / pushing to ollama.com (don't run them), the Codex Desktop proxy (`/api/codex/*`, only used if a Codex client is pointed at Ollama), and `ollama launch <tool>` installers.
* **CORS** always allows localhost origins, browser extensions, `file://` and VS Code webviews. Leave `OLLAMA_ORIGINS` unset.
* **Not audited:** the vendored llama.cpp / ggml / MLX C++ code.
* For total isolation, add an OS-level egress block for the server process.

## ComfyUI integration

`comfyui-ollama` (`~/dev2/tools-and-things/comfyui-ollama`, [stavsap/comfyui-ollama](https://github.com/stavsap/comfyui-ollama) at `6db7560`) lets ComfyUI workflows use Ollama's models, so ComfyUI keeps no LLM copies of its own.

```sh
cd ~/dev2/tools-and-things/ComfyUI
ln -s ~/dev2/tools-and-things/comfyui-ollama custom_nodes/comfyui-ollama
uv pip install --python .venv "ollama==0.6.0"
```

* Reviewed: it only talks to the server URL set on its Connectivity node (default `http://127.0.0.1:11434`). No telemetry. `dotenv` in its `requirements.txt` is unused, so only `ollama` is installed.
* **Local patch (uncommitted in the clone):** `OllamaChat` never passed its `think` input to Ollama, so Qwen3.5 always reasoned, ran out of tokens, and returned an empty reply. Fix, `CompfyuiOllama.py`, in the `client.chat(...)` call: add `think=think,`. Re-apply after pulling upstream updates if it isn't fixed there.
* Workflow: **Story chat (Ollama)** in ComfyUI's Workflows sidebar. 4B quick pass with conversation memory; a muted 9B deep pass (Ctrl+M to enable) shares the same history and reviews the whole conversation.
