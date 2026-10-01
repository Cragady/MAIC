# MAIC

This repo is to help me get myself in order to finally start getting my own LLM stack running. This will likely be an effort to build my own CLI agent as well.

## Basic Repos

Here's some repos/services/orgs that I've come across that look like will help me out:

* https://github.com/ollama/ollama
* https://github.com/OpenRouterTeam
  * https://github.com/OpenRouterTeam/python-sdk?tab=Apache-2.0-1-ov-file
  * https://openrouter.ai/docs/guides/routing/routers/auto-router
  * https://openrouter.ai/
* https://github.com/anomalyco/opencode 
* https://github.com/Aider-AI
  * https://github.com/Aider-AI/aider
* https://github.com/openclaw/openclaw
* https://github.com/earendil-works/pi
* https://github.com/OpenHands/openhands
* https://github.com/aaif-goose/goose
* https://github.com/RunEdgeAI/agents.cpp
* https://github.com/ggml-org/llama.cpp
* https://github.com/warpdotdev/warp


## Reading/Sources

* https://www.firecrawl.dev/blog/claude-code-vs-opencode
* https://dev.to/jim_l_efc70c3a738e9f4baa7/openclaw-vs-claude-code-i-tried-both-for-a-month-of-real-work-19go
* https://www.reddit.com/r/PiCodingAgent/comments/1uls2fd/comment/ov6je1d/
* https://www.reddit.com/r/openclaw/comments/1syl4ot/whats_the_actual_use_case_for_openclaw_vs_claude/


## Top Langs for Agentic CLI

* Go
* Typescript
* Python

### Harder

* Rust
* C++

### Related

* Lua
* Perl
* WASM

## Project At A Glance

**The goal:** drop the $200/month Claude Code plan and rebuild the same workflow — agentic terminal
work, long design conversations, tests, decompilation and pentest analysis — on open weights, without
silent model downgrades mid-task.

**What I'm working with:** an RTX 2070 (8 GB VRAM), Neovim (no IDE), a $1,500 sweet spot and a $3,000
ceiling on hardware. Keeping Claude around at the $20 tier is fine; $200 is not.

| Tier | Runs | Cost |
|---|---|---|
| Local (today) | Qwen3.5 4B and 9B at Q4_K_M via llama.cpp (`llamacpp/current`), the agent and inline work | $0 |
| Cloud heavy | Kimi K3 (talk/analysis), DeepSeek V4 Pro (multi-file builds) | ~$4–12/mo |
| Router | OpenRouter Auto Router as the sentinel/dispatch layer | pass-through |
| Fallback | Claude at the $20 tier | $20/mo |

**Roadmap** — four phases, detailed in [docs/llm-stacking.md](docs/llm-stacking.md):

1. **Now** — 2070 + cloud APIs. Hardware $0, API ~$4–12/mo.
2. **Short term** — single 24 GB card (used 3090 / new 4090). Runs a 32B locally. $650–$2,000.
3. **Mid term** — multi-GPU rig or a 192 GB Mac Studio. Runs 300B+ MoE offline. $3,500–$5,600.
4. **Someday** — 8x H100/H200 node. Unquantized frontier models. $250k+, listed mostly for scale.

**What's in `docs/`:**

* [claude-replacement.md](docs/claude-replacement.md) — the long thread: model comparisons, hardware
  phases, compaction/pricing, throttling, the sentinel-router idea, OpenRouter as the control plane.
* [llm-stacking.md](docs/llm-stacking.md) — the condensed plan, with the CodeCompanion.nvim config.
* [deepseek-local-q.md](docs/deepseek-local-q.md) — what running DeepSeek V4 Pro locally actually costs.
* [misc-llm-chats.md](docs/misc-llm-chats.md) — Ollama vs. Claude Code, and wiring them together.
* [google-chat-dump.md](docs/google-chat-dump.md) / [tts-stack.md](docs/tts-stack.md) — GPU buying
  research and the STT/TTS side quest.
* [templates/sentinel-pipeline-widget.html](templates/sentinel-pipeline-widget.html) — interactive
  diagram of the routing pipeline.

**Set up and measured on this machine** (not AI transcripts — these were installed and tested):

* [harness.md](docs/harness.md) — the safety harness: tripwire (built) and planned layers.
* [tools.md](docs/tools.md): the model's tools, and writing your own, in Lua or any language, behind the harness.
* [settings.md](docs/settings.md) — the settings file: model providers (local and remote), styles, instruction files.
* [themes.md](docs/themes.md): themes (default, gruvbox dark and light, mono), writing one, importing a neovim colorscheme, colour depth.
* [nvim.md](docs/nvim.md): maic.nvim, MAIC inside nvim: the plugin's commands, how MAIC finds and trusts its host, what the host gives (files, diffs, User autocmds, the live theme, `maic.nvim` in your Lua) and what the model never gets.
* [sessions.md](docs/sessions.md): the session file format, every record type, homes, forks and `--fork-at`, `maic sessions import` (claude.ai exports, Claude Code transcripts), `redact`, `export`.
* [llamacpp.md](docs/llamacpp.md): llama.cpp, the local server: every sampler (XTC, DRY, top-n-sigma), logit bias, grammars, the models directory and `maic vendor model`.
* [vendor.md](docs/vendor.md): the services MAIC installs for itself (llama.cpp, ComfyUI) at pinned versions, and the artifact tree.
* [bans.md](docs/bans.md) — string, regex and token bans, XTC, and why MAIC bans after the fact rather than by constrained decoding.
* [opencode-comparison.md](docs/opencode-comparison.md) — what opencode does that MAIC should and should not take.
* [opencode-quick-wins.md](docs/opencode-quick-wins.md) — 23 small, ranked improvements to take from opencode, with file pointers.
* [cleanroom.md](docs/cleanroom.md) — what may go into MAIC, where the design came from, third-party licenses.
* [remote.md](docs/remote.md): remote access: `maic-server`, the phone web client, tokens and TLS, the API, `maic-relay` and the end-to-end tunnel for the phone away from home, why the tripwire cannot be reset remotely.
* [roadmap.md](docs/roadmap.md): everything MAIC should still become: accounts and a native phone client, the harness layers, cai-tools, tools, editor, services.
* [testing.md](docs/testing.md): how to run every suite, the build gate (`scripts/check.sh`, the pre-push hook), the flake rules, the pty harness for the TUI, the fuzzer, the asan preset.
* [remote.md](docs/remote.md): remote access: `maic-server`, the phone web client, tokens and TLS, the API, the relay design, why the tripwire cannot be reset remotely.
* [roadmap.md](docs/roadmap.md): everything MAIC should still become: the relay and a native phone client, the harness layers, cai-tools, tools, editor, services.
* [comfyui-setup.md](docs/comfyui-setup.md) — ComfyUI in its own venv, models on the external drive.
* [local-llm-benchmarks.md](docs/local-llm-benchmarks.md) — measured tok/s per model and runtime.

> Everything else in `docs/` is transcribed AI output. Treat the numbers as leads, not facts — a few of
> them already contradict each other (see the note under Option A below).

## Get the repository

```sh
# just what the build needs (the pinned LuaJIT source); ComfyUI and llama.cpp come later, through maic itself
git clone --recurse-submodules=vendor/lua-pins https://github.com/Cragady/MAIC
# everything, including the ComfyUI (about 100 MB) and llama.cpp (about 180 MB) checkouts, fetched in parallel
git clone --recurse-submodules -j4 https://github.com/Cragady/MAIC
# an existing clone
git submodule update --init -j4              # all of them
git submodule update --init vendor/lua-pins  # the required one only
```

`maic` pulls and builds the non-required vendors itself: `maic vendor add comfyui` fetches the pinned ComfyUI submodule, applies MAIC's patches, sets up its Python with uv and links it in; `maic vendor add llamacpp` builds the pinned llama.cpp out of tree ([docs/llamacpp.md](docs/llamacpp.md)). `maic vendor adopt NAME PATH` uses an install you already have instead. See [docs/vendor.md](docs/vendor.md).

## Structure and Build

MAIC is the control plane for the local AI stack and, eventually, a C++ agentic CLI. The core is C++; other languages are fine in the sub-projects that need them.

```
MAIC/
├── core/       C++ library: agent loop, model providers (llama.cpp, Anthropic, OpenAI-compatible), tools, harness
│               policy, sandbox, sessions, settings, service manager, tripwire
├── cli/        `maic`: the agent UI (vim keys, modes, sessions) and service/harness commands. See cli/README.md
├── harness/    maic-lock (root-owned tripwire helper) + its installer
├── services/   one JSON file per service MAIC runs (llamacpp, comfyui)
├── vendor/     pinned submodules (llama.cpp, ComfyUI), MAIC's own ComfyUI nodes (comfyui-maic-*), install scripts, manifest.json. See docs/vendor.md
├── tools/      examples of user-defined tools, Lua and script (docs/tools.md), and the ComfyUI helpers
├── server/     maic-server: sessions over HTTP with server-sent events, the phone web client. See docs/remote.md
├── relay/      maic-relay: the rendezvous the server dials out to so the phone reaches it from anywhere; sees only sizes
└── docs/
```

Build (needs `VCPKG_ROOT` set, which your shell does; vcpkg fetches nlohmann-json, FTXUI and cpp-httplib into `build/`):

```sh
cd ~/dev2/MAIC
cmake --preset default
cmake --build --preset default
ctest --preset default                           # every suite; scripts/check.sh does all three and gates on each (docs/testing.md)
ln -s ~/dev2/MAIC/build/cli/maic ~/bin/maic      # once
sudo ./harness/install-tripwire.sh               # once, see docs/harness.md
```

Use:

```sh
maic doctor                  # what this machine has and a recommended setup
maic setup                   # or the first run as questions: settings, llama.cpp, ComfyUI, a model, the tripwire
maic vendor add llamacpp     # first run: build llama.cpp (docs/llamacpp.md), ...
maic vendor use llamacpp /path/to/model.gguf   # ... link a GGUF (or: maic vendor model llamacpp URL SHA256), ...
maic up llamacpp             # ... start llama-server on 127.0.0.1:8081, and
maic                         # the agent, in the current directory, on llamacpp/current (see cli/README.md)
maic -c                      # continue the last session here; maic -r picks one
maic -p "prompt"             # one turn, no UI
maic status                  # harness state + every service
maic up llamacpp comfyui     # or: maic up all
maic down all
maic logs llamacpp
maic trip "reason"           # panic button, no password
maic unlock                  # needs your sudo password
maic server token new phone  # a bearer token for one device, shown once
maic server start            # the API and web client on 127.0.0.1:7373; --listen 0.0.0.0:7373 for the LAN, with TLS
maic server pair             # with server.relay set: pair the phone once on the LAN, then it reaches home through the relay
```

State lives in `~/.local/state/maic/`: `run/<service>.pid` (PID plus process start time, so a reused PID is never mistaken for the service), `logs/<service>.log`, `sessions/*.jsonl`, and `server/` (token hashes, the audit log, the self-signed certificate, the relay pairing keys). Settings and standing instructions live in `~/.config/maic/` ([docs/settings.md](docs/settings.md)).

## Immediate Steps

Two ways to start this week without buying anything.

### Option A — Local only, $0, runs on the 2070 today

> This was the first plan: the Claude CLI pointed at Ollama. MAIC's own first-run path is llama.cpp (the `Use` block above and [docs/llamacpp.md](docs/llamacpp.md)); Ollama was removed from MAIC on 2026-10-01 because llama.cpp does all of it. The block below is kept as history.

Good for inline completion, small refactors and offline work. An 8 GB card caps you around a 7B at
Q4_K_M, so this will not carry whole-repo agentic runs — that's what Phase 2 is for.

```bash
# 1. model that fits in 8 GB
ollama pull qwen2.5-coder:7b

# 2. point the Claude CLI at the local server instead of Anthropic
export ANTHROPIC_BASE_URL="http://localhost:11434"
export ANTHROPIC_AUTH_TOKEN="ollama"

# 3. run the agent in a repo
cd /path/to/project && claude
```

Then wire the same model into Neovim with the `qwen_local` adapter block in
[docs/llm-stacking.md](docs/llm-stacking.md).

**Verify before trusting:** the docs disagree on the base URL — `http://localhost:11434` in two
places, `http://localhost:11434/v1` in a third. Try both. The claim that Ollama speaks the Anthropic
Messages API natively is also unverified AI output; if `claude` won't connect, that's the first thing
to check.

### Option B — Keep the hardware, rent the reasoning (~$4–12/mo)

Better for the work that actually needs frontier reasoning: architecture conversations, decompilation
notes, multi-file builds.

1. Get an [OpenRouter](https://openrouter.ai/) key and start with `openrouter/auto` so you aren't
   hand-picking a model per task.
2. Point Aider or CodeCompanion.nvim at it — adapter config is in
   [docs/llm-stacking.md](docs/llm-stacking.md).
3. Drop Claude to the $20 tier and keep it as the fallback rather than the daily driver.

**Trade-off:** code leaves the machine. For pentest and decompilation work that's the whole reason to
push toward Phase 2/3 local — run Option B for general building, Option A for anything sensitive.

### Deciding between them

Run both for a week. If the 7B handles more of the day than expected, Phase 2 (a used 3090) buys most
of the remaining gap for ~$700 and takes the API bill to near zero. If it doesn't, the router path is
the cheaper answer and the local tier stays a convenience.
