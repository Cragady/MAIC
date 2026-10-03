# Local LLM Benchmarks

> Note (2026-10-01): Ollama was removed from MAID in favour of llama.cpp (docs/llamacpp.md). The Ollama references below are history.

Measured throughput for local LLMs on this machine. Add a row per run.

## Hardware

* **GPU:** NVIDIA GeForce RTX 2080 (TU104, Turing), 8 GB VRAM (7.6 GB usable). No bf16 compute.
* **RAM:** 62 GB
* **Driver:** 595.84 kernel module / 595.91 NVML library (mismatch, reboot pending; `nvidia-smi` fails until then)
* **Model storage:** external NVMe (`/run/media/cragady/Extra Storage/LinBox-Overflow/llm-models/`)

## Method

* Prompt: `Describe a rainy city street at night in about 200 words.`
* `max_length=256`, temperature 0.8, top_k 64, top_p 0.95, min_p 0.05, repetition penalty 1.05, seed 1
* One warmup run, then one timed run. Tokens/s = generated tokens / wall time of the generate call.
* Nothing else on the GPU during the run.

## Results

| Date | Runtime | Model | Quant / dtype | Flags | Result | Tok/s | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- | ---: | :--- |
| 2026-09-29 | ComfyUI 0.38.0 (Generate Text node), torch 2.14.0+cu130 | Qwen3.5 4B | bf16 file, fp32 compute | defaults | OOM | - | Upcasting the 248k-vocab output layer to fp32 needs 2.37 GiB more than dynamic VRAM left free |
| 2026-09-29 | ComfyUI 0.38.0 | Qwen3.5 4B | bf16 file, fp32 compute | `--reserve-vram 2.5` | Crash | - | Gets past OOM, then CUDA graph capture fails copying offloaded (unpinned) weights |
| 2026-09-29 | ComfyUI 0.38.0 | Qwen3.5 4B | bf16 file, fp32 compute | `--disable-nvml-pressure --disable-cuda-graphs` | Works | 1.5 | 247 tokens in ~167 s. Most weights stream from RAM each token |
| 2026-09-29 | Ollama 0.35.0 (CUDA 13 backend) | `qwen3.5:4b` | Ollama default (3.4 GB) | `ollama-serve`, `think:false` | Works | 80.7 | 100% GPU, 3.1 GB VRAM. First load 37 s from external drive, then 3.3 s per 256-token reply |
| 2026-09-29 | Ollama 0.35.0 (CUDA 13 backend) | `qwen3.5:9b` | Ollama default (6.6 GB) | `ollama-serve`, `think:false` | Works | 21.4 | 29/34 layers on GPU (79% GPU / 21% CPU), 6.1 GB. ~12 s per 256-token reply |

## Takeaways

* ComfyUI's built-in LLM path is not usable for interactive brainstorming on a 2080: Turing has no bf16, so a 4B model runs in fp32 (~16 GB) and mostly offloads.
* Ollama's quantized models are the way to run LLMs on this card: the 4B is ~54x faster than ComfyUI's path.
* 4B is ~4x faster than 9B. Use 4B for quick back-and-forth, 9B for deeper passes. The 9B just misses fitting in VRAM, so it will speed up if VRAM is freed (close the desktop's GPU-heavy apps) or a smaller quant is used.
* ComfyUI uses the same GGUFs through MAID's llama-server nodes instead of keeping its own copies. See [comfyui-setup.md](comfyui-setup.md).
