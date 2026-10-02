# Settings

Settings are Lua files that return a table (JSON with the same keys works too). They are layered, and every key is optional; MAIC runs fine with no files at all:

1. `~/.config/maic/settings.lua` (or `$XDG_CONFIG_HOME/maic/settings.lua`): yours, for every project. `maic settings init` writes one with every default and a comment; `maic settings path` shows where it goes.
2. `<dir>/.maic/settings.lua` for each directory on the [chain](#project-layers-trust-and-the-chain), from the project root (or just under `$HOME`) down to the workspace: the project's, meant to be committed. Only a directory you trusted counts.
3. `<dir>/.maic/settings.local.lua` next to each of those: personal overrides, keep it out of git.

diction's settings are a file of their own beside the global one, `~/.config/maic/diction.lua`, read only by diction and evaluated at `global_lua`, like this file (`maic settings read diction`; see [diction.md](diction.md#configuration)).


At each location a `settings.lua` is used when it exists, else a `settings.json` (`maic settings init --json` writes that form). Nearer files win. Scalars replace (`theme` too), `providers` merge by name, `style` merges by role. `:settings` in a session lists the files that were read; `maic init` (or `:init`, which also has the agent draft the `MAIC.md`) scaffolds a project's.

Because a settings file is code, it can decide things per machine:

```lua
local model = maic.hostname == "laptop" and "llamacpp/Qwen3.5-4B-Q4_K_M" or "llamacpp/current"
return {
  model = os.getenv("MAIC_MODEL") or model,
  mode = "auto-read",
  models_dir = maic.home .. "/models",
  providers = {
    anthropic = { api_key_command = "pass show anthropic/api-key" },
  },
  style = { user = { fg = "#ff8800", bold = true } },
}
```

Your global file runs with LuaJIT and the standard library; `maic.home`, `maic.hostname`, `maic.workspace` and `maic.version` are set. A project's file runs at the Lua level you trusted its directory with: fully, like yours, or sandboxed or restricted when the project is only partly yours ([below](#lua-levels)). Keys in the JSON examples below are the same in Lua (`sessions_home = "auto"`).


```jsonc
// the same keys, in JSON form
{
  "model": "llamacpp/current",
  "mode": "manual",
  "think": false,
  "markdown": true,
  "mouse": true,
  "sessions_home": "auto",
  "leader": "space",
  "providers": { ... },
  "style": { ... }
}
```

| Key | What |
| :--- | :--- |
| `model` | `provider/model`, or a bare model name for the first provider. Default `llamacpp/current`: the vendored llama-server ([llamacpp.md](llamacpp.md)), which serves whatever GGUF you linked under the name `current`. `--model` on the command line and `:model` in the session override it. |
| `mode` | `manual`, `auto-read`, `edit`, `auto` or `plan` (see [cli/README.md](../cli/README.md#modes)). |
| `think` | Ask the model to reason before answering. Slower; better on hard problems. |
| `markdown` | Render markdown in the conversation window (`:set markdown off` for raw text). The input box always highlights markdown. |
| `mouse` | Scroll wheel support. With it on, the terminal's own text selection needs Shift+drag; `:set mouse off` turns it off for a session. |
| `record` | Keep transcripts of interactive sessions (default `true`). `false` writes them to the runtime directory instead, where they vanish at logout; `--record` / `--no-record` override per session. |
| `compact_at` | Auto-compact when the last model call used this share of the context window (default `0.75`; `0` disables). Old tool results are stubbed first; the oldest turns are summarised only if that was not enough. See `:h compact`. |
| `compact_keep_results` | Tool results that are never stubbed, counting from the most recent (default `4`). |
| `context` | The context window in tokens (default `16384`): the local llama.cpp server's `--ctx-size` and the readout. `--ctx N` and `:ctx N` override and restart the server. |
| `context_2` | The same for the side server, `llamacpp-2` on port 8082 (default `8192`; `${MAIC_CONTEXT_2}` in its service file). `--ctx2 N` and `:ctx2 N` override and restart it. Two models share the card, so this is the window to lower first; `maic gpu` says whether the pair fits ([llamacpp.md](llamacpp.md), Two servers). |
| `models` | Presets by short name, adding to the built-in ones or changing them field by field: `models = { ["opus-5.5"] = { limited = true } }` changes only that field. A new name needs `model`. Fields: `model`, `context` (the provider's window for the readout, and the server's size for a local model: `context` for `llamacpp`, `context_2` for `llamacpp-2`), `think`, `reviewer`, `tier`, `limited`, `subagents`, `subagent`, `on_limit`; see [Model presets and tiers](#model-presets-and-tiers). Built in: `fable-5.1`, `opus-5.5`, `sonnet-5`, `haiku-4.5`, `qwen-4b`, `qwen-9b` (text, 16k), `qwen-9b-vision` (8k). None is shipped for the side server; the pattern is `models = { ["qwen-4b-side"] = { model = "llamacpp-2/Qwen3.5-4B-Q4_K_M", context = 8192 } }`. `--model NAME` and `:model NAME` accept a preset's name. |
| `models_dir` | Where model files live. llama.cpp's router serves every GGUF under `<models_dir>/llamacpp/` (`${MAIC_MODELS}` in service files). Also ComfyUI's model folders (`checkpoints/ diffusion_models/ loras/ text_encoders/ vae/ upscale_models/ ...`): `maic vendor wire comfyui` writes its `extra_model_paths.yaml` from them; a docker service may mount it. `maic setup` asks for it (see [vendor.md](vendor.md)). |
| `forbid` | Terms no tool call may contain, in any letter case (`/.../` for a POSIX extended regex): a search pattern, a command, a path or any argument with one is halted before it runs, under the dumb harness too. Layers add to the built-in list; `:forbid` at run time. |
| `permission` | `{ allow = {...}, ask = {...}, deny = {...} }` of `tool:pattern` entries, a glob over the tool's argument: `run_shell:pytest *`, `write_file:src/**`, `read_file:/etc/**`; `write:` and `read:` stand for any writing or reading tool, a file pattern matches the path as given and relative to the workspace, `~` expands. `deny` wins over `ask` wins over `allow`. An `allow` entry runs in every mode but plan without an approval prompt or the reviewer (that is the allow list, `:allow`), and a `run_shell:` one matches only one simple command (no `;`, `&`, `|`, line break, backtick, `$(`, `<`, `>`); `ask` turns an action the mode would run silently into a prompt; `deny` refuses it with a reason the model reads. A `run_shell:` deny or ask entry matches the whole line and also each command in it: the line split on `;`, `&`, `|`, `&&`, `||` and line breaks, plus what is inside `$(...)`, `(...)` and backticks, each without a leading `{`, `!`, shell keyword or `VAR=value`, so `true; git push`, `x | git push`, `$(git push)` and `GIT_TRACE=1 git push` all meet a `run_shell:git push*` entry. The block is additive: it runs after the trip patterns, secret and system paths, forbidden terms and an isolated session's fence and cannot lift any of them, and `allow` entries are ignored for a remote origin. MAIC's own helpers are always allowed. Layers add up. See `:h permission`. |
| `allow` | The older spelling of `permission.allow` for commands: a list of command patterns, each kept as a `run_shell:` entry. Still accepted; `:allow` edits these at run time. |
| `agents` | Agents by name (opencode's term and built-ins), adding to or narrowing the built-in `build`, `plan`, `general` and `explore` (see `:h agent`): `agents = { explore = { budget_tokens = 20000 }, docs = { mode = "edit", role = "subagent", write_paths = { "docs/**" }, tools = { "read_file", "edit_file", "write_file" }, model = "qwen-4b" } }`. Fields: `mode` (the most the agent allows; the session's mode caps it), `role` (`primary`, `subagent` or `all`: who may run it; the `task` tool runs `subagent` and `all`. opencode names this field `mode`; MAIC says `role` because `mode` already means the harness mode on the same definition), `description` (what it is for; the model reads it with the `task` tool), `write_paths` (globs over the path relative to the workspace; empty means the whole workspace), `read_outside`, `budget_tokens`, `max_steps` (default 40), `tools` (an allow-list of tool names; a list with no write tool makes the agent read-only), `reviewer` (the smart harness reviews its actions, when the session's is on) and `model` (a preset or `provider/model`: your pin; empty, the session's preset chooses, see [Model presets and tiers](#model-presets-and-tiers)). A built-in can only be narrowed: a wider mode, a tool it lacks, a bigger budget or reads outside when it has none are errors when settings load, and no agent gets the network. `profiles` is the older name of this key, and `orchestrator`, `reviewer`, `builder` and `scout` the older names of `build`, `plan`, `general` and `explore`; both are still read. |
| `rules` | Standing one-line instructions (`{ "always answer in French" }`) carried with `system_prompt` at both ends of the system prompt and in the per-turn note; layers add up. `--rule` and `:rule` at run time. A rule is a request; `prefill` is a guarantee. |
| `prefill` | Text every reply starts with, sent as the opening of the assistant turn so the model continues it (a guarantee, where `system_prompt` is a request). `--prefill` and `:prefill` override. A prefilled turn rarely calls tools. |
| `system_prompt` | Operator text placed first in every system prompt, before MAIC's briefing and any instruction file; `"@~/path"` reads a file. `--system` and `:system` override. Front-loads behaviour. |
| `load_instructions` | `false` loads no instruction file anywhere (default `true`); `--no-instructions` and `:instructions off` do it per session. Independent of `system_prompt`; combine them to run on your own text alone. |
| `tripwire` | `"machine"` (default): a trip sets the root-owned lock every MAIC process respects; `maic unlock` asks for sudo. `"session"`: a trip locks that session only, in a file beside its transcript, and `:unlock` removes it without sudo. Nearer settings files win, so a project can choose per project. |
| `allow_isolated` | `true` permits `tripwire = "isolated"`, a session that opts out of the machine lock (default `false`). Such a session is confined: no reads outside its directory, no remote requests, no server work. |
| `browser` | What `maic open SERVICE` / `:open` uses: `default` (the system's browser), `firefox`, `chrome`. |
| `remote` | A maic-server you subscribe to (`https://host:7373`). When it answers, `maic open SERVICE` opens the remote's copy of the service and `maic open server` its web client. |
| `lazy_lock` | nvim's `lazy-lock.json`, watched for plugin and lazy.nvim updates (`~` expands). Default empty: `$XDG_CONFIG_HOME/$NVIM_APPNAME/lazy-lock.json`, so `~/.config/nvim/lazy-lock.json`. See [lazy-lock.md](lazy-lock.md). |
| `lazy_lock_notice` | `false` turns off the start notice and the status strip's `lock≠` when the lock file is out of sync with its recorded hash (default `true`). `maic status`, `maic doctor` and `maic lazy-lock` still report. |
| `harness` | `"smart"` (default): a model reviews every command or write the rules would allow without asking, see `:h harness`. `"dumb"`: the rule list alone. |
| `reviewer_model` | Pins the model that reviews under the smart harness (`provider/model` or a preset). Default empty: the preset's `reviewer`, else `small_model`, else the family's small model (see [Model presets and tiers](#model-presets-and-tiers)). When the reviewer is the session's own model on `llamacpp` and the side server `llamacpp-2` is up, the review goes there with the same model name, so the main server never has to evict its model for a review. `reviewer_model = "llamacpp-2/Qwen3.5-4B-Q4_K_M"` fixes that choice (and the model) regardless; see [llamacpp.md](llamacpp.md), Two servers. |
| `reviewer_budget_tokens` | The reviewer's own token cap (default `0`, none). Its tokens always count toward `budget_tokens`; past this cap it stops, with one notice, and every action it would have reviewed is asked instead. `:harness` shows what it has spent. |
| `dumb_auto_ok` | `true` skips the once-per-session warning when entering auto mode under a dumb harness (default `false`). |
| `bans` | `{ strings = {...}, patterns = {...}, tokens = {...}, retries = 3, replacement = "[banned]", ignore_case = false, window = 64 }`. Strings and POSIX regex patterns are enforced by MAIC on every provider (cut before they show, re-asked, then replaced); tokens (ids or text) become `logit_bias` on OpenAI-compatible providers. Layers add strings, patterns and tokens. `--ban`, `--ban-pattern` and `:ban` at run time. See [bans.md](bans.md). |
| `sampling` | Sampler keys sent with every request: `temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty`, and on llama.cpp-style servers `xtc_probability` / `xtc_threshold`. A provider's `options.sampling` overrides it; `:sampling` changes it live. Nothing is sent to Anthropic. |
| `budget_tokens` | Stop the agent once input plus output tokens over the session reach this (default `0`, unlimited); `:budget` changes it live. |
| `small_model` | opencode's `small_model`: one cheap model for auxiliary calls, a preset or `provider/model`. It titles each session after its first turn, for `maic sessions` (empty: no titles; a remote model is never used for a local session), and it is the reviewer's default when the preset names none. `title_model` is its older name and is read when `small_model` is unset. |
| `timestamps` | Show a time beside each conversation entry (default `false`; `:set timestamps on`). |
| `leader` | The vim leader key for normal and visual modes: `"space"` (default) or a single character. `<leader>y` yanks to the system clipboard, `<leader>p` pastes from it. |
| `server` | `maic server`: `listen` (default `127.0.0.1:7373`; any other address turns TLS on), `workspaces` (directories a remote session may open; default `~/dev2`, else the current directory), `cert` and `key` (a PEM pair; empty makes a self-signed one under `~/.local/state/maic/server/`), `relay` (`https://host:port` of a `maic-relay` the server dials out to and holds open, so a phone paired with `maic server pair` reaches it from anywhere, end to end encrypted; default empty, no relay), `relay_cert` (a PEM that pins a self-signed relay certificate; empty means the system CA store). See [remote.md](remote.md). |
| `highlight` | The input's highlighter: `"builtin"` (default, MAIC's markdown renderer) or `"nvim"`: one `nvim --embed --headless` is started on the first keystroke and asked over msgpack-rpc for treesitter's highlight captures of the text as markdown (headings, code, emphasis, links, lists, and inside fenced blocks the keywords, strings and comments of the languages nvim has parsers for). It gets 50 ms per keystroke; when nvim is missing or fails, a notice says so and the built-in one is used. `:set highlight nvim\|builtin` for a session. See `:h highlight`. |
| `theme` | A theme by name (default `"default"`, the built-in look): `gruvbox-dark`, `gruvbox-light`, `mono`, or a file of yours in `~/.config/maic/themes/NAME.lua`. The nearest layer wins. A theme that fails to load is reported at start with its file and line and the default is used instead. `:theme NAME` switches live. See [Styles and themes](#styles-and-themes) and [themes.md](themes.md). |
| `bare` | `true`: nothing from nvim (no `$NVIM` host connection, the built-in highlighter even with `highlight = "nvim"`, no theme from nvim, `:theme nvim:NAME` refused, no lazy-lock notice or `lock≠`, no keymap check); MAIC's own settings, themes, Lua and tools load as usual. The same as `maic --bare` and `MAIC_BARE=1`; in a settings file it takes effect once the files are read, so a host connection made before is dropped then ([nvim.md](nvim.md#bare)). Default `false`. |
| `follow_nvim_theme` | Inside nvim with a connected host ([nvim.md](nvim.md)): `true` (default) makes the theme follow the host's colorscheme live, read with `nvim_get_hl` on connect and after every `ColorScheme` there, mapped like an import and applied as the session-only theme `nvim:NAME` (never written to a file). `false` keeps `theme`. `:theme NAME` stops following for the session, `:nvim theme` resumes. |
| `colors` | The colour depth: `"auto"` (default: truecolor when `COLORTERM` is `truecolor` or `24bit`, else 256 colours when `TERM` (or `COLORTERM`) contains `256`, else the 16 ANSI colours), `"truecolor"`, `"256"` or `"16"`. Below truecolor a `#rrggbb` becomes the nearest xterm-256 colour (the 6x6x6 cube and the grey ramp, by squared distance in sRGB) or the nearest of the 16. |
| `enter_sends` | `true`: in insert mode Enter sends a one-line input, Shift+Enter or Alt+Enter inserts the line break, and an input that already has several lines keeps Enter as a line break. Default `false`, the vim-like behaviour: Enter is always a line break and Alt+Enter or `:w` sends. `:set enter_sends on\|off` for a session. |
| `server` | `maic server`: `listen` (default `127.0.0.1:7373`; any other address turns TLS on), `workspaces` (directories a remote session may open; default `~/dev2`, else the current directory), `cert` and `key` (a PEM pair; empty makes a self-signed one under `~/.local/state/maic/server/`). See [remote.md](remote.md). |
| `global_lua` | Global file only. How your own Lua data files run: `settings.lua`, your themes and MAIC's shipped ones, `diction.lua`. `"full"` (default): the full standard library, as they always have. `"sandbox"`: a child process that cannot reach the system; `"restricted"`: the restricted state in MAIC's own process ([Lua levels](#lua-levels)). MAIC has to know before the file runs, so it reads this from the file's text: write it literally, `global_lua = "sandbox"`; a value computed in Lua is an error. |
| `lua_memory_mb` | Global file only. The memory cap of settings Lua at the sandbox and restricted levels, in MB (default `256`): the sandbox's child process may grow by this much, and the restricted state's heap may reach it. Read literally from the global file for the global file itself, like `global_lua`. |

| `trust_strictness` | Global file only. The default trust tier for a trusted directory whose files change: `"strict"`, `"standard"` (default) or `"relaxed"`. See [Project layers, trust and the chain](#project-layers-trust-and-the-chain). |
| `trust_identities` | Global file only. The author emails that are yours, for the standard tier (`{ "me@example.com", "work@example.com" }`). Empty (default): `git config --global user.email`. Never read from a repository. |
| `trust_levels` | Global file only. A tier per directory, `{ ["~/dev2/app"] = "relaxed" }`. A tier set with `maic trust PATH --level L` (kept in `<state>/trust.json`) comes first. |
| `instructions` | Global file only. Which instruction files the model sees and where the chain ends: `files` (the classes, lowest priority first; default `{ "CLAUDE.md", "AGENTS.md", "MAIC.md" }`), `read` (`"all"` or `"highest"`), `local_files` (`MAIC.local.md` and the like, default `true`), `imports = { depth = 4 }` (`@path` imports; `0` turns them off), `extra_dirs` (default `false`), `project_markers` (default `{ ".git", ".maic", "MAIC.md" }`) and `bound`: `"project"` (default) stops at the project root, the nearest directory at or above the workspace holding a marker; `"home"` goes up to just under `$HOME` as MAIC did before. Every option, the reading order and why: [instructions.md](instructions.md). The old `instruction_files` key is replaced by `instructions.files` and only warned about. |
| `sessions_home` | Where new transcripts go. `auto` (default): under `sessions/projects/<encoded workspace>/` when the workspace has an instruction file (a name in `instructions.files`; or one is on the chain above it), else `sessions/general/`. Or force it: `general`, `project`, or any name (`sessions/<name>/`). A project can set this in its `.maic/settings.json`; `maic sessions rehome` moves existing transcripts. |
| `init_move_outside_reads` | `:init` moves the running session into the project's home without asking when it wrote nothing outside the project and read at most this many files outside it (default `3`); with more, it asks. See [sessions.md](sessions.md#homes). |

## Model presets and tiers

A preset carries, besides its model, context and thinking, what it may hand work to:

| Field | What |
| :--- | :--- |
| `tier` | Higher is stronger, and costs more. Also the reviewer's cost measure. |
| `limited` | Your plan caps this model's usage. Subagents and the reviewer step aside from it. |
| `subagents` | The presets a subagent of this model may run on, higher or lower tiers; the model itself is always allowed. |
| `subagent` | The preferred pick for a subagent: `"same"`, or a preset. Empty: the rule below. |
| `on_limit` | Where a subagent continues when this model reports a usage limit. Empty: the rule below, without "same". |
| `reviewer` | `"same"` (the model reviews itself), a preset or `provider/model`. Empty: `small_model`, else the family's small model. |

Shipped:

| Preset | Tier | Limited | Subagents | Subagent pick | Reviewer |
| :--- | ---: | :--- | :--- | :--- | :--- |
| `fable-5.1` | 50 | yes | fable-5.1, opus-5.5, sonnet-5, haiku-4.5 | opus-5.5 | haiku-4.5 |
| `opus-5.5` | 40 | no | the same four | itself | haiku-4.5 |
| `sonnet-5` | 30 | no | the same four | itself | haiku-4.5 |
| `haiku-4.5` | 20 | no | the same four | itself | haiku-4.5 |
| `qwen-9b`, `qwen-9b-vision` | 12 | no | qwen-9b, qwen-9b-vision, qwen-4b | itself | itself |
| `qwen-4b` | 10 | no | the same three | itself | itself |

`fable-5.1` ships limited because Fable plans commonly carry a usage cap. The local presets list only local presets, so a local session's data leaves the machine only when you add a cloud preset to a list.

**A subagent's model**, first match wins (each recorded as `model_reason` in the parent's tool record and the child's start record): the agent's `model` (your pin); the `task` call's `model`, which may be any preset on the session preset's `subagents` list (anything else is an error that lists them with their tiers); the preset's `subagent`; the rule: the same model when it is not limited; when it is, the strongest non-limited preset on its list below its tier, else the strongest non-limited one on the list, else the same model; the session's model when it is not a preset. The parent model is told the presets it may use with their tiers, the default first and why, and to choose lower for wide reads, searches and mechanical work and higher only for a hard reasoning subtask.

**A usage limit.** When a subagent's model call fails with a usage limit (Anthropic's "You've reached your Fable limit", a workspace usage limit, OpenAI's `insufficient_quota`, a 402; a per-minute rate limit is not one, and only that kind is retried), the child continues the same job on its preset's `on_limit` with the conversation so far, and you see `explore: fable-5.1 hit its usage limit; continuing on opus-5.5`. A second limit ends the child with an error naming both models. The session's own model never switches by itself: the error says which `:model` continues on the next tier.

**The reviewer**, first match wins: `reviewer_model`; the preset's `reviewer`; `small_model`; the family's small model, the lowest-tier non-limited preset on the preset's `subagents` list (haiku-4.5 for the Anthropic presets; a local model is its own, free and already loaded); the model itself. The term `small_model` and its meaning are opencode's (opencode picks a cheap model per provider for auxiliary calls such as titles; MAIC picks it from the preset's family by tier). When the reviewer hits a usage limit it is replaced for the rest of the session by the failed preset's `on_limit`, else the cheapest non-limited preset on the list, never a higher tier than the one that failed, with one notice; with none left the reviewer is off and every action it would review is asked. The transcript's `review` object carries `model` and `model_reason`.

Examples:

```lua
-- Opus on a plan that caps it: its subagents go to Sonnet, and so do Fable's.
models = { ["opus-5.5"] = { limited = true } }
-- A cloud preset that may hand cheap scans to a local model. Such a scan then runs on the local GPU.
models = { ["opus-5.5"] = { subagents = { "opus-5.5", "sonnet-5", "haiku-4.5", "qwen-4b" } } }
-- Reviews on Sonnet rather than Haiku.
small_model = "sonnet-5"
```

## Providers

Where models come from. MAIC ships with `llamacpp` (local, the default), `llamacpp-2` (the local side server), `anthropic`, `deepseek` and `openrouter`; a `providers` entry adds a new one or changes a shipped one by name.

| Shipped | Kind | `base_url` | Key |
| :--- | :--- | :--- | :--- |
| `llamacpp` | `openai` | `http://127.0.0.1:8081/v1` (the vendored llama-server, [llamacpp.md](llamacpp.md); every `sampling` key reaches it, including `xtc_probability`, `dry_multiplier`, `grammar`, `json_schema`, and `logit_bias` from token bans) | none |
| `llamacpp-2` | `openai` | `http://127.0.0.1:8082/v1` (the side server, `maic up llamacpp-2`: the same router over the same GGUFs, so a second model stays resident; `context_window` follows `context_2`) | none |
| `anthropic` | `anthropic` | `https://api.anthropic.com` | `ANTHROPIC_API_KEY` |
| `deepseek` | `openai` | `https://api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `openrouter` | `openai` | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |

```json
"providers": {
  "anthropic": { "api_key_command": "pass show anthropic/api-key" },
  "deepseek":  { "api_key_env": "DEEPSEEK_API_KEY" },
  "lmstudio":  { "kind": "openai", "base_url": "http://127.0.0.1:1234/v1" },
  "work":      { "kind": "openai", "base_url": "https://llm.example.com/v1", "api_key_env": "WORK_LLM_KEY",
                 "options": { "extra_body": { "temperature": 0.2 } } }
}
```

| Field | What |
| :--- | :--- |
| `kind` | `anthropic` (Messages API), or `openai` (any OpenAI-compatible `/chat/completions`: llama.cpp server, DeepSeek, OpenRouter, vLLM, LM Studio, ...). |
| `base_url` | Where it listens. A path prefix is fine (`https://openrouter.ai/api/v1`). |
| `api_key_env` | Environment variable holding the key. |
| `api_key_command` | A command that prints the key (a password manager). Keys themselves never go in this file; MAIC refuses an `api_key` field. |
| `options.mid_system` | OpenAI-compatible kinds only. `false` (default): a system message after the first (mode changes, resume notes, ban cuts) is sent as a user-role `[system note]`, because local chat templates such as Qwen's reject a second system message. `true` sends them as system, for servers that accept that. |
| `options.thinking_controls` | OpenAI-compatible kinds only. `true` sends `chat_template_kwargs.enable_thinking` and, when thinking is off, `reasoning_effort: "none"` (llama-server honours both; OpenAI's own API rejects unknown fields, so it is on only for `llamacpp` by default). |
| `options.operator_note` | Whether `system_prompt` / `--system` text is also appended to each user turn as the model sees it (default `true`; `false` for Anthropic, whose models follow the system prompt). Measured necessary for small local models once tool schemas are attached. |
| `options.sampling` | Any kind: a table merged into every request to that provider (`temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty`, and for llama.cpp-style servers their own keys such as `xtc_probability`). Anthropic's current models reject sampling parameters, so leave it unset there. |
| `options` | Kind-specific. Anthropic: `max_tokens` (64000), `effort` (`high`), `think_effort` (`xhigh`, used when `:think on`), `fallbacks` (`"default"` turns on server-side refusal fallbacks), `auth: "bearer"` for an OAuth token. OpenAI kinds: `extra_body`, merged into every request. |

Use a provider with `:model anthropic/claude-opus-5-5`, `:model deepseek/deepseek-chat`, `:model lmstudio/whatever-it-serves`, or `maic --model openrouter/some/model`. A bare name with no known prefix goes to the first provider, `llamacpp` (model names can contain `/`).

**Remote providers send data off this machine**: your prompts, every file the agent reads, and every command's output. MAIC says so when you switch to one and shows `REMOTE` in the status line. A provider is local when its `base_url` is on 127.0.0.1, localhost or ::1.

Anthropic models get thinking on by default with `effort` controlling depth, streamed tool input, and refusal fallbacks. Their history is replayed exactly as received (thinking blocks included) and never edited, which the newer models require; mode changes and instruction updates are appended as system messages instead.

## Styles and themes

Every colour MAIC paints is a **role**. What a role looks like comes from three layers, each over the one before:

1. the built-in default (the table in `themes/default.lua`);
2. the **theme** (`theme = "NAME"`): each role it sets replaces the default role whole, the roles it leaves out keep the default;
3. the `style` entries in settings: each merges over the theme's role, so `style = { user = { fg = "#ff8800" } }` changes the colour and keeps the theme's bold.

A style is `{ fg, bg, bold, dim, italic, underline, inverted }`. Colours are FTXUI names (`black red green yellow blue magenta cyan white gray gray_dark red_light green_light yellow_light blue_light magenta_light cyan_light default`), `#rrggbb`, or a 0-255 palette index. `italic` renders as dim (the terminal library has no italic). Writing a theme, the shipped ones and importing from neovim: [themes.md](themes.md).

| Role | Used for |
| :--- | :--- |
| `user` | your messages in the conversation |
| `assistant` | the model's replies |
| `thinking` | the model's reasoning, when thinking is on |
| `tool` | a tool call line (`▸ read_file ...`) |
| `tool_ok` | a tool's result |
| `tool_err` | a failed tool result; also `[n]` and `[N]` in the approval prompt |
| `notice` | MAIC's own notes (`※`), status-strip notes, the ISOLATED marker |
| `error` | errors (`✗`), the DUMB HARNESS marker, `[t]` in the approval prompt, a confirmation's title |
| `shell` | a `!command` of yours and its running marker, Lua mode |
| `md_heading` | markdown headings |
| `md_bold` | `**strong**` |
| `md_italic` | `*emphasis*` |
| `md_code` | inline `` `code` `` |
| `md_code_block` | fenced code blocks |
| `md_link` | a link's text |
| `md_url` | a link's URL |
| `md_quote` | `>` quotes |
| `md_bullet` | list markers |
| `md_rule` | `---` rules |
| `hl_heading`, `hl_code`, `hl_keyword`, `hl_string`, `hl_comment` | what the nvim highlighter's captures paint in the input (`highlight = "nvim"`): headings, raw text, and inside fenced blocks keywords, strings and comments; its emphasis, links, lists and quotes use the `md_*` roles |
| `diff_added`, `diff_removed`, `diff_hunk` | added and removed lines, and `@@` or file headers, in the approval preview of an edit and in tool output that is a diff |
| `input` | the input box text |
| `input_prompt_insert`, `input_prompt_normal` | the input's prompt character in insert and in normal mode |
| `separator` | the line above the input |
| `focus` | the border of the conversation window when it has the focus |
| `visual` | a visual selection, and the selected row of the command palette |
| `search` | search hits |
| `cursor_line` | the cursor line in the conversation window |
| `status` | the top status strip |
| `status_insert`, `status_normal`, `status_visual` | the vim mode in the bottom status line |
| `status_dim` | quiet text in the status lines and the palette's summaries |
| `mode_manual`, `mode_auto-read`, `mode_edit`, `mode_auto`, `mode_plan` | the agent mode's name in the status strip |
| `harness_armed`, `harness_tripped` | the harness state in the status strip; `harness_armed` also colours `[y]` in the approval prompt |
| `remote` | the REMOTE marker |
| `approval` | the approval, question and confirmation boxes |

### From neovim colorschemes

`:theme nvim:NAME` (and `maic themes import NAME`) read these highlight groups, resolved (`nvim_get_hl` with `link = false`), and give each role the colour of the **first group in its list that has one**. A plain role takes the group's foreground (its background when it has no foreground) and keeps the built-in role's attributes (the bold of a mode name, the inverse of `status_insert`); a **filled** role takes the group's foreground and background as nvim shows them, `reverse` applied. The group's own bold, italic and underline are added. A colorscheme that defines only terminal colours (`ctermfg`) is read through the xterm palette. Roles not listed here, and roles none of whose groups has a colour, keep the built-in default: `user`, `assistant`, `input`, `status_dim`, and `md_bold` / `md_italic` unless the scheme colours `@markup.strong` / `@markup.italic`.

| Role | nvim groups, first with a colour wins |
| :--- | :--- |
| `thinking`, `tool_ok`, `md_url`, `hl_comment` | Comment |
| `tool` | Function, Identifier |
| `tool_err`, `error`, `mode_auto`, `harness_tripped`, `remote` | ErrorMsg, Error |
| `notice` | MoreMsg, WarningMsg |
| `shell`, `hl_string`, `input_prompt_insert`, `status_insert`, `mode_plan`, `harness_armed` | String |
| `md_heading`, `hl_heading` | @markup.heading, Title |
| `md_bold` | @markup.strong |
| `md_italic` | @markup.italic |
| `md_code` | @markup.raw, String |
| `hl_code` | @markup.raw, Constant |
| `md_code_block` (filled) | Pmenu, NormalFloat |
| `md_link` | @markup.link, Underlined, Directory |
| `md_quote` | @markup.quote, Comment |
| `md_bullet` | @markup.list, Special |
| `md_rule` | LineNr, Comment |
| `hl_keyword` | Keyword |
| `diff_added` | Added, DiffAdd |
| `diff_removed` | Removed, DiffDelete |
| `diff_hunk` | Changed, DiffText |
| `input_prompt_normal`, `status_normal`, `mode_manual` | Function, Directory |
| `separator` | LineNr |
| `focus` | Special, Function |
| `visual` (filled) | Visual |
| `search` (filled) | Search |
| `cursor_line` (filled) | CursorLine |
| `status` (filled) | StatusLine |
| `status_visual` | Constant, Keyword |
| `mode_auto-read` | Special |
| `mode_edit` | Type, WarningMsg |
| `approval` | Question, MoreMsg |

Normal, StatusLineNC, PmenuSel, Todo and DiffChange are read too but no role takes from them yet. The theme's `background` is nvim's `'background'` after the colorscheme ran.

## Project layers, trust and the chain

**The chain.** Project settings and instruction files are read from the workspace up to the project root: the nearest directory at or above the workspace that holds a project marker (`.git`, `.maic/` or `MAIC.md`; `instructions.project_markers` changes the list). Nothing above the root is read or asked about. With no marker anywhere above the workspace the chain goes up to just under `$HOME`, as before; `instructions.bound = "home"` makes that the rule everywhere. Outside `$HOME` the chain is the workspace alone. `$HOME` and `/` are never on it.

**Trust.** A directory on the chain that holds `.maic/` or an instruction file (a name in `instructions.files` or its `.local.md` variant) is a project directory, and so is a workspace with instruction files in its subdirectories that no project directory above it covers. Nothing from it is used until you trust it: its `.maic/settings.*` are not applied, its instruction files are not given to the model, its `.maic/tools/` are not loaded, and an instruction file below the workspace is attached only when a trusted directory's hash covers it ([instructions.md, Trust](instructions.md#trust)). MAIC asks once per directory on the terminal before the screen is drawn (and in a modal when `:cd` reaches one): trust fully, trust sandboxed, not now (untrusted this session) or never (remembered). Headless runs and runs off a terminal ask nothing and stay untrusted unless `--trust` (fully) or `--trust=sandbox` is given. `:trust`, `maic trust [PATH] [--lua L] [--level L]`, `maic trust --list` and `maic untrust PATH` manage it;
 the record is `<state>/trust.json` (0600), per absolute path, with a SHA-256 over every settings, instruction and tool file, nested instruction files and the files they import inside it included. Your global files (`~/.config/maic/`) are always trusted. The details, and why, are in [harness.md](harness.md#directory-trust-and-restricted-settings-lua-built).

**Tiers.** When a trusted directory's files change:

| Tier | A change passes when | Asked again for |
| :--- | :--- | :--- |
| `strict` | never | any change |
| `standard` (default) | it is yours: an uncommitted edit in a git working tree, or commits whose author email is one of `trust_identities` (else your global git email) | a commit by anyone else (a pull, a merge), a checkout or reset that moved the history, a new untracked file, any change outside a git working tree |
| `relaxed` | it widens nothing | a new `permission.allow` (or `allow`) entry, a removed `ask` or `deny` entry, a looser `mode`, `harness = "dumb"`, a `tripwire` other than `machine`, `allow_isolated`, `dumb_auto_ok`, a new or changed provider, settings that no longer load, a new tool or a manifest whose `run`, `reads` or `writes` changed, a new instruction file |

Under `relaxed`, a changed `settings.lua` in a directory trusted fully is always asked about: its code runs as you, so it can't be judged as data. A change that passes is named in a one-line notice at start, and the record takes the new contents. The tier comes only from you: `trust_strictness`, `trust_levels`, and `maic trust PATH --level L` / `:trust --level L`. In a project's own file `trust_*`, `global_lua`, `lua_memory_mb` and every `instructions` key are ignored, each with a warning naming the file.

### Lua levels

How a settings-like Lua file runs. A trusted directory's level is a separate axis from its tier: the tier says how often to ask again, the level what its Lua may do.

| Level | Who gets it | What the file can do |
| :--- | :--- | :--- |
| `full` | your own files by default (`global_lua`); a project directory you trust fully (`t`, `maic trust PATH`, `--trust`) | anything you can: the whole standard library, `maic.read`, `maic.shell`, `maic.nvim`. It runs as you |
| `sandbox` | a project directory you trust sandboxed (`s`, `--lua sandbox`, `--trust=sandbox`); your own files with `global_lua = "sandbox"` | the restricted environment below, in a forked child process: its address space may grow by `lua_memory_mb` (RLIMIT_AS), its CPU time is capped (RLIMIT_CPU), it has no file descriptor but its result pipe (stdin, stdout and stderr on /dev/null, any open session file closed) and at most 8; it sends its table back as JSON. A child killed by a limit or a signal is an error naming the file ("exceeded its memory limit (256 MB)", "exceeded its time limit (2 s)", "crashed with signal N"); MAIC carries on and applies nothing from the file |
| `restricted` | `--lua restricted`; your own files with `global_lua = "restricted"` | the restricted environment in MAIC's own process, with the early memory checks only |

### Restricted Lua

The environment of the sandbox and restricted levels: base,
 `string`, `table`, `math` and `bit`; `os.getenv`, `os.time`, `os.date` and `os.clock` and no other `os` function; `load` and `loadstring` for text chunks only, compiled into the same environment; no `io`, `package`, `require`, `module`, `dofile`, `loadfile`, `debug`, `collectgarbage`, `gcinfo`, `ffi`, `jit`, `newproxy`, `setfenv`, `getfenv` or `string.dump`; `maic` holds only `workspace`, `version`, `home` and `hostname`. The JIT is off and a file that runs longer than 2 seconds is stopped. Its heap is checked against `lua_memory_mb` every 1000 instructions, and `string.rep` and `table.concat` refuse a result that would pass it before allocating (LuaJIT's own `string.format` already refuses a width or precision over two digits). The table it returns may hold only tables, strings, numbers and booleans: a function is an error naming its key (`providers.lab.hook is a function`). Reaching for anything missing is an error naming the file and line (`.maic/settings.lua:2: os.execute is not available in restricted settings Lua`), a bytecode file or chunk is refused, and nothing from the file is applied.


## Instructions

Standing instructions the model sees on every turn, like CLAUDE.md, in this order, the later taking precedence where two conflict:

1. `/etc/maic/` (a system-wide slot, empty by default)
2. `~/.config/maic/` (`$XDG_CONFIG_HOME` respected)
3. each trusted directory on the [chain](#project-layers-trust-and-the-chain) (the project root, or just under `$HOME`, down to the workspace), outermost first
4. on demand, the instruction files between a file the agent reads and the workspace, once per session

In each directory `CLAUDE.md`, `AGENTS.md` and `MAIC.md` (lowest priority first), then their `.local.md` variants; `@path` imports another file. The classes, `read = "highest"`, local files, imports and extra directories are set in the global `instructions` table: [instructions.md](instructions.md) has every option, the comparison with Claude Code and opencode, and why MAIC's defaults are what they are.

Files are re-read at the start of each turn, so edits apply to the next message. Each is capped at 32 KB. `:instructions` shows what is in effect. The model is told to follow them and never to infer your name or pronouns from paths, usernames or commit authors.
