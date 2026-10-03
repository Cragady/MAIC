-- maid.nvim: nvim as MAID's interface (lua/maid/ui.lua), or MAID's own TUI in an nvim terminal, and nvim's buffers,
-- selections, diagnostics and quickfix list sent into its input. MAID's side of the connection (the host, User
-- autocmds, :e, the theme) is in MAID: docs/nvim.md.
local M = {}

-- Every option and its default, in one table (:h maid-defaults). setup(opts) deep-merges opts over it, which is
-- what lazy.nvim's `opts` does.
M.defaults = {
  cmd = "maid", -- the program: a name on PATH, a path, or a list (program and arguments)
  args = {}, -- arguments for every start, before the ones given to :Maid
  -- "nvim": the conversation and the input as nvim buffers, the engine (`maid --rpc`) as the job; "terminal": MAID's
  -- own TUI in a terminal (also what :Maid falls back to for arguments only the TUI takes, and :MaidTerminal)
  ui = "nvim",
  open = "vsplit", -- "split", "vsplit", "float" or "tab"
  size = nil, -- split: rows, vsplit: columns, float: a fraction of the editor (0.8); nil: a default for the layout
  prefix = "<leader>m", -- the default keys below that start with <leader>m move under it
  -- name = key; false drops one, `keymaps = false` drops them all
  keymaps = {
    open = "<leader>mm",
    toggle = "<leader>mt",
    send = "<leader>ms",
    send_selection = "<leader>ms",
    send_buffer = "<leader>mb",
    diagnostics = "<leader>md",
    workspace_diagnostics = "<leader>mD",
    quickfix = "<leader>mq",
    interrupt = "<leader>mc",
  },
  -- name = key, buffer-local in MAID's own buffers only (<C-c> is never mapped globally): interrupt in every one,
  -- pause and resume in the conversation and the input (insert mode too in the input), send in the input
  buffer_keymaps = { interrupt = "<C-c>", pause = "<C-s>", resume = "<C-q>", send = "<CR>", send_insert = "<M-CR>" },
  input_height = 6, -- the input's rows under the conversation
  fold_output = 4, -- a tool's output of at least this many lines is folded (closed) once it is done; false: never
  -- Leaves terminal mode in MAID's terminal. nvim's own <C-\><C-n> needs no mapping; another key is mapped there.
  terminal_escape = "<C-\\><C-n>",
  -- Keys that reach MAID in its terminal even when a global terminal-mode mapping holds them (buffer-local, so
  -- the global mapping keeps working everywhere else). key = true; false drops one.
  terminal_passthrough = { ["<Esc>"] = true, ["<C-c>"] = true },
  filetypes = { conversation = "maid", terminal = "maid", input = "maid-input" }, -- MAID's buffers, for plugins to include or exclude
}

M.config = vim.deepcopy(M.defaults)

-- MAID's terminal per tab page: { buf, job }. In the "tab" layout the tab is MAID's own.
local terms = {}

local function running(t)
  return t and vim.api.nvim_buf_is_valid(t.buf) and vim.fn.jobwait({ t.job }, 0)[1] == -1
end

-- This tab's MAID; in the "tab" layout, the MAID tab wherever it is.
local function term_here()
  local tab = vim.api.nvim_get_current_tabpage()
  if running(terms[tab]) then return terms[tab], tab end
  if M.config.open == "tab" then
    for t, term in pairs(terms) do
      if vim.api.nvim_tabpage_is_valid(t) and running(term) then return term, t end
    end
  end
end

local function window_of(buf, tab)
  for _, w in ipairs(vim.api.nvim_tabpage_list_wins(tab or 0)) do
    if vim.api.nvim_win_get_buf(w) == buf then return w end
  end
end

-- A window for `buf` (nil: a new empty buffer) in the configured layout; returns the window. The interface uses it too.
local function make_window(buf)
  local c, size = M.config, M.config.size
  if c.open == "float" then
    local frac = (size and size > 0 and size <= 1) and size or 0.8
    local width, height = math.floor(vim.o.columns * frac), math.floor((vim.o.lines - 2) * frac)
    buf = buf or vim.api.nvim_create_buf(false, true)
    return vim.api.nvim_open_win(buf, true, {
      relative = "editor", width = width, height = height, border = "rounded",
      row = math.floor((vim.o.lines - 2 - height) / 2), col = math.floor((vim.o.columns - width) / 2),
    })
  end
  if c.open == "tab" then
    vim.cmd("tabnew")
  elseif c.open == "split" then
    vim.cmd("botright " .. (size or 15) .. "new")
  else
    vim.cmd("botright " .. (size or math.floor(vim.o.columns * 0.4)) .. "vnew")
  end
  local win = vim.api.nvim_get_current_win()
  if buf then
    local empty = vim.api.nvim_get_current_buf()
    vim.api.nvim_win_set_buf(win, buf)
    if empty ~= buf then pcall(vim.api.nvim_buf_delete, empty, { force = true }) end
  end
  return win
end

M.make_window = make_window

local function command(extra)
  local cmd = type(M.config.cmd) == "table" and vim.deepcopy(M.config.cmd) or { M.config.cmd }
  vim.list_extend(cmd, M.config.args or {})
  vim.list_extend(cmd, extra or {})
  return cmd
end

local map -- below: sets a keymap unless another mapping holds its key (M.map, for the interface's buffers too)

local function start(extra)
  make_window(nil)
  local buf = vim.api.nvim_get_current_buf()
  local tab = vim.api.nvim_get_current_tabpage()
  -- Before the job, so a TermOpen autocmd can tell MAID's terminal by its filetype.
  if M.config.filetypes and M.config.filetypes.terminal then vim.bo[buf].filetype = M.config.filetypes.terminal end
  local opts = { on_exit = function() if terms[tab] and terms[tab].buf == buf then terms[tab] = nil end end }
  local job
  if vim.fn.has("nvim-0.11") == 1 then
    opts.term = true
    job = vim.fn.jobstart(command(extra), opts)
  else
    job = vim.fn.termopen(command(extra), opts)
  end
  if job <= 0 then
    vim.notify("maid.nvim: cannot start " .. table.concat(command(extra), " "), vim.log.levels.ERROR)
    return
  end
  vim.bo[buf].bufhidden = "hide"
  terms[tab] = { buf = buf, job = job }
  if vim.g.maid_keymap_check ~= 1 then
    for _, k in ipairs(M.buffer_planned()) do map(k, buf) end
  end
  vim.cmd("startinsert")
end

-- Arguments `maid --rpc` refuses (sessions, prompts, files and images come over the protocol), and the agent's flags
-- that take a value. :Maid with any of the first runs MAID's TUI in a terminal instead.
local tui_only = { ["-p"] = true, ["--print"] = true, ["-i"] = true, ["--interactive"] = true, ["-c"] = true, ["--continue"] = true,
  ["-r"] = true, ["--resume"] = true, ["-C"] = true, ["--context"] = true, ["-I"] = true, ["--image"] = true, ["--bare"] = true, ["--fork-at"] = true }
local valued = { ["-m"] = true, ["--model"] = true, ["--mode"] = true, ["-S"] = true, ["--system"] = true, ["--prefix"] = true, ["--prefill"] = true,
  ["--rule"] = true, ["--harness"] = true, ["--xtc"] = true, ["--sampling"] = true, ["--ban"] = true, ["--ban-pattern"] = true, ["--ctx"] = true, ["--ctx2"] = true }

local function interface_for(args)
  if M.config.ui ~= "nvim" then return false end
  local all = vim.list_extend(vim.deepcopy(M.config.args or {}), args or {})
  local i = 1
  while i <= #all do
    local a = all[i]
    if tui_only[a] or a:sub(1, 1) ~= "-" then return false end
    i = i + (valued[a] and 2 or 1)
  end
  return true
end

-- :Maid [args]: focus this tab's MAID (showing its windows again if they were hidden), or start one with `args`:
-- the interface (ui = "nvim"), or the TUI in a terminal.
function M.open(args)
  local ui = require("maid.ui")
  if ui.here() then return ui.open() end
  if interface_for(args) and not term_here() then return ui.open(args) end
  return M.open_terminal(args)
end

-- :MaidTerminal [args]: MAID's TUI in a terminal, whatever `ui` says.
function M.open_terminal(args)
  local t, tab = term_here()
  if not t then return start(args) end
  if tab ~= vim.api.nvim_get_current_tabpage() then vim.api.nvim_set_current_tabpage(tab) end
  local win = window_of(t.buf) or make_window(t.buf)
  vim.api.nvim_set_current_win(win)
  vim.cmd("startinsert")
end

-- :MaidToggle: hide this tab's MAID window (MAID keeps running), or show it, or start one.
function M.toggle()
  if require("maid.ui").here() then return require("maid.ui").toggle() end
  local t, tab = term_here()
  local win = t and tab == vim.api.nvim_get_current_tabpage() and window_of(t.buf)
  if win and #vim.api.nvim_tabpage_list_wins(0) > 1 then
    vim.api.nvim_win_hide(win)
  else
    M.open({})
  end
end

-- MAID's msgpack-rpc channel: the client named "maid" whose pid is this tab's terminal job; without a MAID
-- terminal in this tab, the newest "maid" client (a MAID started some other way, `:terminal maid`).
function M.channel()
  local t = term_here()
  local pid = t and vim.fn.jobpid(t.job)
  local newest
  for _, c in ipairs(vim.api.nvim_list_chans()) do
    if c.client and c.client.name == "maid" then
      local cpid = c.client.attributes and tonumber(c.client.attributes.pid)
      if pid and cpid == pid then return c.id end
      if not newest or c.id > newest then newest = c.id end
    end
  end
  if not pid then return newest end
end

-- Text into MAID's input, never sent: the interface's input buffer, else through MAID's channel when it is
-- connected, else as a bracketed paste into this tab's terminal. Returns "input", "rpc", "paste", or nil when
-- there is no MAID.
function M.send_text(text)
  local ui = require("maid.ui").here()
  if ui then
    require("maid.ui").add_input(ui, text)
    return "input"
  end
  local chan = M.channel()
  if chan then
    vim.rpcnotify(chan, "maid_send", text)
    return "rpc"
  end
  local t = term_here()
  if t then
    vim.fn.chansend(t.job, M.bracketed(text))
    return "paste"
  end
  vim.notify("maid.nvim: no MAID here; :Maid starts one", vim.log.levels.WARN)
end

-- A command line run in MAID as if typed (":theme gruvbox-dark"). Needs MAID connected, or the interface.
function M.command(line)
  local ui = require("maid.ui").here()
  if ui then
    require("maid.ui").command(ui, (line:gsub("^:", "")))
    return true
  end
  local chan = M.channel()
  if not chan then
    vim.notify("maid.nvim: no connected MAID for " .. line, vim.log.levels.WARN)
    return false
  end
  vim.rpcnotify(chan, "maid_command", line)
  return true
end

-- :MaidInterrupt: stops MAID's running turn as its first Ctrl-C does, from any window. In the interface a cancel to
-- the engine; over MAID's channel when it is connected (idle, MAID says so), else Ctrl-C typed into this tab's
-- terminal. Returns "engine", "rpc", "key" or nil.
function M.interrupt()
  local ui = require("maid.ui").here()
  if ui then
    require("maid.ui").interrupt(ui)
    return "engine"
  end
  local chan = M.channel()
  if chan then
    vim.rpcnotify(chan, "maid_interrupt")
    return "rpc"
  end
  local t = term_here()
  if t then
    vim.fn.chansend(t.job, "\3")
    return "key"
  end
  vim.notify("maid.nvim: no MAID here; :Maid starts one", vim.log.levels.WARN)
end

function M.bracketed(text)
  return "\27[200~" .. text .. "\27[201~"
end

local function shown_path(name)
  if name == "" then return "[No Name]" end
  return vim.fn.fnamemodify(name, ":~:.")
end

-- A fenced snippet with its path and line numbers; the fence is longer than any backtick run inside.
function M.format_snippet(path, first, last, lines, filetype)
  local longest = 2
  for _, l in ipairs(lines) do
    for run in l:gmatch("`+") do longest = math.max(longest, #run) end
  end
  local fence = string.rep("`", longest + 1)
  local where = first == last and ("line " .. first) or ("lines " .. first .. "-" .. last)
  return ("`%s` %s:\n%s%s\n%s\n%s"):format(path, where, fence, filetype or "", table.concat(lines, "\n"), fence)
end

-- :MaidSend: with a range those lines as a snippet; without one the buffer's path, for the agent to read.
function M.send_range(range, line1, line2)
  local buf = vim.api.nvim_get_current_buf()
  local path = shown_path(vim.api.nvim_buf_get_name(buf))
  if range == 0 then return M.send_text("`" .. path .. "`") end
  local lines = vim.api.nvim_buf_get_lines(buf, line1 - 1, line2, false)
  return M.send_text(M.format_snippet(path, line1, line2, lines, vim.bo[buf].filetype))
end

local severities = { "error", "warn", "info", "hint" }

-- vim.diagnostic.get() items as "path:line:col: severity: message [source]".
function M.format_diagnostics(items)
  table.sort(items, function(a, b)
    if a.bufnr ~= b.bufnr then return vim.api.nvim_buf_get_name(a.bufnr) < vim.api.nvim_buf_get_name(b.bufnr) end
    if a.lnum ~= b.lnum then return a.lnum < b.lnum end
    return a.col < b.col
  end)
  local out = {}
  for _, d in ipairs(items) do
    out[#out + 1] = ("%s:%d:%d: %s: %s%s"):format(shown_path(vim.api.nvim_buf_get_name(d.bufnr)), d.lnum + 1, d.col + 1,
      severities[d.severity] or "error", (d.message:gsub("\n", " ")), d.source and (" [" .. d.source .. "]") or "")
  end
  return out
end

-- :MaidDiagnostics[!]: this buffer's diagnostics, or every buffer's.
function M.send_diagnostics(all)
  local lines = M.format_diagnostics(vim.diagnostic.get(not all and 0 or nil))
  if #lines == 0 then
    vim.notify("maid.nvim: no diagnostics", vim.log.levels.INFO)
    return
  end
  local head = all and "Diagnostics from nvim, every buffer:" or ("Diagnostics from nvim for `" .. shown_path(vim.api.nvim_buf_get_name(0)) .. "`:")
  return M.send_text(head .. "\n" .. table.concat(lines, "\n"))
end

local qf_types = { E = "error", W = "warning", I = "info", N = "note", H = "hint" }

-- getqflist() items as "path:line:col: type: text"; an item with no location is its text alone.
function M.format_quickfix(items)
  local out = {}
  for _, it in ipairs(items) do
    local text = it.text:gsub("\n", " ")
    if it.valid == 1 and it.bufnr > 0 then
      local kind = qf_types[it.type] and (qf_types[it.type] .. ": ") or ""
      out[#out + 1] = ("%s:%d:%d: %s%s"):format(shown_path(vim.api.nvim_buf_get_name(it.bufnr)), it.lnum, math.max(it.col, 1), kind, text)
    else
      out[#out + 1] = text
    end
  end
  return out
end

-- :MaidQuickfix: the quickfix list.
function M.send_quickfix()
  local lines = M.format_quickfix(vim.fn.getqflist())
  if #lines == 0 then
    vim.notify("maid.nvim: the quickfix list is empty", vim.log.levels.INFO)
    return
  end
  local title = vim.fn.getqflist({ title = 0 }).title
  return M.send_text("Quickfix list" .. (title ~= "" and (" (" .. title .. ")") or "") .. ":\n" .. table.concat(lines, "\n"))
end

-- What each keymap does: name = { mode, rhs, description }.
local actions = {
  open = { "n", "<cmd>Maid<cr>", "MAID: open or focus" },
  toggle = { "n", "<cmd>MaidToggle<cr>", "MAID: show or hide" },
  send = { "n", "<cmd>MaidSend<cr>", "MAID: send the buffer's path" },
  send_selection = { "x", ":MaidSend<cr>", "MAID: send the selection" },
  send_buffer = { "n", "<cmd>%MaidSend<cr>", "MAID: send the whole buffer" },
  diagnostics = { "n", "<cmd>MaidDiagnostics<cr>", "MAID: send this buffer's diagnostics" },
  workspace_diagnostics = { "n", "<cmd>MaidDiagnostics!<cr>", "MAID: send every buffer's diagnostics" },
  quickfix = { "n", "<cmd>MaidQuickfix<cr>", "MAID: send the quickfix list" },
  interrupt = { "n", "<cmd>MaidInterrupt<cr>", "MAID: interrupt the running turn" },
  pause = { "n", "<cmd>MaidSteer interrupt<cr>", "MAID: pause the running turn" },
  resume = { "n", "<cmd>MaidSteer steer<cr>", "MAID: resume the paused turn" },
  send = { "n", function() require("maid.ui").send() end, "MAID: send the input" },
  send_insert = { "i", function() require("maid.ui").send() end, "MAID: send the input" },
}

-- The buffer keymaps each of MAID's buffers gets, and the modes beyond the action's own.
local buffer_keys = {
  terminal = { interrupt = {} },
  conversation = { interrupt = {}, pause = {}, resume = {} },
  input = { interrupt = {}, pause = { "i" }, resume = { "i" }, send = {}, send_insert = {} },
}

-- The global keymaps the config asks for: { name, mode, lhs, rhs, desc, explicit }, explicit when opts named the key.
function M.planned(opts)
  local c, out = M.config, {}
  if c.keymaps == false then return out end
  local mine = type(c.keymaps) == "table" and c.keymaps or M.defaults.keymaps
  local given = opts and type(opts.keymaps) == "table" and opts.keymaps or {}
  for name, a in pairs(actions) do
    local lhs = mine[name]
    if lhs == nil then lhs = M.defaults.keymaps[name] end
    if lhs and given[name] == nil and c.prefix ~= M.defaults.prefix and vim.startswith(lhs, M.defaults.prefix) then
      lhs = c.prefix .. lhs:sub(#M.defaults.prefix + 1)
    end
    if lhs then
      out[#out + 1] = { name = name, mode = a[1], lhs = lhs, rhs = a[2], desc = a[3], explicit = given[name] ~= nil }
    end
  end
  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

-- Keys left alone or shadowed, said once per session in one vim.notify (:h maid-never-overwrite).
local reported, pending = {}, {}
local function report(line)
  if reported[line] then return end
  reported[line] = true
  pending[#pending + 1] = line
  if #pending > 1 then return end
  vim.schedule(function()
    vim.notify("maid.nvim never overwrites a mapping:\n  " .. table.concat(pending, "\n  ")
      .. "\nName another key in setup() or drop it (:h maid-defaults); :checkhealth maid lists every key.", vim.log.levels.WARN)
    pending = {}
  end)
end

-- Sets `k` unless another mapping holds its key (a default is then skipped and reported; a key the user named is
-- set and reported). `buf`: buffer-local there, with nowait. True when it was set.
function map(k, buf)
  local K = require("maid.keymaps")
  local clash = K.clash(k.mode, k.lhs, buf)
  local where = ("%s (%s%s)"):format(k.lhs, K.mode_name(k.mode), buf and ", MAID's buffer" or "")
  if clash and not clash.shadow and not k.explicit then
    report(where .. " skipped, held by " .. clash.holder)
    return false
  end
  if clash and k.explicit then report(where .. ", your " .. (k.name:find("[.]") and k.name or "keymaps." .. k.name) .. ", shadows " .. clash.holder) end
  vim.keymap.set(k.mode, k.lhs, k.rhs, { desc = k.desc, silent = true, buffer = buf, nowait = buf ~= nil })
  return true
end
M.map = function(k, buf) return map(k, buf) end

-- The buffer-local keymaps of one of MAID's buffers, `kind` "terminal" (the default), "conversation" or "input":
-- its buffer_keymaps, and in the terminal the passthrough keys and terminal_escape when it is not nvim's own.
function M.buffer_planned(kind)
  local c, out = M.config, {}
  kind = kind or "terminal"
  for name, lhs in pairs(c.buffer_keymaps or {}) do
    local extra = buffer_keys[kind][name]
    if lhs and actions[name] and extra then
      for _, mode in ipairs(vim.list_extend({ actions[name][1] }, extra)) do
        out[#out + 1] = { name = "buffer_keymaps." .. name, mode = mode, lhs = lhs, rhs = actions[name][2], desc = actions[name][3],
          explicit = lhs ~= M.defaults.buffer_keymaps[name] }
      end
    end
  end
  if kind ~= "terminal" then
    table.sort(out, function(a, b) return a.lhs .. a.mode < b.lhs .. b.mode end)
    return out
  end
  local escape = c.terminal_escape ~= M.defaults.terminal_escape and c.terminal_escape or nil
  for key, on in pairs(c.terminal_passthrough or {}) do
    if on and key ~= escape then
      out[#out + 1] = { name = "terminal_passthrough", mode = "t", lhs = key, rhs = key, desc = "MAID: " .. key .. " goes to MAID",
        explicit = M.defaults.terminal_passthrough[key] == nil }
    end
  end
  table.sort(out, function(a, b) return a.lhs < b.lhs end)
  if escape then
    out[#out + 1] = { name = "terminal_escape", mode = "t", lhs = escape, rhs = "<C-\\><C-n>", desc = "MAID: leave terminal mode", explicit = true }
  end
  return out
end

local set = {} -- the global keymaps the last setup() made, so a second setup() replaces them

function M.setup(opts)
  M.opts = opts or {}
  M.config = vim.tbl_deep_extend("force", vim.deepcopy(M.defaults), M.opts)
  for _, k in ipairs(set) do pcall(vim.keymap.del, k.mode, k.lhs) end
  set = {}
  -- `maid nvim keymaps` runs the user's config with this set: plan the keys, set none, say nothing.
  if vim.g.maid_keymap_check == 1 then return end
  for _, k in ipairs(M.planned(opts)) do
    if map(k) then set[#set + 1] = k end
  end
end

return M
