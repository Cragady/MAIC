# llama.cpp: the local model server

MAIC vendors [llama.cpp](https://github.com/ggml-org/llama.cpp) and runs its `llama-server` on `127.0.0.1:8081` as the default provider (`llamacpp/current`). Ollama stays installed as the optional second backend; the GGUF files are the same, and the ones Ollama pulled can be served here directly.

## Models are files under one directory

`services/llamacpp.json` runs llama-server in router mode over `<models_dir>/llamacpp/` (`models_dir` in settings; `${MAIC_MODELS}` in service files). Every GGUF there is a model whose name is the file's stem; a model with a vision projector goes in a subdirectory named after it, holding the GGUF and an `mmproj-*.gguf`, and the server picks both up. `:models` lists them, `--model llamacpp/NAME` or `:model llamacpp/NAME` selects one, and the server loads it on demand, unloading the previous one (`--models-max 1`, since an 8 GB card holds one at a time; raise it in the service file on a bigger card). `llamacpp/current`, the default, means whichever file `maic vendor use llamacpp PATH` linked; MAIC resolves it to the real name, so the status line always shows what is loaded.

A bare model name (`--model qwen3.5:9b`) never reaches Ollama by accident: it goes to the first provider whose name does not match `[Oo]llama`. Ollama is used only when written as `ollama/NAME`.

## Context size

One number, `context` (default 16384), drives both the server's `--ctx-size` (through `${MAIC_CONTEXT}` in `services/llamacpp.json`) and MAIC's usage readout and auto-compaction. Set it with `--ctx 32768` on any `maic` run (the running server is restarted to match), `maic up llamacpp --ctx 32768`, `:ctx 32768` in a session, or `context = 32768` in settings. The 4B at Q4_K_M fits 32k on an 8 GB card; the 9B is tighter. A request that still exceeds the window is compacted and retried by MAIC (see `:h compact`).

## Sharing an 8 GB card with ComfyUI

They take turns. `maic up comfyui` (the service is marked `needs_gpu`) first asks llama-server to unload whatever model it holds and says so; the server stays up and reloads the model on the next request, once ComfyUI has let go of its weights. A load that fails with a CUDA out-of-memory means the other side still holds the card: `maic gpu` shows who, `maic gpu free` releases both sides without stopping anything, and a failed `maic up` says exactly that instead of an exit code. Two models resident at once is roadmap item 1.

## Getting a model

`maic vendor model llamacpp URL SHA256` downloads a GGUF with curl into `<models_dir>/llamacpp/` (or `--into DIR`), refuses to keep it unless the SHA-256 matches, and links it as the current model. Hugging Face shows the hash under a file's LFS details; its API gives it as `?blobs=true` on the tree listing. The Ollama blobs on this machine do not work here: Ollama's Qwen3.5 files carry its own engine's metadata (three rope sections where llama.cpp expects four, and the vision tower packed into the same file), so a proper upstream GGUF is needed, for example `unsloth/Qwen3.5-4B-GGUF`'s `Qwen3.5-4B-Q4_K_M.gguf`, with `mmproj-F16.gguf` beside it for images.

## Why, when Ollama is already there

Ollama is the easy path: pull, run, done. It also decides a lot for you, and its API exposes a short list of options (`temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty`, `num_predict`). llama-server exposes the whole sampler chain and a few things Ollama has no equivalent for:

| What | On llama-server | On Ollama |
| :--- | :--- | :--- |
| XTC (`xtc_probability`, `xtc_threshold`) | yes | no |
| DRY repetition penalty (`dry_multiplier`, `dry_base`, `dry_allowed_length`, `dry_penalty_last_n`) | yes | no |
| top-n-sigma (`top_n_sigma`), typical-p, mirostat | yes | no |
| `logit_bias` (token bans, see [bans.md](bans.md)) | yes, by id or text | no |
| GBNF grammar (`grammar`) and JSON schema (`json_schema`) | yes | JSON mode only |
| Prefill: the model continues a partial assistant turn instead of starting a new one | yes (the server's default when the last message is from the assistant) | no |
| Token probabilities in the reply (`n_probs`) | yes | no |
| Context size, GPU layers, KV cache type, batch sizes | flags on the command line, in `services/llamacpp.json` | per model, through a Modelfile |

MAIC reaches all of the request-level ones through the OpenAI-compatible endpoint: `:sampling` and `providers.llamacpp.options.sampling` are merged into every request as they are, so any key llama-server understands can be set (see the mapping below). Prefill and `n_probs` are server abilities MAIC does not drive yet: MAIC never ends a request with an assistant message, and it ignores probability fields in the reply. They are listed so you know they are there for a curl or a script against port 8081.

llama.cpp also ships the tools around GGUF files: `llama-quantize` (make a smaller quantization from a larger one), `llama-gguf-split` (split and merge sharded files), and, separate from serving and honestly still marked work in progress upstream, `llama-finetune` (LoRA-style training of an FP32 model; small models only on an 8 GB card) and `llama-export-lora` (merge a LoRA adapter into a base model). MAIC builds the first two; the training tools are one extra build target away (`cmake --build ~/.local/state/maic/vendor/llama.cpp-build --target llama-finetune llama-export-lora`). None of this touches serving: a LoRA adapter is loaded into `llama-server` with `--lora`, added to the command in `services/llamacpp.json`.

## Install

```sh
maic vendor add llamacpp     # fetches the pinned submodule, builds out of tree; about ten minutes with CUDA
maic vendor                  # llamacpp  b11284  installed  -> ...; model: none
```

The build goes to `~/.local/state/maic/vendor/llama.cpp-build/` (Release, `GGML_CUDA=ON` when `nvcc` is found on `PATH` or under `/usr/local/cuda`, CPU otherwise) and `~/.local/state/maic/vendor/llamacpp/bin` links to its `bin/`: `llama-server`, `llama-cli`, `llama-quantize`, `llama-gguf-split`. The build has no TLS (`LLAMA_OPENSSL=OFF`), so the binaries cannot fetch models from Hugging Face or any other https host; the server runs on loopback and never needs to. Nothing in MAIC's core talks to the network; `add` is a build from the checkout already in the repo. The pin is `vendor/manifest.json` (`llamacpp`, tag `b11284`), like the other vendored services in [vendor.md](vendor.md).

A checkout you already have: `maic vendor adopt llamacpp /path/to/llama.cpp`, then `maic vendor add llamacpp` builds it the same way.

## Get a GGUF

**Use the one Ollama already has.** Every Ollama model is a GGUF file in Ollama's blob store, and `llama-server` can load it directly. No download, no second copy on the drive:

```sh
ollama show --modelfile qwen3.5:9b | grep ^FROM
# FROM /path/to/ollama/models/blobs/sha256-<long hash>
maic vendor use llamacpp /path/to/ollama/models/blobs/sha256-<long hash>
```

The blobs live under the `OLLAMA_MODELS` directory named in `services/ollama.json`. A blob has no `.gguf` suffix; `maic vendor use` reads the file's header instead, so it is accepted. A vision model has a second, smaller blob for the image projector; `llama-server` serves the text model without it, and takes it with `--mmproj` if you want images too. Do not delete the model from Ollama while llama.cpp is linked to its blob.

**Or a file of your own** (a download from Hugging Face, or a quantization you made with `llama-quantize`): put it under `models_dir` from `settings.json`, or anywhere, and point the link at it.

```sh
maic vendor use llamacpp /path/to/Qwen3.5-9B-Q4_K_M.gguf
```

`vendor use` creates exactly one symlink, `~/.local/state/maic/vendor/llamacpp/current-model.gguf`, and refuses a missing path or a file that is neither `.gguf` nor GGUF by its header. The same link by hand:

```sh
ln -sfn /path/to/model.gguf ~/.local/state/maic/vendor/llamacpp/current-model.gguf
```

One model at a time; `vendor use` again to switch, then restart the service. `maic vendor` and `maic doctor` show which file is current.

## Run and use

```sh
maic up llamacpp             # llama-server on 127.0.0.1:8081, loads current-model.gguf; maic logs llamacpp
maic                         # llamacpp/current is the default model
```

`services/llamacpp.json` runs `llama-server --host 127.0.0.1 --port 8081 --model .../current-model.gguf --ctx-size 16384 --jinja -ngl 99 --alias current`. `--jinja` makes the server use the model's own chat template, which is what tool calling needs. `-ngl 99` puts every layer on the GPU; a CPU build ignores it with a warning. A 9B at Q4_K_M with 16k context is about the ceiling of an 8 GB card; if the server dies while loading, lower `--ctx-size` or `-ngl` in the service file, or add `--cache-type-k q8_0 --cache-type-v q8_0` (needs `--flash-attn on`) to halve the KV cache.

**`llamacpp/current` is the default model**, and `current` is the name `--alias` gives whatever GGUF is linked, so `/v1/models` reports it and `:model llamacpp/current` always means "the linked one". llama-server serves the one file it loaded and ignores the `model` field of the request, so any other name after `llamacpp/` reaches the same model; `maic --model llamacpp/qwen3.5-9b` is a way to make the transcript say which GGUF was current at the time. `model = "ollama/qwen3.5:4b"` (or a bare Ollama name) in `settings.lua` makes Ollama the default again; both stay available and `:model` switches between them mid-session.

If the model is not running, the first turn fails with the reason and what to do: `llamacpp is not running: maic up llamacpp (needs a model: maic vendor use llamacpp /path/model.gguf; maic vendor shows what is linked)`. `maic up llamacpp` refuses with the same hint until a GGUF is linked.

The provider is shipped: kind `openai`, `base_url` `http://127.0.0.1:8081/v1`, no key, local (no `REMOTE` in the status line). Override it by name in `providers` if you move the port.

## Samplers, `:sampling` and bans

`:sampling KEY VALUE` (or `sampling = { ... }` in settings, or `providers.llamacpp.options.sampling`) sends the key with every request, and llama-server reads it by the same name. Everything below works today; the first group is what MAIC documents for every provider, the rest is llama.cpp's own.

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

Bans: `:ban token` and `bans.tokens` become `logit_bias` with the token at minus infinity, which llama-server takes by id or by text (a text entry bans each token the text tokenizes to). String and regex bans work as on every provider, on MAIC's side. `grammar` and `json_schema` constrain what the model *can* say; they cannot express "anything except X", which is why the bans exist (the reasoning is in [bans.md](bans.md)).

MAIC sends nothing of this to Anthropic, and Ollama takes only the options it knows and ignores the rest, so a `sampling` table tuned for llama.cpp is safe to leave in settings when you switch provider.

## Where things are

| | |
| :--- | :--- |
| checkout | `vendor/llama.cpp` (submodule, tag `b11284`), linked as `~/.local/state/maic/vendor/llama.cpp` |
| build | `~/.local/state/maic/vendor/llama.cpp-build/` |
| binaries | `~/.local/state/maic/vendor/llamacpp/bin` -> the build's `bin/` |
| model | `~/.local/state/maic/vendor/llamacpp/current-model.gguf` -> your GGUF |
| service | `services/llamacpp.json`; log in `~/.local/state/maic/logs/llamacpp.log` |
| provider | `llamacpp`, `http://127.0.0.1:8081/v1`, in `default_providers()`; `llamacpp/current` is the default model |
