# cli

The `maic` command. With no arguments it starts the agent in the current directory, which becomes the workspace. `maic help` lists the rest.

```sh
maic vendor add llamacpp           # once: build llama.cpp (docs/llamacpp.md)
maic vendor use llamacpp /path/to/model.gguf   # once per model: the GGUF it serves
cd ~/some/project
maic up llamacpp                   # once per boot
maic up llamacpp-2                 # optional: the side server on 8082, a second resident model (docs/llamacpp.md, Two servers)
maic                               # the agent, on llamacpp/current
maic --model anthropic/claude-opus-5-5 --mode auto-read
maic -c                            # continue the last session started in this directory
maic -r                            # pick an earlier session from a list (maic -r ID for one you know, maic -r PATH for any transcript file)
maic -p "explain main.cpp"         # one turn, no UI, no transcript; --record keeps one; -c / -r load an old one; --json for events
cat dialog.txt | maic -p -         # the prompt from stdin
maic -p "summarise these" -C notes.md -C log.txt   # files attached as context before the prompt (also for the interactive maic)
maic -p "let's plan the refactor" -i               # an interactive session that opens with that prompt sent
cat prompt.txt | maic -pi -        # short flags cluster (-p -i); a value-taking flag (-m, -C) goes last in a cluster
maic --system @~/prompts/reviewer.md --no-instructions   # front-load behaviour; ignore every MAIC.md / AGENTS.md
maic --prefix "Sure thing! "                              # every reply starts with these words, guaranteed (:h prefix)
maic --rule "Always answer in French"                      # a standing instruction, reminded every turn (:h rule)
maic --model opus-5.5                                      # a preset: model, 1M context, thinking on; reviews on Haiku 4.5
maic --xtc 0.5,0.1 --sampling min_p=0.05 --ban-pattern @~/bans/tics.re   # samplers and bans for one run
maic --bare                        # nothing from nvim: no host connection, highlighter, theme, lazy-lock notice or keymap check (:h bare)
maic help headless                 # the verbose page for all of the above (same as :h headless in a session)
maic help | grep vendor            # help goes to stdout, so it pipes; maic help topics lists every page
maic lua                           # a LuaJIT REPL in this directory with the maic table loaded
maic sessions                      # every session, with a preview
maic artifacts                     # where MAIC and its services keep transcripts, logs, outputs
maic diction                       # dictation into ./<dir>.md through whisper-server and a local scribe (docs/diction.md); maic help diction
maic model resolve qwen-4b         # what a preset or provider/model means here, as JSON (provider, kind, base_url, model, context)
maic settings init                 # a documented settings file (docs/settings.md)
maic trust                         # trust this directory's project files (settings, MAIC.md / AGENTS.md, .maic/tools/); :h trust
maic trust ~/dev2/app --lua sandbox     # its settings.lua in a child process that cannot reach the system (default full: as you)
maic trust ~/dev2/app --level relaxed   # how often to ask again: strict, standard (default) or relaxed; maic trust --list, maic untrust PATH
maic -p "run the tests" --trust    # headless runs ask nothing: --trust (fully) or --trust=sandbox, for that run only

maic themes                        # the themes (yours in ~/.config/maic/themes, then the shipped ones), the active one marked
maic themes import habamax         # a neovim colorscheme as a theme file, from a headless nvim with your config (docs/themes.md)
maic doctor                        # the machine, the tools MAIC needs, installed models, a recommended setup
maic lazy-lock                     # is nvim's lazy-lock.json as recorded? record / diff; exit 0 in sync (docs/lazy-lock.md)
maic nvim keymaps                  # :checkhealth maic from the shell: maic.nvim's, MAIC's and llama.vim's keys against your nvim config
maic nvim setup llama-vim          # llama.vim for lazy.nvim as one file MAIC owns in your spec's import directory (--dry-run, --remove)
maic setup                         # the first run as yes/no questions: settings, llama.cpp, ComfyUI, a model, the tripwire
maic models                        # the model catalog: what each is for, installed or not, current; maic models install ID [--link] (docs/models.md)
maic up llamacpp-fim               # code completion for llama.vim on 8084, after maic models install qwen2.5-coder-7b --link
maic server token new phone        # remote access (docs/remote.md): a bearer token for one device, shown once
maic server start                  # the API and the phone web client; --listen 0.0.0.0:7373 for the LAN, with TLS
maic server pair                   # with server.relay set: a code and a maic://pair/... string the phone pastes on the LAN
maic server pairs                  # the phones paired for the relay; maic server unpair NAME forgets one
maic server status                 # the configuration, the relay link (connected since, or not and why), whether it is up
```

## Screen

```
 conversation window          (markdown rendered; Ctrl-W k to move into it)
 ⏵ manual (shift-tab to cycle)        qwen3.5:4b local · harness armed · 1 queued (:w now)
 ───────────────────────────────────────────────────────────────────────────────────────
 ❯ your input                 (markdown highlighted as you type)
  INSERT   ↑12 (G follows)                                       Ctrl-W k: conversation · :help
```

The strip above the input shows the agent mode, the model, whether it is local or `REMOTE`, `nvim` while MAIC is connected to the nvim it runs inside (below), `bare` when it was started with `--bare`, the harness state, queued messages and whether the agent is working. The bottom line shows the vim mode, the focused window, your position, and the last message.

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
| anywhere | **Shift-Tab** cycles the mode; **Ctrl-Z** suspends to the shell (`fg` resumes, like vim); **Ctrl-C** interrupts the agent, else stops a `!command`, else clears the input, else (twice) quits; scroll wheel scrolls the conversation (in insert mode: prompt history) |
| approval prompt | **y** yes · **n** no · **N** no, then type a sentence the model receives as the reason · **a** always allow this file / program for the session · **t** trip the harness · **e** open the file asked about (in the host nvim when connected, else `$EDITOR`) · **d** the proposed change as a diff in a new tab of the host nvim. Edits show the lines that would change, removed in red and added in green (`:h diff`) |

Ctrl-W in insert mode deletes a word, as in vim; the window chord works from insert mode only when the input is empty, otherwise press Esc first.

The system clipboard is reached through `wl-copy` or `xclip` when present, and always through the terminal (OSC 52), which also works over ssh. Your terminal's own copy (Ctrl-Shift-C) keeps working on text you select with the mouse; with the scroll wheel enabled, that selection needs Shift+drag.

The input is highlighted as markdown by MAIC's own renderer. `highlight = "nvim"` in settings has an embedded `nvim --embed --headless` do it instead, with treesitter (fenced code in the languages nvim has parsers for gets keywords, strings and comments); it falls back to the built-in one when nvim is missing. See `:h highlight`.

## Inside nvim (maic.nvim)

`maic.nvim/` is the nvim plugin: `:Maic` runs MAIC in a terminal split, float or tab, `:MaicSend` puts the buffer's path or a range (as a fenced snippet with path and line numbers) into MAIC's input, `:MaicDiagnostics` and `:MaicQuickfix` send those lists ([maic.nvim/README.md](../maic.nvim/README.md)). From MAIC's side, nvim sets `$NVIM` for every job, and MAIC connects back to that socket as a msgpack-rpc client, but only when it is a Unix socket of this user whose nvim is one of MAIC's own parent processes (anything else is refused with the reason at start). Connected, `:e FILE` and `e` at an approval open files in the editing window, `d` shows a write's proposed change as a diff, the theme follows nvim's colorscheme live (`follow_nvim_theme`), User autocmds (`MaicTurnStart`, `MaicToolCall`, `MaicApproval`, `MaicFileWritten`, `MaicTurnEnd`) fire there, an approved write runs `:checktime`, your Lua gets `maic.nvim.*`, and the model gets the read-only `diagnostics` tool; `:MaicInterrupt` (`<leader>mc`) there stops a running turn as Ctrl-C does. The rules: [docs/nvim.md](../docs/nvim.md).

`maic --bare` (also `MAIC_BARE=1`, or `bare = true` in settings) starts MAIC with nothing from nvim: no `$NVIM` host even inside nvim's terminal, the built-in highlighter, no theme from nvim (`:theme nvim:NAME` is refused and says why), no lazy-lock notice or marker, no keymap check. MAIC's own settings, themes (saved `nvim-NAME.lua` files included), Lua and script tools load as usual; the strip shows `bare`. See `:h bare`.

`maic nvim keymaps [--all] [-u FILE]` runs maic.nvim's keymap check (what `:checkhealth maic` shows) in a headless nvim with your own config, after `User VeryLazy`, and prints the collisions with their fixes: keys maic.nvim had to skip, keys MAIC's own input needs in its terminal (Esc, Ctrl-W, Ctrl-P, Shift-Tab, Ctrl-Z, Ctrl-C, Alt-Enter) that a terminal-mode mapping takes, and llama.vim's keys. Exit 0 none, 1 collisions, 2 nvim could not run; `maic doctor` has a one-line summary. MAIC keeps the result under its state directory and, when `lazy-lock.json` changes, runs the check once more and says at start if a plugin update added a collision.

`maic nvim setup llama-vim [--dry-run] [--remove] [--yes]` writes llama.vim's spec from docs/models.md (port 8084, `model_fim = 'current'`, `<M-f>` and `<M-]>`, normal-mode keys off) as `maic-llama-vim.lua` in the directory your lazy.nvim spec imports, after showing it and asking (`--yes` off a terminal). It edits no other file: without nvim, lazy.nvim or an import directory it explains what is missing and exits 0, and it refuses a file of that name it did not write or a llama.vim already in your spec. `--remove` deletes only MAIC's file. A user command: the agent's `run_shell` is refused it. See docs/nvim.md.

## Commands

`:` in normal mode, or from the conversation window. Every command also works typed as a message beginning with `/`. As you type after `:`, a palette lists the matching commands with a one-line description (arguments too, for `:mode`, `:set`, `:h`, `:model`, `:up`); **Tab** completes to the highlighted one and cycles on repeat, Shift-Tab goes back. A unique prefix runs the command (`:inst` is `:instructions`), as in vim.

| Command | Does |
| :--- | :--- |
| `:w` | send the input (same as Alt+Enter). `:w now` or `:ww` sends immediately even while the agent is working (see below) |
| `:e` | edit the input in nvim (`$VISUAL`, then `$EDITOR`, then `nvim`); a non-zero exit leaves the input unchanged |
| `:e FILE` | open a file: in the host nvim's editing window when connected (`:drop`), else in `$VISUAL` / `$EDITOR` / `nvim` in MAIC's place |
| `:nvim [theme]` | whether MAIC is connected to the nvim it runs inside and what that gives; `theme` follows its colorscheme again. See below and `:h nvim` |
| `:mode manual\|auto-read\|edit\|auto\|plan` | set the agent mode |
| `:model NAME` | switch model (when idle): `llamacpp/current`, `llamacpp/Qwen3.5-9B-Q4_K_M` (any GGUF under the models directory), `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, ... `:model` alone lists providers |
| `:models` | models the current provider serves (llama.cpp: every GGUF under the models directory, by file name); `maic models` in the shell is the catalog of models MAIC can install (`:h models`, docs/models.md) |
| `:think on\|off` | let the model reason before answering |
| `:set markdown\|mouse\|enter_sends on\|off`, `:set highlight nvim\|builtin` | rendering, scroll-wheel, Enter and input-highlighter toggles |
| `:theme [NAME\|reload\|nvim:NAME]` | list the themes (the active one marked), switch one live, re-read the active one's file, or import a neovim colorscheme as `~/.config/maic/themes/nvim-NAME.lua` (Tab after `nvim:` lists them). Shipped: `default`, `gruvbox-dark`, `gruvbox-light`, `mono`. `theme` and `colors` in settings; `maic themes` and `maic themes import NAME` in the shell. See docs/themes.md |
| `:!cmd` or `!cmd` | run a command in **your** shell, unsandboxed, in the workspace; output shows in the conversation and is passed to the model as context (Ctrl-C stops it) |
| `:status` | harness, every service with where it runs (`[host]` pid or `[docker]` container) and what it holds (resident model; ComfyUI's VRAM and queue), model and whether it is remote, session file, queue. See `:h status` |
| `:up NAME` / `:down NAME` | start / stop a service |
| `:init` | scaffold `MAIC.md` and `.maic/settings.json` here, then have the agent draft the `MAIC.md` from the project; a recorded session that worked here throughout moves into the project's transcript home (`projects/<encoded workspace>/`), and one with more than `init_move_outside_reads` files read or any file written outside asks first. See `:h init` |
| `:cd PATH\|-\|PLACE` / `:pwd` | move this session's workspace (when idle): an absolute path, one relative to the workspace, `~/...`, a place name, or `-` for the previous one; the harness root, shell commands, the project settings layers and the instruction files follow, the model gets a note and the transcript a `workspace` record. `:cd` alone or `:pwd` shows it. See `:h cd` |
| `:settings` | which settings files are in effect and where this session's transcript lives |
| `:instructions` | the MAIC.md / AGENTS.md files in effect |
| `:trust [PATH] [--lua full\|sandbox\|restricted] [--level strict\|standard\|relaxed]` / `:untrust [PATH]` | trust this workspace's project directories (or one), or forget it. `--lua` is how its settings.lua runs (full: as you; sandbox: in a child process that cannot reach the system; restricted: a restricted state in process), `--level` how often it is asked about again. Until trusted, a directory's `.maic/settings.*` are not applied, its instruction files are not given to the model and its `.maic/tools/` are not loaded; MAIC asks once on the terminal before the screen is drawn (trust fully, trust sandboxed, not now, never). `maic trust [PATH] [--lua L] [--level L]`, `maic trust --list`, `maic untrust PATH` in the shell. See `:h trust` and docs/harness.md |

| `:session` / `:artifacts` | where this transcript is; where everything is kept, with sizes |
| `:path [NAME] [copy]` / `:open NAME` | every place maic knows by a short name (`workspace`, `session`, `sessions`, `models`, `workflows`, `templates`, `vendor/llamacpp`, `comfyui/outputs`, ...); show one, copy it to the clipboard, or open it in the file manager. `maic path`, `maic open`, and `eval "$(maic shell-init)"` for `mcd NAME` in your shell. See `:h path` |
| `:gpu [free [llamacpp\|llamacpp-2\|llamacpp-fim\|whisper\|comfyui] \| load llamacpp-fim]` | who holds the card (each llama server's resident model, the code completion server's as loaded, unloaded or not linked, whisper's, ComfyUI's VRAM view) and one sentence on whether the models fit it; `free` unloads without stopping anything (whisper cannot, and says so); `load llamacpp-fim` loads the coder, which never loads by itself. `maic gpu` in the shell. A failed `maic up` explains a CUDA out of memory in plain words |
| `:lazylock [record\|diff]` | whether nvim's `lazy-lock.json` still matches the hash you recorded; `record` writes the hash file (for your dotfiles) and a snapshot, `diff` lists plugins added, removed and updated and calls out a lazy.nvim update. Out of sync: a notice at start and `lock≠` in the status strip. `maic lazy-lock` in the shell. See docs/lazy-lock.md |
| `:ctx [N]` / `:ctx2 [N]` | the context window of the main llama server (`--ctx`, `context`) and of the side server `llamacpp-2` (`--ctx2`, `context_2`); setting one restarts that server when it runs with another size. See `:h ctx` |
| `:open NAME folder` / `maic cd NAME [--subshell]` | the containing folder in the file manager; the place's directory printed for `cd "$(maic cd NAME)"`, or with `--subshell` a shell there (`exit` returns) |
| `:open SERVICE [firefox\|chrome]` | a service's URL in the browser (`browser` in settings picks the default one; `remote` in settings opens a subscribed maic-server's copy) |
| `:image [FILE\|clear]` | a picture for the next message; `![alt](path)` in the text or a file dragged onto the terminal is attached on send (a plain path inside a sentence stays text). `--image FILE` on the command line. Vision models only. See `:h image` |
| `:forbid [TERM]` | terms no tool call may contain; a search, command or path with one is halted in every mode, under every harness. See `:h forbid` |
| `:allow [PATTERN]` | commands pre-approved for every mode: no asking, no reviewer. MAIC's helpers are on it by default. See `:h allow`; the full `permission` block (allow / ask / deny over `tool:pattern`) lives in settings, see `:h permission` |
| `:reg` | the yank register and the named registers `"a`..`"z` |
| `:undo [N]` | restore the file(s) the agent changed last; every write saves the previous content first, a delete keeps the file, a move is moved back |
| `:copy` / `:export [FILE]` | copy the last reply to the clipboard; write the transcript as markdown |
| `:stash` / `:pop` | park the input draft and bring it back (survives restarts). `:q` with an unsent draft stashes it for you |
| `:wq` | send, then quit when the reply is in (Ctrl-C while waiting stays) |
| `:rename TITLE` | title the session (`maic sessions` shows it); `small_model` in settings (older name: `title_model`) auto-titles after the first turn |
| `:ban add TEXT` / `:ban pattern REGEX` / `:ban token ID` / `:ban list` | phrases and regexes the model must not say (cut before they show, re-asked, then replaced) and token bans (`logit_bias` on OpenAI-compatible providers). `--ban`, `--ban-pattern`, `bans` in settings. See docs/bans.md |
| `:sampling [KEY VALUE\|xtc P T]` | temperature, top_k, min_p, seed, ... for this session; `xtc` (exclude top choices) on llama.cpp-style servers. See `:h sampling` |
| `:harness [smart\|dumb]` | the model reviewer on (default) or off. Auto under a dumb harness warns once and asks; see `:h harness` |
| `:budget [N\|off]` | tokens used; a per-session budget that stops the agent when reached |
| `:set timestamps on` | a time beside each message (also `timestamps` in settings) |
| `:lua [CODE]` / `:luafile PATH` / `:chat` | run Lua (vendored LuaJIT) in the workspace; an expression shows its value. `:lua` alone enters **Lua mode**: the input becomes a REPL (`lua❯`) until `:chat`. Globals persist; output goes to the conversation and to the model as context. Outside a session `maic lua` is a REPL, `maic lua FILE [args]` runs a file. See `:h lua` |
| `:compact [prune\|head\|all]` | free context. Default order: stub old tool results (dialog untouched), then, only if still needed, summarise the oldest turns into a handover note. `all` is a whole-conversation summary. Runs automatically at `compact_at` (75%) |
| `:clear` | start a new conversation (the session file keeps both) |
| `:trip REASON` / `:unlock` | trip the harness now; reset it without leaving the session (asks for your sudo password) |
| `:h [TOPIC]` | vim-style help. `:h` alone is an index; `:h w`, `:h u`, `:h f`, `:h .`, `:h gq`, `:h Ctrl-W`, `:h Alt+Enter`, `:h modes`, `:h harness`, `:h sessions`; a unique prefix is enough and an ambiguous one lists the candidates |
| `:q` | quit |

### After every turn

A footer line shows the model, how long the turn took and how many tools ran (`▣ qwen3.5:4b · 12.3s · 3 tool calls`, `· interrupted` when you stopped it). `run_shell` takes a `workdir` argument, so the approval prompt shows `pytest` in `services/api` rather than a `cd` chain; a workdir outside the workspace is asked about. Reading a file under a directory with its own `AGENTS.md` (or any name in `instruction_files`) attaches those instructions to the result once.

### Messages while the agent works

Typing and sending while the agent is busy queues the message; it reaches the model at its next step in the current turn (a notice says so, and the status strip counts it). `:w now` delivers it immediately: the model's current output is abandoned and it is asked again with your message included. If a tool is running, the message lands the moment the tool returns. Messages still queued when a turn ends start the next turn by themselves.

## Modes

| Mode | Reads in workspace | Read-only commands | Edits in workspace | Other commands | Outside workspace |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **manual** (default) | yes | ask | ask | ask | ask |
| **auto-read** | yes | yes (read-only sandbox) | ask | ask | reads yes, writes ask |
| **edit** | yes | ask | yes | ask | ask |
| **auto** | yes | yes | yes | yes (sandboxed) | reads yes, writes ask |
| **plan** | yes | yes (read-only sandbox) | no | no | reads ask |

"Read-only commands" are ones MAIC recognises as only looking (`ls`, `cat`, `grep`, `git log`, `find` without `-delete`/`-exec`, ...) with no redirection or substitution. They run with the workspace mounted read-only as well, so a wrong guess still cannot change anything.

Whatever the mode: secrets are never read, system paths are never written, startup files and MAIC's own harness are always asked about, dangerous commands trip the harness, and a request that did not come from this terminal is always asked. See [docs/harness.md](../docs/harness.md).

## Sessions

Every session is a JSONL file under `~/.local/state/maic/sessions/`, readable only by you: each message as sent to the model, every tool call with the harness's decision, and the displayable transcript. `maic -c` resumes the newest session started or last opened in the current directory; `maic -r` lists them; `maic -r ID` (a unique prefix is enough) resumes one; `maic -r PATH` resumes any transcript file by path, which is how a temporary `--no-record` transcript (never listed) comes back; when a session ends, `maic` prints the transcript's path and that exact command. The model is told it resumed and what the current mode and instructions are. A session that ended mid tool call resumes from the last complete step.

**Homes.** A project (a workspace with a `MAIC.md`, its own or inherited from a parent directory) keeps its transcripts under `sessions/projects/<encoded workspace path>/`, the Claude Code layout; everything else goes to `sessions/general/`. `sessions_home` in any settings layer overrides this (`general`, `project`, or a name for `sessions/<name>/`), so a project can opt back into `general`. `maic init` or `:init` turns a directory into a project. Every open (the first start and each resume) records the workspace, host and pid, so `maic sessions` shows where a transcript was started and, when different, where it was last opened and how many times. `maic sessions rehome ID [ID...] [project|general|NAME]` moves transcripts to another home (HOME last; one ID alone goes to its own project's directory), printing `from -> to` for each and appending a `rehomed` record; forks keep working because they find their parent by id. Subagent sessions move only when asked: by default a session's `sub` sessions stay where they are and still find it by id, `--subagent` moves them with it (children of children too), `--subagent-only` moves only them, and `--children-of ID` picks one parent's without naming them. All targets resolve or none move: an ambiguous prefix lists its candidates, and a running session is refused. `-n` (`--dry-run`) prints the plan and moves nothing. `maic sessions path ID` prints a path.

Where the continuation is written depends on `--append` / `--no-append`:

* **append** (the interactive default): the old file keeps growing; one conversation, one file.
* **no-append**: a new file whose first record points at the old one and says how many records were loaded; the old messages are not copied. Loading follows that pointer, so the new file resumes and lists normally (`maic sessions` shows "resumed from"). This is also how a session forks: two continuations of the same past never touch each other or the original.
* **`--fork-at N`** (with `-c` or `-r`, interactive or `-p`): the same kind of new file, but pointing at the old session's first N records only, so the conversation continues from an earlier point; nothing after record N is loaded and the old file is never changed. `maic sessions path ID` finds the file to count records in; a `user` record is a natural cut. It cannot be combined with `--append`.

**Import and redact.** `maic sessions import FILE` turns a claude.ai export (JSON; `--conversation UUID` picks one out of a full export) or a Claude Code transcript (JSONL from `~/.claude/projects/`) into a session here and prints its id; the format is detected from the content, `--as claude-ai|claude-code` overrides it, `--home general|project|NAME` says where it goes. `maic sessions redact ID` writes `./<id>.redacted.jsonl` with credential material replaced by `[REDACTED:kind]` and reports the count per kind; `-o FILE` and `--in-place` choose the destination (`--in-place` first copies the original to `sessions/.backups/<id>/`, where `cai trans-fairy-write restore` finds it), and an existing file is never overwritten. The record types, the import rules and every redaction kind: [docs/sessions.md](../docs/sessions.md).

**Looking at and building on a session.** `maic sessions read ID [--range A-B] [--tools]` prints the conversation as text; `state ID` is a one-screen summary (turns, tool calls per tool, files touched, tokens against the budget, compactions, forks, subagents); `time ID` shows how long each turn took and the slowest tool calls; `name ID` titles it with the title model. Three commands make a new session without changing any existing one: `inject ID --text T [--at N] [--role user|system]` forks at N and adds one note marked as injected; `graft ID --onto TARGET [--at N]` forks TARGET at N and copies ID's conversation in after a note saying where it came from; `compose ID --from N [--root FILE]` copies ID's records from N on, after an optional root text and a note that the earlier part is missing, which is a cheap re-root of a long session. Details and the record types: [docs/sessions.md](../docs/sessions.md#building-sessions-from-sessions).

**cai.** `maic cai TOOL ...` (the same as `cai TOOL ...` on PATH) runs Micaiah's cai-tools, every tool with its own command line: `cai read SESSION.jsonl` prints what was said, `maic trans-fairy` cuts, composes, grafts and installs transcripts, `maic trans-fairy-write` overwrites one after copying it to `sessions/.backups/`, and `maic help cai TOOL` prints a tool's help. They take a MAIC session or a Claude Code transcript, told apart by content. See [docs/cai.md](../docs/cai.md).

**Context for a turn.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several files, and `-C -` reads stdin (then the prompt itself has to be an argument). It combines with `-c`/`-r`: `maic -p "compare these" -c -C new-draft.md` puts the file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maic`.

**Interactive from a prompt.** `maic -p "…" --interactive` (`-i`) opens the normal session with the prompt already sent, context files attached and `-c`/`-r` honoured. Because it is an interactive session, interactive transcript rules apply no matter where the flags appear: a transcript is always kept and resumes append unless `--no-append`. `--record` is redundant there and `--json` is ignored with a note.

**Unrecorded sessions** still get a transcript, but in the runtime directory: `$XDG_RUNTIME_DIR/maic/sessions/` (a tmpfs the system clears at logout), or `/tmp/maic-<uid>/sessions/` without one; on Windows this will be `%TEMP%\maic\sessions`. Nothing there is listed by `maic sessions` or found by `-c`. `maic --no-record` does this for one interactive session, `"record": false` in settings makes it the default, and `--record` turns it back on. `maic -p` is unrecorded unless `--record` (or `--transcript`); with `-c` / `-r` that keeps a pointer-style fork, and `--append` writes into the old file instead. So `cat big-context.md | maic -p - -r ID` runs a large context against an old conversation and leaves nothing behind, `... --record` keeps that as a fork, and `maic -c -p "..." --append` extends the old one in place.

## Tools the model gets

`read_file` (`grep` returns only the matching lines of a big file), `list_dir` (`depth` for a tree), `glob` (files by name pattern), `search_files` (grep -E syntax), `write_file`, `edit_file`, `multi_edit` (several replacements in one file, all or none), `apply_patch` (a unified diff over one or more files, all or none), `move_file`, `copy_file`, `delete_file`, `make_dir`, `run_shell`, `question` (asks you something, with options to pick by number), `todo` (the model's plan; `:todo` shows it, the status strip counts it) and `task` (a subagent, see below); inside a connected host nvim also `diagnostics` (its LSP diagnostics for a file or the workspace, judged as a read). Every call goes through the harness in `core/`: the file tools are judged as writes to every path they touch, so a move or copy out of the workspace asks, a delete under `~/.ssh` trips, and a patch is refused whole if one of its files is. The model is briefed at the start of the conversation about MAIC, the tools, the modes, the harness and what a denial means (`core/src/agent.cpp`, `system_prompt`).

**Subagents.** `task {agent, prompt, context, model}` (opencode's names: the tool was `delegate` and agents were profiles) runs a second agent in the same workspace as an agent that only narrows what it may do: `explore` (auto-read, read-only tools and commands, 50k tokens) for a long read or search the model does not want in its own context, `plan` (plan mode, reads inside the workspace only) for a review of its own change, `general` (edit mode) for a self-contained piece of work, or an agent from `agents` in settings whose `role` lets task run it. Its model comes from the session's preset: the same one, or a non-limited tier when the session's model is `limited` (Fable hands subagents to Opus), or a preset the model asks for from its `subagents` list; a usage limit mid-task moves the child once to the preset's `on_limit`. The child's mode is capped by the session's, so in a manual session its commands are asked about like anything else; approvals come to you through the parent with the agent named, its tool calls show indented under the task call (`↳ explore on opus-5.5 (fable-5.1 is limited)`, then `↳ explore: ...`), its final answer is the tool result with its steps and tokens, and it has no `task`, `question` or `todo` of its own. Each child has its own transcript (kind `sub`) that `maic sessions` lists under the parent. See `:h task`, `:h agent`, `:h model`, [docs/tools.md](../docs/tools.md).

Your own tools are Lua files in `.maic/tools/` or `~/.config/maic/tools/`, each call in its own sandboxed LuaJIT state whose `maic.read` / `write` / `list` / `search` / `shell` go through the same authorisation step as the built-ins, or script tools: a directory there with a `tool.json` manifest (name, description, a JSON schema for the arguments, `run` as argv for Python, shell, Perl, Node, Deno, a Go binary or `wasmtime`, a timeout, and the `reads` and `writes` globs the harness judges before the script starts) and the script, which gets the arguments as JSON on stdin and runs in the same bubblewrap sandbox as `run_shell`, the workspace writable only when it declared writes. `:tools` and `maic tools` list both kinds, with a script tool's language and declared reads and writes; `maic tools check` validates every manifest; `maic tools new NAME --lang python|sh|perl|node` scaffolds one. Format and examples: [docs/tools.md](../docs/tools.md).
