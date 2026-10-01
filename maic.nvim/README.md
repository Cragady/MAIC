# maic.nvim

MAIC inside nvim. The plugin runs MAIC in a terminal and sends your buffers, selections, diagnostics and quickfix list into its input; MAIC, running inside nvim, connects back to it and uses it as its editor (files and diffs open here, the theme follows your colorscheme, User autocmds fire). The MAIC half and the rules it keeps are in [docs/nvim.md](../docs/nvim.md); in nvim, `:h maic`.

Needs nvim 0.10 or later and `maic` on `PATH` (or `cmd` set to it).

## Install

With lazy.nvim, from the MAIC checkout:

```lua
{ dir = "~/dev2/MAIC/maic.nvim", opts = {} }
```

`opts` makes lazy.nvim call `require("maic").setup(opts)`, which sets the keymaps; leave it out for the commands alone. The plugin is small enough to load at start; to load it on first use instead, add `cmd = { "Maic", "MaicToggle", "MaicSend", "MaicDiagnostics", "MaicQuickfix" }`.

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
| `:Maic [args]` | Opens MAIC in a terminal in the configured layout, or focuses the one this tab has (showing its window again if it was hidden). One MAIC per tab, reused; `args` go to `maic` when it starts (`:Maic -c`, `:Maic --model qwen-9b`). In the `tab` layout MAIC has a tab of its own and `:Maic` goes there. |
| `:MaicToggle` | Hides this tab's MAIC window (MAIC keeps running) or shows it; starts one if there is none. |
| `:[range]MaicSend` | With a range (`:'<,'>MaicSend`, `:%MaicSend`, `:10,20MaicSend`): those lines as a fenced snippet headed with the path and line numbers. Without a range: the buffer's path, for the agent to read itself. |
| `:MaicDiagnostics[!]` | This buffer's LSP diagnostics as `path:line:col: severity: message [source]` lines; with `!`, every buffer's. |
| `:MaicQuickfix` | The quickfix list, `path:line:col: type: text` (an item with no location is its text). |

What is sent lands in MAIC's input; nothing is sent to the model until you send it there. When MAIC is connected to this nvim the text goes over its channel (`rpcnotify(chan, "maic_send", text)`); when it is not, it is typed into the terminal as a bracketed paste, which MAIC takes whole in any mode.

A snippet looks like this:

````
`src/agent.cpp` lines 120-124:
```cpp
...
```
````

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
  open = "vsplit",       -- "split", "vsplit", "float" or "tab"
  size = nil,            -- split: rows (15), vsplit: columns (40% of the screen), float: a fraction of the editor (0.8)
  prefix = "<leader>m",  -- a default key that starts with <leader>m moves under it
  keymaps = {            -- name = key; false drops one, keymaps = false drops all
    open = "<leader>mm", toggle = "<leader>mt", send = "<leader>ms", send_selection = "<leader>ms",
    send_buffer = "<leader>mb", diagnostics = "<leader>md", workspace_diagnostics = "<leader>mD", quickfix = "<leader>mq",
  },
}
```

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

maic.nvim never overwrites a mapping. Before it sets a default key it checks `maparg()` and `mapcheck()` in that mode; when another mapping holds the key or shares a prefix with it, the key is skipped, and every skipped key is reported once per session in one warning naming the key, the mode and what holds it (its description and the script and line that set it). A key you name in `keymaps` is set anyway, since you chose it, and reported as shadowing what held it.

## Lua

`require("maic")` also has `send_text(text)` (returns `"rpc"`, `"paste"` or nil), `command(":cmd")` (runs a command in a connected MAIC as if typed), `channel()`, `open(args)`, `toggle()`, and the formatters `format_snippet`, `format_diagnostics`, `format_quickfix`.

## Events from MAIC

A connected MAIC fires User autocmds here, each with `data.session` and what applies:

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

MAIC's `ctest -R nvim` runs it too, after the tests of MAIC's half against a headless nvim.
