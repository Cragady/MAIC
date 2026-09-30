# ComfyUI Setup

Local image generation (full-color manga shorts). Runs in its own Python so system Python stays clean. LLM work goes through Ollama: see [ollama-setup.md](ollama-setup.md).

## Layout

| What | Where | Drive |
| :--- | :--- | :--- |
| ComfyUI (git clone) | `~/dev2/tools-and-things/ComfyUI` | main |
| Python 3.13 + packages | `ComfyUI/.venv/` (gitignored) | main |
| Python interpreter (uv-managed) | `~/.local/share/uv/python/` | main |
| Models | `/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/{checkpoints,diffusion_models,loras,text_encoders,vae}/` | external |
| Model path config | `ComfyUI/extra_model_paths.yaml` (gitignored) | main |
| Saved workflows | `ComfyUI/user/default/workflows/` (gitignored) | main |
| Outputs | `ComfyUI/output/` | main |

## Install

Needs `uv` (`~/.local/bin/uv`). System Python (3.14) is never used.

```sh
cd ~/dev2/tools-and-things/ComfyUI

# Python 3.13 venv; uv downloads its own interpreter
uv venv --python 3.13 .venv

# PyTorch with CUDA 13.0 (required for 20-series and newer per ComfyUI's README)
uv pip install --python .venv torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu130
uv pip install --python .venv -r requirements.txt

# Check the GPU is visible
.venv/bin/python -c "import torch; print(torch.__version__, torch.cuda.is_available())"
# -> 2.14.0+cu130 True
```

Create `extra_model_paths.yaml` in the ComfyUI folder:

```yaml
extra_storage:
    base_path: "/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/"
    checkpoints: checkpoints/
    diffusion_models: diffusion_models/
    loras: loras/
    text_encoders: text_encoders/
    vae: vae/
```

The external drive must be mounted before ComfyUI starts, or those models won't show up.

### Update

```sh
git pull && uv pip install --python .venv -r requirements.txt
```

Custom node requirements go into the same venv: `uv pip install --python .venv -r custom_nodes/<node>/requirements.txt`.

### Uninstall / reset

`rm -rf .venv` removes everything installed. Nothing outside the repo depends on it.

## Run

```sh
maic up comfyui       # open http://127.0.0.1:8188
maic down comfyui
maic logs comfyui
```

MAIC starts it from `services/comfyui.json` with `--disable-api-nodes --listen 127.0.0.1 --port 8188`. The Story chat workflow also needs `maic up ollama`.

Useful flags (add them to the `command` in `services/comfyui.json`):

* `--disable-api-nodes`: no paid cloud nodes, and a strict Content-Security-Policy so the browser UI can't fetch from the internet.
* `--disable-metadata`: don't embed prompts/workflows in saved images.

## Privacy notes

* Core ComfyUI makes no telemetry, analytics or update-check requests. Outbound paths are opt-in: API nodes, `--front-end-version` set to a non-default value, and ComfyUI-Manager (`--enable-manager`).
* Custom nodes are arbitrary code: review before installing.
* On disk: `output/` (images embed the prompt + workflow unless `--disable-metadata`), `input/`, `temp/` (cleared on start), `user/` (settings, workflows, `comfyui.db`). Queue history and node caches are RAM-only.

## Models

| File | Folder | Size | Source | Use |
| :--- | :--- | ---: | :--- | :--- |
| `NoobAI-XL-v1.1.safetensors` | `checkpoints/` | 7.1 GB | [Laxhar/noobai-XL-1.1](https://huggingface.co/Laxhar/noobai-XL-1.1) | **Primary** anime/manga model (SDXL) |
| `anima-base-v1.0.safetensors` | `diffusion_models/` | 4.2 GB | circlestone-labs/Anima | Previous primary |
| `anima-turbo-lora-v0.2.safetensors` | `loras/` | 0.1 GB | | Anima speed LoRA |
| `qwen_3_06b_base.safetensors` | `text_encoders/` | 1.2 GB | | Anima text encoder |
| `qwen_image_vae.safetensors` | `vae/` | 0.3 GB | | Anima + Qwen-Image VAE |
| `qwen_image_edit_2509_fp8_e4m3fn.safetensors` | `diffusion_models/` | 20.4 GB | [Comfy-Org/Qwen-Image-Edit_ComfyUI](https://huggingface.co/Comfy-Org/Qwen-Image-Edit_ComfyUI) | Lettering speech bubbles (legible text) |
| `qwen_2.5_vl_7b_fp8_scaled.safetensors` | `text_encoders/` | 9.4 GB | [Comfy-Org/Qwen-Image_ComfyUI](https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI) | Qwen-Image-Edit text encoder |
| `Qwen-Image-Edit-2509-Lightning-4steps-V1.0-bf16.safetensors` | `loras/` | 0.8 GB | [lightx2v/Qwen-Image-Lightning](https://huggingface.co/lightx2v/Qwen-Image-Lightning) | 4-step speed LoRA for Qwen-Image-Edit |
| `qwen3.5_4b_bf16.safetensors` | `text_encoders/` | 9.3 GB | Comfy-Org/Qwen3.5 | ComfyUI-native LLM. **Superseded by Ollama** (1.5 vs 80.7 tok/s); safe to delete |

Download pattern (resumable): `curl -fL -C - -o "<folder>/<file>" "<huggingface resolve URL>"`.

Model shortlist to remember: **NoobAI** (primary), **Illustrious** (SDXL anime family NoobAI builds on, big LoRA ecosystem), **Anima**, **Qwen-Image** (only one that letters legibly).

### NoobAI settings (from the model card)

Euler a, 25-30 steps, CFG 5-6, ~1 MP (832x1216, 896x1152, 1024x1024, ...). Danbooru tags.
Positive start: `masterpiece, best quality, newest, absurdres, highres, safe,`
Negative: `nsfw, worst quality, old, early, low quality, lowres, signature, username, logo, bad hands, mutated hands, mammal, anthro, furry, ambiguous form, feral, semi-anthro`

## Workflows (Workflows sidebar)

| Workflow | What it does |
| :--- | :--- |
| Manga 4-panel short (NoobAI) | 4 panels, shared character tags, fixed seeds, narration captions, stitched 2x2 page |
| Manga 4-panel short (NoobAI) - BETA captions | Same, plus dialogue as caption strips until speech bubbles are ready |
| Manga 4-panel short (Anima) [+ BETA] | Anima versions of the above |
| Story chat (Ollama) | Story co-writer via Ollama: 4B quick pass with memory, optional 9B deep pass |

Text in images: diffusion models garble lettering. Current approach: generate clean panels (negative prompt includes `text, speech bubble`), then add text with the **Draw Text Overlay** node (captions), Krita/GIMP (bubbles), or Qwen-Image-Edit (in progress).
