# cli

The `maid` command. With no arguments it starts the agent in the current directory, which becomes the workspace. `maid help` lists the rest.

```sh
maid vendor add llamacpp           # once: build llama.cpp (docs/llamacpp.md)
maid vendor use llamacpp /path/to/model.gguf   # once per model: the GGUF it serves
cd ~/some/project
maid up llamacpp                   # once per boot
maid up llamacpp-2                 # optional: the side server on 8082, a second resident model (docs/llamacpp.md, Two servers)
maid                               # the agent, on llamacpp/current
maid --model anthropic/claude-opus-5-5 --mode auto-read
maid -c                            # continue the last session started in this directory
maid -r                            # pick an earlier session from a list (maid -r ID for one you know, maid -r PATH for any transcript file)
maid -p "explain main.cpp"         # one turn, no UI, no transcript; --record keeps one; -c / -r load an old one; --json for events
cat dialog.txt | maid -p -         # the prompt from stdin
maid -p "summarise these" -C notes.md -C log.txt   # files attached as context before the prompt (also for the interactive maid)
maid -p "let's plan the refactor" -i               # an interactive session that opens with that prompt sent
cat prompt.txt | maid -pi -        # short flags cluster (-p -i); a value-taking flag (-m, -C) goes last in a cluster
maid --system @~/prompts/reviewer.md --no-instructions   # front-load behaviour; ignore every MAID.md / AGENTS.md
maid --prefix "Sure thing! "                              # every reply starts with these words, guaranteed (:h prefix)
maid --rule "Always answer in French"                      # a standing instruction, reminded every turn (:h rule)
maid --model opus-5.5                                      # a preset: model, 1M context, thinking on; reviews on Haiku 4.5
maid --xtc 0.5,0.1 --sampling min_p=0.05 --ban-pattern @~/bans/tics.re   # samplers and bans for one run
maid --bare                        # nothing from nvim: no host connection, highlighter, theme, lazy-lock notice or keymap check (:h bare)
maid help headless                 # the verbose page for all of the above (same as :h headless in a session)
maid help | grep vendor            # help goes to stdout, so it pipes; maid help topics lists every page
maid lua                           # a LuaJIT REPL in this directory with the maid table loaded
maid sessions                      # every session, with a preview
maid artifacts                     # where MAID and its services keep transcripts, logs, outputs
maid diction                       # dictation into ./<dir>.md through whisper-server and a local scribe (docs/diction.md); maid help diction
maid model resolve qwen-4b         # what a preset or provider/model means here, as JSON (provider, kind, base_url, model, context)
maid settings init                 # a documented settings file (docs/settings.md)
maid trust                         # trust this directory's project files (settings, MAID.md / AGENTS.md, .maid/tools/); :h trust
maid trust ~/dev2/app --lua sandbox     # its settings.lua in a child process that cannot reach the system (default full: as you)
maid trust ~/dev2/app --level relaxed   # how often to ask again: strict, standard (default) or relaxed; maid trust --list, maid untrust PATH
maid -p "run the tests" --trust    # headless runs ask nothing: --trust (fully) or --trust=sandbox, for that run only

maid themes                        # the themes (yours in ~/.config/maid/themes, then the shipped ones), the active one marked
maid themes import habamax         # a neovim colorscheme as a theme file, from a headless nvim with your config (docs/themes.md)
maid doctor                        # the machine, the tools MAID needs, installed models, a recommended setup
maid lazy-lock                     # is nvim's lazy-lock.json as recorded? record / diff; exit 0 in sync (docs/lazy-lock.md)
maid nvim keymaps                  # :checkhealth maid from the shell: maid.nvim's, MAID's and llama.vim's keys against your nvim config
maid nvim setup llama-vim          # llama.vim for lazy.nvim as one file MAID owns in your spec's import directory (--dry-run, --remove)
maid setup                         # the first run as yes/no questions: settings, llama.cpp, ComfyUI, a model, the tripwire
maid models                        # the model catalog: what each is for, installed or not, current; maid models install ID [--link] (docs/models.md)
maid up llamacpp-fim               # code completion for llama.vim on 8084, after maid models install qwen2.5-coder-7b --link
maid daemon start                  # sessions that outlive their window: maid and maid.nvim open theirs in it (docs/daemon.md)
maid server token new phone        # remote access (docs/remote.md): a bearer token for one device, shown once
maid server start                  # the API and the phone web client; --listen 0.0.0.0:7373 for the LAN, with TLS
maid server pair                   # with server.relay set: a code and a maid://pair/... string the phone pastes on the LAN
maid server pairs                  # the phones paired for the relay; maid server unpair NAME forgets one
maid server status                 # the configuration, the relay link (connected since, or not and why), whether it is up
```

## Screen

```
 conversation window          (markdown rendered; Ctrl-W k to move into it)
 ⏵ manual (shift-tab to cycle)        qwen3.5:4b local · harness armed · 1 queued (:w now)
 ───────────────────────────────────────────────────────────────────────────────────────
 ❯ your input                 (markdown highlighted as you type)
  INSERT   ↑12 (G follows)                                       Ctrl-W k: conversation · :help
```

The strip above the input shows the agent mode, the model, whether it is local or `REMOTE`, `nvim` while MAID is connected to the nvim it runs inside (below), `bare` when it was started with `--bare`, the harness state, queued messages and whether the agent is working. The bottom line shows the vim mode, the focused window, your position, and the last message.

## Keys

The input starts in normal mode, like opening vim: `i` to type. Cursor: a bar in insert mode, a block in normal mode. **Alt+Enter** (or `:w`) sends from any mode; **Enter** is a newline. nvim has no default Alt mappings, so nothing is lost. The default stays vim-like; `enter_sends = true` in settings makes Enter send a one-line input in insert mode, with Shift+Enter or Alt+Enter for the line break (see `:h enter`).

| Where | Keys |
| :--- | :--- |
| insert | type; **Enter** new line; **Esc** to normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register, **Ctrl-R** `{reg}` a named one; **Ctrl-O** runs one normal-mode command and comes back; ↑ ↓ or Ctrl-P / Ctrl-N prompt history, which persists across sessions (the last 500 prompts) |
| normal (input) | `i a I A o O s S R` insert (a count repeats what you type: `3ix<Esc>`); `h j k l w b e W B E ge gE 0 ^ $ gg G` move (Enter = down a line), `%` to the matching `( ) [ ] { }`; `f{c} F{c} t{c} T{c}` to a character, `;` `,` repeat; `}` `{` paragraphs, `)` `(` sentences; `x X D C J r{c} ~` edit; operators `d c y > < gq gu gU g~` + motion, doubled for the line (`dd cc yy >> gqq guu gUU g~~`), `Y`; text objects after an operator or in visual mode: `iw aw iW aW ip ap is as i" a" i' i` i( a( ib i[ a[ i{ a{ iB i< a<` (so `ciw`, `dap`, `gqip`, `di"`, `ya(`, `viw`); `.` repeats the last change (with its count, or `5.`); `q{a-z}` records a macro until `q`, `@{a-z}` replays it, `@@` again, `3@a` three times (a failed motion stops it); `m{a-z}` marks, `'a` / `` `a `` jump, `''` back; `"a`..`"z` name a register, `"A` appends; `v V` select; `p P` paste (lines go on their own line); `u` undo, **Ctrl-R** redo (multi-level; an insert session is one step); counts everywhere (`3w`, `2d3w`, `3fa`, `2p`, `3J`); `*` / `#` search the conversation for the word under the cursor; `:e` or **Ctrl-X Ctrl-E** opens the input in `$VISUAL` / `$EDITOR` / nvim as markdown and loads it back when you quit |
| visual | `y d x c s` on the selection; `> <` shift, `J` join, `~ u U` case, `r{c}` replace every character, `gq` re-wrap; `o` swaps the ends; text objects (`vip` goes linewise) |
| clipboard | `"+y` / `"*y` before any yank sends it to the system clipboard; `"+p` pastes from it; the **leader** (Space by default, `leader` in settings) then `y` yanks the line (normal) or the selection (visual) to the clipboard, leader then `p`/`P` pastes from it. In the conversation window every yank reaches the clipboard; `yiw`, `yw`, `y$`, `Y`, `yy` work there |
| normal (input empty) | `j k` Ctrl-D/U Ctrl-F/B `G` scroll the conversation without leaving the input; `v` / `V` jump into the conversation window selecting |
| conversation window | **Ctrl-W k** enters, **Ctrl-W j** (or Esc, `i`, Enter) returns; `j k h l w b e 0 $ gg G` Ctrl-D/U/F/B move, `H M L` to the top / middle / bottom of the window; `f t F T ; ,` on the line; `}` / `{` next / previous message, `]]` / `[[` your messages only; `v` / `V` select; `y` yanks (to the register **and** the system clipboard); `yy` a line; `/pattern` then `n` / `N` search (smart case), `*` / `#` for the word under the cursor; `o` swaps selection ends; a left click folds or unfolds a tool result |
| anywhere | **Shift-Tab** cycles the mode; **Ctrl-Z** suspends to the shell (`fg` resumes, like vim); **Ctrl-C** interrupts the agent, else stops a `!command`, else clears the input, else (twice) quits; **Ctrl-S** pauses a running turn (the `interrupt` steer: the reply so far is kept and the turn waits), then **Ctrl-Q** resumes it and the pause menu offers `s` steer, `d` drop, `f` further (what is in the input goes with them as the note), `k` keep and `h` halt, Esc to type a message that resumes it instead; a message typed while the agent works reaches it at its next step (`response.steer`); scroll wheel scrolls the conversation (in insert mode: prompt history) |
| approval prompt | **y** yes · **n** no · **N** no, then type a sentence the model receives as the reason · **a** always allow this file / program for the session · **t** trip the harness · **e** open the file asked about (in the host nvim when connected, else `$EDITOR`) · **d** the proposed change as a diff in a new tab of the host nvim. Edits show the lines that would change, removed in red and added in green (`:h diff`) |

Ctrl-W in insert mode deletes a word, as in vim; the window chord works from insert mode only when the input is empty, otherwise press Esc first.

The system clipboard is reached through `wl-copy` or `xclip` when present, and always through the terminal (OSC 52), which also works over ssh. Your terminal's own copy (Ctrl-Shift-C) keeps working on text you select with the mouse; with the scroll wheel enabled, that selection needs Shift+drag.

The input is highlighted as markdown by MAID's own renderer. `highlight = "nvim"` in settings has an embedded `nvim --embed --headless` do it instead, with treesitter (fenced code in the languages nvim has parsers for gets keywords, strings and comments); it falls back to the built-in one when nvim is missing. See `:h highlight`.

## Inside nvim (maid.nvim)

`maid.nvim/` is the nvim plugin: `:Maid` runs MAID in a terminal split, float or tab, `:MaidSend` puts the buffer's path or a range (as a fenced snippet with path and line numbers) into MAID's input, `:MaidDiagnostics` and `:MaidQuickfix` send those lists ([maid.nvim/README.md](../maid.nvim/README.md)). From MAID's side, nvim sets `$NVIM` for every job, and MAID connects back to that socket as a msgpack-rpc client, but only when it is a Unix socket of this user whose nvim is one of MAID's own parent processes (anything else is refused with the reason at start). Connected, `:e FILE` and `e` at an approval open files in the editing window, `d` shows a write's proposed change as a diff, the theme follows nvim's colorscheme live (`follow_nvim_theme`), User autocmds (`MaidTurnStart`, `MaidToolCall`, `MaidApproval`, `MaidFileWritten`, `MaidTurnEnd`) fire there, an approved write runs `:checktime`, your Lua gets `maid.nvim.*`, and the model gets the read-only `diagnostics` tool; `:MaidInterrupt` (`<leader>mc`) there stops a running turn as Ctrl-C does. The rules: [docs/nvim.md](../docs/nvim.md).

`maid --bare` (also `MAID_BARE=1`, or `bare = true` in settings) starts MAID with nothing from nvim: no `$NVIM` host even inside nvim's terminal, the built-in highlighter, no theme from nvim (`:theme nvim:NAME` is refused and says why), no lazy-lock notice or marker, no keymap check. MAID's own settings, themes (saved `nvim-NAME.lua` files included), Lua and script tools load as usual; the strip shows `bare`. See `:h bare`.

`maid nvim keymaps [--all] [-u FILE]` runs maid.nvim's keymap check (what `:checkhealth maid` shows) in a headless nvim with your own config, after `User VeryLazy`, and prints the collisions with their fixes: keys maid.nvim had to skip, keys MAID's own input needs in its terminal (Esc, Ctrl-W, Ctrl-P, Shift-Tab, Ctrl-Z, Ctrl-C, Alt-Enter) that a terminal-mode mapping takes, and llama.vim's keys. Exit 0 none, 1 collisions, 2 nvim could not run; `maid doctor` has a one-line summary. MAID keeps the result under its state directory and, when `lazy-lock.json` changes, runs the check once more and says at start if a plugin update added a collision.

`maid nvim setup llama-vim [--dry-run] [--remove] [--yes]` writes llama.vim's spec from docs/models.md (port 8084, `model_fim = 'current'`, `<M-f>` and `<M-]>`, normal-mode keys off) as `maid-llama-vim.lua` in the directory your lazy.nvim spec imports, after showing it and asking (`--yes` off a terminal). It edits no other file: without nvim, lazy.nvim or an import directory it explains what is missing and exits 0, and it refuses a file of that name it did not write or a llama.vim already in your spec. `--remove` deletes only MAID's file. A user command: the agent's `run_shell` is refused it. See docs/nvim.md.

## Commands

`:` in normal mode, or from the conversation window. Every command also works typed as a message beginning with `/`. As you type after `:`, a palette lists the matching commands with a one-line description (arguments too, for `:mode`, `:set`, `:h`, `:model`, `:up`); **Tab** completes to the highlighted one and cycles on repeat, Shift-Tab goes back. A unique prefix runs the command (`:inst` is `:instructions`), as in vim.

| Command | Does |
| :--- | :--- |
| `:w` | send the input (same as Alt+Enter). `:w now` or `:ww` sends immediately even while the agent is working (see below) |
| `:e` | edit the input in nvim (`$VISUAL`, then `$EDITOR`, then `nvim`); a non-zero exit leaves the input unchanged |
| `:e FILE` | open a file: in the host nvim's editing window when connected (`:drop`), else in `$VISUAL` / `$EDITOR` / `nvim` in MAID's place |
| `:nvim [theme]` | whether MAID is connected to the nvim it runs inside and what that gives; `theme` follows its colorscheme again. See below and `:h nvim` |
| `:mode manual\|auto-read\|edit\|auto\|plan` | set the agent mode |
| `:model NAME` | switch model (when idle): `llamacpp/current`, `llamacpp/Qwen3.5-9B-Q4_K_M` (any GGUF under the models directory), `anthropic/claude-opus-5-5`, `deepseek-flash` (a preset; `deepseek/deepseek-v4-pro` plainly), ... `:model` alone lists providers |
| `:models` | models the current provider serves (llama.cpp: every GGUF under the models directory, by file name); `maid models` in the shell is the catalog of models MAID can install (`:h models`, docs/models.md) |
| `:think on\|off` | let the model reason before answering |
| `:set markdown\|mouse\|enter_sends on\|off`, `:set highlight nvim\|builtin` | rendering, scroll-wheel, Enter and input-highlighter toggles |
| `:theme [NAME\|reload\|nvim:NAME]` | list the themes (the active one marked), switch one live, re-read the active one's file, or import a neovim colorscheme as `~/.config/maid/themes/nvim-NAME.lua` (Tab after `nvim:` lists them). Shipped: `default`, `gruvbox-dark`, `gruvbox-light`, `mono`. `theme` and `colors` in settings; `maid themes` and `maid themes import NAME` in the shell. See docs/themes.md |
| `:!cmd` or `!cmd` | run a command in **your** shell, unsandboxed, in the workspace; output shows in the conversation and is passed to the model as context (Ctrl-C stops it) |
| `:status` | harness, every service with where it runs (`[host]` pid or `[docker]` container) and what it holds (resident model; ComfyUI's VRAM and queue), model and whether it is remote, session file, queue. See `:h status` |
| `:up NAME` / `:down NAME` | start / stop a service |
| `:init` | scaffold `MAID.md` and `.maid/settings.json` here, then have the agent draft the `MAID.md` from the project; a recorded session that worked here throughout moves into the project's transcript home (`projects/<encoded workspace>/`), and one with more than `init_move_outside_reads` files read or any file written outside asks first. See `:h init` |
| `:cd PATH\|-\|PLACE` / `:pwd` | move this session's workspace (when idle): an absolute path, one relative to the workspace, `~/...`, a place name, or `-` for the previous one; the harness root, shell commands, the project settings layers and the instruction files follow, the model gets a note and the transcript a `workspace` record. `:cd` alone or `:pwd` shows it. See `:h cd` |
| `:settings` | which settings files are in effect and where this session's transcript lives |
| `:instructions` | the MAID.md / AGENTS.md files in effect |
| `:trust [PATH] [--lua full\|sandbox\|restricted] [--level strict\|standard\|relaxed]` / `:untrust [PATH]` | trust this workspace's project directories (or one), or forget it. `--lua` is how its settings.lua runs (full: as you; sandbox: in a child process that cannot reach the system; restricted: a restricted state in process), `--level` how often it is asked about again. Until trusted, a directory's `.maid/settings.*` are not applied, its instruction files are not given to the model and its `.maid/tools/` are not loaded; MAID asks once on the terminal before the screen is drawn (trust fully, trust sandboxed, not now, never). `maid trust [PATH] [--lua L] [--level L]`, `maid trust --list`, `maid untrust PATH` in the shell. See `:h trust` and docs/harness.md |

| `:session` / `:artifacts` | where this transcript is; where everything is kept, with sizes |
| `:path [NAME] [copy]` / `:open NAME` | every place maid knows by a short name (`workspace`, `session`, `sessions`, `models`, `workflows`, `templates`, `vendor/llamacpp`, `comfyui/outputs`, ...); show one, copy it to the clipboard, or open it in the file manager. `maid path`, `maid open`, and `eval "$(maid shell-init)"` for `mcd NAME` in your shell. See `:h path` |
| `:gpu [free [llamacpp\|llamacpp-2\|llamacpp-fim\|whisper\|comfyui] \| load llamacpp-fim]` | who holds the card (each llama server's resident model, the code completion server's as loaded, unloaded or not linked, whisper's, ComfyUI's VRAM view) and one sentence on whether the models fit it; `free` unloads without stopping anything (whisper cannot, and says so); `load llamacpp-fim` loads the coder, which never loads by itself. `maid gpu` in the shell. A failed `maid up` explains a CUDA out of memory in plain words |
| `:lazylock [record\|diff]` | whether nvim's `lazy-lock.json` still matches the hash you recorded; `record` writes the hash file (for your dotfiles) and a snapshot, `diff` lists plugins added, removed and updated and calls out a lazy.nvim update. Out of sync: a notice at start and `lock≠` in the status strip. `maid lazy-lock` in the shell. See docs/lazy-lock.md |
| `:ctx [N]` / `:ctx2 [N]` | the context window of the main llama server (`--ctx`, `context`) and of the side server `llamacpp-2` (`--ctx2`, `context_2`); setting one restarts that server when it runs with another size. See `:h ctx` |
| `:open NAME folder` / `maid cd NAME [--subshell]` | the containing folder in the file manager; the place's directory printed for `cd "$(maid cd NAME)"`, or with `--subshell` a shell there (`exit` returns) |
| `:open SERVICE [firefox\|chrome]` | a service's URL in the browser (`browser` in settings picks the default one; `remote` in settings opens a subscribed maid-server's copy) |
| `:image [FILE\|clear]` | a picture for the next message; `![alt](path)` in the text or a file dragged onto the terminal is attached on send (a plain path inside a sentence stays text). `--image FILE` on the command line. Vision models only. See `:h image` |
| `:forbid [TERM]` | terms no tool call may contain; a search, command or path with one is halted in every mode, under every harness. See `:h forbid` |
| `:allow [PATTERN]` | commands pre-approved for every mode: no asking, no reviewer. MAID's helpers are on it by default. See `:h allow`; the full `permission` block (allow / ask / deny over `tool:pattern`) lives in settings, see `:h permission` |
| `:reg` | the yank register and the named registers `"a`..`"z` |
| `:undo [N]` | restore the file(s) the agent changed last; every write saves the previous content first, a delete keeps the file, a move is moved back |
| `:copy` / `:export [FILE]` | copy the last reply to the clipboard; write the transcript as markdown |
| `:stash` / `:pop` | park the input draft and bring it back (survives restarts). `:q` with an unsent draft stashes it for you |
| `:wq` | send, then quit when the reply is in (Ctrl-C while waiting stays) |
| `:steer ACTION [NOTE]` | steer the running (or paused) turn: `steer` and `drop` stop the reply now and go on with the note (drop trims the reply being written), `further` asks for more at the next step, `interrupt` pauses, `keep` ends the turn with the reply so far, `halt` discards it ([design, section 11](../docs/design/engine-protocol.md#11-steering)) |
| `:steering` | the steering settings in force and which file set each |
| `:rename TITLE` | title the session (`maid sessions` shows it); `small_model` in settings (older name: `title_model`) auto-titles after the first turn |
| `:ban add TEXT` / `:ban pattern REGEX` / `:ban token ID` / `:ban list` | phrases and regexes the model must not say (cut before they show, re-asked, then replaced) and token bans (`logit_bias` on OpenAI-compatible providers). `--ban`, `--ban-pattern`, `bans` in settings. See docs/bans.md |
| `:sampling [KEY VALUE\|xtc P T]` | temperature, top_k, min_p, seed, ... for this session; `xtc` (exclude top choices) on llama.cpp-style servers. See `:h sampling` |
| `:harness [smart\|dumb]` | the model reviewer on or off (default). With `dumb_auto_ok = false`, auto under a dumb harness warns once and asks; see `:h harness` |
| `:budget [N\|off]` | tokens used; a per-session budget that stops the agent when reached |
| `:set timestamps on` | a time beside each message (also `timestamps` in settings) |
| `:lua [CODE]` / `:luafile PATH` / `:chat` | run Lua (vendored LuaJIT) in the workspace; an expression shows its value. `:lua` alone enters **Lua mode**: the input becomes a REPL (`lua❯`) until `:chat`. Globals persist; output goes to the conversation and to the model as context. Outside a session `maid lua` is a REPL, `maid lua FILE [args]` runs a file. See `:h lua` |
| `:compact [prune\|head\|all]` | free context. Default order: stub old tool results (dialog untouched), then, only if still needed, summarise the oldest turns into a handover note. `all` is a whole-conversation summary. Runs automatically at `compact_at` (75%) |
| `:clear` | start a new conversation (the session file keeps both) |
| `:trip REASON` / `:unlock` | trip the harness now; reset it without leaving the session (asks for your sudo password) |
| `:h [TOPIC]` | vim-style help. `:h` alone is an index; `:h w`, `:h u`, `:h f`, `:h .`, `:h gq`, `:h Ctrl-W`, `:h Alt+Enter`, `:h modes`, `:h harness`, `:h sessions`; a unique prefix is enough and an ambiguous one lists the candidates |
| `:q` | quit |

### After every turn

A footer line shows the model, how long the turn took and how many tools ran (`▣ qwen3.5:4b · 12.3s · 3 tool calls`, `· interrupted` when you stopped it). `run_shell` takes a `workdir` argument, so the approval prompt shows `pytest` in `services/api` rather than a `cd` chain; a workdir outside the workspace is asked about. Reading a file under a directory with its own `AGENTS.md` (or any name in `instructions.files`) attaches those instructions to the result once, when a trusted directory covers them ([docs/instructions.md](../docs/instructions.md)).

### Messages while the agent works

Typing and sending while the agent is busy queues the message; it reaches the model at its next step in the current turn (a notice says so, and the status strip counts it). `:w now` delivers it immediately: the model's current output is abandoned and it is asked again with your message included. If a tool is running, the message lands the moment the tool returns. Messages still queued when a turn ends start the next turn by themselves.

## Modes

| Mode | Reads in workspace | Read-only commands | Edits in workspace | Other commands | Outside workspace |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **manual** | yes | ask | ask | ask | ask |
| **auto-read** | yes | yes (read-only sandbox) | ask | ask | reads yes, writes ask |
| **edit** | yes | ask | yes | ask | ask |
| **auto** (default) | yes | yes | yes | yes (sandboxed) | reads yes, writes ask |
| **plan** | yes | yes (read-only sandbox) | no | no | reads ask |

"Read-only commands" are ones MAID recognises as only looking (`ls`, `cat`, `grep`, `git log`, `find` without `-delete`/`-exec`, ...) with no redirection or substitution. They run with the workspace mounted read-only as well, so a wrong guess still cannot change anything.

A session starts in **auto** only where every project directory from the project root down is trusted fully (trusted, with full Lua), and at least one is; anywhere else, a directory with no `.maid/` or instruction file included, it starts in **manual**, says so once, and `:mode auto` turns auto on. `--mode auto` starts in auto anywhere.

Whatever the mode: secrets are never read, system paths are never written, startup files and MAID's own harness are always asked about, dangerous commands trip the harness, and a request that did not come from this terminal is always asked. See [docs/harness.md](../docs/harness.md).

## Sessions

Every session is a JSONL file under `~/.local/state/maid/sessions/`, readable only by you: each message as sent to the model, every tool call with the harness's decision, and the displayable transcript. `maid -c` resumes the newest session started or last opened in the current directory; `maid -r` lists them; `maid -r ID` (a unique prefix is enough) resumes one; `maid -r PATH` resumes any transcript file by path, which is how a temporary `--no-record` transcript (never listed) comes back; when a session ends, `maid` prints the transcript's path and that exact command. The model is told it resumed and what the current mode and instructions are. A session that ended mid tool call resumes from the last complete step.

**Homes.** A project (a workspace with a `MAID.md`, its own or inherited from a parent directory) keeps its transcripts under `sessions/projects/<encoded workspace path>/`, the Claude Code layout; everything else goes to `sessions/general/`. `sessions_home` in any settings layer overrides this (`general`, `project`, or a name for `sessions/<name>/`), so a project can opt back into `general`. `maid init` or `:init` turns a directory into a project. Every open (the first start and each resume) records the workspace, host and pid, so `maid sessions` shows where a transcript was started and, when different, where it was last opened and how many times. `maid sessions rehome ID [ID...] [project|general|NAME]` moves transcripts to another home (HOME last; one ID alone goes to its own project's directory), printing `from -> to` for each and appending a `rehomed` record; forks keep working because they find their parent by id. Subagent sessions move only when asked: by default a session's `sub` sessions stay where they are and still find it by id, `--subagent` moves them with it (children of children too), `--subagent-only` moves only them, and `--children-of ID` picks one parent's without naming them. All targets resolve or none move: an ambiguous prefix lists its candidates, and a running session is refused. `-n` (`--dry-run`) prints the plan and moves nothing. `maid sessions path ID` prints a path.

Where the continuation is written depends on `--append` / `--no-append`:

* **append** (the interactive default): the old file keeps growing; one conversation, one file.
* **no-append**: a new file whose first record points at the old one and says how many records were loaded; the old messages are not copied. Loading follows that pointer, so the new file resumes and lists normally (`maid sessions` shows "resumed from"). This is also how a session forks: two continuations of the same past never touch each other or the original.
* **`--fork-at N`** (with `-c` or `-r`, interactive or `-p`): the same kind of new file, but pointing at the old session's first N records only, so the conversation continues from an earlier point; nothing after record N is loaded and the old file is never changed. `maid sessions path ID` finds the file to count records in; a `user` record is a natural cut. It cannot be combined with `--append`.

**Import and redact.** `maid sessions import FILE` turns a claude.ai export (JSON; `--conversation UUID` picks one out of a full export) or a Claude Code transcript (JSONL from `~/.claude/projects/`) into a session here and prints its id; the format is detected from the content, `--as claude-ai|claude-code` overrides it, `--home general|project|NAME` says where it goes. `maid sessions redact ID` writes `./<id>.redacted.jsonl` with credential material replaced by `[REDACTED:kind]` and reports the count per kind; `-o FILE` and `--in-place` choose the destination (`--in-place` first copies the original to `sessions/.backups/<id>/`, where `cai trans-fairy-write restore` finds it), and an existing file is never overwritten. The record types, the import rules and every redaction kind: [docs/sessions.md](../docs/sessions.md).

**Looking at and building on a session.** `maid sessions read ID [--range A-B] [--tools]` prints the conversation as text; `state ID` is a one-screen summary (turns, tool calls per tool, files touched, tokens against the budget, compactions, forks, subagents); `time ID` shows how long each turn took and the slowest tool calls; `name ID` titles it with the title model; `output ID [CALL] [--replay]` lists, prints or replays a command's whole output where the model got it capped, labelled display only ([docs/sessions.md](../docs/sessions.md#full-output)). Three commands make a new session without changing any existing one: `inject ID --text T [--at N] [--role user|system]` forks at N and adds one note marked as injected; `graft ID --onto TARGET [--at N]` forks TARGET at N and copies ID's conversation in after a note saying where it came from; `compose ID --from N [--root FILE]` copies ID's records from N on, after an optional root text and a note that the earlier part is missing, which is a cheap re-root of a long session. Details and the record types: [docs/sessions.md](../docs/sessions.md#building-sessions-from-sessions).

**cai.** `maid cai TOOL ...` (the same as `cai TOOL ...` on PATH) runs Micaiah's cai-tools, every tool with its own command line: `cai read SESSION.jsonl` prints what was said, `maid trans-fairy` cuts, composes, grafts and installs transcripts, `maid trans-fairy-write` overwrites one after copying it to `sessions/.backups/`, and `maid help cai TOOL` prints a tool's help. They take a MAID session or a Claude Code transcript, told apart by content. See [docs/cai.md](../docs/cai.md).

**Leak audit.** `maid-leak-audit` reads every transcript for tool calls that reached for a host socket from the sandbox (the hole v0.3.1 closed), has a local model judge each one (`qwen-9b` by default; a model off this machine is refused), writes the findings to a private file under `~/.local/state/maid/audits/` and prints one line: whether something was reached for. See [docs/leak-audit.md](../docs/leak-audit.md).

**Audit trail.** Off by default. With `enabled = true` in `~/.config/maid/audit.lua` (`maid audit-trail init`), every tool call of every session, recorded or not, leaves one entry for the leak audit: the call and the harness's decisions, never text or output. `maid audit-trail status`, `purge`, `offsite DEST` (prints, never runs, the commands that move old archive chunks on) and `schedule install|remove`. See [docs/audit-trail.md](../docs/audit-trail.md).

**Context for a turn.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several files, and `-C -` reads stdin (then the prompt itself has to be an argument). It combines with `-c`/`-r`: `maid -p "compare these" -c -C new-draft.md` puts the file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maid`.

**Interactive from a prompt.** `maid -p "…" --interactive` (`-i`) opens the normal session with the prompt already sent, context files attached and `-c`/`-r` honoured. Because it is an interactive session, interactive transcript rules apply no matter where the flags appear: a transcript is always kept and resumes append unless `--no-append`. `--record` is redundant there and `--json` is ignored with a note.

**Unrecorded sessions** still get a transcript, but in the runtime directory: `$XDG_RUNTIME_DIR/maid/sessions/` (a tmpfs the system clears at logout), or `/tmp/maid-<uid>/sessions/` without one; on Windows this will be `%TEMP%\maid\sessions`. Nothing there is listed by `maid sessions` or found by `-c`. `maid --no-record` does this for one interactive session, `"record": false` in settings makes it the default, and `--record` turns it back on. `maid -p` is unrecorded unless `--record` (or `--transcript`); with `-c` / `-r` that keeps a pointer-style fork, and `--append` writes into the old file instead. So `cat big-context.md | maid -p - -r ID` runs a large context against an old conversation and leaves nothing behind, `... --record` keeps that as a fork, and `maid -c -p "..." --append` extends the old one in place.

## Tools the model gets

`read_file` (`grep` returns only the matching lines of a big file), `list_dir` (`depth` for a tree), `glob` (files by name pattern), `search_files` (grep -E syntax), `write_file`, `edit_file`, `multi_edit` (several replacements in one file, all or none), `apply_patch` (a unified diff over one or more files, all or none), `move_file`, `copy_file`, `delete_file`, `make_dir`, `run_shell`, `question` (asks you something, with options to pick by number), `todo` (the model's plan; `:todo` shows it, the status strip counts it) and `task` (a subagent, see below); inside a connected host nvim also `diagnostics` (its LSP diagnostics for a file or the workspace, judged as a read). Every call goes through the harness in `core/`: the file tools are judged as writes to every path they touch, so a move or copy out of the workspace asks, a delete under `~/.ssh` trips, and a patch is refused whole if one of its files is. The model is briefed at the start of the conversation about MAID, the tools, the modes, the harness and what a denial means (`core/src/agent.cpp`, `system_prompt`).

**Subagents.** `task {agent, prompt, context, model}` (opencode's names: the tool was `delegate` and agents were profiles) runs a second agent in the same workspace as an agent that only narrows what it may do: `explore` (auto-read, read-only tools and commands, 50k tokens) for a long read or search the model does not want in its own context, `plan` (plan mode, reads inside the workspace only) for a review of its own change, `general` (edit mode) for a self-contained piece of work, or an agent from `agents` in settings whose `role` lets task run it. Its model comes from the session's preset: the same one, or a non-limited tier when the session's model is `limited` (Fable hands subagents to Opus), or a preset the model asks for from its `subagents` list; a usage limit mid-task moves the child once to the preset's `on_limit`. The child's mode is capped by the session's, so in a manual session its commands are asked about like anything else; approvals come to you through the parent with the agent named, its tool calls show indented under the task call (`↳ explore on opus-5.5 (fable-5.1 is limited)`, then `↳ explore: ...`), its final answer is the tool result with its steps and tokens, and it has no `task`, `question` or `todo` of its own. Each child has its own transcript (kind `sub`) that `maid sessions` lists under the parent. See `:h task`, `:h agent`, `:h model`, [docs/tools.md](../docs/tools.md).

Your own tools are Lua files in `.maid/tools/` or `~/.config/maid/tools/`, each call in its own sandboxed LuaJIT state whose `maid.read` / `write` / `list` / `search` / `shell` go through the same authorisation step as the built-ins, or script tools: a directory there with a `tool.json` manifest (name, description, a JSON schema for the arguments, `run` as argv for Python, shell, Perl, Node, Deno, a Go binary or `wasmtime`, a timeout, and the `reads` and `writes` globs the harness judges before the script starts) and the script, which gets the arguments as JSON on stdin and runs in the same bubblewrap sandbox as `run_shell`, the workspace writable only when it declared writes. `:tools` and `maid tools` list both kinds, with a script tool's language and declared reads and writes; `maid tools check` validates every manifest; `maid tools new NAME --lang python|sh|perl|node` scaffolds one. Format and examples: [docs/tools.md](../docs/tools.md).
