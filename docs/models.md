# Models: the catalog

MAIC keeps a catalog of the models it knows how to install: `models/catalog.json` in the repository (installed to `share/maic/models/`). Every file in it is pinned to a commit on Hugging Face with its SHA-256 and size, both from Hugging Face's API (`/api/models/<repo>/tree/<revision>?blobs=true`; `/api/models/<repo>` gives the commit to pin). `maic models` lists it, installs from it, verifies and removes. Nothing is downloaded unless you type `maic models install` (or say yes to it in `maic setup`), and every download is refused unless its hash matches.

## Commands

| Command | Does |
| :--- | :--- |
| `maic models` | the table: id, role, size, installed, which entry is linked as current for its server, the presets that use it |
| `maic models info ID` | the brief first (what it is good at, what it is weak at, when to pick it), then license, source and commit, every file with its URL and SHA-256, the VRAM estimates, presets, notes, and which installed entry needs it |
| `maic models install ID [--link]` | each file into `<models_dir>/<root>/<dir>/` through the same checked download as `maic vendor model` (curl, then SHA-256; a mismatch keeps nothing). A file already there with the catalog's size counts as present and is not fetched again, so the models already on a drive show as installed without a download. An entry that `shares` another's weights makes only its relative link, installing the other entry first. `--link` makes it the current model of its server. Prints the VRAM estimate and the presets |
| `maic models verify ID` | hashes the files that are there against the catalog (through a link for a shared entry) |
| `maic models remove ID [--yes]` | asks on a terminal; off one it refuses without `--yes`. Never removes weights another installed entry links to: removing `qwen3.5-9b` while `qwen3.5-9b-text` is installed is refused and names it. Removing `qwen3.5-9b-text` removes its folder and link only. The current model of a server is not removed either; link another first |
| `maic models check` | validates the catalog offline: every file has a URL, a SHA-256 and a size; Hugging Face URLs are pinned to the entry's revision; ids are unique; shares resolve to a file of the same hash and size; roles, kinds and roots are known; every VRAM figure agrees with the arithmetic below |

"Current" means a different link per server: `llamacpp` serves `~/.local/state/maic/vendor/llamacpp/current-model.gguf` as `llamacpp/current` ([llamacpp.md](llamacpp.md)), `whisper` loads `<models_dir>/whisper/current.bin` ([diction.md](diction.md)), and `llamacpp-fim` serves `<models_dir>/fim/current.gguf` as its model `current` (Code completion, below). A llama server reads its folder at start, so a newly installed folder appears after `maic down` and `maic up` of that server.

`maic setup` offers the agent and vision entries from the catalog (the recommendation for the card marked) and installs the ones you say yes to the same way. `maic doctor` points here. `maic help models` and `:h models` are the short form of this page.

## The fields

| Field | Meaning |
| :--- | :--- |
| `id` | the name `maic models` takes |
| `name` | a longer label |
| `role` | `agent`, `vision`, `scribe`, `completion`, `speech` or `vad` |
| `brief` | two to five sentences: what it is good at, what it is weak at, when to pick it |
| `license`, `license_url` | the model's license, as its publisher states it |
| `source` | `{repo, revision}`: the Hugging Face repository and the commit every URL is pinned to |
| `files` | `[{name, url, sha256, size, kind}]`; `kind` is `weights`, `mmproj` (a vision projector, loaded with the weights from the same folder) or `vad` |
| `install` | `{root, dir}`: `root` is `llamacpp`, `whisper` or `fim` (a folder under `models_dir`), `dir` the folder under it (`""` for whisper, whose models sit directly in `<models_dir>/whisper`) |
| `shares` | optional `{entry, file}`: this entry's one weights file is a relative link to that entry's file; the entry's own `sha256` and `size` are the shared file's |
| `vram` | `[{context, gb}]`: estimated GB on the card at each context that fits an 8 GB card (context 0: a whisper model, which has none) |
| `context` | the recommended context in tokens |
| `presets` | MAIC preset names that use it; `maic models` adds any preset in your settings whose model names the entry's folder |
| `notes` | anything else |

The VRAM figures are estimates, computed the way `maic gpu` computes its budget sentence: the files' size on disk (a projector counts) plus a KV cache of 65 MB per 1k tokens, or 130 MB per 1k tokens for a model over 6B parameters; a whisper model is its file plus 300 MB of buffers. Each process's own CUDA overhead (a few hundred MB) is not in them. `maic models check` recomputes every figure from the sizes and fails on a mismatch.

## The entries

| id | role | source | size | brief |
| :--- | :--- | :--- | :--- | :--- |
| `qwen3.5-4b` | agent | unsloth/Qwen3.5-4B-GGUF, Q4_K_M + mmproj-F16 | 2.6 + 0.6 GB | Qwen3.5's 4B at 4-bit with its vision projector: MAIC's quick model, the default scribe for diction, and able to read images. It fits an 8 GB card at up to 32k context and leaves room beside a parked ComfyUI. It is weaker than the 9B at long multi-step work and makes more tool-call mistakes. Pick it when the card is shared or speed matters more than depth. |
| `qwen3.5-9b` | vision | unsloth/Qwen3.5-9B-GGUF, Q4_K_M + mmproj-F16 | 5.3 + 0.9 GB | Qwen3.5's 9B at 4-bit with its vision projector, for image work: describing, captioning and reading pictures. With the projector loaded an 8 GB card holds it at 8k context and no more, so it is the wrong entry for long agent sessions; qwen3.5-9b-text links these same weights without the projector for those. Pick it when a picture is part of the task. |
| `qwen3.5-9b-text` | agent | a link to qwen3.5-9b's GGUF | 0 (link) | The 9B without image processing: a folder holding a relative link to qwen3.5-9b's GGUF and no projector, so nothing is stored twice and the gigabyte the projector costs goes to context instead (16k on an 8 GB card). This is the entry for MAIC's agent and for the diction scribe: stronger than the 4B at planning, tool calls and long instructions. It cannot see images, and it needs most of the card, so ComfyUI has to be stopped or unloaded. Installing it installs qwen3.5-9b first. |
| `whisper-distil-large-v3` | speech | distil-whisper/distil-large-v3-ggml | 1.4 GB | distil-whisper's large-v3 in ggml form, English only, fp16: the model legacy diction always used and the most accurate speech model here for English dictation. It holds about 1.7 GB on the card while whisper-server runs, which counts on a shared 8 GB card. Pick it when whisper has the card to itself or shares it with the 4B scribe. |
| `whisper-large-v3-turbo-q5_0` | speech | ggerganov/whisper.cpp | 0.5 GB | OpenAI's large-v3-turbo quantised to 5 bits by the whisper.cpp project: the small-VRAM choice, about 0.8 GB resident, and multilingual. The quantisation costs some accuracy against an fp16 model, so for English dictation on a card with room, distil-large-v3 is the better pick. Pick this one when the card is crowded, for example with the 9B scribe or the code completion model resident. |
| `silero-vad-v6.2.0` | vad | ggml-org/whisper-vad | 0.9 MB | Silero's voice activity detector in ggml form. services/whisper.json requires it: whisper-server runs it over each utterance so that silence and breathing are not transcribed into hallucinated stock phrases. It is under 1 MB, it is not a speech model, and it is never linked as the current one. |
| `qwen2.5-coder-7b` | completion | mradermacher/Qwen2.5-Coder-7B-i1-GGUF, i1-Q4_K_M | 4.4 GB | Qwen2.5-Coder 7B, the base model (not Instruct), trained for fill-in-the-middle: the default code completion model, served on port 8084 for llama.vim. Qwen publishes no GGUF of its base coders and ggml-org's 7B is Q8_0 at 8.1 GB, which does not fit an 8 GB card, so this is mradermacher's imatrix Q4_K_M made from Qwen/Qwen2.5-Coder-7B; mradermacher is a long-standing quantiser, and an imatrix quant keeps more quality than a plain one of the same size. It gives the best suggestions of the three, at about 5.4 GB with 8k context, so no agent model fits beside it. It is a poor diction scribe and a poor agent model: it completes code rather than following instructions, and it is a generation older than Qwen3.5. |
| `qwen2.5-coder-3b` | completion | ggml-org/Qwen2.5-Coder-3B-Q8_0-GGUF | 3.1 GB | Qwen2.5-Coder 3B, the base model, at Q8_0 from ggml-org, the llama.cpp project's own repository and the file llama.vim's 3B preset fetches. Its suggestions are weaker than the 7B's on longer completions but arrive faster, and at about 3.6 GB with 8k context it fits beside the 4B agent at 16k. It is under Qwen's research licence (non-commercial use), unlike the 7B and the 1.5B. Like the other coders it is the wrong model for the scribe or the agent. |
| `qwen2.5-coder-1.5b` | completion | ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF | 1.5 GB | Qwen2.5-Coder 1.5B, the base model, at Q8_0 from ggml-org (llama.vim's smallest preset). The fastest of the three and about 2.1 GB with 8k context, so it fits beside the 4B agent, whisper or a parked ComfyUI; its suggestions are shorter and more often wrong. Like the other coders it is the wrong model for the scribe or the agent. |

Sizes are GiB, as `maic models` prints them. `maic models info ID` has the commit, the URLs and the hashes.

## VRAM on an 8 GB card

Estimates, as above; "8k" is 8192 tokens.

| Entry | 8k | 16k | 32k | Notes |
| :--- | :--- | :--- | :--- | :--- |
| `qwen3.5-4b` (with projector) | 3.7 GB | 4.2 GB | 5.3 GB | `qwen-4b` runs it at 16k |
| `qwen3.5-9b` (with projector) | 7.2 GB | | | `qwen-9b-vision`: 8k is the most it holds |
| `qwen3.5-9b-text` | 6.3 GB | 7.4 GB | | `qwen-9b`: 16k |
| `qwen2.5-coder-7b` | 5.4 GB | 6.4 GB | | the completion server runs at 8k |
| `qwen2.5-coder-3b` | 3.6 GB | 4.1 GB | 5.1 GB | |
| `qwen2.5-coder-1.5b` | 2.1 GB | 2.6 GB | 3.6 GB | |
| `whisper-distil-large-v3` | 1.7 GB resident | | | |
| `whisper-large-v3-turbo-q5_0` | 0.8 GB resident | | | |

Pairs that fit, by these figures: the 4B at 16k with the 3B coder (7.8 GB, tight) or the 1.5B (6.3 GB); whisper distil-large-v3 with the 4B scribe at 8k (5.4 GB); the 9B text entry at 8k with whisper turbo (7.1 GB). Pairs that do not: the 7B coder beside any agent model, and the 9B text entry at 16k beside anything. `maic gpu` adds up what is actually resident.

## Your own models

`~/.config/maic/models.json` (`$XDG_CONFIG_HOME/maic/models.json`) has the same shape as the catalog. An entry whose id is in the catalog replaces only the fields it names; any other id is added.

```json
{
  "models": [
    {"id": "qwen3.5-4b", "context": 32768},
    {
      "id": "qwen2.5-coder-7b-q5",
      "name": "Qwen2.5-Coder 7B base, i1-Q5_K_M",
      "role": "completion",
      "brief": "The 7B coder at 5 bits: slightly better suggestions than Q4_K_M for 0.7 GB more.",
      "license": "apache-2.0",
      "license_url": "https://huggingface.co/Qwen/Qwen2.5-Coder-7B/blob/main/LICENSE",
      "source": {"repo": "mradermacher/Qwen2.5-Coder-7B-i1-GGUF", "revision": "8876f319c1aad46592be371cd11c4dee46cd111e"},
      "files": [
        {"name": "Qwen2.5-Coder-7B.i1-Q5_K_M.gguf",
         "url": "https://huggingface.co/mradermacher/Qwen2.5-Coder-7B-i1-GGUF/resolve/8876f319c1aad46592be371cd11c4dee46cd111e/Qwen2.5-Coder-7B.i1-Q5_K_M.gguf",
         "sha256": "260f0e1883277b9c52873e627ee39b1bbc5699ce55e26767d235da4348721d0a", "size": 5444832320, "kind": "weights"}
      ],
      "install": {"root": "fim", "dir": "Qwen2.5-Coder-7B-Q5_K_M"},
      "context": 8192
    }
  ]
}
```

Take the hash and size from `https://huggingface.co/api/models/<repo>/tree/<revision>?blobs=true` (`lfs.oid` is the SHA-256, `lfs.size` the size) and the commit from `https://huggingface.co/api/models/<repo>` (`sha`), then run `maic models check`. `vram` is optional; when given it has to agree with the arithmetic.

## Code completion

Copilot-style suggestions in neovim come from [llama.vim](https://github.com/ggml-org/llama.vim) talking to `services/llamacpp-fim.json`: llama-server on `127.0.0.1:8084` serving a Qwen2.5-Coder **base** model (Instruct models are worse at fill-in-the-middle) through `/infill`. It is not a chat model, so there is no chat provider for it.

```sh
maic models install qwen2.5-coder-7b --link   # once: the coder, linked as <models_dir>/fim/current.gguf
maic up llamacpp-fim                          # the server on 8084; it loads the model on the first request
```

The llama.vim spec for lazy.nvim:

```lua
{
    'ggml-org/llama.vim',
    init = function()
        vim.g.llama_config = {
            endpoint_fim = 'http://127.0.0.1:8084/infill',
            model_fim = 'current',  -- the coder `maic models install ID --link` chose
        }
    end,
}
```

`model_fim` needs a llama.vim recent enough to have it (it is in the current README). Tab accepts a suggestion, Shift-Tab its first line; llama.vim's own `:help llama` has the rest. Instruction editing (`<leader>lli`) uses `endpoint_inst`, a chat model: point it at `http://127.0.0.1:8081/v1/chat/completions` with `model_inst = 'Qwen3.5-4B-Q4_K_M'` if you want it, or leave it unused.

**Router mode, and why.** The server runs llama-server in router mode (`--models-dir <models_dir>/fim --models-max 1`), like `llamacpp` and `llamacpp-2`. The vendored llama.cpp (b11284) proxies `/infill` in router mode (`routes.post_infill = models_routes->proxy_post` in `tools/server/server.cpp`), routing by the request's `model` field, which llama.vim sends as `model_fim`. So `current.gguf` is the model `current`, `/models/unload` works, and the GPU turn-taking treats it like the other llama servers: `maic up comfyui` and `maic up whisper` unload it first, `maic gpu` shows what it holds, `maic gpu free llamacpp-fim` unloads it without stopping it, and it reloads on the next completion. Each folder under `<models_dir>/fim/` is also a model by its own name, so `model_fim = 'Qwen2.5-Coder-3B-Q8_0'` pins one regardless of the link.

The flags follow llama.vim's recommended server line (`--fim-qwen-7b-default`: `-ub 1024 -b 1024 --cache-reuse 256`, all layers on the GPU) with two changes: the model is the local one rather than the preset, which would download from Hugging Face (and MAIC's llama.cpp is built without TLS, so it could not anyway), and the context is 8192 rather than the model's full 32k, so the 7B fits an 8 GB card. llama.vim's ring of context from other files is trimmed by the server to fit; with the 3B or 1.5B there is room to raise `--ctx-size` in the service file to 16384 or 32768 (see the VRAM table).

**Which coder.** The 7B is the default: the best suggestions, 5.4 GB at 8k, so on an 8 GB card it runs alone (the agent on 8081 has to be unloaded, or remote). The 3B (3.6 GB) fits beside the 4B agent at 16k. The 1.5B (2.1 GB) fits beside the 4B agent or whisper. Generation on a GPU is bound by memory bandwidth, so speed roughly follows file size: the 3B at Q8_0 (3.1 GB) generates about 1.4 times as fast as the 7B (4.4 GB), the 1.5B (1.5 GB) about 2.8 times; prompt processing scales with parameter count, which favours the small ones more. These are estimates; llama.vim shows the real timings of every suggestion (`show_info`). Switch with `maic models install qwen2.5-coder-3b --link` and `maic gpu free llamacpp-fim`.

**Nothing leaves the machine.** llama.vim sends the code around the cursor to 127.0.0.1:8084; the server is MAIC's own llama.cpp build, bound to loopback, without TLS, serving a file already on disk. The only network access is `maic models install`, when you type it.
