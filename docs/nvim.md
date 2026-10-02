# MAIC inside nvim: maic.nvim

Two halves that meet over nvim's own socket. The nvim side is a plugin in this repository, `maic.nvim/`, which runs MAIC in a terminal and sends buffers, selections, diagnostics and the quickfix list into its input. The MAIC side finds the nvim it runs inside and connects back as a msgpack-rpc client, so the host becomes MAIC's editor: files open there, approved edits show as diffs there, the theme follows its colorscheme, and the user's Lua can reach `vim.*`. The model gets almost none of that, on purpose (below).

## The nvim side: the plugin

Standard layout: `plugin/maic.lua` (the commands, nothing else loads until one is used), `lua/maic/init.lua`, `doc/maic.txt` (`:h maic`), tests in `tests/`. Install and every option: [maic.nvim/README.md](../maic.nvim/README.md).

| Command | Does |
| :--- | :--- |
| `:Maic [args]` | Opens MAIC in a terminal (`open = "split" \| "vsplit" \| "float" \| "tab"`), or focuses the one this tab already has. `args` are added to `maic` when it starts. |
| `:MaicToggle` | Hides this tab's MAIC window (MAIC keeps running) or shows it again. |
| `:[range]MaicSend` | With a range, those lines as a fenced snippet headed with the path and line numbers; without one, the buffer's path for the agent to read. Visual mode gives the range. |
| `:MaicDiagnostics[!]` | The buffer's LSP diagnostics (`!`: every buffer's), one `path:line:col: severity: message` line each. |
| `:MaicQuickfix` | The quickfix list, the same shape. |
| `:MaicInterrupt` | Stops MAIC's running turn as its first Ctrl-C does, from any window: `rpcnotify(chan, "maic_interrupt")` when MAIC is connected, else Ctrl-C typed into this tab's MAIC terminal. |

Text goes into MAIC's input as a paste and is never sent by itself. The plugin finds MAIC through `nvim_list_chans()`: the client named `maic` whose `pid` attribute is this tab's terminal job (with no MAIC terminal in the tab, the newest `maic` client), and calls `rpcnotify(chan, "maic_send", text)`; `require("maic").command(":theme mono")` sends `maic_command`, run in MAIC as if typed. When MAIC is not connected (it was refused, or it is an older build), `:MaicSend` writes the text into the terminal job as a bracketed paste (`ESC [200~ ... ESC [201~`), which MAIC also takes into the input whole, in any mode.

Every option and its default is one table, `require("maic").defaults` (`:h maic-defaults`): the layout, the keymaps and whatever comes next. `require("maic").setup(opts)` (lazy.nvim's `opts`) deep-merges `opts` over it with `vim.tbl_deep_extend("force", ...)` and sets the keymaps, `<leader>m` then `m` (open), `t` (toggle), `s` (send the path; in visual mode the selection), `b` (the whole buffer), `d` / `D` (diagnostics, every buffer's), `q` (quickfix), `c` (interrupt). In MAIC's own buffers `<C-c>` interrupts too, buffer-local (`buffer_keymaps`; passed through to MAIC in terminal mode); `<C-c>` is never mapped globally. `keymaps = false` sets none; a table moves or drops single ones by name. It never overwrites a mapping: before each default key it checks `maparg()` and `mapcheck()` for that mode, skips a key another mapping holds or shares a prefix with, and reports every skipped key once per session in one `vim.notify` warning naming the key, the mode and what holds it (the description, or the script and line from the map's `sid`). A key the user names in `keymaps` is set anyway, and reported if it shadows something.

In MAIC's terminal, `<Esc>` goes to MAIC (its input is vim-like and needs Esc for normal mode) even when the user has a global `tnoremap <Esc> <C-\><C-n>`: maic.nvim maps `<Esc>` to itself buffer-local, noremap and nowait, in that terminal only (`terminal_passthrough`, a set of keys), so the global mapping keeps working everywhere else. `terminal_escape` (default `<C-\><C-n>`, nvim's own, which needs no mapping) leaves terminal mode there. MAIC's terminal gets the filetype `maic` before its job starts, so a `TermOpen` autocmd can skip it (`filetypes` in the defaults table).

`:checkhealth maic` (`lua/maic/health.lua`, the check itself in `lua/maic/keymaps.lua`) lists every key maic.nvim sets, every key llama.vim uses when it is installed (`g:llama_config` set, or `autoload/llama.vim` on the runtimepath; its defaults and older option names are mirrored from `autoload/llama.vim`), and the keys MAIC's own input needs in its terminal (Esc, Ctrl-W, Ctrl-P, Shift-Tab, Ctrl-Z, Ctrl-C, Alt-Enter), against the user's mappings in the modes each is used in, global and buffer-local: it opens a plain file buffer, a scratch buffer of filetype `maic-input` and a terminal of filetype `maic` in a tab of its own, reads what the user's autocmds mapped there, and closes them. Each key is an OK, WARN or ERROR line with a fix hint per collision: ERROR for a key MAIC needs that never reaches it in its terminal (the hint is `terminal_passthrough`), WARN for a skipped or shadowed key, two keys sharing a prefix, or an insert-mode key that starts with a printable key (so typing it waits `timeoutlen`). Keys are compared as `keytrans()` names, so `<C-w>` typed as `\x17` and kept with a modifier match.

### `maic nvim keymaps`

The same check from the shell, against the user's own config: `nvim --headless -i NONE -n -V1<log>` with stdin, stdout and stderr on `/dev/null`, `--cmd 'let g:maic_keymap_check = 1'` (maic.nvim's `setup()` then plans its keys and sets none, so the user's own mappings are what is checked), and `-c` loading `maic.nvim/lua/maic/keymaps.lua` from MAIC's tree by path (so it works whether or not the config installs maic.nvim; when it does, the user's configured copy and options are used). It dispatches `User VeryLazy` first so lazy.nvim's lazy-loaded plugins map their keys, writes the report as JSON and quits; `-V1` makes nvim record the file and line of mappings made from Lua. It runs in its own process group with `$NVIM` dropped from its environment and is killed at 60 s. It prints the collisions with their fixes (`--all`: every key; `-u FILE`: that config instead of the user's init) and exits 0 with none, 1 with collisions, 2 when nvim could not run. `maic doctor` prints a one-line summary.

Every run from the user's config is kept in `<state>/nvim/keymaps.json`: the collision ids (a key, a mode and what holds it, without line numbers), their sha256 hash, and the hash of `lazy-lock.json` at the time. When the lazy-lock watch sees a `lazy-lock.json` that differs from the one the record was made at (at start, and at each turn end), MAIC runs the check once in the background and reports any collision the record did not have as a notice, `a plugin update added keymaps that collide: maic nvim keymaps`, with each one listed; on a machine with no record yet it says how many it found. A machine without a lock file or without nvim stays quiet, and `--bare` never runs it.

### `maic nvim setup llama-vim`

`maic nvim setup llama-vim [--dry-run] [--remove] [--yes]` adds [llama.vim](models.md#code-completion) to a lazy.nvim setup as one file MAIC owns. It looks first and changes nothing while it looks:

1. **lazy.nvim.** nvim has to be on PATH and lazy.nvim installed at `stdpath("data")/lazy/lazy.nvim` (`$XDG_DATA_HOME`, `$NVIM_APPNAME`). Without that directory your init is not run at all, since a bootstrap snippet in it would clone lazy.nvim. With it, a headless nvim with your config (stdin, stdout and stderr on `/dev/null`, `$NVIM` dropped, `GIT_ALLOW_PROTOCOL=file` so nothing can be cloned, killed at 60 s) answers whether `require("lazy")` loads and whether your config called its setup (`require("lazy.core.config").spec` is set). Any of these missing: it says which, prints the spec to paste once you have lazy.nvim, writes nothing and exits 0. No other plugin manager is touched.
2. **The import.** The same nvim lists the modules your spec imports (`{ import = "plugins" }` entries in `require("lazy.core.config").options.spec`, or `setup("plugins")`); the first one that is a directory of your config, `<config>/lua/plugins/`, is where the file goes. With none, it does not edit your init files: it prints the import to add, the file to save the spec as, and exits 0.
3. **The file.** `<import dir>/maic-llama-vim.lua`, the spec from [models.md](models.md#code-completion) after a header: MAIC as its writer, the date, `maic nvim setup llama-vim --remove` to undo it, and the docs section. A file of that name without MAIC's header is refused (exit 1). When llama.vim is in your spec already from anywhere else (`require("lazy.core.config").plugins`, with the files that name it), it refuses and names them (exit 1), printing MAIC's settings to merge by hand. MAIC's own file is updated in place with a diff, or left alone when it already holds the spec.

It shows the plan (the file, its content or the diff, the import it relies on) and asks yes or no; off a terminal it needs `--yes` (exit 2 without), and `--dry-run` shows the plan and writes nothing. Afterwards it says what is next: open nvim (lazy.nvim installs llama.vim on its next start, or `:Lazy sync`); that changes `lazy-lock.json`, which `maic lazy-lock` reports until you run `maic lazy-lock record`, and MAIC re-runs its keymap check after that lock change; and, only when missing, `maic models install qwen2.5-coder-7b --link` (no coder linked) and `maic up llamacpp-fim` (the completion server is not running). `--remove` deletes only that file, and refuses one without MAIC's header, with the same question. `-u FILE` runs nvim with that init instead of yours (the tests use it).

It is a user command: as a tool call it is denied in every mode, over any allow list, and the read-only classifier counts it as a write (the sandbox's read-only home would stop the write anyway).

## The MAIC side: finding the host

nvim sets `$NVIM` to its server socket for every job it starts, so a MAIC in an nvim terminal knows where its host listens. MAIC connects to it only when all of this holds, checked before any request is sent:

1. `$NVIM` is a path (an address like `127.0.0.1:6666` is refused) to a Unix socket owned by the user running MAIC.
2. After connecting, the listening process's credentials (`SO_PEERCRED`) say it runs as the same user.
3. That process is one of MAIC's ancestors: its pid is found walking MAIC's parent chain through `/proc/<pid>/stat`. nvim 0.10 and later run the server as a child of the TUI process, and the terminal job as a child of the server, so the server is always in the chain.
4. When the socket has nvim's default name, `nvim.<pid>.<n>`, the pid in the name is the process behind it.

Anything else is refused, and the reason is the first line in the conversation (`nvim: not connecting to $NVIM (...)`); MAIC then runs as it would outside nvim. A socket of another user, of an unrelated nvim, or of a process that merely sits in `$NVIM` gets nothing.

**For tests only:** `MAIC_NVIM_TRUST_SOCKET=1` skips the ancestry part (3), so a test can connect MAIC to a headless `nvim --listen` it started itself. It is ignored unless `MAIC_TESTING=1` is set as well; the ownership checks still apply. Nothing outside the test suite should set either.

Connected, MAIC calls `nvim_set_client_info("maic", ..., "remote", {}, {pid = ...})` so the plugin can find it, the status strip shows `nvim`, and `:nvim` says what the connection gives. The connection happens before the settings files are read, so their Lua can use `maic.nvim` too. When the host goes away a notice says so and everything falls back.

## Bare

`maic --bare` (also `MAIC_BARE=1`, or `bare = true` in settings) starts MAIC with nothing from nvim: no `$NVIM` host connection even inside nvim's terminal (maic.nvim then uses its fallbacks: a bracketed paste for `:MaicSend`, Ctrl-C into the terminal for `:MaicInterrupt`), the built-in input highlighter even with `highlight = "nvim"` (`:set highlight nvim` is refused), no theme following nvim's colorscheme, `:theme nvim:NAME` refused with a message naming bare, no lazy-lock notice and no `lock≠`, no keymap check after a lazy-lock change. MAIC's own settings, themes (including saved `nvim-NAME.lua` theme files, which are MAIC's files), Lua and script tools still load, and `:e FILE` still runs `$VISUAL` / `$EDITOR` when asked. The status strip shows `bare`; `:nvim` says what is off. `--bare` and `MAIC_BARE=1` never connect; `bare = true` in a settings file is known only after the files are read, and the host connection comes before that (so the settings Lua can use `maic.nvim`), so it is dropped right after. A future `--ui nvim` (nvim as MAIC's interface, [roadmap.md](roadmap.md)) will be refused together with `--bare`.

## What the connection gives

* **Inbound.** `maic_send` text is appended to the input (after a blank line when there is a draft), never sent; `maic_command` runs as if typed; `maic_interrupt` does what the first Ctrl-C does to a running turn, a running `!` command or a pending approval or question (stops it, answers no), and when MAIC is idle shows `nvim: nothing to interrupt (MAIC is idle)` instead of clearing the draft or arming the quit. Notifications are handled on the connection's own thread and handed to the UI, never acted on from the reader.
* **`:e FILE`** (relative to the workspace) opens the file in the host with `:drop`, in the window you edit in: the current window unless it is a terminal or a float (MAIC's own), else the previous window, else any plain window in the tab, else a new split. Outside a host `:e FILE` runs `$VISUAL` / `$EDITOR` / `nvim` on it in MAIC's place. `:e` alone still edits the input.
* **At an approval prompt**, `e` opens the file being asked about the same way, and `d` (for `write_file`, `edit_file`, `multi_edit`, `apply_patch`) opens a new host tab with the file against a scratch buffer holding the proposed content, both `diffthis`, the scratch buffer read-only and wiped when closed. Answer in MAIC's window as usual.
* **User autocmds** fire in the host through `nvim_exec_autocmds("User", {pattern = ..., data = ...})`, as notifications (MAIC never waits on them). Every `data` has `session` (the session id).

  | Pattern | When | `data` |
  | :--- | :--- | :--- |
  | `MaicTurnStart` | a turn starts | `session`, `model` |
  | `MaicToolCall` | the model calls a tool | `session`, `tool`, `path` (absolute, `""` for none), `summary` |
  | `MaicApproval` | MAIC asks, then again with the answer | `session`, `tool`, `path`, `summary`, `reason`, `verdict` (`"pending"`, then `"yes"`, `"no"`, `"always"`, `"trip"`) |
  | `MaicFileWritten` | a tool changed a file (write, edit, patch, a move's two ends, a delete, a Lua tool's `maic.write`) | `session`, `tool`, `path` |
  | `MaicTurnEnd` | the turn is over | `session`, `model`, `tool_calls`, `seconds`, `interrupted` |

* **`:checktime`** runs in the host after every file a tool changed and after `:undo`, so an open buffer reloads (or asks, if it has unsaved changes).
* **The theme follows the host** (`follow_nvim_theme`, default `true`). On connect, MAIC registers a `ColorScheme` autocmd in the host (`nvim_create_autocmd`, in its own augroup) that `rpcnotify`s MAIC's channel; on connect and on every notification, MAIC reads the highlight groups with `nvim_get_hl(0, {name = ..., link = false})`, maps them with `theme_from_nvim` (the same table as `:theme nvim:NAME`, [settings.md](settings.md#from-neovim-colorschemes)) and applies them with `apply_theme` as a session-only theme named `nvim:<colors_name>`: nothing is written to the themes directory, and `style` entries in settings still win. `follow_nvim_theme = false` keeps `theme`; `:theme NAME` stops following for the session and `:nvim theme` resumes. No headless nvim is started for this; the host answers.

## Lua

The user's own Lua in an interactive session (the settings files as the session reads them, `:lua`, `:luafile`) gets a `maic.nvim` table while a host is connected, and none without one (nor in `maic -p` or `maic lua`, which never connect):

| Function | Runs in the host |
| :--- | :--- |
| `maic.nvim.exec(code, ...)` | `nvim_exec_lua(code, {...})`; returns what the chunk returns (`maic.nvim.exec("return vim.fn.getcwd()")`) |
| `maic.nvim.buffers()` | the listed file buffers: `{bufnr, path, modified, loaded}` |
| `maic.nvim.diagnostics(path)` | `vim.diagnostic.get()` for the buffer showing `path` (relative to the workspace), or every buffer without one: `{path, line, col, severity, message, source}`, 1-based |
| `maic.nvim.current()` | the editing window (not MAIC's terminal): `{path, cursor = {line, col}, selection = {from, to, text}}`, the selection being the last visual one in that buffer, by lines |

A failed call (an error in the host, no answer within 5 s, the host gone) is a Lua error saying so.

## The harness rules

Everything the agent does through the host is an action like any other ([harness.md](harness.md), layer 25):

* The model has one tool from the host, `diagnostics {path?}`, offered only while a host is connected. It is a Read action on the file, or on the workspace when `path` is left out (then only files inside the workspace are reported), judged exactly like `read_file`: a key or credential is never read, a path outside the workspace is asked about in manual and plan modes, every decision is in the transcript.
* Lua tools (the sandboxed ones the model calls) get `maic.nvim.diagnostics(path?)` and `maic.nvim.buffers()`, each authorised through the same step as `maic.read` as a read of the file or the workspace, answering only for files in the workspace. They do not get `exec` or `current`.
* Nothing lets the model run Lua in the host or change a host buffer. A model's edit is a file write through the harness with its preview, undo point and approval; the host sees the file change and reloads it on `:checktime`.
* The host's requests to MAIC are notifications only; a request (`rpcrequest`) is answered with an error, so nvim never blocks on MAIC.
* `maic nvim setup` is never run for the model: `run_shell` refuses it in every mode, whatever an allow list says.

## Tests

ctest `nvim` (`cli/tests/nvim_test.cpp`, skipped when nvim is not on PATH) starts `nvim --headless -u NONE --listen <socket> --cmd 'set rtp+=maic.nvim'` and checks: the ancestry check refuses that socket without the test variables (and with `MAIC_NVIM_TRUST_SOCKET` alone), accepts a socket whose listener is an ancestor, refuses a default name naming another pid; MAIC connects with both variables; `maic_send` and `maic_command` from the plugin arrive; `:e` drops a file into the host (checked with `nvim_buf_get_name` from a second client); the diff tab; `colorscheme habamax` in the host changes a role's colour as `nvim:habamax`; a User autocmd registered in the host sees `MaicToolCall`'s data; the `diagnostics` tool returns what `vim.diagnostic.set` put there, without asking inside the workspace in plan mode, asked as a read outside it, refused for a key; a Lua tool's `maic.nvim` reads are authorised as reads; the user's Lua has `maic.nvim.*`; the real `maic` binary started as a terminal job of that nvim (whose environment never had the test variables) passes the ancestry check on its own and connects as client `maic` with its pid; the host quitting. Then it runs the plugin's own tests, `nvim --headless -u NONE -l maic.nvim/tests/maic_test.lua` (snippet, diagnostics and quickfix formatting, the bracketed paste into a terminal job, one MAIC per tab, toggling, keymaps). `tests/test_tui.py` has the TUI cases: a bracketed paste lands in the input unsent, and a real `maic` inside a headless host shows `nvim` in the strip, takes `maic_send`, opens `:e` there, runs a `maic_command`, and stops a held turn on `maic_interrupt` (idle: the notice, the draft kept); `maic --bare` and `MAIC_BARE=1` show `bare` in the strip and refuse `:theme nvim:NAME`; a changed `lazy-lock.json` re-runs the keymap check once and reports only new collisions. ctest `nvim` also drives `<Esc>` through `nvim_input` into MAIC's terminal over a global `tnoremap <Esc>`, cancels a running turn from `:MaicInterrupt`, runs `maic nvim keymaps -u FILE` (exit 1 with a collision and its fix, 0 without), runs `maic nvim setup llama-vim -u FILE` against a stand-in for lazy.nvim defined in that init (the file written with the spec from models.md and loadable, a second run changing nothing, MAIC's own file updated with a diff, a file of that name MAIC did not write refused, llama.vim specified elsewhere refused, no import directory printing instructions, no lazy.nvim or no nvim a noop with exit 0, `--dry-run`, `--remove` removing only MAIC's file, no `--yes` off a terminal refused), and starts `maic --bare` and `MAIC_BARE=1 maic` as terminal jobs of the host to see that neither connects; `lazy_lock_test` covers the keymap record and which collisions are new; `harness_test` that `maic nvim setup` is denied as a tool call; `robustness_test` the `bare` setting and `MAIC_BARE`.
