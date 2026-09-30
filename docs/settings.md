# Settings

Settings are Lua files that return a table (JSON with the same keys works too). They are layered, and every key is optional; MAIC runs fine with no files at all:

1. `~/.config/maic/settings.lua` (or `$XDG_CONFIG_HOME/maic/settings.lua`): yours, for every project. `maic settings init` writes one with every default and a comment; `maic settings path` shows where it goes.
2. `<dir>/.maic/settings.lua` for each directory from just under `$HOME` down to the workspace: the project's, meant to be committed.
3. `<dir>/.maic/settings.local.lua` next to each of those: personal overrides, keep it out of git.

At each location a `settings.lua` is used when it exists, else a `settings.json` (`maic settings init --json` writes that form). Nearer files win. Scalars replace, `providers` merge by name, `style` merges by role. `:settings` in a session lists the files that were read; `maic init` (or `:init`, which also has the agent draft the `MAIC.md`) scaffolds a project's.

Because a settings file is code, it can decide things per machine:

```lua
local deep = maic.hostname == "workstation" and "qwen3.5:9b" or "qwen3.5:4b"
return {
  model = os.getenv("MAIC_MODEL") or deep,
  mode = "auto-read",
  models_dir = maic.home .. "/models",
  providers = {
    anthropic = { api_key_command = "pass show anthropic/api-key" },
  },
  style = { user = { fg = "#ff8800", bold = true } },
}
```

The file runs with LuaJIT and the standard library; `maic.home`, `maic.hostname`, `maic.workspace` and `maic.version` are set. Keys in the JSON examples below are the same in Lua (`sessions_home = "auto"`).

```jsonc
// the same keys, in JSON form
{
  "model": "qwen3.5:4b",
  "mode": "manual",
  "think": false,
  "markdown": true,
  "mouse": true,
  "sessions_home": "auto",
  "leader": "space",
  "instruction_files": ["MAIC.md", "AGENTS.md"],
  "providers": { ... },
  "style": { ... }
}
```

| Key | What |
| :--- | :--- |
| `model` | `provider/model`, or a bare Ollama model name. `--model` on the command line and `:model` in the session override it. |
| `mode` | `manual`, `auto-read`, `edit`, `auto` or `plan` (see [cli/README.md](../cli/README.md#modes)). |
| `think` | Ask the model to reason before answering. Slower; better on hard problems. |
| `markdown` | Render markdown in the conversation window (`:set markdown off` for raw text). The input box always highlights markdown. |
| `mouse` | Scroll wheel support. With it on, the terminal's own text selection needs Shift+drag; `:set mouse off` turns it off for a session. |
| `instruction_files` | File names looked for from `$HOME` down to the workspace, like CLAUDE.md. See [Instructions](#instructions). |
| `record` | Keep transcripts of interactive sessions (default `true`). `false` writes them to the runtime directory instead, where they vanish at logout; `--record` / `--no-record` override per session. |
| `compact_at` | Auto-compact when the last model call used this share of the context window (default `0.75`; `0` disables). Old tool results are stubbed first; the oldest turns are summarised only if that was not enough. See `:h compact`. |
| `compact_keep_results` | Tool results that are never stubbed, counting from the most recent (default `4`). |
| `models_dir` | Where model files live: ComfyUI's `checkpoints/ diffusion_models/ loras/ text_encoders/ vae/` and Ollama's `ollama/` store. Used when MAIC installs ComfyUI (see [vendor.md](vendor.md)). |
| `system_prompt` | Operator text placed first in every system prompt, before MAIC's briefing and any instruction file; `"@~/path"` reads a file. `--system` and `:system` override. Front-loads behaviour. |
| `load_instructions` | `false` loads no `MAIC.md` / `AGENTS.md` anywhere (default `true`); `--no-instructions` and `:instructions off` do it per session. Independent of `system_prompt`; combine them to run on your own text alone. |
| `harness` | `"smart"` (default): a model reviews every command or write the rules would allow without asking, see `:h harness`. `"dumb"`: the rule list alone. |
| `reviewer_model` | The model that reviews under the smart harness (default: the session's model). Same `provider/model` form as `model`. |
| `dumb_auto_ok` | `true` skips the once-per-session warning when entering auto mode under a dumb harness (default `false`). |
| `bans` | `{ strings = {...}, patterns = {...}, tokens = {...}, retries = 3, replacement = "[banned]", ignore_case = false, window = 64 }`. Strings and POSIX regex patterns are enforced by MAIC on every provider (cut before they show, re-asked, then replaced); tokens (ids or text) become `logit_bias` on OpenAI-compatible providers. Layers add strings, patterns and tokens. `--ban`, `--ban-pattern` and `:ban` at run time. See [bans.md](bans.md). |
| `sampling` | Sampler keys sent with every request: `temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty`, and on llama.cpp-style servers `xtc_probability` / `xtc_threshold`. A provider's `options.sampling` overrides it; `:sampling` changes it live. Nothing is sent to Anthropic. |
| `budget_tokens` | Stop the agent once input plus output tokens over the session reach this (default `0`, unlimited); `:budget` changes it live. |
| `title_model` | A model that names the session after its first turn, for `maic sessions` (default off). A remote model is never used for a local session. |
| `timestamps` | Show a time beside each conversation entry (default `false`; `:set timestamps on`). |
| `leader` | The vim leader key for normal and visual modes: `"space"` (default) or a single character. `<leader>y` yanks to the system clipboard, `<leader>p` pastes from it. |
| `server` | `maic server`: `listen` (default `127.0.0.1:7373`; any other address turns TLS on), `workspaces` (directories a remote session may open; default `~/dev2`, else the current directory), `cert` and `key` (a PEM pair; empty makes a self-signed one under `~/.local/state/maic/server/`). See [remote.md](remote.md). |
| `sessions_home` | Where new transcripts go. `auto` (default): under `sessions/projects/<encoded workspace>/` when the workspace has a `MAIC.md` (or one is in effect from a parent directory), else `sessions/general/`. Or force it: `general`, `project`, or any name (`sessions/<name>/`). A project can set this in its `.maic/settings.json`; `maic sessions rehome` moves existing transcripts. |

## Providers

Where models come from. MAIC ships with `ollama` (local), `anthropic`, `deepseek` and `openrouter`; a `providers` entry adds a new one or changes a shipped one by name.

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
| `kind` | `ollama` (native Ollama API), `anthropic` (Messages API), or `openai` (any OpenAI-compatible `/chat/completions`: DeepSeek, OpenRouter, vLLM, LM Studio, llama.cpp server, ...). |
| `base_url` | Where it listens. A path prefix is fine (`https://openrouter.ai/api/v1`). |
| `api_key_env` | Environment variable holding the key. |
| `api_key_command` | A command that prints the key (a password manager). Keys themselves never go in this file; MAIC refuses an `api_key` field. |
| `options.sampling` | Any kind: a table merged into every request to that provider (`temperature`, `top_k`, `top_p`, `min_p`, `seed`, `repeat_penalty`, and for llama.cpp-style servers their own keys such as `xtc_probability`). Anthropic's current models reject sampling parameters, so leave it unset there. |
| `options` | Kind-specific. Anthropic: `max_tokens` (64000), `effort` (`high`), `think_effort` (`xhigh`, used when `:think on`), `fallbacks` (`"default"` turns on server-side refusal fallbacks), `auth: "bearer"` for an OAuth token. OpenAI kinds: `extra_body`, merged into every request. |

Use a provider with `:model anthropic/claude-opus-5-5`, `:model deepseek/deepseek-chat`, `:model lmstudio/whatever-it-serves`, or `maic --model openrouter/some/model`. A bare name with no known prefix goes to Ollama (its names can contain `/`).

**Remote providers send data off this machine**: your prompts, every file the agent reads, and every command's output. MAIC says so when you switch to one and shows `REMOTE` in the status line. A provider is local when its `base_url` is on 127.0.0.1, localhost or ::1.

Anthropic models get thinking on by default with `effort` controlling depth, streamed tool input, and refusal fallbacks. Their history is replayed exactly as received (thinking blocks included) and never edited, which the newer models require; mode changes and instruction updates are appended as system messages instead.

## Styles

`style` maps a role to `{ "fg", "bg", "bold", "dim", "italic", "underline", "inverted" }`. Colors are FTXUI names (`black red green yellow blue magenta cyan white gray gray_dark red_light green_light yellow_light blue_light magenta_light cyan_light default`), `#rrggbb`, or a 0-255 palette index. A role you set is merged over its default, so `{"fg": "#ff8800"}` keeps the default's bold. (`italic` renders as dim: the terminal library has no italic.)

| Role | Used for |
| :--- | :--- |
| `user`, `assistant`, `thinking`, `tool`, `tool_ok`, `tool_err`, `notice`, `error`, `shell` | conversation entries by kind |
| `md_heading`, `md_bold`, `md_italic`, `md_code`, `md_code_block`, `md_link`, `md_url`, `md_quote`, `md_bullet`, `md_rule` | markdown, layered over the entry's style |
| `input`, `input_prompt_insert`, `input_prompt_normal` | the input box and its prompt character |
| `separator`, `focus`, `visual`, `search`, `cursor_line` | the line above the input, the border of the focused conversation window, selections, search hits, the cursor line |
| `status`, `status_insert`, `status_normal`, `status_visual`, `status_dim` | the status lines |
| `mode_manual`, `mode_auto-read`, `mode_edit`, `mode_auto`, `mode_plan` | the mode name |
| `harness_armed`, `harness_tripped`, `remote`, `approval` | harness state, the REMOTE marker, the approval box |

`maic settings init` writes every default, so the easiest way to restyle is to run it once and edit.

## Instructions

Standing instructions the model sees on every turn, like CLAUDE.md:

1. `~/.config/maic/MAIC.md` (global; `$XDG_CONFIG_HOME` respected)
2. every `MAIC.md` or `AGENTS.md` from just under `$HOME` down to the workspace, outermost first

Files are re-read at the start of each turn, so edits apply to the next message. Each is capped at 32 KB. `:instructions` shows what is in effect. The model is told to follow them and never to infer your name or pronouns from paths, usernames or commit authors.
