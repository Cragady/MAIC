# Models: the catalog

MAIC keeps a catalog of the models it knows how to install: `models/catalog.json` in the repository (installed to `share/maic/models/`). Every file in it is pinned to a commit on Hugging Face with its SHA-256 and size, both from Hugging Face's API (`/api/models/<repo>/tree/<revision>?blobs=true`; `/api/models/<repo>` gives the commit to pin). `maic models` lists it, installs from it, verifies and removes. Nothing is downloaded unless you type `maic models install` (or say yes to it in `maic setup`), and every download is refused unless its hash matches.

## Commands

| Command | Does |
| :--- | :--- |
| `maic models` | the table: id, role, size, installed, which entry is linked as current for its server, the presets that use it; then the API models with their window and output (from `GET /models` where it was read, with when; nothing is fetched) and the average cost per session |
| `maic models info ID` | the brief first (what it is good at, what it is weak at, when to pick it), then license, source and commit, every file with its URL and SHA-256, the VRAM estimates, presets, notes, and which installed entry needs it |
| `maic models install ID [--link]` | each file into `<models_dir>/<root>/<dir>/` through the same checked download as `maic vendor model` (curl, then SHA-256; a mismatch keeps nothing). A file already there with the catalog's size counts as present and is not fetched again, so the models already on a drive show as installed without a download. An entry that `shares` another's weights makes only its relative link, installing the other entry first. `--link` makes it the current model of its server. Prints the VRAM estimate and the presets |
| `maic models verify ID` | hashes the files that are there against the catalog (through a link for a shared entry) |
| `maic models remove ID [--yes]` | asks on a terminal; off one it refuses without `--yes`. Never removes weights another installed entry links to: removing `qwen3.5-9b` while `qwen3.5-9b-text` is installed is refused and names it. Removing `qwen3.5-9b-text` removes its folder and link only. The current model of a server is not removed either; link another first |
| `maic models refresh` | reads `GET /models` again from every provider with `options.read_models` (the shipped `deepseek`; it needs the key), saves what it says, and lists the API models (API models, below) |
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
| `notes` | anything else, including measured behaviour: what the model was found to do, with the date and the page that holds the measurement (a finding from one family is recorded on that family's entries only) |

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

## API models

Some models are not installed but reached over an API that bills per token. Their limits and prices sit in the catalog's `api_models`, one entry per model, so usage can be costed per model: `provider` and `model` (what a preset names as `provider/model`), `limits` (`context`, `output`), `capabilities`, and `pricing` with `currency`, `per` (tokens) and `periods`. A period holds `input_cache_hit`, `input_cache_miss` and `output`; the one without `days` and `utc` is the default, and one with them applies on those UTC weekdays and hours. `source` and `checked` say where and when the figures were read. `maic models check` validates them; `models.json` may carry `api_models` of its own.

| Id | Model | Context | Output | Pictures | Per 1M tokens, off-peak (peak) |
| :--- | :--- | ---: | ---: | :--- | :--- |
| `deepseek-flash` | DeepSeek-V4.1-Flash | 1M | 384K | yes | cache hit 0.003 (0.006), miss 0.15 (0.3), output 0.6 (1.2) USD |
| `deepseek-v4-pro` | DeepSeek-V4-Pro | 1M | 384K | no | cache hit 0.022 (0.044), miss 0.66 (1.32), output 1.98 (3.96) USD |

Peak is 01:00 to 04:00 and 06:00 to 10:00 UTC, Monday to Friday. Figures from [DeepSeek's pricing page](https://api-docs.deepseek.com/quick_start/pricing/), checked 2026-10-02.

**What the API says wins over the catalog.** A provider with `options.read_models` (the shipped `deepseek`) has its `GET /models` read once per process, at its first use, with a short timeout (2 s to connect, 3 s to answer) and no retry, so a turn never waits on it for long. Each model's `context_window` is then its window (the readout and compaction), its `max_output_tokens` caps `max_tokens` (the shipped 65536 is lower and stays), and a `reasoning_effort` outside its `effort.supported_levels` is not sent, which is told once. What it said is saved with the time in `<state>/api-models/<provider>.json`; when a later read fails, that copy stands in, and with no copy the provider's options and the figures above do. `maic models` shows which, and `maic models refresh` reads again.

**Cost is an estimate.** Each model call to a model the catalog prices is costed from its usage record: the cached input tokens at `input_cache_hit`, the rest of the input at `input_cache_miss` and the output at `output`, in the period in force when the reply came (peak by UTC weekday and hour). The usage record in the transcript carries `cost`, the session's sum shows in the status line (`~0.0123 USD est.`) and in `:status`, and a subagent's or background task's counts in its parent's. Each session's estimate is kept locally in `<state>/costs.json` (the newest 1000 sessions, never sent anywhere), and `:status` and `maic models` give the average per session from it. Calls to a model the catalog does not price (local models, Anthropic, Claude Code) count nothing. DeepSeek's own bill is the truth: its prices change, and the catalog says when they were read.

### Rate limits and retries

Every API provider retries the same way, so a key is never hammered into a ban. A 429 that is not a used-up allowance, a 408, a 409, a 5xx and a refused connection are retried, at most `retries` times (3), and only while nothing has streamed. The wait is full jitter: a random time from 0 to the smaller of 60 s and 1 s doubled per attempt (1, 2, 4, 8 s), or the provider's `Retry-After` when it asks for longer. 400, 401, 402 and 403 are never retried, and a metered request whose answer was lost after it went out is not sent again.

Requests to one provider account (its name and `base_url`) share what they learn. A 429 holds every request to it, the ones already waiting and the ones that start, until that wait is over, with a notice when it is a second or more. Five 429s within 60 s open a circuit breaker: MAIC sends nothing to that provider for 60 s, the request that opened it ends with an error saying so, and every request meanwhile fails at once, unsent, saying how long is left. Then the breaker closes and requests go again.

`options.max_concurrent` caps MAIC's own open requests per model, below the provider's: DeepSeek's 429 counts open requests per account (2,500 for Flash, 500 for V4 Pro), so the shipped `deepseek` keeps to a third of each, 833 and 166. A request past the cap waits for a slot (said once per request), and passing half the cap is told once until it falls back to a quarter. Any provider takes the option, as a number for all its models or a table by model.

### Adding an OpenAI-compatible API

Any service that speaks OpenAI's `/chat/completions` takes a provider entry and a preset or two in `settings.lua`. The key stays in the environment: the entry names the variable, never the key.

```lua
providers = {
  acme = {
    kind = "openai",
    base_url = "https://api.acme.example/v1",  -- https; plain http only to loopback
    api_key_env = "ACME_API_KEY",
    options = { context_window = 200000, max_tokens = 32000 },
  },
},
models = {
  ["acme-large"] = { model = "acme/acme-large-2", context = 200000, tier = 30, subagents = { "acme-large" } },
},
```

Then `export ACME_API_KEY=...` and `maic --model acme-large`, or `:model acme-large` in a session. A remote provider with a key is metered unless it says `options.metered = false`: no automatic pick moves onto it from another provider ([settings.md](settings.md#model-presets-and-tiers)). The other options describe what the API can do, all optional: `think_on` and `think_off` (request fields for thinking on and off), `think_sampling` (sampler keys the API refuses or ignores while thinking), `replay_reasoning`, `vision`, `extra_body` ([settings.md, Providers](settings.md#providers)).

### DeepSeek, the worked example

DeepSeek ships built in, so all it needs is its key:

```sh
export DEEPSEEK_API_KEY=sk-...
maic --model deepseek-flash
```

What ships is the example above filled in from DeepSeek's documentation ([standards.md](standards.md#model-apis-and-protocol-prior-art)):

```lua
providers = {
  deepseek = {
    kind = "openai",
    base_url = "https://api.deepseek.com",
    api_key_env = "DEEPSEEK_API_KEY",
    options = {
      context_window = 1000000,  -- GET /models' figure replaces it when the read works
      read_models = true,        -- GET /models at first use: each model's window, output cap and effort levels
      max_concurrent = { ["deepseek-flash"] = 833, ["deepseek-v4-pro"] = 166 },  -- a third of DeepSeek's per-account limits
      max_tokens = 65536,  -- the API allows 384K; a lower cap bounds what one runaway reply costs
      think_on = { thinking = { type = "enabled" } },
      think_off = { thinking = { type = "disabled" } },
      -- while thinking, these do nothing, and top_p must be 0.95 to 1.0: a value outside is not sent
      think_sampling = { temperature = false, presence_penalty = false, frequency_penalty = false, top_p = { 0.95, 1.0 } },
      -- without thinking top_p is fixed at 1.0 and the penalties are ignored
      nothink_sampling = { top_p = false, presence_penalty = false, frequency_penalty = false },
      retry_empty = true,            -- a `stop` with nothing in it is sent again, within `retries`
      replay_reasoning = true,       -- every earlier turn's reasoning_content goes back with a request that carries tools
      vision = { "deepseek-flash" }, -- Pro takes no pictures: one sent to it becomes a note saying so
    },
  },
},
```

| Preset | Model | Thinking | Tier |
| :--- | :--- | :--- | ---: |
| `deepseek-pro` | `deepseek/deepseek-v4-pro` | on | 35 |
| `deepseek-pro-nothink` | `deepseek/deepseek-v4-pro` | off | 35 |
| `deepseek-flash` | `deepseek/deepseek-flash` | on | 25 |
| `deepseek-flash-nothink` | `deepseek/deepseek-flash` | off | 25 |

Thinking depth is DeepSeek's top-level `reasoning_effort` (`low`, `high`, the default, or `max`): `think_on = { thinking = { type = "enabled" }, reasoning_effort = "max" }` under `providers.deepseek.options` asks for the most. Titles and other calls MAIC makes with thinking off send `thinking = { type = "disabled" }`, since DeepSeek thinks unless told not to. MAIC never sends `tool_choice` (DeepSeek refuses a forced one while thinking), `user_id`, or any field DeepSeek does not document: no session ids, no telemetry.

Each has a 1M context, the four as its `subagents` list, and `deepseek-flash-nothink` as its reviewer. They work as the session's model, as a `task` subagent's, and with `:model`, which also says METERED. A thinking turn's `reasoning_content` is kept in the transcript with the turn, as received, and replayed on every assistant turn while tools are in the request, so tool use over many turns works, after a resume too; a turn without one (another model wrote it, or thinking was off) goes back with an empty one, which DeepSeek's documentation does not settle (the live check below covers it). Each `usage` record in the transcript carries `cached`, the prompt tokens DeepSeek served from its cache, and `cost`, the estimate for that call. A tool call cut off by `max_tokens` (`finish_reason: length`) is dropped, never run; `insufficient_system_resource` and `aborted` end the call with an error saying so, and `content_filter` keeps the text with a note.

**The live check** (needs a real key; MAIC's tests never call the API): `DEEPSEEK_API_KEY=sk-... maic --model deepseek-pro`, ask for something that reads two files, send a second message, quit, `maic -r` and send a third; then `:model deepseek-flash-nothink` and once more. Every turn should succeed, and `cached` in the transcript's usage records should rise.

## API keys: the environment variable names

Until MAIC has its own keystore (planned: keys stored encrypted with argon2id), API keys come from the environment, or from a command. Keys never go in a settings file (`api_key` there is an error). The names MAIC reads:

| Provider | Variable | Notes |
| :- | :- | :- |
| `anthropic` (the paid API) | `ANTHROPIC_API_KEY` | Not needed for `claude-cli`, which uses your Claude Code login. |
| `deepseek` | `DEEPSEEK_API_KEY` | For `deepseek-pro`, `deepseek-flash` and their `-nothink` variants. |
| `openrouter` | `OPENROUTER_API_KEY` | |
| A provider you add | whatever its `api_key_env` names | In your global settings file only. |

Set the variable in the shell that starts `maic` (or the daemon); MAIC reads it from its own environment:

```sh
export DEEPSEEK_API_KEY=sk-...
maic --model deepseek-pro
```

To keep a key out of the environment, give the provider `api_key_command` instead: MAIC runs the command and reads the key from its output, for example `api_key_command = "pass show deepseek/api-key"`.

Either way, MAIC strips every key variable (any `*_API_KEY`, and any name a provider's `api_key_env` gives) from the commands a model runs in the sandbox, from `claude -p`, and from the helper programs it starts, and redacts a key that shows up in an error.

## Code completion

Copilot-style suggestions in neovim come from [llama.vim](https://github.com/ggml-org/llama.vim) talking to `services/llamacpp-fim.json`: llama-server on `127.0.0.1:8084` serving a Qwen2.5-Coder **base** model (Instruct models are worse at fill-in-the-middle) through `/infill`. It is not a chat model, so there is no chat provider for it.

```sh
maic models install qwen2.5-coder-7b --link   # once: the coder, linked as <models_dir>/fim/current.gguf
maic up llamacpp-fim                          # the server on 8084; MAIC loads the coder once it is up
```

**Set it up with one command.** With lazy.nvim, `maic nvim setup llama-vim` writes the spec below as one file MAIC owns, `maic-llama-vim.lua` in the directory your spec imports (`{ import = "plugins" }`: `~/.config/nvim/lua/plugins/`), after showing it and asking; `--dry-run` only shows it, `--remove` deletes it again. It never edits another file: without lazy.nvim, or with no import directory, it says what is missing and prints the spec to add by hand, and it refuses when llama.vim is already in your spec elsewhere. Details in [nvim.md](nvim.md#maic-nvim-setup-llama-vim).

The llama.vim spec for lazy.nvim:

```lua
{
    'ggml-org/llama.vim',
    init = function()
        vim.g.llama_config = {
            endpoint_fim = 'http://127.0.0.1:8084/infill',
            model_fim = 'current',  -- the coder `maic models install ID --link` chose
            keymap_fim_trigger = '<M-f>',      -- off the leader in insert mode (below)
            keymap_fim_accept_word = '<M-]>',
            keymap_inst_accept = '',           -- completion only: leave normal mode alone (below)
            keymap_inst_cancel = '',
        }
    end,
}
```

**The keys, and why two of them move.** llama.vim's defaults put two insert-mode keys under the leader: `<leader>llf` asks for a suggestion and `<leader>ll]` accepts its first word. With a Space leader those are insert-mode mappings that start with a space, so every space typed in insert mode waits `timeoutlen` (or the next key) before it appears, and typing " ll" waits again; the trigger is mapped in every buffer on `InsertEnter`. `<M-f>` and `<M-]>` are chords that type nothing. `maic nvim keymaps` (`:checkhealth maic` in nvim) checks llama.vim's keys against your mappings; against `nvim -u NONE` with this spec and llama.vim (master, e1ca1cc) loaded with a Space leader, it finds both free in nvim 0.12 in insert mode and reports the defaults as the typing delay above. Without a mapping nvim treats an Alt chord in insert mode as Esc followed by the key, so `<M-]>` does that when no suggestion shows.

`<Tab>` and `<S-Tab>` stay. llama.vim maps them (with `<M-]>`, `<C-L>` and `<C-H>`) buffer-local in insert mode only while a suggestion shows, and removes them with `iunmap <buffer>` when it hides the suggestion (`fim_render` and `fim_hide` in `autoload/llama.vim`). The rest of the time nvim 0.12's own insert-mode `<Tab>` / `<S-Tab>` (jump to the next or previous snippet field when a snippet is active, else the key itself) work as before; while a suggestion is showing inside an active snippet, `<Tab>` accepts the suggestion instead of jumping. The `iunmap <buffer>` also removes a buffer-local insert-mode `<Tab>` of another plugin, which the check reports. In normal mode llama.vim maps `<Tab>` (accept an instruction's result, otherwise it feeds `<Tab>`, the jump forward that is also Ctrl-I) and `<Esc>` (cancel an instruction, otherwise nothing) globally for as long as it is enabled, not only while an instruction runs; a normal-mode `<Esc>` of your own (`:nohlsearch`, say) and llama.vim's replace each other in load order, so set `keymap_inst_cancel = ''` to keep yours.

**Normal-mode `<Tab>` and `<Esc>`, and why the spec turns them off.** Besides its insert-mode keys, llama.vim maps normal-mode `<Tab>` (accept) and `<Esc>` (cancel) for its instruction mode, and it maps them globally for as long as it is enabled, not only while an instruction runs. Outside an instruction `<Tab>` falls back to a plain `<Tab>` and `<Esc>` does nothing. Three things follow:

1. **Load order decides who wins.** If you or a plugin map normal-mode `<Esc>` or `<Tab>`, one mapping silently replaces the other, depending on which loads last. The likeliest case is `<Esc>` for clearing search highlights (`:nohlsearch`), which kickstart.nvim and LazyVim both ship: either the highlight clearing stops working or llama.vim's instruction cancel does, and nothing says which.
2. **`<Tab>` is also `<C-i>`.** Most terminals send the same keycode for both, so a global `<Tab>` mapping also captures `<C-i>` (jump forward in the jumplist). llama.vim's fallback keeps `<C-i>` working, but anything later mapped on `<C-i>` or normal-mode `<Tab>` (a buffer-switching plugin, say) fights with it the same way as in point 1.
3. **They serve a feature completion does not use.** The two keys belong to instruction mode (select code, type an instruction, accept or cancel the rewrite). Fill-in-the-middle completion uses only the insert-mode keys above, and those exist only while a suggestion shows.

So for completion alone, the spec sets `keymap_inst_accept = ''` and `keymap_inst_cancel = ''`, which leaves normal mode untouched. If you want instruction mode, give those two keys values of your own, or keep llama.vim's and accept the above. Either way, a collision is named by both sides in `:checkhealth maic` and `maic nvim keymaps`, and a lazy.nvim update that introduces one is reported at MAIC's next start (the keymap re-check after a `lazy-lock.json` change; see [nvim.md](nvim.md)).

`model_fim` needs a llama.vim recent enough to have it (it is in the current README). Keep it `current`: the server loads only the model MAIC asks for, so another folder's name is refused ("model is not loaded").

**When completions stop while ComfyUI runs**, that is MAIC keeping the card for ComfyUI: starting ComfyUI (or whisper) unloaded the coder, and llama.vim's requests are refused until it is loaded again, so llama.vim simply shows no suggestion. `maic gpu` says `unloaded (maic gpu load llamacpp-fim)`. `maic gpu load llamacpp-fim` (`:gpu load llamacpp-fim` in MAIC) brings the suggestions back at once; stopping that service through MAIC (`maic down comfyui`, `maic down whisper`) brings them back by itself, when the coder was loaded before it was unloaded for them. Tab accepts a suggestion, Shift-Tab its first line; llama.vim's own `:help llama` has the rest. Instruction editing (`<leader>lli`) uses `endpoint_inst`, a chat model: point it at `http://127.0.0.1:8081/v1/chat/completions` with `model_inst = 'Qwen3.5-4B-Q4_K_M'` if you want it, or leave it unused.

**Router mode, and why.** The server runs llama-server in router mode (`--models-dir <models_dir>/fim --models-max 1`), like `llamacpp` and `llamacpp-2`. The vendored llama.cpp (b11284) proxies `/infill` in router mode (`routes.post_infill = models_routes->proxy_post` in `tools/server/server.cpp`), routing by the request's `model` field, which llama.vim sends as `model_fim`. So `current.gguf` is the model `current`, `/models/unload` and `/models/load` work, and the GPU turn-taking treats it like the other llama servers: `maic up comfyui` and `maic up whisper` unload it first, `maic gpu` shows it as `loaded`, `unloaded (maic gpu load llamacpp-fim)` or `not linked`, and `maic gpu free llamacpp-fim` unloads it without stopping it.

**Autoload off, and why completion differs.** `llamacpp` and `llamacpp-2` load a model on the next request, which is right for them: a chat request is something you sent. llama.vim sends a completion request at every pause while typing, so with autoload on the next keystroke after MAIC unloaded the coder for ComfyUI would load it again behind ComfyUI's back and could push ComfyUI out of memory. So the completion server runs with `--no-models-autoload` (the router refuses a request for a model that is not loaded) and MAIC loads `current` itself with `POST /models/load`: after `maic up llamacpp-fim` reports ready (waiting until it is loaded; a failure names the card's budget), on `maic gpu load llamacpp-fim`, after a relink, and when the last service that unloaded it stops through MAIC. Which services owe it back is kept in `<state>/maic/run/llamacpp-fim.evicted`; a coder that was already unloaded when ComfyUI started is not loaded when ComfyUI stops.

The flags follow llama.vim's recommended server line (`--fim-qwen-7b-default`: `-ub 1024 -b 1024 --cache-reuse 256`, all layers on the GPU) with two changes: the model is the local one rather than the preset, which would download from Hugging Face (and MAIC's llama.cpp is built without TLS, so it could not anyway), and the context is 8192 rather than the model's full 32k, so the 7B fits an 8 GB card. llama.vim's ring of context from other files is trimmed by the server to fit; with the 3B or 1.5B there is room to raise `--ctx-size` in the service file to 16384 or 32768 (see the VRAM table).

**Which coder.** The 7B is the default: the best suggestions, 5.4 GB at 8k, so on an 8 GB card it runs alone (the agent on 8081 has to be unloaded, or remote). The 3B (3.6 GB) fits beside the 4B agent at 16k. The 1.5B (2.1 GB) fits beside the 4B agent or whisper. Generation on a GPU is bound by memory bandwidth, so speed roughly follows file size: the 3B at Q8_0 (3.1 GB) generates about 1.4 times as fast as the 7B (4.4 GB), the 1.5B (1.5 GB) about 2.8 times; prompt processing scales with parameter count, which favours the small ones more. These are estimates; llama.vim shows the real timings of every suggestion (`show_info`). Switch with `maic models install qwen2.5-coder-3b --link` (or `maic vendor use llamacpp <models_dir>/fim/FOLDER/FILE.gguf`): a running server unloads the old coder and loads the new link, no restart; while the coder is unloaded for ComfyUI the new link waits for `maic gpu load llamacpp-fim`.

**Nothing leaves the machine.** llama.vim sends the code around the cursor to 127.0.0.1:8084; the server is MAIC's own llama.cpp build, bound to loopback, without TLS, serving a file already on disk. The only network access is `maic models install`, when you type it.
