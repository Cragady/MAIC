# maid.nvim

MAID inside nvim. The plugin makes nvim MAID's interface (the conversation and the input as nvim buffers, MAID's engine as their job), or runs MAID's own TUI in a terminal, and sends your buffers, selections, diagnostics and quickfix list into its input; MAID, running inside nvim, connects back to it and uses it as its editor (files and diffs open here, the theme follows your colorscheme, User autocmds fire). The MAID half and the rules it keeps are in [docs/nvim.md](../docs/nvim.md); in nvim, `:h maid`.

Needs nvim 0.10 or later and `maid` on `PATH` (or `cmd` set to it).

## Install

With lazy.nvim, from the MAID checkout:

```lua
{ dir = "~/dev2/MAID/maid.nvim", opts = {} }
```

`opts` makes lazy.nvim call `require("maid").setup(opts)`, which sets the keymaps; leave it out for the commands alone. The plugin is small enough to load at start; to load it on first use instead, add `cmd = { "Maid", "MaidTerminal", "MaidToggle", "MaidSend", "MaidDiagnostics", "MaidQuickfix", "MaidInterrupt", "MaidSteer" }`.

Without a plugin manager, as a package:

```sh
mkdir -p ~/.local/share/nvim/site/pack/maid/opt
ln -s ~/dev2/MAID/maid.nvim ~/.local/share/nvim/site/pack/maid/opt/maid.nvim
```

```lua
vim.cmd.packadd("maid.nvim")
require("maid").setup({})
```

then `:helptags ALL` once for `:h maid`. `plugin/maid.lua` only defines the commands; the rest loads on first use. Keymaps come from `setup()`, so without it there are none.

## Commands

| Command | Does |
| :--- | :--- |
| `:Maid [args]` | Opens MAID in the configured layout, or focuses the one this tab has (showing its windows again if they were hidden): the interface (below), or MAID's TUI in a terminal with `ui = "terminal"` or arguments only the TUI takes (`-p`, `-i`, `-c`, `-r`, `--context`, `--image`, `--fork-at`, `--bare`, a prompt). One MAID per tab, reused; `args` go to `maid` when it starts (`:Maid --model qwen-9b`). In the `tab` layout MAID has a tab of its own and `:Maid` goes there. |
| `:MaidTerminal [args]` | MAID's own TUI in a terminal, whatever `ui` says. |
| `:MaidSteer ACTION [NOTE]` | Steers the interface's turn: `steer`, `drop`, `further`, `interrupt` (pause), `keep`, `halt`. |
| `:MaidToggle` | Hides this tab's MAID windows (MAID keeps running) or shows them; starts one if there is none. |
| `:[range]MaidSend` | With a range (`:'<,'>MaidSend`, `:%MaidSend`, `:10,20MaidSend`): those lines as a fenced snippet headed with the path and line numbers. Without a range: the buffer's path, for the agent to read itself. |
| `:MaidDiagnostics[!]` | This buffer's LSP diagnostics as `path:line:col: severity: message [source]` lines; with `!`, every buffer's. |
| `:MaidQuickfix` | The quickfix list, `path:line:col: type: text` (an item with no location is its text). |
| `:MaidInterrupt` | Stops MAID's running turn as its first Ctrl-C does, from any window: in the interface a cancel to the engine; over MAID's channel (`rpcnotify(chan, "maid_interrupt")`) when it is connected, where an idle MAID only says so; else Ctrl-C typed into this tab's MAID terminal. |

What is sent lands in MAID's input (in the interface, its input buffer); nothing is sent to the model until you send it there. When MAID is connected to this nvim the text goes over its channel (`rpcnotify(chan, "maid_send", text)`); when it is not, it is typed into the terminal as a bracketed paste, which MAID takes whole in any mode.

A snippet looks like this:

````
`src/agent.cpp` lines 120-124:
```cpp
...
```
````

## The interface

With `ui = "nvim"` (the default) `:Maid` starts `maid --rpc` as the tab's job and shows the conversation (filetype `maid`, markdown, written only by the plugin) above the input (filetype `maid-input`, an ordinary buffer). `maid --ui nvim` from the shell starts nvim with it.

* **Sending.** `<CR>` in normal mode or `<M-CR>` in insert mode sends the input. `/cmd` runs a MAID command (`/mode plan`), `!cmd` a shell command; while a turn runs, a message goes to it at its next step.
* **The conversation.** `❯` your messages, the replies as they stream, `⏺` tool calls with their output under them (folded once done when `fold_output` lines or longer), `↯` steering, and a footer per turn; the winbar shows the model, mode and state.
* **Floats.** Approvals (`y`, `n`, `N` with a reason, `a` always, `t` trip, `e` open the file, `d` diff a write), questions (a digit picks, `<CR>` types), and the pause menu.
* **Steering.** `<C-c>` cancels the turn, `<C-s>` pauses it (the menu: `<C-q>` resume, `s` steer, `d` drop, `f` further, `k` keep, `h` halt), `<C-q>` resumes; `:MaidSteer` does any of them.
* **History.** A resumed session shows its last three exchanges; the top line, or `:MaidOlder`, loads the ten before them.
* **Sessions.** `:MaidNew`, `:MaidSwitch` (alone, a switcher), `:MaidFork`, `:MaidBg`, `:MaidPark` and `:MaidStop`: each session has a conversation buffer of its own under the one input, and a background one keeps working and writing its buffer.

The User autocmds below fire from the plugin as it renders. More: `:h maid-interface`, [docs/nvim.md](../docs/nvim.md#nvim-as-maids-interface).

## Options

Every option and its default is in one table, `require("maid").defaults` (`:h maid-defaults`). `setup(opts)` deep-merges `opts` over it with `vim.tbl_deep_extend("force", ...)`, which is what lazy.nvim's `opts` passes, so name only what you change:

```lua
{ dir = "~/dev2/MAID/maid.nvim", opts = { open = "float", keymaps = { quickfix = false } } }
```

The table as shipped:

```lua
{
  cmd = "maid",          -- the program: a name on PATH, a path, or a list such as { "maid", "--harness", "dumb" }
  args = {},             -- arguments for every start, before those given to :Maid
  ui = "nvim",           -- "nvim": the interface; "terminal": MAID's TUI in a terminal
  open = "vsplit",       -- "split", "vsplit", "float" or "tab"
  size = nil,            -- split: rows (15), vsplit: columns (40% of the screen), float: a fraction of the editor (0.8)
  prefix = "<leader>m",  -- a default key that starts with <leader>m moves under it
  keymaps = {            -- name = key; false drops one, keymaps = false drops all
    open = "<leader>mm", toggle = "<leader>mt", send = "<leader>ms", send_selection = "<leader>ms",
    send_buffer = "<leader>mb", diagnostics = "<leader>md", workspace_diagnostics = "<leader>mD", quickfix = "<leader>mq",
    interrupt = "<leader>mc",
  },
  -- buffer-local in MAID's own buffers only: interrupt everywhere, pause and resume in the interface's two, send in its input
  buffer_keymaps = { interrupt = "<C-c>", pause = "<C-s>", resume = "<C-q>", send = "<CR>", send_insert = "<M-CR>" },
  input_height = 6,      -- the interface's input rows
  fold_output = 4,       -- a tool output this long or longer is folded once done; false: never
  terminal_escape = "<C-\\><C-n>",            -- leaves terminal mode in MAID's terminal; another key is mapped there
  terminal_passthrough = { ["<Esc>"] = true, ["<C-c>"] = true },  -- keys that reach MAID in its terminal over a global terminal-mode mapping
  filetypes = { conversation = "maid", terminal = "maid", input = "maid-input" },  -- MAID's buffers, for plugins to include or exclude
}
```

**Esc in MAID's terminal.** MAID's input is vim-like and needs `<Esc>` for normal mode. With the common global `tnoremap <Esc> <C-\><C-n>`, `<Esc>` would leave terminal mode instead, so maid.nvim maps `<Esc>` to itself buffer-local (noremap, nowait) in MAID's terminal only: MAID gets it, and the global mapping keeps working in every other terminal. In MAID's terminal, `<C-\><C-n>` (nvim's own) leaves terminal mode, or `terminal_escape` names another key. MAID's terminal has the filetype `maid` before its job starts, so a `TermOpen` autocmd can skip it.

## Keymaps

Set by `setup()` unless `keymaps = false`; the first column is the name in the `keymaps` table. A second `setup()` removes the keys the first one set before it sets its own.

| Name | Keys | Mode | Does |
| :--- | :--- | :--- | :--- |
| `open` | `<leader>mm` | normal | `:Maid` |
| `toggle` | `<leader>mt` | normal | `:MaidToggle` |
| `send` | `<leader>ms` | normal | `:MaidSend` (the path) |
| `send_selection` | `<leader>ms` | visual | `:'<,'>MaidSend` |
| `send_buffer` | `<leader>mb` | normal | `:%MaidSend` |
| `diagnostics` | `<leader>md` | normal | `:MaidDiagnostics` |
| `workspace_diagnostics` | `<leader>mD` | normal | `:MaidDiagnostics!` |
| `quickfix` | `<leader>mq` | normal | `:MaidQuickfix` |
| `interrupt` | `<leader>mc` | normal | `:MaidInterrupt` |

In MAID's own buffers only, buffer-local: `<C-c>` in normal mode is `:MaidInterrupt` (`buffer_keymaps.interrupt`), and in terminal mode it is passed through to MAID, whose Ctrl-C interrupts. `<C-c>` is never mapped globally. The interface's conversation and input also get `<C-s>` (`pause`) and `<C-q>` (`resume`), in insert mode too in the input, and the input `<CR>` (`send`, normal mode) and `<M-CR>` (`send_insert`).

maid.nvim never overwrites a mapping. Before it sets a default key it checks `maparg()` and `mapcheck()` in that mode; when another mapping holds the key or shares a prefix with it, the key is skipped, and every skipped key is reported once per session in one warning naming the key, the mode and what holds it (its description and the script and line that set it). A key you name in `keymaps` is set anyway, since you chose it, and reported as shadowing what held it.

## `:checkhealth maid`

Lists every key maid.nvim sets (global, and buffer-local in MAID's own buffers), the keys MAID's own input needs in its terminal (`<Esc>`, `<C-w>`, `<C-p>`, `<S-Tab>`, `<C-z>`, `<C-c>`, `<M-CR>`) and, when llama.vim is installed (`g:llama_config` set, or its `autoload/llama.vim` on the runtimepath), every key llama.vim uses, against your mappings in the modes that matter, global and buffer-local: it opens a plain file buffer, scratch `maid-input` and `maid` buffers and a `maid` terminal in a tab of its own to collect what your FileType, BufEnter and TermOpen autocmds map there, then closes them. OK when a key is free, WARN when maid.nvim skipped it, a key you named shadows something, two mappings share a prefix (one waits `timeoutlen`) or an insert-mode key starts with a key that types, ERROR when a key MAID needs never reaches it in its terminal; every WARN and ERROR names the fix. `maid nvim keymaps` runs the same check from the shell against your config.

## Lua

`require("maid")` also has `interrupt()` (returns `"engine"`, `"rpc"`, `"key"` or nil), `send_text(text)` (returns `"input"`, `"rpc"`, `"paste"` or nil), `command(":cmd")` (runs a command in MAID as if typed), `channel()`, `open(args)`, `toggle()`, and the formatters `format_snippet`, `format_diagnostics`, `format_quickfix`. `require("maid.ui").state()` is this tab's interface as plain values (session, response, paused, mode, model, buffers), for a statusline.

## Events from MAID

A connected MAID fires User autocmds here, and the interface fires the same ones, each with `data.session` and what applies:

| Pattern | `data` |
| :--- | :--- |
| `MaidTurnStart` | `model` |
| `MaidToolCall` | `tool`, `path` (absolute, `""` for none), `summary` |
| `MaidApproval` | `tool`, `path`, `summary`, `reason`, `verdict`: `"pending"` when MAID asks, then `"yes"`, `"no"`, `"always"` or `"trip"` |
| `MaidFileWritten` | `tool`, `path` |
| `MaidTurnEnd` | `model`, `tool_calls`, `seconds`, `interrupted` |

```lua
vim.api.nvim_create_autocmd("User", {
  pattern = "MaidApproval",
  callback = function(ev)
    if ev.data.verdict == "pending" then
      vim.notify("MAID asks: " .. ev.data.summary, vim.log.levels.WARN)
    end
  end,
})
```

MAID also runs `:checktime` after it changes a file, so open buffers reload.

## From MAID's side

Inside this nvim, MAID connects to `$NVIM` only when the socket is yours and the nvim behind it is one of MAID's own parent processes. Then `:e FILE` and `e` at an approval open the file in your editing window, `d` at a write's approval opens the proposed change as a diff in a new tab, the theme follows your colorscheme (`follow_nvim_theme` in MAID's settings), and MAID's own Lua gets `maid.nvim.exec`, `buffers`, `diagnostics` and `current`. The model gets only a `diagnostics` tool, judged as a read; it can never run Lua here or change a buffer. Details: [docs/nvim.md](../docs/nvim.md).

## Tests

```sh
nvim --headless -u NONE -i NONE -n -l maid.nvim/tests/maid_test.lua
```

MAID's `ctest -R nvim` runs it too, after the tests of MAID's half against a headless nvim. `tests/ui_test.lua` drives the interface against a real `maid --rpc` and needs a fake model, so MAID's `tests/cli_smoke.py` runs it. The Lua tests cover the formatters, the bracketed paste and Ctrl-C into an unconnected terminal, one MAID per tab, the defaults table and its deep merge, the never-overwrite rule and its one warning, the `<Esc>`, `<C-c>` and `terminal_escape` maps of MAID's terminal (and a `TermOpen` autocmd skipping it by filetype), and the keymap check behind `:checkhealth maid`.

`maid --bare` (or `MAID_BARE=1`) never connects to this nvim: `:MaidSend` then uses the bracketed paste and `:MaidInterrupt` types Ctrl-C.
