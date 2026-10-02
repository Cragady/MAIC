# maic.nvim

MAIC inside nvim. The plugin makes nvim MAIC's interface (the conversation and the input as nvim buffers, MAIC's engine as their job), or runs MAIC's own TUI in a terminal, and sends your buffers, selections, diagnostics and quickfix list into its input; MAIC, running inside nvim, connects back to it and uses it as its editor (files and diffs open here, the theme follows your colorscheme, User autocmds fire). The MAIC half and the rules it keeps are in [docs/nvim.md](../docs/nvim.md); in nvim, `:h maic`.

Needs nvim 0.10 or later and `maic` on `PATH` (or `cmd` set to it).

## Install

With lazy.nvim, from the MAIC checkout:

```lua
{ dir = "~/dev2/MAIC/maic.nvim", opts = {} }
```

`opts` makes lazy.nvim call `require("maic").setup(opts)`, which sets the keymaps; leave it out for the commands alone. The plugin is small enough to load at start; to load it on first use instead, add `cmd = { "Maic", "MaicTerminal", "MaicToggle", "MaicSend", "MaicDiagnostics", "MaicQuickfix", "MaicInterrupt", "MaicSteer" }`.

Without a plugin manager, as a package:

```sh
mkdir -p ~/.local/share/nvim/site/pack/maic/opt
ln -s ~/dev2/MAIC/maic.nvim ~/.local/share/nvim/site/pack/maic/opt/maic.nvim
```

```lua
vim.cmd.packadd("maic.nvim")
require("maic").setup({})
```

then `:helptags ALL` once for `:h maic`. `plugin/maic.lua` only defines the commands; the rest loads on first use. Keymaps come from `setup()`, so without it there are none.

## Commands

| Command | Does |
| :--- | :--- |
| `:Maic [args]` | Opens MAIC in the configured layout, or focuses the one this tab has (showing its windows again if they were hidden): the interface (below), or MAIC's TUI in a terminal with `ui = "terminal"` or arguments only the TUI takes (`-p`, `-i`, `-c`, `-r`, `--context`, `--image`, `--fork-at`, `--bare`, a prompt). One MAIC per tab, reused; `args` go to `maic` when it starts (`:Maic --model qwen-9b`). In the `tab` layout MAIC has a tab of its own and `:Maic` goes there. |
| `:MaicTerminal [args]` | MAIC's own TUI in a terminal, whatever `ui` says. |
| `:MaicSteer ACTION [NOTE]` | Steers the interface's turn: `steer`, `drop`, `further`, `interrupt` (pause), `keep`, `halt`. |
| `:MaicToggle` | Hides this tab's MAIC windows (MAIC keeps running) or shows them; starts one if there is none. |
| `:[range]MaicSend` | With a range (`:'<,'>MaicSend`, `:%MaicSend`, `:10,20MaicSend`): those lines as a fenced snippet headed with the path and line numbers. Without a range: the buffer's path, for the agent to read itself. |
| `:MaicDiagnostics[!]` | This buffer's LSP diagnostics as `path:line:col: severity: message [source]` lines; with `!`, every buffer's. |
| `:MaicQuickfix` | The quickfix list, `path:line:col: type: text` (an item with no location is its text). |
| `:MaicInterrupt` | Stops MAIC's running turn as its first Ctrl-C does, from any window: in the interface a cancel to the engine; over MAIC's channel (`rpcnotify(chan, "maic_interrupt")`) when it is connected, where an idle MAIC only says so; else Ctrl-C typed into this tab's MAIC terminal. |

What is sent lands in MAIC's input (in the interface, its input buffer); nothing is sent to the model until you send it there. When MAIC is connected to this nvim the text goes over its channel (`rpcnotify(chan, "maic_send", text)`); when it is not, it is typed into the terminal as a bracketed paste, which MAIC takes whole in any mode.

A snippet looks like this:

````
`src/agent.cpp` lines 120-124:
```cpp
...
```
````

## The interface

With `ui = "nvim"` (the default) `:Maic` starts `maic --rpc` as the tab's job and shows the conversation (filetype `maic`, markdown, written only by the plugin) above the input (filetype `maic-input`, an ordinary buffer). `maic --ui nvim` from the shell starts nvim with it.

* **Sending.** `<CR>` in normal mode or `<M-CR>` in insert mode sends the input. `/cmd` runs a MAIC command (`/mode plan`), `!cmd` a shell command; while a turn runs, a message goes to it at its next step.
* **The conversation.** `❯` your messages, the replies as they stream, `⏺` tool calls with their output under them (folded once done when `fold_output` lines or longer), `↯` steering, and a footer per turn; the winbar shows the model, mode and state.
* **Floats.** Approvals (`y`, `n`, `N` with a reason, `a` always, `t` trip, `e` open the file, `d` diff a write), questions (a digit picks, `<CR>` types), and the pause menu.
* **Steering.** `<C-c>` cancels the turn, `<C-s>` pauses it (the menu: `<C-q>` resume, `s` steer, `d` drop, `f` further, `k` keep, `h` halt), `<C-q>` resumes; `:MaicSteer` does any of them.
* **History.** A resumed session shows its last three exchanges; the top line, or `:MaicOlder`, loads the ten before them.

The User autocmds below fire from the plugin as it renders. More: `:h maic-interface`, [docs/nvim.md](../docs/nvim.md#nvim-as-maics-interface).

## Options

Every option and its default is in one table, `require("maic").defaults` (`:h maic-defaults`). `setup(opts)` deep-merges `opts` over it with `vim.tbl_deep_extend("force", ...)`, which is what lazy.nvim's `opts` passes, so name only what you change:

```lua
{ dir = "~/dev2/MAIC/maic.nvim", opts = { open = "float", keymaps = { quickfix = false } } }
```

The table as shipped:

```lua
{
  cmd = "maic",          -- the program: a name on PATH, a path, or a list such as { "maic", "--harness", "dumb" }
  args = {},             -- arguments for every start, before those given to :Maic
  ui = "nvim",           -- "nvim": the interface; "terminal": MAIC's TUI in a terminal
  open = "vsplit",       -- "split", "vsplit", "float" or "tab"
  size = nil,            -- split: rows (15), vsplit: columns (40% of the screen), float: a fraction of the editor (0.8)
  prefix = "<leader>m",  -- a default key that starts with <leader>m moves under it
  keymaps = {            -- name = key; false drops one, keymaps = false drops all
    open = "<leader>mm", toggle = "<leader>mt", send = "<leader>ms", send_selection = "<leader>ms",
    send_buffer = "<leader>mb", diagnostics = "<leader>md", workspace_diagnostics = "<leader>mD", quickfix = "<leader>mq",
    interrupt = "<leader>mc",
  },
  -- buffer-local in MAIC's own buffers only: interrupt everywhere, pause and resume in the interface's two, send in its input
  buffer_keymaps = { interrupt = "<C-c>", pause = "<C-s>", resume = "<C-q>", send = "<CR>", send_insert = "<M-CR>" },
  input_height = 6,      -- the interface's input rows
  fold_output = 4,       -- a tool output this long or longer is folded once done; false: never
  terminal_escape = "<C-\\><C-n>",            -- leaves terminal mode in MAIC's terminal; another key is mapped there
  terminal_passthrough = { ["<Esc>"] = true, ["<C-c>"] = true },  -- keys that reach MAIC in its terminal over a global terminal-mode mapping
  filetypes = { conversation = "maic", terminal = "maic", input = "maic-input" },  -- MAIC's buffers, for plugins to include or exclude
}
```

**Esc in MAIC's terminal.** MAIC's input is vim-like and needs `<Esc>` for normal mode. With the common global `tnoremap <Esc> <C-\><C-n>`, `<Esc>` would leave terminal mode instead, so maic.nvim maps `<Esc>` to itself buffer-local (noremap, nowait) in MAIC's terminal only: MAIC gets it, and the global mapping keeps working in every other terminal. In MAIC's terminal, `<C-\><C-n>` (nvim's own) leaves terminal mode, or `terminal_escape` names another key. MAIC's terminal has the filetype `maic` before its job starts, so a `TermOpen` autocmd can skip it.

## Keymaps

Set by `setup()` unless `keymaps = false`; the first column is the name in the `keymaps` table. A second `setup()` removes the keys the first one set before it sets its own.

| Name | Keys | Mode | Does |
| :--- | :--- | :--- | :--- |
| `open` | `<leader>mm` | normal | `:Maic` |
| `toggle` | `<leader>mt` | normal | `:MaicToggle` |
| `send` | `<leader>ms` | normal | `:MaicSend` (the path) |
| `send_selection` | `<leader>ms` | visual | `:'<,'>MaicSend` |
| `send_buffer` | `<leader>mb` | normal | `:%MaicSend` |
| `diagnostics` | `<leader>md` | normal | `:MaicDiagnostics` |
| `workspace_diagnostics` | `<leader>mD` | normal | `:MaicDiagnostics!` |
| `quickfix` | `<leader>mq` | normal | `:MaicQuickfix` |
| `interrupt` | `<leader>mc` | normal | `:MaicInterrupt` |

In MAIC's own buffers only, buffer-local: `<C-c>` in normal mode is `:MaicInterrupt` (`buffer_keymaps.interrupt`), and in terminal mode it is passed through to MAIC, whose Ctrl-C interrupts. `<C-c>` is never mapped globally. The interface's conversation and input also get `<C-s>` (`pause`) and `<C-q>` (`resume`), in insert mode too in the input, and the input `<CR>` (`send`, normal mode) and `<M-CR>` (`send_insert`).

maic.nvim never overwrites a mapping. Before it sets a default key it checks `maparg()` and `mapcheck()` in that mode; when another mapping holds the key or shares a prefix with it, the key is skipped, and every skipped key is reported once per session in one warning naming the key, the mode and what holds it (its description and the script and line that set it). A key you name in `keymaps` is set anyway, since you chose it, and reported as shadowing what held it.

## `:checkhealth maic`

Lists every key maic.nvim sets (global, and buffer-local in MAIC's own buffers), the keys MAIC's own input needs in its terminal (`<Esc>`, `<C-w>`, `<C-p>`, `<S-Tab>`, `<C-z>`, `<C-c>`, `<M-CR>`) and, when llama.vim is installed (`g:llama_config` set, or its `autoload/llama.vim` on the runtimepath), every key llama.vim uses, against your mappings in the modes that matter, global and buffer-local: it opens a plain file buffer, scratch `maic-input` and `maic` buffers and a `maic` terminal in a tab of its own to collect what your FileType, BufEnter and TermOpen autocmds map there, then closes them. OK when a key is free, WARN when maic.nvim skipped it, a key you named shadows something, two mappings share a prefix (one waits `timeoutlen`) or an insert-mode key starts with a key that types, ERROR when a key MAIC needs never reaches it in its terminal; every WARN and ERROR names the fix. `maic nvim keymaps` runs the same check from the shell against your config.

## Lua

`require("maic")` also has `interrupt()` (returns `"engine"`, `"rpc"`, `"key"` or nil), `send_text(text)` (returns `"input"`, `"rpc"`, `"paste"` or nil), `command(":cmd")` (runs a command in MAIC as if typed), `channel()`, `open(args)`, `toggle()`, and the formatters `format_snippet`, `format_diagnostics`, `format_quickfix`. `require("maic.ui").state()` is this tab's interface as plain values (session, response, paused, mode, model, buffers), for a statusline.

## Events from MAIC

A connected MAIC fires User autocmds here, and the interface fires the same ones, each with `data.session` and what applies:

| Pattern | `data` |
| :--- | :--- |
| `MaicTurnStart` | `model` |
| `MaicToolCall` | `tool`, `path` (absolute, `""` for none), `summary` |
| `MaicApproval` | `tool`, `path`, `summary`, `reason`, `verdict`: `"pending"` when MAIC asks, then `"yes"`, `"no"`, `"always"` or `"trip"` |
| `MaicFileWritten` | `tool`, `path` |
| `MaicTurnEnd` | `model`, `tool_calls`, `seconds`, `interrupted` |

```lua
vim.api.nvim_create_autocmd("User", {
  pattern = "MaicApproval",
  callback = function(ev)
    if ev.data.verdict == "pending" then
      vim.notify("MAIC asks: " .. ev.data.summary, vim.log.levels.WARN)
    end
  end,
})
```

MAIC also runs `:checktime` after it changes a file, so open buffers reload.

## From MAIC's side

Inside this nvim, MAIC connects to `$NVIM` only when the socket is yours and the nvim behind it is one of MAIC's own parent processes. Then `:e FILE` and `e` at an approval open the file in your editing window, `d` at a write's approval opens the proposed change as a diff in a new tab, the theme follows your colorscheme (`follow_nvim_theme` in MAIC's settings), and MAIC's own Lua gets `maic.nvim.exec`, `buffers`, `diagnostics` and `current`. The model gets only a `diagnostics` tool, judged as a read; it can never run Lua here or change a buffer. Details: [docs/nvim.md](../docs/nvim.md).

## Tests

```sh
nvim --headless -u NONE -i NONE -n -l maic.nvim/tests/maic_test.lua
```

MAIC's `ctest -R nvim` runs it too, after the tests of MAIC's half against a headless nvim. `tests/ui_test.lua` drives the interface against a real `maic --rpc` and needs a fake model, so MAIC's `tests/cli_smoke.py` runs it. The Lua tests cover the formatters, the bracketed paste and Ctrl-C into an unconnected terminal, one MAIC per tab, the defaults table and its deep merge, the never-overwrite rule and its one warning, the `<Esc>`, `<C-c>` and `terminal_escape` maps of MAIC's terminal (and a `TermOpen` autocmd skipping it by filetype), and the keymap check behind `:checkhealth maic`.

`maic --bare` (or `MAIC_BARE=1`) never connects to this nvim: `:MaicSend` then uses the bracketed paste and `:MaicInterrupt` types Ctrl-C.
