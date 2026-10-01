-- maic.nvim: MAIC in an nvim terminal, and nvim's buffers, selections, diagnostics and quickfix list sent into
-- its input. MAIC's side of the connection (the host, User autocmds, :e, the theme) is in MAIC: docs/nvim.md.
local M = {}

-- Every option and its default, in one table (:h maic-defaults). setup(opts) deep-merges opts over it, which is
-- what lazy.nvim's `opts` does.
M.defaults = {
  cmd = "maic", -- the program: a name on PATH, a path, or a list (program and arguments)
  args = {}, -- arguments for every start, before the ones given to :Maic
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
  },
  -- Leaves terminal mode in MAIC's terminal. nvim's own <C-\><C-n> needs no mapping; another key is mapped there.
  terminal_escape = "<C-\\><C-n>",
  -- Keys that reach MAIC in its terminal even when a global terminal-mode mapping holds them (buffer-local, so
  -- the global mapping keeps working everywhere else). key = true; false drops one.
  terminal_passthrough = { ["<Esc>"] = true },
  filetypes = { terminal = "maic", input = "maic-input" }, -- MAIC's buffers, for plugins to include or exclude
}

M.config = vim.deepcopy(M.defaults)

-- MAIC's terminal per tab page: { buf, job }. In the "tab" layout the tab is MAIC's own.
local terms = {}

local function running(t)
  return t and vim.api.nvim_buf_is_valid(t.buf) and vim.fn.jobwait({ t.job }, 0)[1] == -1
end

-- This tab's MAIC; in the "tab" layout, the MAIC tab wherever it is.
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

-- A window for `buf` (nil: a new empty buffer) in the configured layout; returns the window.
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

local function command(extra)
  local cmd = type(M.config.cmd) == "table" and vim.deepcopy(M.config.cmd) or { M.config.cmd }
  vim.list_extend(cmd, M.config.args or {})
  vim.list_extend(cmd, extra or {})
  return cmd
end

local map -- below: sets a keymap unless another mapping holds its key

local function start(extra)
  make_window(nil)
  local buf = vim.api.nvim_get_current_buf()
  local tab = vim.api.nvim_get_current_tabpage()
  -- Before the job, so a TermOpen autocmd can tell MAIC's terminal by its filetype.
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
    vim.notify("maic.nvim: cannot start " .. table.concat(command(extra), " "), vim.log.levels.ERROR)
    return
  end
  vim.bo[buf].bufhidden = "hide"
  terms[tab] = { buf = buf, job = job }
  if vim.g.maic_keymap_check ~= 1 then
    for _, k in ipairs(M.buffer_planned()) do map(k, buf) end
  end
  vim.cmd("startinsert")
end

-- :Maic [args]: focus this tab's MAIC (showing its window again if it was hidden), or start one with `args`.
function M.open(args)
  local t, tab = term_here()
  if not t then return start(args) end
  if tab ~= vim.api.nvim_get_current_tabpage() then vim.api.nvim_set_current_tabpage(tab) end
  local win = window_of(t.buf) or make_window(t.buf)
  vim.api.nvim_set_current_win(win)
  vim.cmd("startinsert")
end

-- :MaicToggle: hide this tab's MAIC window (MAIC keeps running), or show it, or start one.
function M.toggle()
  local t, tab = term_here()
  local win = t and tab == vim.api.nvim_get_current_tabpage() and window_of(t.buf)
  if win and #vim.api.nvim_tabpage_list_wins(0) > 1 then
    vim.api.nvim_win_hide(win)
  else
    M.open({})
  end
end

-- MAIC's msgpack-rpc channel: the client named "maic" whose pid is this tab's terminal job; without a MAIC
-- terminal in this tab, the newest "maic" client (a MAIC started some other way, `:terminal maic`).
function M.channel()
  local t = term_here()
  local pid = t and vim.fn.jobpid(t.job)
  local newest
  for _, c in ipairs(vim.api.nvim_list_chans()) do
    if c.client and c.client.name == "maic" then
      local cpid = c.client.attributes and tonumber(c.client.attributes.pid)
      if pid and cpid == pid then return c.id end
      if not newest or c.id > newest then newest = c.id end
    end
  end
  if not pid then return newest end
end

-- Text into MAIC's input, never sent: through MAIC's channel when it is connected, else as a bracketed paste
-- into this tab's terminal. Returns "rpc", "paste", or nil when there is no MAIC.
function M.send_text(text)
  local chan = M.channel()
  if chan then
    vim.rpcnotify(chan, "maic_send", text)
    return "rpc"
  end
  local t = term_here()
  if t then
    vim.fn.chansend(t.job, M.bracketed(text))
    return "paste"
  end
  vim.notify("maic.nvim: no MAIC here; :Maic starts one", vim.log.levels.WARN)
end

-- A command line run in MAIC as if typed (":theme gruvbox-dark"). Needs MAIC connected.
function M.command(line)
  local chan = M.channel()
  if not chan then
    vim.notify("maic.nvim: no connected MAIC for " .. line, vim.log.levels.WARN)
    return false
  end
  vim.rpcnotify(chan, "maic_command", line)
  return true
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

-- :MaicSend: with a range those lines as a snippet; without one the buffer's path, for the agent to read.
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

-- :MaicDiagnostics[!]: this buffer's diagnostics, or every buffer's.
function M.send_diagnostics(all)
  local lines = M.format_diagnostics(vim.diagnostic.get(not all and 0 or nil))
  if #lines == 0 then
    vim.notify("maic.nvim: no diagnostics", vim.log.levels.INFO)
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

-- :MaicQuickfix: the quickfix list.
function M.send_quickfix()
  local lines = M.format_quickfix(vim.fn.getqflist())
  if #lines == 0 then
    vim.notify("maic.nvim: the quickfix list is empty", vim.log.levels.INFO)
    return
  end
  local title = vim.fn.getqflist({ title = 0 }).title
  return M.send_text("Quickfix list" .. (title ~= "" and (" (" .. title .. ")") or "") .. ":\n" .. table.concat(lines, "\n"))
end

-- What each keymap does: name = { mode, rhs, description }.
local actions = {
  open = { "n", "<cmd>Maic<cr>", "MAIC: open or focus" },
  toggle = { "n", "<cmd>MaicToggle<cr>", "MAIC: show or hide" },
  send = { "n", "<cmd>MaicSend<cr>", "MAIC: send the buffer's path" },
  send_selection = { "x", ":MaicSend<cr>", "MAIC: send the selection" },
  send_buffer = { "n", "<cmd>%MaicSend<cr>", "MAIC: send the whole buffer" },
  diagnostics = { "n", "<cmd>MaicDiagnostics<cr>", "MAIC: send this buffer's diagnostics" },
  workspace_diagnostics = { "n", "<cmd>MaicDiagnostics!<cr>", "MAIC: send every buffer's diagnostics" },
  quickfix = { "n", "<cmd>MaicQuickfix<cr>", "MAIC: send the quickfix list" },
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

-- Keys left alone or shadowed, said once per session in one vim.notify (:h maic-never-overwrite).
local reported, pending = {}, {}
local function report(line)
  if reported[line] then return end
  reported[line] = true
  pending[#pending + 1] = line
  if #pending > 1 then return end
  vim.schedule(function()
    vim.notify("maic.nvim never overwrites a mapping:\n  " .. table.concat(pending, "\n  ")
      .. "\nName another key in setup() or drop it (:h maic-defaults); :checkhealth maic lists every key.", vim.log.levels.WARN)
    pending = {}
  end)
end

-- Sets `k` unless another mapping holds its key (a default is then skipped and reported; a key the user named is
-- set and reported). `buf`: buffer-local there, with nowait. True when it was set.
function map(k, buf)
  local K = require("maic.keymaps")
  local clash = K.clash(k.mode, k.lhs, buf)
  local where = ("%s (%s%s)"):format(k.lhs, K.mode_name(k.mode), buf and ", MAIC's buffer" or "")
  if clash and not clash.shadow and not k.explicit then
    report(where .. " skipped, held by " .. clash.holder)
    return false
  end
  if clash and k.explicit then report(where .. ", your " .. (k.name:find("[.]") and k.name or "keymaps." .. k.name) .. ", shadows " .. clash.holder) end
  vim.keymap.set(k.mode, k.lhs, k.rhs, { desc = k.desc, silent = true, buffer = buf, nowait = buf ~= nil })
  return true
end

-- The buffer-local keymaps of MAIC's terminal: the passthrough keys, and terminal_escape when it is not nvim's own.
function M.buffer_planned()
  local c, out = M.config, {}
  local escape = c.terminal_escape ~= M.defaults.terminal_escape and c.terminal_escape or nil
  for key, on in pairs(c.terminal_passthrough or {}) do
    if on and key ~= escape then
      out[#out + 1] = { name = "terminal_passthrough", mode = "t", lhs = key, rhs = key, desc = "MAIC: " .. key .. " goes to MAIC",
        explicit = M.defaults.terminal_passthrough[key] == nil }
    end
  end
  table.sort(out, function(a, b) return a.lhs < b.lhs end)
  if escape then
    out[#out + 1] = { name = "terminal_escape", mode = "t", lhs = escape, rhs = "<C-\\><C-n>", desc = "MAIC: leave terminal mode", explicit = true }
  end
  return out
end

local set = {} -- the global keymaps the last setup() made, so a second setup() replaces them

function M.setup(opts)
  M.config = vim.tbl_deep_extend("force", vim.deepcopy(M.defaults), opts or {})
  for _, k in ipairs(set) do pcall(vim.keymap.del, k.mode, k.lhs) end
  set = {}
  -- `maic nvim keymaps` runs the user's config with this set: plan the keys, set none, say nothing.
  if vim.g.maic_keymap_check == 1 then return end
  for _, k in ipairs(M.planned(opts)) do
    if map(k) then set[#set + 1] = k end
  end
end

return M
