# llama.cpp: the local model server

MAID vendors [llama.cpp](https://github.com/ggml-org/llama.cpp) and runs its `llama-server` on `127.0.0.1:8081` as the default provider (`llamacpp/current`). It is the only local backend: Ollama was removed on 2026-10-01 because llama-server does everything it did and exposes the rest of the sampler chain besides.

## Models are files under one directory

`services/llamacpp.json` runs llama-server in router mode over `<models_dir>/llamacpp/` (`models_dir` in settings; `${MAID_MODELS}` in service files). Every GGUF there is a model whose name is the file's stem; a model with a vision projector goes in a subdirectory named after it, holding the GGUF and an `mmproj-*.gguf`, and the server picks both up. `:models` lists them, `--model llamacpp/NAME` or `:model llamacpp/NAME` selects one, and the server loads it on demand, unloading the previous one (`--models-max 1`; a second resident model is the second server, below). `llamacpp/current`, the default, means whichever file `maid vendor use llamacpp PATH` linked; MAID resolves it to the real name, so the status line always shows what is loaded.

A bare model name (`--model Qwen3.5-9B-Q4_K_M`) goes to the first provider, `llamacpp`. (A provider you configure under a name matching `[Oo]llama` is never chosen for a bare name, since its tags look like bare names; write `NAME/model` to reach it.)

## Context size

One number, `context` (default 16384), drives both the server's `--ctx-size` (through `${MAID_CONTEXT}` in `services/llamacpp.json`) and MAID's usage readout and auto-compaction. Set it with `--ctx 32768` on any `maid` run (the running server is restarted to match), `maid up llamacpp --ctx 32768`, `:ctx 32768` in a session, or `context = 32768` in settings. The 4B at Q4_K_M fits 32k on an 8 GB card; the 9B is tighter. A request that still exceeds the window is compacted and retried by MAID (see `:h compact`).

## Two servers

One llama-server keeps one model resident (`--models-max 1`): ask it for another and it evicts the first. When two models are wanted at the same time, the Story chat's quick and deep passes, or a reviewer beside the session's model, MAID runs a second server. `services/llamacpp-2.json` is the same `llama-server` in router mode over the same `<models_dir>/llamacpp/`, on `127.0.0.1:8082`, with its own context size, `context_2` (default 8192; `${MAID_CONTEXT_2}` in the service file; `--ctx2 N`, `:ctx2 N`). `maid up llamacpp-2` starts it; it loads nothing until asked, so starting it never evicts the main server's model. The provider `llamacpp-2` is shipped (`http://127.0.0.1:8082/v1`), so `llamacpp-2/Qwen3.5-4B-Q4_K_M` is a model like any other, and a preset puts a short name on it:

```lua
models = { ["qwen-4b-side"] = { model = "llamacpp-2/Qwen3.5-4B-Q4_K_M", context = 8192 } }
```

**The reviewer never evicts the main model.** With `reviewer_model = "llamacpp-2/Qwen3.5-4B-Q4_K_M"` and the session's model on 8081, the smart harness reviews on the side server and the main model stays loaded. The default does this by itself: with `reviewer_model` empty and the model on `llamacpp`, a review goes to `llamacpp-2` with the same model name whenever that server answers, and to the main server as before when it does not.

**ComfyUI.** The Story chat workflow's "Deep model (llamacpp-2)" node points at 8082 and the quick one at 8081, so both models stay loaded through a run; `vendor/comfyui-maic-llamacpp/README.md` has the node details.

**Does it fit?** `maid gpu` and `maid doctor` end with one sentence: each GGUF's size on disk (the mmproj beside it counts) plus a rough KV cache estimate from its context (about 65 MB per 1k tokens for a 4B, 130 for a 9B; the figure is labelled an estimate), against the card's total from ComfyUI's `/system_stats` when it is up, else `nvidia-smi`, else "unknown". For example `4B at 16k (4.0 GB est.) + 4B at 8k (3.5 GB est.) = 7.6 GB of 8.0 GB: fits with ComfyUI stopped`. The side server's window, `context_2`, is the one to lower first. `maid gpu free llamacpp-2` unloads the side server's model alone; `maid gpu free` unloads both.

## Sharing an 8 GB card with ComfyUI

They take turns. `maid up comfyui` (the service is marked `needs_gpu`) first asks each llama server to unload whatever model it holds and says so; the servers stay up and reload the model on the next request, once ComfyUI has let go of its weights. `maid up whisper` does the same and also asks a running ComfyUI to unload its models (ComfyUI's `/free`; it reloads them on its next run). whisper-server has no unload, so a running whisper is named instead (`maid down whisper` releases it). A load that fails with a CUDA out-of-memory means the other side still holds the card: `maid gpu` shows who, `maid gpu free` releases every side that can let go without stopping anything (the llama servers and ComfyUI), and a failed `maid up` says exactly that instead of an exit code.

## One model, two entries

A folder with an `mmproj` file always loads with vision, which costs about a gigabyte and, for the 9B on an 8 GB card, limits the context to 8k. To run the same weights as a text model, make a second folder holding a link to the GGUF and no projector: `Qwen3.5-9B-Q4_K_M-text/Qwen3.5-9B-Q4_K_M-text.gguf -> ../Qwen3.5-9B-Q4_K_M/Qwen3.5-9B-Q4_K_M.gguf`. The router lists it as its own model, nothing is duplicated, and the presets use both: `qwen-9b` is the text entry at 16k, `qwen-9b-vision` the projector entry at 8k. The router reads the folder at start, so a new entry appears after `maid down llamacpp` and `maid up llamacpp`. `maid models install qwen3.5-9b-text` makes exactly this folder and link (installing `qwen3.5-9b` first), and `maid models remove qwen3.5-9b` refuses while the text entry is installed ([models.md](models.md)).

## Getting a model

`maid models` is the catalog: the Qwen3.5 GGUFs above (and the speech and code completion models), each pinned to a commit with its hash, `maid models install ID [--link]` to fetch one, what each is for and what fits an 8 GB card ([models.md](models.md)). By hand:

`maid vendor model llamacpp URL SHA256` downloads a GGUF with curl into `<models_dir>/llamacpp/` (or `--into DIR`), refuses to keep it unless the SHA-256 matches, and links it as the current model (a file named `mmproj-*` is a projector: saved where asked, never linked). `maid setup` offers the catalog's agent and vision entries. Hugging Face shows the hash under a file's LFS details; its API gives it as `?blobs=true` on the tree listing. Use an upstream GGUF, for example `unsloth/Qwen3.5-4B-GGUF`'s `Qwen3.5-4B-Q4_K_M.gguf`, with `mmproj-F16.gguf` beside it for images. (Blobs from an Ollama store do not load: its Qwen3.5 files carry that engine's own metadata, three rope sections where llama.cpp expects four, with the vision tower packed into the same file.)

## What llama-server exposes

The whole sampler chain, and a few things a pull-and-run wrapper has no equivalent for (this list is why Ollama went):

| What | On llama-server |
| :--- | :--- |
| XTC (`xtc_probability`, `xtc_threshold`) | yes |
| DRY repetition penalty (`dry_multiplier`, `dry_base`, `dry_allowed_length`, `dry_penalty_last_n`) | yes |
| top-n-sigma (`top_n_sigma`), typical-p, mirostat | yes |
| `logit_bias` (token bans, see [bans.md](bans.md)) | yes, by id or text |
| GBNF grammar (`grammar`) and JSON schema (`json_schema`) | yes |
| Prefill: the model continues a partial assistant turn instead of starting a new one | yes (the server's default when the last message is from the assistant) |
| Token probabilities in the reply (`n_probs`) | yes |
| Context size, GPU layers, KV cache type, batch sizes | flags on the command line, in `services/llamacpp.json` |

MAID reaches all of the request-level ones through the OpenAI-compatible endpoint: `:sampling` and `providers.llamacpp.options.sampling` are merged into every request as they are, so any key llama-server understands can be set (see the mapping below). Prefill and `n_probs` are server abilities MAID does not drive yet: MAID never ends a request with an assistant message, and it ignores probability fields in the reply. They are listed so you know they are there for a curl or a script against port 8081.

llama.cpp also ships the tools around GGUF files: `llama-quantize` (make a smaller quantization from a larger one), `llama-gguf-split` (split and merge sharded files), and, separate from serving and honestly still marked work in progress upstream, `llama-finetune` (LoRA-style training of an FP32 model; small models only on an 8 GB card) and `llama-export-lora` (merge a LoRA adapter into a base model). MAID builds the first two; the training tools are one extra build target away (`cmake --build ~/.local/state/maid/vendor/llama.cpp-build --target llama-finetune llama-export-lora`). None of this touches serving: a LoRA adapter is loaded into `llama-server` with `--lora`, added to the command in `services/llamacpp.json`.

## Install

```sh
maid vendor add llamacpp     # fetches the pinned submodule, builds out of tree; about ten minutes with CUDA
maid vendor                  # llamacpp  b11284  installed  -> ...; model: none
```

The build goes to `~/.local/state/maid/vendor/llama.cpp-build/` (Release, `GGML_CUDA=ON` when `nvcc` is found on `PATH` or under `/usr/local/cuda`, CPU otherwise) and `~/.local/state/maid/vendor/llamacpp/bin` links to its `bin/`: `llama-server`, `llama-cli`, `llama-quantize`, `llama-gguf-split`. The build has no TLS (`LLAMA_OPENSSL=OFF`), so the binaries cannot fetch models from Hugging Face or any other https host; the server runs on loopback and never needs to. Nothing in MAID's core talks to the network; `add` is a build from the checkout already in the repo. The pin is `vendor/manifest.json` (`llamacpp`, tag `b11284`), like the other vendored services in [vendor.md](vendor.md).

A checkout you already have: `maid vendor adopt llamacpp /path/to/llama.cpp`, then `maid vendor add llamacpp` builds it the same way.

## Get a GGUF

`maid vendor model llamacpp URL SHA256` (above) downloads and verifies one. **Or a file of your own** (a download from Hugging Face, or a quantization you made with `llama-quantize`): put it under `<models_dir>/llamacpp/` and point the link at it.

```sh
maid vendor use llamacpp /path/to/Qwen3.5-9B-Q4_K_M.gguf
```

`vendor use` creates exactly one symlink, `~/.local/state/maid/vendor/llamacpp/current-model.gguf`, and refuses a missing path or a file that is neither `.gguf` nor GGUF by its header (a suffixless file is accepted when its header says GGUF). The same link by hand:

```sh
ln -sfn /path/to/model.gguf ~/.local/state/maid/vendor/llamacpp/current-model.gguf
```

One model at a time; `vendor use` again to switch, then restart the service. `maid vendor` and `maid doctor` show which file is current.

## Run and use

```sh
maid up llamacpp             # llama-server on 127.0.0.1:8081, loads current-model.gguf; maid logs llamacpp
maid                         # llamacpp/current is the default model
```

`services/llamacpp.json` runs `llama-server --host 127.0.0.1 --port 8081 --model .../current-model.gguf --ctx-size 16384 --jinja -ngl 99 --alias current`. `--jinja` makes the server use the model's own chat template, which is what tool calling needs. `-ngl 99` puts every layer on the GPU; a CPU build ignores it with a warning. A 9B at Q4_K_M with 16k context is about the ceiling of an 8 GB card; if the server dies while loading, lower `--ctx-size` or `-ngl` in the service file, or add `--cache-type-k q8_0 --cache-type-v q8_0` (needs `--flash-attn on`) to halve the KV cache.

**`llamacpp/current` is the default model**, and `current` is the name `--alias` gives whatever GGUF is linked, so `/v1/models` reports it and `:model llamacpp/current` always means "the linked one". llama-server serves the one file it loaded and ignores the `model` field of the request, so any other name after `llamacpp/` reaches the same model; `maid --model llamacpp/qwen3.5-9b` is a way to make the transcript say which GGUF was current at the time.

If the model is not running, the first turn fails with the reason and what to do: `llamacpp is not running: maid up llamacpp (needs a model: maid vendor use llamacpp /path/model.gguf; maid vendor shows what is linked)`. `maid up llamacpp` refuses with the same hint until a GGUF is linked.

The provider is shipped: kind `openai`, `base_url` `http://127.0.0.1:8081/v1`, no key, local (no `REMOTE` in the status line). Override it by name in `providers` if you move the port.

## Samplers, `:sampling` and bans

`:sampling KEY VALUE` (or `sampling = { ... }` in settings, or `providers.llamacpp.options.sampling`) sends the key with every request, and llama-server reads it by the same name. Everything below works today; the first group is what MAID documents for every provider, the rest is llama.cpp's own.

| `:sampling` key | Does |
| :--- | :--- |
| `temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty` | the usual; llama-server's defaults are temperature 0.8, top_k 40, top_p 0.95, min_p 0.05 |
| `xtc_probability`, `xtc_threshold` (or `:sampling xtc P T`) | exclude top choices, see [bans.md](bans.md) |
| `dry_multiplier`, `dry_base`, `dry_allowed_length`, `dry_penalty_last_n` | DRY: penalises repeating a sequence it already produced; `dry_multiplier 0.8` is the common starting point |
| `top_n_sigma` | keeps tokens within N standard deviations of the top logit; `1.0` to `1.5` |
| `typical_p`, `mirostat`, `mirostat_tau`, `mirostat_eta` | the older samplers, all still there |
| `n_predict` | reply length cap; llama.cpp's name for `max_tokens` |
| `grammar` | a GBNF grammar the reply must match |
| `json_schema` | a JSON schema, compiled to a grammar server-side |
| `samplers` | the order of the chain, as a list of names (`{"dry", "top_k", "min_p", "xtc", "temperature"}` in Lua); the server default is penalties, dry, top_n_sigma, top_k, typ_p, top_p, min_p, xtc, temperature |

Bans: `:ban token` and `bans.tokens` become `logit_bias` with the token at minus infinity, which llama-server takes by id or by text (a text entry bans each token the text tokenizes to). String and regex bans work as on every provider, on MAID's side. `grammar` and `json_schema` constrain what the model *can* say; they cannot express "anything except X", which is why the bans exist (the reasoning is in [bans.md](bans.md)).

MAID sends nothing of this to Anthropic, so a `sampling` table tuned for llama.cpp is safe to leave in settings when you switch provider.

## Where things are

| | |
| :--- | :--- |
| checkout | `vendor/llama.cpp` (submodule, tag `b11284`), linked as `~/.local/state/maid/vendor/llama.cpp` |
| build | `~/.local/state/maid/vendor/llama.cpp-build/` |
| binaries | `~/.local/state/maid/vendor/llamacpp/bin` -> the build's `bin/` |
| model | `~/.local/state/maid/vendor/llamacpp/current-model.gguf` -> your GGUF |
| service | `services/llamacpp.json`; log in `~/.local/state/maid/logs/llamacpp.log` |
| provider | `llamacpp`, `http://127.0.0.1:8081/v1`, in `default_providers()`; `llamacpp/current` is the default model |
| side server | `services/llamacpp-2.json` on `127.0.0.1:8082`, provider `llamacpp-2`, context `context_2`; log in `~/.local/state/maid/logs/llamacpp-2.log` |
