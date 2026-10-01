# Vendored services

MAIC can install the services it drives (llama.cpp, the model server; ComfyUI), so nothing has to live in `~/program-files` or a random clone. Everything sits in one tree:

```
~/.local/state/maic/
├── vendor/
│   ├── ComfyUI          -> the pinned submodule checkout (or an install you adopted)
│   ├── llama.cpp        -> the pinned submodule checkout (or an adopted one)
│   ├── llama.cpp-build/ its out-of-tree CMake build (bin/llama-server, ...)
│   ├── llamacpp/
│   │   ├── bin                -> ../llama.cpp-build/bin
│   │   └── current-model.gguf -> the GGUF llama-server loads (maic vendor use llamacpp PATH)
├── workflows/comfyui/   your saved ComfyUI workflows, editable (ComfyUI's own folder points here)
├── templates/comfyui/   your own templates: originals, shown in ComfyUI's template browser, opened as copies
├── sessions/            transcripts
└── logs/                service logs
```

`services/*.json` reach everything through `${MAIC_VENDOR}` and `${MAIC_STATE}`, and `maic artifacts` shows each item with where it really is:

```
comfyui/outputs       14.4 MB    12     ~/.local/state/maic/vendor/ComfyUI/output -> ~/dev2/tools-and-things/ComfyUI/output
comfyui/workflows     52.1 KB    5      ~/.local/state/maic/workflows/comfyui
```

## What is pinned

`vendor/manifest.json` is the single source of truth. Each service is a submodule checked into the MAIC repo at a tag or commit. Nothing tracks a branch head.

| Service | Submodule | Pinned | Install |
| :--- | :--- | :--- | :--- |
| `comfyui` | `vendor/ComfyUI` | `v0.38.0` | `vendor/comfyui.sh`: uv venv with Python 3.13 (system Python untouched), CUDA 13 torch, requirements, MAIC's custom nodes linked in (`vendor/comfyui-maic-llamacpp`, the chat nodes for llama-server; `vendor/comfyui-maic-templates`, the template shelf), `extra_model_paths.yaml` from `models_dir`, workflows moved into the artifact tree |
| `llamacpp` | `vendor/llama.cpp` | `b11284` | `vendor/llamacpp.sh`: CMake out of tree into `llama.cpp-build/`, Release, CUDA when `nvcc` is found, no TLS (the binaries cannot download models), targets `llama-server llama-cli llama-quantize llama-gguf-split`, `llamacpp/bin` link. See [llamacpp.md](llamacpp.md) |

Privacy is the same as the hand-built setup: ComfyUI runs with `--disable-api-nodes --disable-auto-launch --listen 127.0.0.1`; llama-server runs with `--host 127.0.0.1` from a build without TLS, models on the external drive. Those are in `services/*.json`, not in the vendored code.

## Commands

```sh
maic vendor                      # each service: pinned version, installed / linked / missing, where it points
maic vendor add comfyui          # fetch the submodule, apply patches, run the install script (network)
maic vendor adopt comfyui ~/dev2/tools-and-things/ComfyUI     # use an install you already have
maic vendor add llamacpp         # build the submodule out of tree (about ten minutes, no download)
maic vendor use llamacpp /path/to/model.gguf   # the GGUF llama-server loads (llamacpp.md)
maic vendor wire comfyui         # redo the links and the maic: block of extra_model_paths.yaml (offline, idempotent)
maic vendor unlink comfyui       # stop using it; nothing is deleted
maic setup                       # a guided first run over all of this, one yes/no per step
```

`add` and `vendor model` are the only things in MAIC that reach the internet, and only because you typed them. It refuses while the harness is tripped. `adopt` moves nothing: it creates the link, reads the checkout's git remote and ref (a note when the remote is not the upstream, the ref against the manifest's), and runs the wiring. `maic vendor wire NAME` is that wiring on its own, idempotent and offline: the install script's `wire` step (custom node links, the workflows folder), and for ComfyUI the `maic:` block of `extra_model_paths.yaml`, below.

## Model paths for ComfyUI

`vendor/manifest.json` carries a `models` map for `comfyui`: ComfyUI model category to subfolder under `models_dir` (checkpoints, diffusion_models, loras, text_encoders, vae, clip_vision, embeddings, controlnet, upscale_models, style_models, model_patches). `maic vendor wire comfyui` (and `add`, and `adopt`) rewrites only the `maic:` root key of `<ComfyUI>/extra_model_paths.yaml` from it, with `base_path` set to `models_dir`; every other root key in the file stays as it is, so an adopted install keeps its own entries, and running it again changes nothing. No symlinking into `ComfyUI/models` is needed: ComfyUI reads the extra paths beside its own. A new category means a line in the manifest and `maic vendor wire comfyui`.

## Docker as a runtime

A service file may say `"runtime": "docker"` instead of running a host process. `services/comfyui-docker.json.example` shows the shape (it is not loaded: `load_services` skips `.example` files; copy it to `comfyui-docker.json` and take `comfyui.json` out, since both want port 8188):

```jsonc
{
  "name": "comfyui",
  "runtime": "docker",
  "image": "ghcr.io/example/comfyui:latest",      // you pull it; MAIC never does
  "command": ["--listen", "0.0.0.0", "--port", "8188"],   // appended after the image
  "port": 8188,                                   // -p 127.0.0.1:8188:8188, always loopback, never --network host
  "gpu": true,                                    // --gpus all
  "volumes": { "${MAIC_MODELS}": "/models" },     // host paths only under ${MAIC_STATE}, ${MAIC_MODELS} or ${MAIC_VENDOR}
  "env": { "PYTHONUNBUFFERED": "1" },             // -e
  "ready_pattern": "To see the GUI go to"
}
```

`maic up` runs `docker run --rm -d --name maic-<service> ...` and records the container name where a pid file would go; `maic status` asks `docker inspect` and shows `[docker]` and `container maic-comfyui` where a host service shows `[host]` and its pid; `maic down` is `docker stop`; `maic logs` reads `docker logs` while the container runs (MAIC's own log keeps the start headers and docker's errors, for a run that failed). A volume outside MAIC's three trees is refused when the file loads. `needs_gpu` works the same as for a host service.

## Health

`maic status` (and `:status`) show under each running service what it holds: a llama server's resident model from `/v1/models` (or `no model resident`), ComfyUI's VRAM in use and whether its queue is busy (`/system_stats`, `/queue`). A service file's `ready_pattern` is a POSIX extended regex that must match the service's output since this start, besides the open port, before `maic up` says ready (`To see the GUI go to` for ComfyUI, `listening on` for llama-server); a server that bound its port but is still loading shows as `starting`. `maic doctor` compares the CUDA version ComfyUI's torch was built for (`.venv/bin/python -c "import torch; print(torch.version.cuda)"`, only when the venv exists) with the one the driver supports (`nvidia-smi`) and says whether they agree, naming the fix when they do not.

## First run: `maic setup`

`maic setup` chains the pieces above for a machine that has nothing yet: the prerequisites (python3, git, uv, curl, bubblewrap, the tripwire; docker and nvidia-smi as optional), then one yes/no question per step: write the global settings file (it asks for `models_dir`), build llama.cpp (`maic vendor add llamacpp`), install ComfyUI (`maic vendor add comfyui`), fetch a known Qwen3.5 GGUF with its SHA-256 (`maic vendor model`; the 4B with its vision projector, or the 9B; which one is recommended comes from the card), install the tripwire (`sudo ./harness/install-tripwire.sh`), and it ends with `maic status`. Nothing runs without a yes. Off a terminal it prints the plan and exits 2.

## Updating a pinned version

1. Change `ref` in `vendor/manifest.json`, `git -C vendor/ComfyUI checkout <tag>` (or `vendor/llama.cpp`, whose tags are `b<build>`) and commit the submodule pointer.
2. `maic vendor add NAME` again: it checks out the new ref, re-applies patches (already-applied ones are skipped), and re-runs the install script, which updates packages in place.

## Settings

`models_dir` in settings is where model files live (`llamacpp/` for the GGUFs, and the ComfyUI folders named by the manifest's `models` map: `checkpoints/ diffusion_models/ loras/ text_encoders/ vae/ ...`). `maic vendor wire comfyui` writes the `maic:` block of `extra_model_paths.yaml` from it; `maic setup` asks for it when writing the settings file.

## Workflows and templates

ComfyUI has three kinds of workflow files, and only one of them is yours to edit:

| Artifact | What | Editable? |
| :--- | :--- | :--- |
| `comfyui/workflows` | what you save in the UI; `~/.local/state/maic/workflows/comfyui/` | yes, that is the point |
| `comfyui/templates` | your originals; `~/.local/state/maic/templates/comfyui/`. A no-op custom node (`vendor/comfyui-maic-templates`) serves this folder in ComfyUI's template browser, so opening one creates a new workflow and saving goes to `workflows`, never back into the template | by you, on purpose; never by the UI |
| `comfyui/builtin-templates` | the templates ComfyUI ships (a pip package, 594 files); replaced on upgrade | no. Copy one into `templates/` to make it yours |

So to change something while keeping the original safe: put the original in `templates/`, open it from the template browser, edit, save. `maic artifacts` shows all three with sizes.

## LuaJIT

`vendor/lua-pins` is Micaiah's `lua-collection-nvim-pins`, pinned at `37bb6c6`: the exact LuaJIT and PUC Lua revisions Neovim v0.12.2 builds against, each with its own PROVENANCE, upstream hash and `verify.sh`. `vendor/luajit.cmake` copies the LuaJIT tree into the build directory and runs its own Makefile there (`libluajit.a`, static, PIC), so the submodule is never written to. It links into `maic_core` and powers `:lua`, `:luafile`, Lua mode in the session, and the `maic lua` REPL (see `:h lua`). This is the first of the polyglot tool runtimes from the roadmap.

## Windows

Same tree under `%LOCALAPPDATA%\maic\` with junctions instead of symlinks; the scripts would become PowerShell. Not built yet; see [roadmap.md](roadmap.md).
