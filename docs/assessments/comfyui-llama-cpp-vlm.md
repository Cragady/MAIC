# Assessment: ComfyUI-llama-cpp_vlm as a replacement for comfyui-ollama

**Done (2026-09-30):** the section 7 alternative is built as `vendor/comfyui-maic-llamacpp` (nodes `MaicLlmServer`, `MaicLlmChat`), `comfyui-ollama` and its patch are gone, and `example_workflows/story-chat-llamacpp.json` is the rewritten workflow.

Date: 2026-09-30. Read-only review of `~/dev2/tools-and-things/ComfyUI-llama-cpp_vlm` (upstream `lihaoyun6/ComfyUI-llama-cpp_vlm`, HEAD `f2209cc`, 2026-08-17) against the vendored `~/dev2/tools-and-things/comfyui-ollama` (`stavsap/comfyui-ollama` at `6db7560` plus `vendor/patches/comfyui-ollama-think.patch`). Question: can it replace the Ollama nodes in Micaiah's ComfyUI workflows so ComfyUI no longer needs Ollama at all.

**Verdict: reject as a vendored replacement.** It has no license file, it loads the GGUF in-process (a second copy of the model next to MAIC's `llama-server`, on an 8 GB card), it cannot express the per-run `think` switch that the Ollama patch exists for, and the two-model "Story chat" cannot be rebuilt on its single global model slot. The right replacement is a small MAIC-owned node that talks to `llama-server` on `127.0.0.1:8081` over HTTP (section 7). Nothing in the Manga workflows depends on Ollama, so only one workflow is affected.

Line numbers below refer to the current checkouts; `nodes.py` is the candidate's `nodes.py` unless another file is named.

## 1. What the workflows use today

`~/.local/state/maic/workflows/comfyui/` holds six files. Node types per file:

| Workflow | Ollama nodes |
| :--- | :--- |
| `Manga 4-panel short (Anima).json`, `... (Anima) - BETA captions.json` | none (CLIPLoader, CLIPTextEncode, EmptyLatentImage, ImageStitch, KSampler, LoraLoaderModelOnly, Note, SaveImage, TextOverlay, UNETLoader, VAEDecode, VAELoader) |
| `Manga 4-panel short (NoobAI).json`, `... (NoobAI) - BETA captions.json`, `cust \| m4ps (NoobAI) - BETA captions.json` | none (CLIPTextEncode, CheckpointLoaderSimple, EmptyLatentImage, ImageStitch, KSampler, Note, SaveImage, TextOverlay, VAEDecode) |
| `Story chat (Ollama).json` | `OllamaConnectivityV2` x2, `OllamaChat` x2 |

The "BETA captions" variants put captions on with core `TextOverlay`; no LLM is involved. So the whole question reduces to **Story chat**.

### Story chat (Ollama).json, exactly

| Node | Type | Widgets (in order) | Links |
| :--- | :--- | :--- | :--- |
| 1 "Quick model" | `OllamaConnectivityV2` | url `http://127.0.0.1:11434`, model `qwen3.5:4b`, keep_alive `5`, unit `minutes` | -> node 2 `connectivity` |
| 2 "Quick pass" | `OllamaChat` | system (co-writer prompt with the story bible), prompt, think `false`, format `text`, reset_session `false` | result -> 3 `PreviewAny`, 4 `SaveText` (`story/chat_4b`, md); history (output 3) -> node 6 `history` |
| 5 "Deep model" | `OllamaConnectivityV2` | same, model `qwen3.5:9b` | -> node 6; muted (mode 2) |
| 6 "Deep pass" | `OllamaChat` | same system, a "review the whole conversation" prompt, think `false`, format `text` | result -> 7 `PreviewAny`, 8 `SaveText` (`story/chat_9b`); muted (mode 2) |
| 9 | `Note` | "Needs the Ollama server running: `maic up ollama`" ... "think = true makes Qwen3.5 reason first" | |

Neither chat node connects `options` or `images`. Nodes 5 to 8 are muted and enabled by hand for the deep pass, which reads the quick pass's history through the `OLLAMA_HISTORY` link.

### What those two nodes do (comfyui-ollama, `CompfyuiOllama.py`)

* `OllamaConnectivityV2` (203 to 236) only packs `url`, `model`, `keep_alive`, `keep_alive_unit` into a dict. The `model` dropdown is filled by `web/js/OllamaNode.js:27-28`, which POSTs to the node's own server route `/ollama/get_models` (62 to 76); that route asks the Ollama server for its model list.
* `OllamaChat` (391 to 658). Sessions are an in-process dict `CHAT_SESSIONS` (25 to 32) keyed by the `history` input if connected, else the node's `unique_id` (579). `reset_session` replaces the session (582 to 583). The system prompt is written into message 0 on every run (597 to 601), the user prompt appended (610), images added as base64 PNG only to the API copy of the last message (549 to 558, 622 to 626), then `client.chat(model, messages, options, keep_alive, format, think)` (628 to 635). Returns `result`, `thinking` (only when `think`), `meta`, and `history`, which is just the session key string (653 to 658). `format` is `""` or `"json"` (534 to 539): Ollama's JSON mode.
* `OllamaOptionsV2` (133 to 201), not used by the workflow: mirostat, num_ctx, repeat_last_n, repeat_penalty, temperature, seed, stop, tfs_z, num_predict, top_k, top_p, min_p, each behind an enable flag.
* **The think patch** (`vendor/patches/comfyui-ollama-think.patch`) adds one line, `think=think,` at 634. Without it the node ignored its own `think` widget, and Qwen3.5 (thinking on by default) reasoned until `num_predict` and returned nothing usable. The point of the patch is that `think` is a **per-request** switch sent with every call.

## 2. What ComfyUI-llama-cpp_vlm is

Repository facts (from `git log`, `git tag`, `git ls-files`):

* One author (`lihaoyun6`), 50-odd commits since 2025-11-25, last commit `f2209cc` on 2026-08-17. Only one tag, `1.2.2` at `cc71cc3` (2026-01-01); `pyproject.toml:4` says version `1.3.1`, untagged. **MAIC would have to pin a commit, not a tag.**
* **No LICENSE file.** `pyproject.toml:5` declares `license = {file = "LICENSE"}` but no such file exists in the tree or in any commit (`git log --all -- LICENSE` is empty). Without a license grant the code is all rights reserved by default; MAIC cannot vendor it as a submodule and redistribute it.
* `README.md:13` tells users to clone `ComfyUI-llama-cpp.git`, a different repository name from the one in `pyproject.toml:26` (`ComfyUI-llama-cpp_vlm`). Minor, but it shows the README is not maintained with the code.
* Files: `nodes.py` (1559 lines), `support/cqdm.py` (progress bar), `support/gguf_layers.py` (reads block_count from a GGUF header), `support/prompt_enhancer_preset.py` (108 KB of system prompts for Qwen-Image, Flux.2, Wan and friends), two requirements files, one preview image. No example workflows in the repository.

### Node list (`nodes.py:1529-1559`)

| Node (display name) | Inputs | Outputs |
| :--- | :--- | :--- |
| `llama_cpp_model_loader` "Llama-cpp Model Loader" (449 to 495) | `model` (files under the `LLM` folder whose name lacks "mmproj", 452 to 454), `mmproj` (files containing "mmproj", or None), `chat_handler` (list built at 30 to 117 from whichever handlers the installed llama-cpp-python has), `n_ctx` 1024 to 327680 default 8192, `vram_limit` GB (-1 = all layers on GPU), `image_min_tokens`, `image_max_tokens`, `load_mtp` | `LLAMACPPMODEL`, which is just the config dict (495); the load itself happens here, into a module-level singleton (492 to 494) |
| `llama_cpp_instruct_adv` "Llama-cpp Instruct" (497 to 760) | `llama_model`, `preset_prompt` (presets at 350 to 363), `custom_prompt`, `system_prompt`, `inference_mode` (one by one / images / video), `max_frames`, `max_size`, `seed`, `force_offload`, `save_states`; optional `parameters`, `images`, `queue_handler` (any type, "used to control the execution order of instruct nodes", 540) | `output` STRING, `output_list` (list), `state_uid` INT |
| `llama_cpp_parameters` "Llama-cpp Parameters" (762 to 790) | `max_tokens` 0 to 4096 default 1024 (767), `top_k`, `top_p`, `min_p`, `typical_p`, `temperature`, `repeat_penalty`, `frequency_penalty`, `present_penalty`, `mirostat_mode/eta/tau`, `state_uid` (-1 = node's unique_id) | `LLAMACPPARAMS` dict |
| `llama_cpp_clean_states` (792 to 813) | any, `state_uid` (-1 = all) | passthrough |
| `llama_cpp_unload_model` (815 to 828) | any | passthrough |
| `parse_json_node` "Parse JSON" (1160 to 1223), `remove_code_block` "Unpack Code Block" (1225 to 1262) | string post-processing | |
| `json_to_bbox`, `bbox_to_segs`, `bbox_to_mask`, `bboxes_to_bbox` (830 to 1144) | VLM bounding boxes to Impact-Pack SEGS / masks | |
| `PromptEnhancerPreset` (1264 to 1317) | picks one of the 108 KB presets | `system_prompt` STRING |
| `llama_cpp_text_encoder` "Llama-cpp Text Encoder (BETA)" (1319 to 1527) | uses a GGUF's hidden states as CONDITIONING for Z-Image, Qwen-Image, Lumina2, MiniMax | `CONDITIONING` |

### Feature checklist

* **Text-only LLM**: yes. With `chat_handler` = None the model's own chat template from the GGUF is used through llama-cpp-python (312 to 318; the text path at 726 to 737).
* **Vision**: yes, with an `mmproj` file plus a matching `chat_handler` (280 to 310); images are sent as JPEG quality 85 base64 (366 to 370); multi-image and "video" (frame sampling) modes; images without an mmproj raise (626 to 627).
* **Multi-turn**: partial. `save_states=True` keeps the message list in `LLAMA_CPP_STORAGE.messages` keyed by the node's `unique_id` or the `state_uid` from the Parameters node (587 to 610, 739 to 745). The history is **erased** when: the system prompt text changes (596 to 598), `save_states` is off (745), any model is (re)loaded (260 calls `clean(all=True)`, which clears every conversation at 201 to 202), the Unload node runs, or ComfyUI's "unload all models" fires (339 to 346). There is no history socket between nodes; two nodes share a conversation only by typing the same `state_uid` integer.
* **System prompt**: yes (505).
* **Sampling**: temperature, top_k, top_p, min_p, typical_p, repeat_penalty, frequency_penalty, mirostat, seed. `present_penalty` is silently dropped when the MTMD handler exists (584 to 585), which it does in the pinned wheels, so that widget is dead. **Not exposed anywhere** (grep of `nodes.py`): `stop`, `logit_bias`, `grammar`, `json_schema`, `response_format`, XTC, DRY, streaming. No JSON mode; "Parse JSON" only parses whatever text came back.
* **Thinking toggle**: only at **load time**, by picking a `*-Thinking` handler name (294). And the `enable_thinking` kwarg is only passed inside the `mmproj` branch (280 to 301); a text-only GGUF with no mmproj never receives it (312 to 318). With `chat_handler` = None the GGUF template decides, and Qwen3.5 thinks by default. The reply is then run through `re.sub(r'<think>.*?(?:</think>|$)', '', ...)` (690, 734), which also swallows an **unterminated** `<think>` to the end of the text, so when reasoning runs into `max_tokens` (hard cap 4096, line 767) the visible reply is empty. This is the exact failure the Ollama think patch fixed, and here there is no request-level switch to patch in. The thinking text is never exposed as an output. Line 300 has a bug from the "fix think mode" commit `3330e52`: `["GLM-4.6V" "MiniCPM-v4.5", ...]` lacks a comma, so the two strings concatenate and those handlers no longer get `enable_thinking`.
* **Batch**: yes, the Instruct node is `INPUT_IS_LIST` and emits an `output_list`. **Streaming**: no. **Structured output**: no.

## 3. How it runs the model

**In-process, through llama-cpp-python** (`import llama_cpp` at 22 to 27; `Llama(**kwargs)` at 335). It never talks to a server. Consequences for the 8 GB RTX 2080:

* `n_gpu_layers` is -1 (everything on the GPU, 270) unless `vram_limit` is set, in which case it estimates layers from the GGUF's block_count and `file_size * 1.55` (275 to 278, 288 to 290, 313 to 314). That estimate calls `support/gguf_layers.py:get_layer_count`, whose fallback at line 100 prints an undefined variable `e` (NameError) for any GGUF without a `.block_count` key.
* The weights live outside torch's allocator, so `comfy.model_management` cannot see or evict them. The node monkeypatches `mm.unload_all_models` (339 to 346) so a manual "free memory" clears it, but the automatic path that loads the diffusion model before a KSampler works from torch's own accounting and will OOM or thrash with a 3.4 GB 4B (plus KV cache at `n_ctx` 8192) already resident. `force_offload=True` (525 to 528, 747 to 748) unloads after each run, at the price of re-reading the GGUF from the external drive on every run. There is **no keep_alive timer**; the model stays until something unloads it or ComfyUI exits.
* **One global model slot** (`LLAMA_CPP_STORAGE`, 123 to 129). The Instruct node does not check that the loaded model is the one it was handed: it loads only if nothing is loaded (576 to 577) and otherwise uses whatever is there. A loader with a different config reloads (492 to 494) and wipes every conversation (260, 201 to 202). The `queue_handler` input exists precisely because two Instruct nodes in one graph need manual ordering.
* **VRAM next to MAIC**: `services/llamacpp.json` runs `llama-server -ngl 99 --ctx-size 16384`, which `docs/llamacpp.md:68` calls the ceiling of the card for a 9B Q4. A second in-process copy of a Qwen3.5 inside ComfyUI cannot coexist with that; one of the two has to be stopped, and ComfyUI's diffusion model needs the card too.

**Wheels.** `requirements.txt:9-27` and `requirements_cu131.txt:9-20` pin `llama-cpp-python` 0.3.46 as direct URLs to GitHub release assets of the **JamePeng fork** (not PyPI, not hash-pinned): cu128 or cu131 for cp310 to cp314 on Linux x86_64 and Windows, Metal on macOS. Her venv is Python 3.13.9 with torch 2.14.0+cu130 (checked with `.venv/bin/python`). Neither file matches cu130 exactly; cu131 is the closer one but `vendor/comfyui.sh:70` installs every `custom_nodes/*/requirements.txt`, so it would pull the cu128 wheel unless the script special-cases this node. Whether the fork's wheels are built with `sm_75` (Turing) and which CUDA driver they require cannot be checked offline; it is a must-test before any adoption. MAIC's own llama.cpp is compiled from source with the local `nvcc`, which sidesteps all of that. Other dependencies: `diskcache`, `scipy`, `numpy`, `pillow`, `gguf`, `tqdm`; the venv already has scipy, numpy, PIL and tqdm, and lacks `diskcache`, `gguf` and `llama_cpp`.

**Where it expects GGUFs.** Hardcoded `<ComfyUI>/models/LLM` (272, 281). Line 349 **assigns** `folder_paths.folder_names_and_paths["LLM"]` outright instead of calling `add_model_folder_path`, and custom nodes load (`main.py:591` -> `init_extra_nodes` at 526) after `apply_custom_paths()` (`main.py:231`), so an `LLM:` line in `extra_model_paths.yaml` would be discarded; and `load_model` joins `models_dir/LLM/<name>` itself anyway. The only wiring that works is a symlink `ComfyUI/models/LLM -> <models_dir>/LLM` (`folder_paths.recursive_search` follows symlinks, `folder_paths.py:416`). The external drive has no `LLM/` folder today; the Qwen3.5 GGUFs she has are Ollama blobs (`ollama/blobs/sha256-81fb...`, 3.4 GB, tag `4b`; `sha256-dec5...`, 6.6 GB, tag `9b`) with no `.gguf` suffix, and the loader lists only files with the extensions at 348, so each blob would also need a `.gguf`-named symlink.

## 4. Feature mapping, Story chat -> candidate

| comfyui-ollama | Candidate | Verdict |
| :--- | :--- | :--- |
| `OllamaConnectivityV2.url` | none (in-process) | drop |
| `.model` (Ollama tag from the server's list) | `Model Loader.model` (a file under `models/LLM`) | workflow edit plus the symlinks above |
| `.keep_alive` / unit | none; `force_offload` per run or the Unload node | **no keep_alive equivalent** |
| (Ollama's per-model num_ctx) | `n_ctx` widget | new, must be chosen |
| `OllamaChat.system` | `Instruct.system_prompt` | direct |
| `.prompt` | `Instruct.custom_prompt` with `preset_prompt` = "Empty - Nothing" (custom overrides any preset without `*`, 615) | direct |
| `.think` (per request) | `chat_handler` "Qwen3.5" vs "Qwen3.5-Thinking" at load time, and only honoured with an mmproj | **missing**; text-only Qwen3.5 thinks by default and the output is stripped, replies empty when reasoning overruns `max_tokens` |
| `.format` json | none | **no JSON mode** |
| `.images` | `Instruct.images` (JPEG) | direct, needs mmproj + handler |
| `.history` link from another chat node | same integer in both nodes' `Parameters.state_uid`, `save_states` on, identical system prompt | workflow edit; Instruct's `state_uid` output can be wired into the second Parameters node's `state_uid` (widget to input) to force ordering |
| `.reset_session` | Clean States node, or change the system prompt | workflow edit |
| `.meta` chaining | none | drop |
| output `thinking` | none | **missing** |
| output `history` | `state_uid` INT | different shape |
| `OllamaOptionsV2` (unused here) | `Parameters` | roughly equal; no `stop`, no `num_ctx` here (it is on the loader) |

**Can "Story chat (Ollama)" be rebuilt on it?** The quick pass alone, yes: Loader (4B GGUF, handler None or Qwen3.5) -> Parameters (`state_uid` 1) -> Instruct (`save_states` on, system, prompt) -> PreviewAny / SaveText. What changes for her:

1. No `think` switch per run. To stop Qwen3.5 reasoning she would need the "Qwen3.5" handler, which the code only wires up when an mmproj is loaded; otherwise she gets the default-on reasoning and the empty-reply failure. This is a regression to the state before the think patch.
2. **The 9B deep pass cannot coexist.** Loading the second model wipes the shared history (260, 201 to 202), and an Instruct node uses whichever model happens to be loaded (576 to 577), so with both loaders in one graph the quick pass can silently run on the 9B. The two-model design becomes one model, or a reload with lost memory each time.
3. The model stays resident in VRAM with no timeout; she would toggle `force_offload` by hand before running a Manga workflow, or restart ComfyUI.
4. `maic up ollama` in the note becomes "nothing to start", but `maic up llamacpp` for MAIC's own use would then fight ComfyUI for the card.

## 5. Code quality and risk

* **License: none** (see section 2). Blocker on its own.
* **Activity**: single maintainer, last commit six weeks ago, one stale tag; pin would be commit `f2209cc`.
* **Network at runtime**: none. The only URL in the code is inside an error message (`nodes.py:310`). No telemetry, update checks, downloads, or subprocess calls (`grep` for `http|urllib|requests|socket|subprocess|os.system|popen|download` over all Python files finds only that line). Install time does fetch wheels from a third-party GitHub release.
* **Reaches into private state**: llama-cpp-python's `_ctx`, `_model`, `_hybrid_cache_mgr`, `_exit_stack` (136 to 172, 193, 753 to 756) and `llama_cpp.llama_get_*` C entry points by name (1427 to 1451). These break across llama-cpp-python versions, which is why the wheels are pinned to one fork build.
* **Monkeypatches ComfyUI** (`mm.unload_all_models`, 339 to 346) and overwrites a `folder_paths` registry entry (349), which will collide with any other custom node that registers `LLM`.
* **Bugs found while reading**: `support/gguf_layers.py:100` undefined `e`; `nodes.py:300` missing comma; `present_penalty` dead (584 to 585); 28 bare `except Exception`, 19 `print`s, a Chinese-language instruction prepended to the system prompt in video mode (594) and a Chinese error message (1454).
* **Python**: `match` statements (209, 1279), so 3.10+; wheels for 3.10 to 3.14. **Platforms**: Linux x86_64, Windows, macOS arm64 by the wheel matrix; `pyproject.toml:9-13`.
* **Errors surface** as ComfyUI node exceptions with reasonable messages (283, 286, 310, 333, 627).

## 6. Maintenance burden versus today

Today MAIC carries one one-line patch. Adopting the candidate would mean at least four:

1. a per-run `think` control for text-only models (not a one-liner: the fork's handlers take `enable_thinking` at construction, so it needs a handler rebuild or a `chat_template_kwargs` path that `create_chat_completion` does not take);
2. `folder_paths.get_full_path("LLM", ...)` instead of the hardcoded join, so `extra_model_paths.yaml` works (272, 281, 349);
3. `vendor/comfyui.sh` choosing `requirements_cu131.txt` for this node and verifying the wheel against cu130 torch and sm_75;
4. the line 300 comma.

Plus re-checking the private-attribute code on every llama-cpp-python bump. **Coexistence with comfyui-ollama** is fine: different node class names (`llama_cpp_*` vs `Ollama*`), categories and types; nothing shared. Coexistence with **MAIC's llama-server** is the problem: two copies of a Qwen3.5 on one 8 GB card.

## 7. Recommendation: reject, and write the small HTTP node instead

Do not add `ComfyUI-llama-cpp_vlm` to `vendor/manifest.json`. Reasons in order: no license; in-process second model copy on the same card as `llama-server` and the diffusion model; loses the per-run `think` switch (the reason the think patch exists); cannot host the two-model Story chat; one-maintainer untagged code that reaches into a fork's private API and needs matching prebuilt CUDA wheels.

**Alternative: `vendor/comfyui-maic-llamacpp`**, a MAIC-owned custom node beside `vendor/comfyui-maic-templates`, under MAIC's license, calling `llama-server`'s OpenAI-compatible `/v1/chat/completions` on `127.0.0.1:8081`. It avoids a second copy of the model (the node uses the one MAIC already serves), needs no wheel and no new dependency (`urllib.request` from the standard library, or the `aiohttp` ComfyUI already ships), and gives ComfyUI every sampler `docs/llamacpp.md:78-92` lists.

Shape, mirroring the Ollama nodes one for one so the workflow is a rename:

* **"llama.cpp Server"** -> `LLAMACPP_SERVER`: `url` (default `http://127.0.0.1:8081`), `model` (default `current`, the `--alias` in `services/llamacpp.json`; a free string is enough because the server ignores the field, `docs/llamacpp.md:70`). About 30 lines, the same as `OllamaConnectivityV2` (203 to 236). A model dropdown fed by `GET /v1/models`, like the `/ollama/get_models` route (62 to 76) and `web/js/OllamaNode.js`, is optional and adds about 40 lines of JS.
* **"llama.cpp Chat"** -> `result`, `thinking`, `history`: `system`, `prompt`, `think` (BOOLEAN sent as `chat_template_kwargs: {"enable_thinking": ...}`, with `reasoning_format` so the reasoning comes back in `reasoning_content` and fills the `thinking` output), `format` text/json (`response_format: {"type": "json_object"}`), optional `images` (base64 `image_url` parts; works when the service adds `--mmproj`), optional `history` and `reset_session` using the same in-process `CHAT_SESSIONS` dict as `CompfyuiOllama.py:25-32` and `578-610`, optional `sampling` as a JSON string merged into the request body (temperature, top_k, top_p, min_p, repeat_penalty, n_predict, seed, XTC, DRY, `grammar`, `json_schema`, `logit_bias`, all by llama-server's own names). About 150 lines: `OllamaChat` is 170 lines (391 to 658) and the replacement swaps the `ollama` client for one `urllib` POST and a 20-line body builder.

Total roughly 200 lines of Python in one file plus a 10-line `__init__.py` and `pyproject.toml`. The "no internet requests" rule in `AGENTS.md` is about core ComfyUI; a MAIC custom node talking to loopback is the same category as the Ollama node it replaces. Two things to confirm against the llama.cpp checkout once `maic vendor add llamacpp` has fetched it (the submodule directory is empty on this machine, so they could not be verified for tag `b11284` offline): that `chat_template_kwargs` and `reasoning_format` are accepted on `/v1/chat/completions`, and whether the server's multi-model router mode (`--models-dir`) is present, which would let the 4B and 9B both stay reachable from one server for the deep pass.

**Plan**

1. Leave `comfyui-ollama` vendored and patched until the new node runs; it costs nothing while Ollama stays an optional backend.
2. Add `vendor/comfyui-maic-llamacpp/` (nodes as above) and a `wire()` line in `vendor/comfyui.sh` next to line 43 (`ln -s "$MAIC_ROOT/vendor/comfyui-maic-llamacpp" custom_nodes/comfyui-maic-llamacpp`). No requirements file, so line 70's loop is untouched.
3. Put `story-chat-llamacpp.json` in `~/.local/state/maic/templates/comfyui/`: the same nine nodes with `OllamaConnectivityV2` -> "llama.cpp Server" and `OllamaChat` -> "llama.cpp Chat", widgets carried over, the note changed to `maic up llamacpp` and `maic vendor use llamacpp <gguf>`. The deep pass stays as muted nodes pointing at a second server on another port (a `services/llamacpp-9b.json` with `--port 8082`), which on 8 GB means stopping the 4B first; or, if router mode is available at `b11284`, one server with both GGUFs. Micaiah decides which; the quick pass alone is the safe first cut.
4. Test by status and timing only (no reading of generated text): a quick-pass run with `think` off returns in the 3 s range she measured on Ollama, a run with `think` on fills the `thinking` output, `reset_session` empties the session, and a Manga workflow still loads its checkpoint while `llama-server` is up.
5. When the template works, drop `comfyui-ollama` from `comfyui.needs` in `vendor/manifest.json`, delete the patch, and update `docs/comfyui-setup.md:3,70,114` and `docs/vendor.md`, which still say the Story chat needs Ollama.
