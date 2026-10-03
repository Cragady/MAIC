# comfyui-maic-llamacpp

MAID's own ComfyUI custom node: two nodes under **MAID/llm** that chat with the `llama-server` MAID runs (`maid up llamacpp`, `http://127.0.0.1:8081/v1`; the side server `maid up llamacpp-2` on `http://127.0.0.1:8082/v1` for a second resident model). Standard library only, one non-streaming POST per turn, loopback by default. It replaces the vendored `comfyui-ollama` nodes and their think patch; `vendor/comfyui.sh wire` links this folder into `custom_nodes/`.

| Node | Inputs | Outputs |
| :--- | :--- | :--- |
| **MAID LLM Server (llama.cpp)** `MaicLlmServer` | `base_url` (default `http://127.0.0.1:8081/v1`), `model` (default `current`, the `--alias` in `services/llamacpp.json`; a single-model server ignores the name), `timeout` seconds | `connection` (`MAID_LLM`) |
| **MAID LLM Chat** `MaicLlmChat` | `connection`, `system`, `prompt`, `think`, `format` text/json, `temperature`, `top_k`, `top_p`, `min_p`, `seed` (-1: the server picks), `extra_json`, `keep_context`, `session_id`, `reset`, optional `images` | `response`, `thinking`, `session_id` (all STRING) |

What each request carries, by llama-server's names (`tools/server/README.md` at tag b11284):

* `think` sends `chat_template_kwargs: {"enable_thinking": <bool>}` and `reasoning_format: "deepseek"`, so reasoning comes back in `message.reasoning_content` and fills the `thinking` output instead of the response. With `think` off it also sends `reasoning_effort: "none"`, the other documented off switch, which is the one Ollama's `/v1` reads. That is the per-request switch the old patch existed for: a thinking model such as Qwen3.5 answers instead of reasoning until the token limit.
* `format` json sends `response_format: {"type": "json_object"}`.
* `temperature`, `top_k`, `top_p`, `min_p`, `seed` go as they are; the defaults are llama-server's.
* `extra_json` is a JSON object merged into the body last, so any other field the server takes works and overrides the widgets: `n_predict`, `xtc_probability`, `dry_multiplier`, `grammar`, `json_schema`, `logit_bias`, `samplers` (see `docs/llamacpp.md`).
* `images` (an IMAGE batch) become PNG `image_url` data parts on this turn's user message. The server has to be started with `--mmproj <projector.gguf>` and a vision model, or it rejects the request; the history keeps only the text.
* `keep_context` on keeps the conversation in this process per `session_id` (an empty id means the node's own) and sends it with every turn until ComfyUI restarts. `reset` forgets it before this turn. The `session_id` output is the id used, so a second chat node can take it as input and continue the same conversation.

Errors are node errors with the server's reply text; an unreachable server says `llama.cpp is not running: maid up llamacpp`.

## From the Ollama nodes

| comfyui-ollama | Here |
| :--- | :--- |
| `OllamaConnectivityV2.url` `http://127.0.0.1:11434` | `MaicLlmServer.base_url` `http://127.0.0.1:8081/v1` |
| `.model` (a tag from the server's list) | `.model`, a free string; `current` is the linked GGUF |
| `.keep_alive`, `.keep_alive_unit` | none; each llama-server keeps its one resident model loaded while it runs |
| `OllamaChat.system`, `.prompt`, `.think`, `.format` | the same names, same order, same values |
| `OllamaOptionsV2` node on `.options` | `temperature`, `top_k`, `top_p`, `min_p`, `seed` widgets; everything else through `extra_json` |
| `.images` | `.images` |
| `.history` link (`OLLAMA_HISTORY`) from another chat node | `.session_id` input linked from that node's `session_id` output (or the same string typed in both) |
| `.reset_session` | `.reset` |
| always remembers | `keep_context` on (the default) |
| outputs `result`, `thinking`, `meta`, `history` | `response`, `thinking`, `session_id`; `meta` is gone (connect the server node to each chat node) |

`example_workflows/story-chat-llamacpp.json` is the old Story chat on these nodes. The quick pass talks to MAID's main server (`http://127.0.0.1:8081/v1`); the deep pass is a second server node, "Deep model (llamacpp-2)", pointing at MAID's side server on `http://127.0.0.1:8082/v1` (`maid up llamacpp-2`, `docs/llamacpp.md`, Two servers). Each server keeps one model resident (`--models-max 1`), so with both up the quick model and the deep model stay loaded side by side; set the deep node's `model` to the GGUF's name (`Qwen3.5-9B-Q4_K_M-text`, say) and unmute it. `maid gpu` says whether the pair fits the card. The node's own `base_url` default stays 8081.

## Test

```sh
python3 -m unittest -v test_node        # or: ctest --preset default -R comfy_node
```

A fake OpenAI-compatible server on a random loopback port checks the request body, the per-session history, the thinking split and the error text. No ComfyUI, torch or network needed.
