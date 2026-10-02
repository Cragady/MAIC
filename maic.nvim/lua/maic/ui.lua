-- nvim as MAIC's interface: the engine (`maic --rpc`) as this tab's job, JSON-RPC one message per line each way;
-- the conversation and the input as nvim buffers; approvals, questions, the engine's own questions and the pause
-- menu as floats. The protocol and its events: MAIC's docs/design/engine-protocol.md.
local U = {}

local function maic() return require("maic") end

local ns = vim.api.nvim_create_namespace("maic.ui")
local uv = vim.uv or vim.loop

-- Highlight groups, linked by default so a colorscheme can set its own.
local groups = { MaicUser = "Title", MaicTool = "Function", MaicToolOk = "DiagnosticOk", MaicToolErr = "DiagnosticError", MaicNotice = "Comment",
  MaicError = "ErrorMsg", MaicSteer = "WarningMsg", MaicFooter = "NonText", MaicThinking = "Comment" }
for name, link in pairs(groups) do vim.api.nvim_set_hl(0, name, { link = link, default = true }) end

-- The interface of each tab page: the view of the session in its focus. A tab's engine (`conn`) holds every
-- session it opened, and each has a view of its own (`ui`), its conversation a buffer of its own.
local uis = {}

local function alive(ui) return ui and ui.conn.job and vim.api.nvim_buf_is_valid(ui.conv) end

-- This tab's interface, while its engine runs.
function U.here()
  local ui = uis[vim.api.nvim_get_current_tabpage()]
  if alive(ui) then return ui end
end

-- ---------- the engine ----------

local function request(ui, method, params, cb)
  local conn = ui.conn
  if not conn.job then return end
  local id = conn.next
  conn.next = id + 1
  conn.waiting[id] = cb or false
  vim.fn.chansend(conn.job, vim.json.encode({ jsonrpc = "2.0", id = id, method = method, params = params or vim.empty_dict() }) .. "\n")
end

local function err_text(e) return (e.data and e.data.message) or e.message or "the engine refused it" end

-- ---------- the conversation buffer ----------

-- Writes into the conversation (unmodifiable between writes); a window whose cursor was on its last line follows.
local function edit(ui, fn)
  local buf = ui.conv
  local n = vim.api.nvim_buf_line_count(buf)
  local follow = {}
  for _, w in ipairs(vim.fn.win_findbuf(buf)) do
    if vim.api.nvim_win_get_cursor(w)[1] >= n then follow[#follow + 1] = w end
  end
  vim.bo[buf].modifiable = true
  fn(buf)
  vim.bo[buf].modifiable = false
  local m = vim.api.nvim_buf_line_count(buf)
  for _, w in ipairs(follow) do pcall(vim.api.nvim_win_set_cursor, w, { m, 0 }) end
end

local function mark(ui, b)
  if not b.hl then return end
  b.mark = vim.api.nvim_buf_set_extmark(ui.conv, ns, b.start, 0, { id = b.mark, end_row = b.stop, end_col = 0, hl_group = b.hl, priority = 150 })
end

-- Lines [start, stop) after `b` in the buffer moved by `d`, and `b` grew by it.
local function grow(ui, b, d)
  if d == 0 then return end
  for i = #ui.blocks, 1, -1 do
    local o = ui.blocks[i]
    if o == b then break end
    o.start, o.stop = o.start + d, o.stop + d
  end
  b.stop = b.stop + d
end

-- A block of lines at the end: `o.gap` puts a blank line before it, `o.prefix` starts each line, `o.hl` colours
-- it, `o.id` names it for later deltas.
local function add(ui, text, o)
  o = o or {}
  local b = { prefix = o.prefix or "", hl = o.hl, fold = o.fold }
  local rows = {}
  if o.gap and not ui.empty then rows[1] = "" end
  local first = #rows
  for _, l in ipairs(vim.split(text, "\n", { plain = true })) do rows[#rows + 1] = b.prefix .. l end
  edit(ui, function(buf)
    local n = ui.empty and 0 or vim.api.nvim_buf_line_count(buf)
    vim.api.nvim_buf_set_lines(buf, n, ui.empty and 1 or n, false, rows)
    b.start, b.stop = n + first, n + #rows
  end)
  ui.empty = false
  ui.blocks[#ui.blocks + 1] = b
  if o.id then ui.by_id[o.id] = b end
  mark(ui, b)
  return b
end

local function append(ui, b, text)
  if text == "" then return end
  local pieces = vim.split(text, "\n", { plain = true })
  edit(ui, function(buf)
    local row = b.stop - 1
    local rows = { (vim.api.nvim_buf_get_lines(buf, row, row + 1, false)[1] or "") .. pieces[1] }
    for i = 2, #pieces do rows[i] = b.prefix .. pieces[i] end
    vim.api.nvim_buf_set_lines(buf, row, row + 1, false, rows)
  end)
  grow(ui, b, #pieces - 1)
  mark(ui, b)
end

local function replace(ui, b, text)
  local rows = {}
  for _, l in ipairs(vim.split(text, "\n", { plain = true })) do rows[#rows + 1] = b.prefix .. l end
  edit(ui, function(buf) vim.api.nvim_buf_set_lines(buf, b.start, b.stop, false, rows) end)
  grow(ui, b, #rows - (b.stop - b.start))
  mark(ui, b)
end

local function notice(ui, text, hl, gap) return add(ui, text, { hl = hl or "MaicNotice", gap = gap }) end

-- A closed fold over `b` in every window showing the conversation (manual folds live in the window; BufWinEnter
-- makes them again for a window that shows it later).
local function fold(ui, b)
  local least = maic().config.fold_output
  if not least or b.stop - b.start < least then return end
  b.fold = true
  for _, w in ipairs(vim.fn.win_findbuf(ui.conv)) do
    vim.api.nvim_win_call(w, function() vim.cmd(("silent! %d,%dfold"):format(b.start + 1, b.stop)) end)
  end
end

local function refold(ui, win)
  vim.api.nvim_win_call(win, function()
    vim.cmd("silent! normal! zE")
    for _, b in ipairs(ui.blocks) do
      if b.fold then vim.cmd(("silent! %d,%dfold"):format(b.start + 1, b.stop)) end
    end
  end)
end

function U.foldtext()
  local n = vim.v.foldend - vim.v.foldstart + 1
  return ("  ▸ %d lines: %s"):format(n, vim.trim(vim.fn.getline(vim.v.foldstart)))
end

local function bar_text(s) return (tostring(s or ""):gsub("%%", "%%%%")) end

-- The winbar of the conversation: the session's title, model, mode and what it is doing.
local function status(ui)
  local parts = { "MAIC" }
  if ui.title and ui.title ~= "" then parts[#parts + 1] = ui.title end
  if ui.model then parts[#parts + 1] = ui.model end
  if ui.mode then parts[#parts + 1] = ui.mode end
  if ui.paused then
    parts[#parts + 1] = "PAUSED: <C-q> resumes"
  elseif ui.activity and ui.activity ~= "idle" then
    parts[#parts + 1] = ui.activity
  end
  if not ui.conn.job then parts[#parts + 1] = "engine stopped" end
  local others, waiting, finished = 0, 0, 0
  for id, e in pairs(ui.conn.index) do
    if id ~= ui.session and e.state ~= "parked" then
      others = others + 1
      if e.activity == "waiting" then waiting = waiting + 1 end
      if e.unseen then finished = finished + 1 end
    end
  end
  if others > 0 then
    local t = others .. (others == 1 and " other session" or " other sessions")
    if waiting > 0 then t = t .. ", " .. waiting .. " waiting" end
    if finished > 0 then t = t .. ", " .. finished .. " finished" end
    parts[#parts + 1] = t .. " (:MaicSwitch)"
  end
  local text = " " .. bar_text(table.concat(parts, " · "))
  for _, w in ipairs(vim.fn.win_findbuf(ui.conv)) do vim.wo[w].winbar = text end
end

local function fire(ui, pattern, data)
  data.session = ui.session
  pcall(vim.api.nvim_exec_autocmds, "User", { pattern = pattern, modeline = false, data = data })
end

local function absolute(ui, path)
  if not path or path == "" then return "" end
  if path:sub(1, 1) ~= "/" and ui.workspace then path = ui.workspace .. "/" .. path end
  return vim.fs.normalize(path)
end

-- ---------- floats: approvals, questions, the engine's questions, the pause menu ----------

local function close_float(ui)
  local f = ui.float
  ui.float = nil
  if f and vim.api.nvim_win_is_valid(f.win) then
    local back = f.back
    vim.api.nvim_win_close(f.win, true)
    if back and vim.api.nvim_win_is_valid(back) and vim.api.nvim_get_current_tabpage() == ui.tab then vim.api.nvim_set_current_win(back) end
  end
end

-- A float over the bottom of the conversation with `lines` and `keys` (key = function), focused when this tab is
-- the interface's.
local function float(ui, kind, title, lines, keys, id)
  close_float(ui)
  local buf = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, lines)
  vim.bo[buf].modifiable = false
  vim.bo[buf].bufhidden = "wipe"
  local width = #title + 4
  for _, l in ipairs(lines) do width = math.max(width, vim.fn.strdisplaywidth(l) + 2) end
  local conv = vim.fn.win_findbuf(ui.conv)[1]
  local cfg
  if conv and vim.api.nvim_win_get_tabpage(conv) == vim.api.nvim_get_current_tabpage() then
    local cw, ch = vim.api.nvim_win_get_width(conv), vim.api.nvim_win_get_height(conv)
    width = math.max(math.min(width, cw - 4), 10)
    local height = math.min(#lines, math.max(ch - 3, 1))
    cfg = { relative = "win", win = conv, row = math.max(ch - height - 2, 0), col = 1, width = width, height = height }
  else
    width = math.min(width, vim.o.columns - 4)
    local height = math.min(#lines, vim.o.lines - 6)
    cfg = { relative = "editor", row = math.floor((vim.o.lines - height) / 2), col = math.floor((vim.o.columns - width) / 2), width = width, height = height }
  end
  cfg.border, cfg.title, cfg.style = "rounded", " " .. title .. " ", "minimal"
  local enter = vim.api.nvim_get_current_tabpage() == ui.tab
  local back = vim.api.nvim_get_current_win()
  local win = vim.api.nvim_open_win(buf, enter, cfg)
  ui.float = { win = win, buf = buf, kind = kind, id = id, back = enter and back or nil }
  for key, fn in pairs(keys) do
    vim.keymap.set("n", key, fn, { buffer = buf, nowait = true, silent = true, desc = "MAIC: " .. kind })
  end
end

local show_next

local function approval_lines(e)
  local lines = { e.summary or "", "why asking: " .. (e.reason or "") .. (e.origin == "remote" and "  [REMOTE REQUEST]" or "") }
  if e.thread and e.thread.session then lines[#lines + 1] = "from the background task " .. (e.thread.title or e.thread.session) end
  local preview = vim.split(e.preview or "", "\n", { plain = true })
  if preview[#preview] == "" then table.remove(preview) end
  for i = 1, math.min(#preview, 14) do lines[#lines + 1] = "  " .. preview[i] end
  local covers = e.always_covers or (e.tool == "run_shell" and "this program" or "this file")
  lines[#lines + 1] = "[y] yes  [n] no  [N] no, and say why  [a] always: " .. covers .. " (this session)  [t] trip the harness"
  if e.path and e.path ~= "" then lines[#lines + 1] = "[e] open the file" .. (e.proposed_size and "  [d] diff the proposed content in a new tab" or "") end
  return lines
end

local function answer(ui, e, choice, feedback)
  table.remove(ui.asks, 1)
  close_float(ui)
  request(ui, "maic.approval.answer", { session = ui.session, approval = e.id, choice = choice, feedback = feedback }, function(_, err)
    if err then notice(ui, "✗ " .. err_text(err), "MaicError") end
  end)
  show_next(ui)
end

-- A window to open a file in: the previous one unless it is MAIC's or a float, else any plain one in the tab.
local function editing_window(ui)
  local function plain(w)
    local b = vim.api.nvim_win_get_buf(w)
    return vim.api.nvim_win_get_config(w).relative == "" and b ~= ui.conv and b ~= ui.input and vim.bo[b].buftype == ""
  end
  local prev = vim.fn.win_getid(vim.fn.winnr("#"))
  if prev ~= 0 and plain(prev) then return prev end
  for _, w in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if plain(w) then return w end
  end
end

local function open_file(ui, path)
  close_float(ui)
  local w = editing_window(ui)
  if w then
    vim.api.nvim_set_current_win(w)
  else
    vim.cmd("aboveleft split")
  end
  vim.cmd("drop " .. vim.fn.fnameescape(absolute(ui, path)))
end

-- The file against the proposed content, both diffthis, in a new tab; the scratch side is read-only.
local function diff(ui, e)
  request(ui, "maic.approval.proposed", { session = ui.session, approval = e.id }, function(r, err)
    if err or type(r) ~= "table" or type(r.text) ~= "string" then
      vim.notify("maic.nvim: no proposed content to diff for this call", vim.log.levels.WARN)
      return
    end
    local path = absolute(ui, e.path)
    vim.cmd("tabnew " .. vim.fn.fnameescape(path))
    vim.cmd("diffthis")
    vim.cmd("vnew")
    local buf = vim.api.nvim_get_current_buf()
    vim.api.nvim_buf_set_lines(buf, 0, -1, false, vim.split(r.text, "\n", { plain = true }))
    vim.bo[buf].buftype, vim.bo[buf].bufhidden, vim.bo[buf].modifiable = "nofile", "wipe", false
    vim.bo[buf].filetype = vim.filetype.match({ filename = path }) or ""
    pcall(vim.api.nvim_buf_set_name, buf, path .. " (proposed)")
    vim.cmd("diffthis")
  end)
end

local function show_approval(ui, e)
  local function choose(c) return function() answer(ui, e, c) end end
  local keys = { y = choose("yes"), Y = choose("yes"), n = choose("no"), ["<Esc>"] = choose("no"), a = choose("always"), A = choose("always"),
    t = choose("trip"), T = choose("trip") }
  keys.N = function()
    vim.ui.input({ prompt = "no, because: " }, function(why)
      if why then answer(ui, e, "no", why) end
    end)
  end
  keys["<C-c>"] = function()
    answer(ui, e, "no")
    U.interrupt(ui)
  end
  if e.path and e.path ~= "" then
    keys.e = function() open_file(ui, e.path) end
    if e.proposed_size then keys.d = function() diff(ui, e) end end
  end
  float(ui, "approval", "approve?", approval_lines(e), keys, e.id)
end

local function show_question(ui, q)
  local lines = { q.text }
  for i, o in ipairs(q.options or {}) do lines[#lines + 1] = ("[%d] %s"):format(i, o) end
  lines[#lines + 1] = (#(q.options or {}) > 0 and "a number picks; " or "") .. "<CR> types an answer; <Esc> gives none"
  local function reply(text)
    table.remove(ui.asks, 1)
    close_float(ui)
    add(ui, "❯ " .. (text == "" and "(no answer)" or text), { gap = true, hl = "MaicUser" })
    request(ui, "maic.question.reply", { session = ui.session, question = q.id, text = text })
    show_next(ui)
  end
  local keys = { ["<Esc>"] = function() reply("") end, ["<C-c>"] = function() reply("") end }
  keys["<CR>"] = function()
    vim.ui.input({ prompt = "answer: " }, function(text)
      if text then reply(text) end
    end)
  end
  keys.i = keys["<CR>"]
  for i, o in ipairs(q.options or {}) do
    if i <= 9 then keys[tostring(i)] = function() reply(o) end end
  end
  float(ui, "question", "the agent asks", lines, keys, q.id)
end

local show_result

-- The engine's own question about a command (maic.session.command's `ask`): one key answers, Esc is n.
local function show_confirm(ui, a)
  local lines = vim.deepcopy(a.lines or {})
  lines[#lines + 1] = "[" .. table.concat(vim.split(a.keys, ""), "] [") .. "]"
  local function pick(key)
    table.remove(ui.asks, 1)
    close_float(ui)
    request(ui, "maic.session.command", { session = ui.session, ask = a.id, key = key }, function(r, err) show_result(ui, r, err) end)
    show_next(ui)
  end
  local keys = { ["<Esc>"] = function() pick("n") end, ["<C-c>"] = function() pick("n") end }
  for _, k in ipairs(vim.split(a.keys, "")) do
    keys[k] = function() pick(k) end
    if a.keys == "yn" then keys[k:upper()] = keys[k] end
  end
  float(ui, "confirm", a.title or "MAIC asks", lines, keys, a.id)
end

local function show_pause(ui)
  local function act(action) return function() U.steer(ui, action) end end
  float(ui, "pause", "paused", {
    "[<C-q>] resume  [s] steer  [d] drop  [f] further  [k] keep  [h] halt",
    "what is in the input goes with s, d and f as their note; <Esc> leaves this to type a message (sending it resumes)",
  }, { ["<C-q>"] = act("steer"), s = act("steer"), d = act("drop"), f = act("further"), k = act("keep"), h = act("halt"),
    ["<C-c>"] = function() U.interrupt(ui) end, ["<Esc>"] = function() close_float(ui) end })
end

-- The first waiting approval or question, else the pause menu while paused.
function show_next(ui)
  local first = ui.asks[1]
  if uis[ui.tab] ~= ui then
    -- A session in the background asks: it waits, named, until it is switched to.
    if first and not ui.told then
      ui.told = true
      vim.notify(("maic.nvim: %s waits for you (:MaicSwitch)"):format(ui.title and ui.title ~= "" and ui.title or ui.session), vim.log.levels.WARN)
    end
    return
  end
  ui.told = nil
  if ui.float and first and ui.float.id == first.id then return end
  if first then
    if first.kind == "approval" then show_approval(ui, first.e)
    elseif first.kind == "question" then show_question(ui, first.e)
    else show_confirm(ui, first.e) end
  elseif ui.paused then
    show_pause(ui)
  end
end

local function drop_ask(ui, id)
  for i, a in ipairs(ui.asks) do
    if a.id == id then table.remove(ui.asks, i) break end
  end
  if ui.float and ui.float.id == id then close_float(ui) end
  show_next(ui)
end

-- ---------- events ----------

local function item_text(item)
  local t = {}
  for _, p in ipairs(item.content or {}) do t[#t + 1] = p.text or "" end
  return table.concat(t)
end

local function user_block(ui, text, extra)
  local lines = vim.split(text, "\n", { plain = true })
  for i = 2, #lines do lines[i] = "  " .. lines[i] end
  add(ui, "❯ " .. table.concat(lines, "\n") .. (extra or ""), { gap = true, hl = "MaicUser" })
end

local function tool_line(ui, id, summary)
  ui.tool_calls = ui.tool_calls + 1
  add(ui, "⏺ " .. summary, { gap = true, hl = "MaicTool", id = id })
end

-- The output block of a call (a live one is made on its first chunk), under its id and the call's.
local function output_block(ui, id, call)
  local b = ui.by_id[id] or (call and ui.by_id["out:" .. call])
  if not b then b = add(ui, "", { prefix = "  ", id = id }) end
  ui.by_id[id] = b
  if call then ui.by_id["out:" .. call] = b end
  return b
end

local function finish_output(ui, b, call, text, ok, full)
  if text ~= "" or b.stop - b.start > 1 then replace(ui, b, text) end
  if full then append(ui, b, "\n[full output: maic sessions output " .. (full.session or "") .. " " .. (full.call or "") .. "]") end
  local line = call and ui.by_id["call:" .. call]
  if line then
    line.hl = ok and "MaicToolOk" or "MaicToolErr"
    mark(ui, line)
  end
  fold(ui, b)
end

local function duration(s)
  if s < 1 then return ("%.0fms"):format(s * 1000) end
  if s < 60 then return ("%.1fs"):format(s) end
  if s < 3600 then return ("%dm %ds"):format(s / 60, s % 60) end
  return ("%dh %dm"):format(s / 3600, (s % 3600) / 60)
end

-- ---------- history ----------

-- A history item (maic.session.attach, listConversationItems) drawn as its live events would have drawn it. A
-- collapsed one shows its head; the engine's maic.item.expand has the rest.
local function render_item(ui, item)
  local m = item.maic or {}
  local function shown(text) return m.collapsed and ((m.head or "") .. ("\n… %d bytes in all"):format(m.size or 0)) or text end
  if item.type == "message" and item.role == "user" then
    user_block(ui, item_text(item))
  elseif item.type == "message" then
    add(ui, item_text(item), { gap = true })
  elseif item.type == "function_call_output" or item.type == "shell_call_output" then
    add(ui, "⏺ " .. (m.summary or "tool"), { gap = true, hl = "MaicTool", id = "call:" .. item.call_id })
    local text = item.type == "function_call_output" and (type(item.output) == "string" and item.output or "")
      or (item.output and item.output[1] and item.output[1].stdout or "")
    finish_output(ui, add(ui, "", { prefix = "  ", id = item.id }), item.call_id, shown(text), m.ok ~= false, m.full_output)
  elseif item.type == "maic.notice" then
    local b = notice(ui, shown(item.text or ""), nil, true)
    if item.kind == "context" or item.kind == "compact" then fold(ui, b) end
  end
end

-- What attach's inflight holds: the reply being written and a running tool's output so far, as blocks the
-- events that follow keep writing into.
local function render_inflight(ui, inflight)
  for _, f in ipairs(inflight and inflight.items or {}) do
    local item = f.item
    if item.type == "message" or item.type == "reasoning" then
      append(ui, add(ui, "", { gap = true, id = item.id, hl = item.type == "reasoning" and "MaicThinking" or nil }), f.text or "")
    elseif item.type == "function_call_output" or item.type == "shell_call_output" then
      local call = f.call or {}
      add(ui, "⏺ " .. ((call.maic or {}).summary or call.name or "tool"), { gap = true, hl = "MaicTool", id = "call:" .. (item.call_id or "") })
      local text = f.text or ""
      append(ui, output_block(ui, item.id, item.call_id), (f.size or 0) > #text and ("…" .. text) or text)
    end
  end
end

-- The line at the top while older history is left to load: reaching it (or :MaicOlder) loads the next page.
local function set_marker(ui, on)
  if (ui.marker ~= nil) == on then return end
  local d = on and 1 or -1
  edit(ui, function(buf)
    if on then
      vim.api.nvim_buf_set_lines(buf, 0, 0, false, { "⋯ earlier history: move here or :MaicOlder" })
    else
      vim.api.nvim_buf_del_extmark(buf, ns, ui.marker)
      vim.api.nvim_buf_set_lines(buf, 0, 1, false, {})
    end
  end)
  for _, b in ipairs(ui.blocks) do b.start, b.stop = b.start + d, b.stop + d end
  ui.marker = on and vim.api.nvim_buf_set_extmark(ui.conv, ns, 0, 0, { end_row = 1, end_col = 0, hl_group = "MaicNotice" }) or nil
end

-- Draws with `draw(t)` into a scratch buffer, with the functions that draw everything else, and moves the result
-- above all that is shown, its blocks with it.
local function prepend(ui, draw)
  local t = setmetatable({ conv = vim.api.nvim_create_buf(false, true), blocks = {}, by_id = {}, empty = true }, { __index = ui })
  draw(t)
  local rows = vim.api.nvim_buf_get_lines(t.conv, 0, -1, false)
  vim.api.nvim_buf_delete(t.conv, { force = true })
  if t.empty then return end
  rows[#rows + 1] = ""
  edit(ui, function(buf) vim.api.nvim_buf_set_lines(buf, 0, 0, false, rows) end)
  for _, b in ipairs(ui.blocks) do b.start, b.stop = b.start + #rows, b.stop + #rows end
  for _, b in ipairs(t.blocks) do
    b.mark = nil
    mark(ui, b)
  end
  ui.blocks = vim.list_extend(t.blocks, ui.blocks)
  for id, b in pairs(t.by_id) do ui.by_id[id] = ui.by_id[id] or b end
  for _, w in ipairs(vim.fn.win_findbuf(ui.conv)) do refold(ui, w) end
end

-- The page of history before the oldest item shown, drawn above it.
function U.older(ui)
  ui = ui or U.here()
  if not ui or not ui.more_before or ui.loading then return end
  ui.loading = true
  request(ui, "listConversationItems", { conversation_id = ui.session, after = ui.oldest, maic = { exchanges = 10 } }, function(r, err)
    ui.loading = false
    if err then return notice(ui, "✗ " .. err_text(err), "MaicError") end
    local items = {}
    for i = #r.data, 1, -1 do items[#items + 1] = r.data[i] end
    set_marker(ui, false)
    prepend(ui, function(t)
      for _, item in ipairs(items) do render_item(t, item) end
    end)
    if items[1] then ui.oldest = items[1].id end
    ui.more_before = r.has_more
    set_marker(ui, ui.more_before)
  end)
end

local handlers = {}

handlers["maic.input.added"] = function(ui, e)
  local pics = e.item.maic and e.item.maic.images and #e.item.maic.images or 0
  user_block(ui, item_text(e.item), pics > 0 and ("\n  (with %d image%s)"):format(pics, pics == 1 and "" or "s") or nil)
end

handlers["response.created"] = function(ui, e)
  ui.response, ui.paused = e.response.id, false
  if ui.float and ui.float.kind == "pause" then close_float(ui) end
  if not e.response.previous_response_id then
    ui.t0, ui.tool_calls = uv.hrtime(), 0
    fire(ui, "MaicTurnStart", { model = e.response.model or ui.model })
  end
  status(ui)
end

handlers["response.output_item.added"] = function(ui, e)
  local item = e.item
  if item.type == "message" then
    add(ui, "", { gap = true, id = item.id })
  elseif item.type == "reasoning" then
    add(ui, "", { gap = true, id = item.id, hl = "MaicThinking" })
  elseif item.type == "function_call" or item.type == "shell_call" then
    local summary = item.maic and item.maic.summary or item.name or "tool"
    ui.by_id["out:" .. item.call_id] = nil -- call ids repeat across turns: the newest call owns its id
    tool_line(ui, "call:" .. item.call_id, summary)
    local tool, path = "run_shell", ""
    if item.type == "function_call" then
      tool = item.name
      local ok, args = pcall(vim.json.decode, item.arguments or "{}")
      if ok and type(args) == "table" then path = type(args.path) == "string" and args.path or type(args.from) == "string" and args.from or "" end
    end
    fire(ui, "MaicToolCall", { tool = tool, path = absolute(ui, path), summary = summary })
  elseif item.type == "function_call_output" or item.type == "shell_call_output" then
    output_block(ui, item.id, item.call_id)
  end
end

local function text_delta(ui, e)
  local b = ui.by_id[e.item_id] or add(ui, "", { gap = true, id = e.item_id })
  append(ui, b, e.delta)
end
handlers["response.output_text.delta"] = text_delta
handlers["response.reasoning_text.delta"] = text_delta

handlers["response.output_text.done"] = function(ui, e)
  local b = ui.by_id[e.item_id]
  if b and e.maic and e.maic.trimmed then replace(ui, b, e.text) end
end

handlers["response.reasoning_text.done"] = function(ui, e)
  local b = ui.by_id[e.item_id]
  if b then fold(ui, b) end
end

handlers["response.shell_call_output_content.delta"] = function(ui, e)
  local d = e.delta or {}
  append(ui, output_block(ui, e.item_id), (d.stdout or "") .. (d.stderr or ""))
end

handlers["maic.tool.output.delta"] = function(ui, e)
  if not e.data then return end
  if not e.output_index then
    if ui.shell then append(ui, ui.shell, e.data) end -- a `!cmd` of this session's
    return
  end
  append(ui, output_block(ui, e.item_id or e.call, e.call), e.data)
end

handlers["response.output_item.done"] = function(ui, e)
  local item = e.item
  if item.type == "message" and item.maic and item.maic.status == "discarded" then
    notice(ui, "(the reply above was discarded: the model never sees it)")
  elseif (item.type == "function_call_output" or item.type == "shell_call_output") and item.maic then
    local text = item.type == "function_call_output" and (type(item.output) == "string" and item.output or "")
      or (item.output and item.output[1] and item.output[1].stdout or "")
    finish_output(ui, output_block(ui, item.id, item.call_id), item.call_id, text, item.maic.ok, item.maic.full_output)
  end
end

handlers["maic.notice"] = function(ui, e)
  if e.kind == "tool_call" then
    tool_line(ui, "sub:" .. ui.tool_calls, e.text)
    if e.tool then fire(ui, "MaicToolCall", { tool = e.tool, path = absolute(ui, e.path), summary = (e.text:gsub("^↳ ", "")) }) end
  elseif e.kind == "tool_result" then
    local b = add(ui, "", { prefix = "  " })
    finish_output(ui, b, nil, e.text, e.ok, e.full_output)
  else
    notice(ui, (e.level == "info" or not e.level) and e.text or ("✗ " .. e.text), e.level == "error" and "MaicError" or e.level == "warn" and "MaicSteer" or nil)
  end
end

handlers["maic.task.created"] = function(ui, e)
  notice(ui, "⧉ the " .. e.agent .. " agent works in the background: " .. (e.prompt_head or "") .. "  (:MaicSwitch shows it)")
end

for _, kind in ipairs({ "maic.task.completed", "maic.task.failed" }) do
  handlers[kind] = function(ui, e)
    local ok = kind == "maic.task.completed"
    notice(ui, ("⧉ the %s task %s (%d steps, %d tokens); its answer goes to the agent  (:MaicSwitch %s reads it)"):format(e.agent, ok and "finished" or (e.reason or "failed"),
      e.steps or 0, e.tokens or 0, e.task), ok and nil or "MaicError")
  end
end

handlers["maic.approval.requested"] = function(ui, e)
  ui.approvals[e.id] = { tool = e.tool or "", path = absolute(ui, e.path), summary = e.summary or "", reason = e.reason or "", verdict = "pending" }
  fire(ui, "MaicApproval", vim.deepcopy(ui.approvals[e.id]))
  ui.asks[#ui.asks + 1] = { kind = "approval", id = e.id, e = e }
  show_next(ui)
end

handlers["maic.approval.answered"] = function(ui, e)
  local seen = ui.approvals[e.id]
  if seen then
    seen.verdict = e.choice or "no"
    fire(ui, "MaicApproval", seen)
    ui.approvals[e.id] = nil
  end
  drop_ask(ui, e.id)
end

handlers["maic.question.asked"] = function(ui, e)
  ui.asks[#ui.asks + 1] = { kind = "question", id = e.id, e = e }
  show_next(ui)
end

handlers["maic.question.answered"] = function(ui, e) drop_ask(ui, e.id) end

handlers["maic.steer.applied"] = function(ui, e)
  local line = "↯ " .. e.action .. (e.trigger == "ban" and " (a ban's steer)" or "") .. (e.note and e.note ~= "" and (": " .. e.note) or "")
  if e.waits_for then line = line .. "  (at the next step)" end
  if e.withdrawn and #e.withdrawn > 0 then line = line .. ("  (withdrew %d waiting)"):format(#e.withdrawn) end
  notice(ui, line, "MaicSteer")
end

handlers["maic.turn.paused"] = function(ui, e)
  ui.paused, ui.response = true, e.response_id
  status(ui)
  show_next(ui)
end

handlers["response.steer.failed"] = function(ui, e)
  notice(ui, "✗ not delivered: " .. ((e.error and e.error.message) or "the turn ended first"), "MaicError")
end

handlers["error"] = function(ui, e) notice(ui, "halted: " .. (e.message or ""), "MaicError") end

handlers["maic.file.written"] = function(ui, e)
  pcall(vim.cmd, "checktime")
  fire(ui, "MaicFileWritten", { tool = e.tool or "", path = absolute(ui, e.path) })
end

-- A session parked or stopped: its view goes, its buffer too once no window shows it (the focused one's when this
-- tab moves on).
local function drop_view(ui)
  close_float(ui)
  if vim.api.nvim_buf_is_valid(ui.conv) and #vim.fn.win_findbuf(ui.conv) == 0 then vim.api.nvim_buf_delete(ui.conv, { force = true }) end
end

handlers["maic.session.state"] = function(ui, e)
  ui.activity = e.activity
  if e.state == "parked" or e.state == "stopped" then
    ui.ended = e.state
    ui.conn.views[ui.session] = nil
    if uis[ui.tab] ~= ui then return drop_view(ui) end
  end
  status(ui)
end

handlers["maic.session.settings"] = function(ui, e)
  ui.mode, ui.model = e.mode or ui.mode, e.model or ui.model
  if e.workspace then ui.workspace = e.workspace end
  status(ui)
end

handlers["maic.session.title"] = function(ui, e)
  ui.title = e.text
  notice(ui, e.source == "auto" and ("titled: " .. e.text .. "  (/rename changes it)") or ("titled: " .. e.text))
  status(ui)
end

local function finished(ui, e, kind)
  local r = e.response
  if r.maic and r.maic.final == false then
    if kind == "maic.response.cancelled" then notice(ui, "paused · <C-q> resumes · s steer · d drop · f further · k keep · h halt", "MaicSteer") end
    return
  end
  if kind == "response.failed" then notice(ui, "✗ " .. ((r.error and r.error.message) or "the response failed"), "MaicError") end
  local cancelled = kind == "maic.response.cancelled"
  local secs = ui.t0 and (uv.hrtime() - ui.t0) / 1e9 or 0
  local model = r.model or ui.model or ""
  local calls = ui.tool_calls or 0
  add(ui, "▣ " .. model .. " · " .. duration(secs) .. (calls > 0 and (" · %d tool call%s"):format(calls, calls == 1 and "" or "s") or "")
    .. (cancelled and " · interrupted" or ""), { gap = true, hl = "MaicFooter" })
  ui.response, ui.paused = nil, false
  if ui.float and ui.float.kind == "pause" then close_float(ui) end
  fire(ui, "MaicTurnEnd", { model = model, tool_calls = calls, seconds = secs, interrupted = cancelled })
  status(ui)
end
for _, kind in ipairs({ "response.completed", "response.failed", "response.incomplete", "maic.response.cancelled" }) do
  handlers[kind] = function(ui, e) finished(ui, e, kind) end
end

local function on_message(conn, msg)
  local ui = uis[conn.tab] or conn.first
  if msg.method == "maic.event" then
    local view, h = conn.views[msg.params.stream_id], handlers[msg.params.type]
    if view and h then h(view, msg.params) end
  elseif msg.method == "maic.index" then
    if msg.params.removed then conn.index[msg.params.removed] = nil end
    if msg.params.entry then conn.index[msg.params.entry.id] = msg.params.entry end
    if ui.conn == conn then status(ui) end
  elseif msg.method == "maic.engine" then
    if msg.params.tripped then notice(ui, "✗ the harness is TRIPPED: " .. (msg.params.reason or ""), "MaicError") end
    if msg.params.notice then notice(ui, msg.params.notice, msg.params.level == "error" and "MaicError" or nil) end
  elseif msg.id ~= nil and not msg.method then
    local cb = conn.waiting[msg.id]
    conn.waiting[msg.id] = nil
    if cb then cb(msg.result, msg.error) end
  end
end

-- ---------- requests from the person ----------

function show_result(ui, r, err)
  if err then
    notice(ui, "✗ " .. err_text(err), "MaicError")
    return
  end
  for i, l in ipairs(r.lines or {}) do notice(ui, l.text, l.level == "info" and "MaicNotice" or "MaicError", i == 1) end
  if r.ask then
    ui.asks[#ui.asks + 1] = { kind = "confirm", id = r.ask.id, e = r.ask }
    show_next(ui)
  end
  if r.send then U.submit(ui, r.send) end
end

-- An engine `:` command, its colon left out ("mode plan").
function U.command(ui, line)
  request(ui, "maic.session.command", { session = ui.session, line = line }, function(r, err) show_result(ui, r, err) end)
end

local function shell(ui, command)
  if ui.shell_running then
    notice(ui, "✗ a shell command is still running; <C-c> stops it", "MaicError")
    return
  end
  add(ui, "$ " .. command, { gap = true, hl = "MaicTool" })
  ui.shell = add(ui, "", { prefix = "  " })
  ui.shell_running = true
  request(ui, "maic.session.shell", { session = ui.session, command = command }, function(r, err)
    ui.shell_running = false
    if err then
      notice(ui, "✗ " .. err_text(err), "MaicError")
    else
      local b = ui.shell
      local last = vim.api.nvim_buf_get_lines(ui.conv, b.stop - 1, b.stop, false)[1]
      if b.stop - b.start > 1 and last == b.prefix then -- the output's own final newline
        edit(ui, function(buf) vim.api.nvim_buf_set_lines(buf, b.stop - 1, b.stop, false, {}) end)
        grow(ui, b, -1)
      end
      if r and r.exit_code and r.exit_code ~= 0 then append(ui, b, "\n[exit code " .. r.exit_code .. "]") end
    end
    fold(ui, ui.shell)
  end)
end

local function create(ui, text)
  request(ui, "response.create", { conversation = ui.session, input = text }, function(r, err)
    if err then
      notice(ui, "✗ " .. err_text(err), "MaicError")
    elseif r.maic and r.maic.queued then
      notice(ui, "queued: it runs when the turn before it ends")
    else
      ui.response = r.id
    end
  end)
end

-- Text from the input: `/cmd` an engine command (`//` a message starting with one slash), `!cmd` a shell command,
-- a message while a turn runs goes to it (OpenAI's response.steer), else a new turn.
function U.submit(ui, text)
  if text:match("^/[^/%s]") then return U.command(ui, text:sub(2)) end
  if text:match("^//") then text = text:sub(2) end
  if text:match("^!.") then return shell(ui, text:sub(2)) end
  if ui.response then
    request(ui, "response.steer", { previous_response_id = ui.response, input = text }, function(_, err)
      if err then return create(ui, text) end -- it ended meanwhile: this one starts the next turn
      if not ui.paused then notice(ui, "queued: it reaches the model at its next step") end
    end)
    return
  end
  create(ui, text)
end

-- Sends what the input holds and empties it.
function U.send(ui)
  ui = ui or U.here()
  if not ui then return end
  if not ui.session then
    vim.notify("maic.nvim: MAIC is still starting", vim.log.levels.INFO)
    return
  end
  local text = vim.trim(table.concat(vim.api.nvim_buf_get_lines(ui.input, 0, -1, false), "\n"))
  if text == "" then return end
  vim.api.nvim_buf_set_lines(ui.input, 0, -1, false, {})
  U.submit(ui, text)
end

-- Text into the input, after a blank line when it holds a draft; never sent.
function U.add_input(ui, text)
  local lines = vim.split(text, "\n", { plain = true })
  local have = vim.api.nvim_buf_get_lines(ui.input, 0, -1, false)
  if #have == 1 and have[1] == "" then
    vim.api.nvim_buf_set_lines(ui.input, 0, -1, false, lines)
  else
    table.insert(lines, 1, "")
    vim.api.nvim_buf_set_lines(ui.input, -1, -1, false, lines)
  end
end

-- Ctrl-C: a running `!cmd` stops, else the running (or paused) turn is cancelled.
function U.interrupt(ui)
  if ui.shell_running then return request(ui, "maic.session.shell", { session = ui.session, interrupt = true }) end
  if not ui.response then
    vim.notify("maic.nvim: nothing to interrupt (MAIC is idle)", vim.log.levels.INFO)
    return
  end
  request(ui, "cancelResponse", { response_id = ui.response }, function(_, err)
    if err then notice(ui, "✗ " .. err_text(err), "MaicError") end
  end)
end

-- A steering action on the running response or the paused turn; steer, drop and further take the input as their
-- note (or `note`).
function U.steer(ui, action, note)
  if not ui.response then
    vim.notify("maic.nvim: no turn to " .. action .. " (MAIC is idle)", vim.log.levels.INFO)
    return
  end
  if action == "steer" and not ui.paused and not note then
    vim.notify("maic.nvim: the turn is not paused; type a message to steer it", vim.log.levels.INFO)
    return
  end
  local from_input = false
  if not note and (action == "steer" or action == "drop" or action == "further") then
    local text = vim.trim(table.concat(vim.api.nvim_buf_get_lines(ui.input, 0, -1, false), "\n"))
    if text ~= "" then note, from_input = text, true end
  end
  if ui.float and ui.float.kind == "pause" then close_float(ui) end
  request(ui, "maic.steer", { session = ui.session, response_id = ui.response, action = action, note = note }, function(_, err)
    if err then
      notice(ui, "✗ " .. err_text(err), "MaicError")
      if ui.paused then show_next(ui) end
    elseif from_input then
      vim.api.nvim_buf_set_lines(ui.input, 0, -1, false, {})
    end
  end)
end

-- :MaicSteer ACTION [NOTE]
function U.steer_command(fargs)
  local ui = U.here()
  if not ui then
    vim.notify("maic.nvim: :MaicSteer steers the interface's turn; no interface in this tab", vim.log.levels.WARN)
    return
  end
  local note = #fargs > 1 and table.concat(fargs, " ", 2) or nil
  U.steer(ui, fargs[1], note)
end

-- ---------- windows ----------

local function scratch(name, filetype)
  local buf = vim.api.nvim_create_buf(false, true)
  vim.bo[buf].bufhidden = "hide"
  vim.bo[buf].swapfile = false
  pcall(vim.api.nvim_buf_set_name, buf, name)
  vim.bo[buf].filetype = filetype
  if not pcall(vim.treesitter.start, buf, "markdown") then vim.bo[buf].syntax = "markdown" end
  return buf
end

local function window_opts(ui, w)
  local wo = vim.wo[w]
  wo.wrap, wo.linebreak, wo.number, wo.relativenumber, wo.signcolumn = true, true, false, false, "no"
  wo.foldmethod, wo.foldtext, wo.foldenable = "manual", "v:lua.require'maic.ui'.foldtext()", true
  refold(ui, w)
  status(ui)
end

-- The conversation (in `win`, or a window of the configured layout) and the input under it.
local function show(ui, win)
  local c = maic().config
  local conv = win or vim.fn.win_findbuf(ui.conv)[1]
  if conv and vim.api.nvim_win_get_tabpage(conv) ~= vim.api.nvim_get_current_tabpage() then conv = nil end
  if conv then
    vim.api.nvim_win_set_buf(conv, ui.conv)
  else
    conv = maic().make_window(ui.conv)
  end
  window_opts(ui, conv)
  local input
  for _, w in ipairs(vim.fn.win_findbuf(ui.input)) do
    if vim.api.nvim_win_get_tabpage(w) == vim.api.nvim_get_current_tabpage() then input = w end
  end
  if not input then
    local cfg = vim.api.nvim_win_get_config(conv)
    if cfg.relative ~= "" then
      local h = c.input_height
      cfg.height = math.max(cfg.height - h - 2, 3)
      vim.api.nvim_win_set_config(conv, cfg)
      input = vim.api.nvim_open_win(ui.input, true, { relative = "editor", row = cfg.row + cfg.height + 2, col = cfg.col, width = cfg.width, height = h, border = "rounded" })
    else
      input = vim.api.nvim_open_win(ui.input, true, { split = "below", win = conv, height = c.input_height })
    end
  end
  vim.wo[input].winfixheight = true
  vim.api.nvim_set_current_win(input)
  return conv, input
end

local function set_keys(buf, which)
  if vim.g.maic_keymap_check == 1 then return end
  for _, k in ipairs(maic().buffer_planned(which)) do maic().map(k, buf) end
end

local function engine_args(args)
  local c = maic().config
  local cmd = type(c.cmd) == "table" and vim.deepcopy(c.cmd) or { c.cmd }
  cmd[#cmd + 1] = "--rpc"
  vim.list_extend(cmd, c.args or {})
  vim.list_extend(cmd, args or {})
  return cmd
end

-- A view of one session: its conversation buffer, beside the tab's one input.
local function new_view(conn)
  local c = maic().config
  conn.made = conn.made + 1
  local ui = { conn = conn, tab = conn.tab, input = conn.input, blocks = {}, by_id = {}, empty = true, asks = {}, approvals = {}, tool_calls = 0 }
  ui.conv = scratch("maic://conversation/" .. conn.tab .. (conn.made > 1 and ("/" .. conn.made) or ""), c.filetypes.conversation)
  vim.bo[ui.conv].modifiable = false
  vim.api.nvim_create_autocmd("BufWinEnter", { buffer = ui.conv, callback = function()
    window_opts(ui, vim.api.nvim_get_current_win())
  end })
  vim.api.nvim_create_autocmd("CursorMoved", { buffer = ui.conv, callback = function()
    if ui.marker and vim.api.nvim_win_get_cursor(0)[1] == 1 then U.older(ui) end
  end })
  set_keys(ui.conv, "conversation")
  return ui
end

-- The view follows session `id` from its snapshot on: the last exchanges, what is in flight, what waits.
local function attach(ui, id, done)
  ui.session = id
  ui.conn.views[id] = ui
  request(ui, "maic.session.attach", { session = id, exchanges = 3 }, function(r, aerr)
    if aerr then return notice(ui, "✗ " .. err_text(aerr), "MaicError") end
    local entry = r.entry or {}
    ui.workspace, ui.model, ui.mode, ui.title, ui.activity = entry.workspace, entry.model, entry.mode, entry.title, entry.activity
    ui.response = entry.response
    for _, item in ipairs(r.items or {}) do render_item(ui, item) end
    render_inflight(ui, r.inflight)
    ui.oldest, ui.more_before = r.items and r.items[1] and r.items[1].id, r.more_before
    set_marker(ui, ui.more_before and ui.oldest ~= nil)
    for _, a in ipairs(r.pending or {}) do handlers["maic.approval.requested"](ui, a) end
    for _, q in ipairs(r.questions or {}) do handlers["maic.question.asked"](ui, q) end
    status(ui)
    if done then done() end
  end)
end

-- This tab shows `ui` now, in the window that showed the session it left; a view whose session ended goes.
local function focus_view(ui)
  local old = uis[ui.tab]
  local win
  if old and old ~= ui then
    close_float(old)
    win = vim.fn.win_findbuf(old.conv)[1]
  end
  uis[ui.tab] = ui
  show(ui, win)
  if old and old ~= ui and old.ended then drop_view(old) end
  show_next(ui)
  status(ui)
end

-- Starts the engine and opens a session in it (or resumes `o.session`), in this tab: `o.win` holds the
-- conversation, else a window of the configured layout.
function U.start(args, o)
  o = o or {}
  local c = maic().config
  local tab = vim.api.nvim_get_current_tabpage()
  local conn = { tab = tab, waiting = {}, next = 1, partial = "", stderr = {}, views = {}, index = {}, made = 0 }
  conn.input = scratch("maic://input/" .. tab, c.filetypes.input)
  set_keys(conn.input, "input")
  local ui = new_view(conn)
  conn.first = ui
  uis[tab] = ui
  show(ui, o.win)
  local cmd = engine_args(args)
  conn.job = vim.fn.jobstart(cmd, {
    cwd = vim.fn.getcwd(),
    on_stdout = function(_, data)
      data[1] = conn.partial .. data[1]
      conn.partial = table.remove(data)
      for _, line in ipairs(data) do
        if line ~= "" then
          local ok, msg = pcall(vim.json.decode, line, { luanil = { object = true, array = true } })
          if ok and type(msg) == "table" then on_message(conn, msg) end
        end
      end
    end,
    on_stderr = function(_, data)
      for _, l in ipairs(data) do
        if l ~= "" then conn.stderr[#conn.stderr + 1] = l end
      end
      while #conn.stderr > 40 do table.remove(conn.stderr, 1) end
    end,
    on_exit = function(_, code)
      conn.job = nil
      local here = uis[tab] and uis[tab].conn == conn and uis[tab] or ui
      if not vim.api.nvim_buf_is_valid(here.conv) then return end
      local tail = table.concat(conn.stderr, "\n")
      notice(here, ("the engine exited (code %d)%s"):format(code, tail ~= "" and (":\n" .. tail) or ""), code == 0 and "MaicNotice" or "MaicError")
      if not here.session then notice(here, "`maic --rpc` did not start; :MaicTerminal runs MAIC's own TUI (or ui = \"terminal\" in setup())") end
      status(here)
    end,
  })
  if conn.job <= 0 then
    conn.job = nil
    uis[tab] = nil
    vim.notify("maic.nvim: cannot start " .. table.concat(cmd, " "), vim.log.levels.ERROR)
    return
  end
  local v = vim.version()
  local hello = { protocol = 1, client = { name = "maic.nvim", version = ("nvim %d.%d.%d"):format(v.major, v.minor, v.patch) },
    capabilities = { "tool_output", "autocmds" }, view = { collapse_over = 0 } }
  request(ui, "maic.hello", hello, function(_, err)
    if err then return notice(ui, "✗ " .. err_text(err), "MaicError") end
    request(ui, "maic.index.subscribe")
    request(ui, "maic.index.get", nil, function(r)
      for _, e in ipairs(r and r.entries or {}) do conn.index[e.id] = e end
    end)
    if o.session then
      request(ui, "maic.session.resume", { session = o.session }, function(r, rerr)
        if rerr then return notice(ui, "✗ " .. err_text(rerr), "MaicError") end
        attach(ui, (r.entry and r.entry.id) or r.id or o.session)
      end)
    else
      -- The workspace is said, not assumed: under a daemon the engine's own directory is not this nvim's.
      request(ui, "createConversation", { maic = { workspace = vim.fn.getcwd() } }, function(r, cerr)
        if cerr then return notice(ui, "✗ " .. err_text(cerr), "MaicError") end
        attach(ui, r.id)
      end)
    end
  end)
  vim.cmd("startinsert")
  return ui
end

-- ---------- sessions: :MaicNew, :MaicSwitch, :MaicFork, :MaicBg, :MaicPark, :MaicStop ----------

-- One line about a session: its title (or id), what it is doing, its model and where it works.
local function session_line(e)
  -- `waiting` on a parent also covers its tasks: one of them waits for an approval.
  local waiting = type(e.waiting) == "table" and e.waiting or nil
  local doing = e.state == "parked" and (e.unseen and "finished, parked" or "parked") or (e.activity == "waiting" or waiting) and "waiting" or (e.activity and e.activity ~= "idle") and "working"
    or e.unseen and "finished" or "idle"
  if doing == "waiting" and waiting then doing = doing .. ": " .. (waiting.summary or waiting.kind or "") .. (waiting.session and " (in a task)" or "") end
  local ws = vim.fn.fnamemodify(e.workspace or "", ":~")
  return ("%s%s  ·  %s  ·  %s  ·  %s"):format(e.kind == "sub" and "↳ " or "", (e.title and e.title ~= "") and e.title or e.id, doing, e.model or "", ws)
end

-- An id, a unique id prefix or a title (any letter case) among the engine's sessions; else what was typed, for
-- maic.session.resume to find among the transcripts.
local function resolve(ui, given)
  if ui.conn.index[given] then return given end
  local hits = {}
  for id, e in pairs(ui.conn.index) do
    if id:sub(1, #given) == given or (e.title or ""):lower() == given:lower() then hits[#hits + 1] = id end
  end
  return #hits == 1 and hits[1] or given
end

-- Moves this tab's focus: to a new session ("new"), a fork of this one ("fork"), or session `target` ("to"),
-- loaded or parked. `as` says what happens to the one left (the leave.switch setting when nil, which may be to ask:
-- the engine answers maic_leave_ask); parking or stopping it mid-turn interrupts the turn, so that is asked first.
-- `after` runs on the view it lands in.
function U.go(ui, how, target, as, sure, after)
  as = as or "default"
  if not sure and ui.response and (as == "park" or as == "stop") then
    return vim.ui.select({ "yes", "no" }, { prompt = as .. " a working session? Its turn is interrupted." }, function(pick)
      if pick == "yes" then U.go(ui, how, target, as, true, after) end
    end)
  end
  local conn, leave = ui.conn, { as = as }
  local function done(r, err)
    if err and err.data and err.data.code == "maic_leave_ask" then
      return vim.ui.select({ "bg", "park", "stop" }, { prompt = "leave this session (bg: it keeps working, park: it stops for now, stop: it ends)" }, function(pick)
        if pick then U.go(ui, how, target, pick, sure, after) end
      end)
    end
    if err then return notice(ui, "✗ " .. err_text(err), "MaicError") end
    local view = conn.views[r.id]
    local function land()
      focus_view(view)
      if after then after(view) end
    end
    if view then return land() end
    view = new_view(conn)
    attach(view, r.id, land)
  end
  if how == "new" then
    request(ui, "createConversation", { maic = { workspace = ui.workspace, leave = leave } }, done)
  elseif how == "fork" then
    request(ui, "maic.session.fork", { session = ui.session, leave = leave }, done)
  elseif conn.index[target] and conn.index[target].state ~= "parked" then
    request(ui, "maic.session.focus", { session = target, leave = leave }, done)
  else
    request(ui, "maic.session.resume", { session = target, leave = leave }, done)
  end
end

-- The switcher: every other session the engine holds, and a new one; a task under its parent, this session's first.
function U.switcher(ui, prompt, as)
  local top, tasks = {}, {}
  for id, e in pairs(ui.conn.index) do
    local parent = e.kind == "sub" and type(e.parent) == "string" and (e.parent == ui.session or ui.conn.index[e.parent]) and e.parent or nil
    if parent then
      tasks[parent] = tasks[parent] or {}
      table.insert(tasks[parent], e)
    elseif id ~= ui.session then
      top[#top + 1] = e
    end
  end
  local function newest(a, b) return (a.last_activity or "") > (b.last_activity or "") end
  table.sort(top, function(a, b)
    if (a.state == "parked") ~= (b.state == "parked") then return b.state == "parked" end
    return newest(a, b)
  end)
  local list = { { new = true } }
  local function add(id, e)
    if e then list[#list + 1] = e end
    table.sort(tasks[id] or {}, newest)
    for _, t in ipairs(tasks[id] or {}) do list[#list + 1] = t end
  end
  add(ui.session)
  for _, e in ipairs(top) do add(e.id, e) end
  vim.ui.select(list, { prompt = prompt, format_item = function(e)
    return e.new and ("+ a new session in " .. vim.fn.fnamemodify(ui.workspace or "", ":~")) or session_line(e)
  end }, function(e)
    if e then U.go(ui, e.new and "new" or "to", e.id, as) end
  end)
end

-- :MaicPark ID or :MaicStop ID: another session ends, asked first when it is mid-turn.
local function end_other(ui, id, verb, sure)
  local e = ui.conn.index[id]
  if not sure and e and e.state ~= "parked" and e.activity ~= "idle" then
    return vim.ui.select({ "yes", "no" }, { prompt = verb .. " " .. session_line(e) .. "? Its turn is interrupted." }, function(pick)
      if pick == "yes" then end_other(ui, id, verb, true) end
    end)
  end
  request(ui, "maic.session." .. verb, { session = id, interrupt = true }, function(r, err)
    if err then return notice(ui, "✗ " .. err_text(err), "MaicError") end
    notice(ui, (verb == "park" and "parked " or "stopped ") .. session_line(r) .. (verb == "park" and "  (:MaicSwitch resumes it)" or "  (maic -r resumes it)"))
  end)
end

-- :MaicNew [DIR], :MaicSwitch [ID], :MaicFork, :MaicBg, :MaicPark [ID], :MaicStop [ID]; --bg, --park or --stop
-- names what happens to the session left, this once.
function U.session_command(verb, fargs)
  local ui = U.here()
  if not ui or not ui.session then
    vim.notify("maic.nvim: no interface in this tab (:Maic starts one)", vim.log.levels.WARN)
    return
  end
  local as, rest = nil, {}
  for _, a in ipairs(fargs or {}) do
    if a == "--bg" or a == "--park" or a == "--stop" then as = a:sub(3) else rest[#rest + 1] = a end
  end
  local target = table.concat(rest, " ")
  if verb == "new" then return U.go(ui, "new", nil, as, false, target ~= "" and function(view) U.command(view, "cd " .. target) end or nil) end
  if verb == "fork" then return U.go(ui, "fork", nil, as) end
  if verb == "switch" and target ~= "" then return U.go(ui, "to", resolve(ui, target), as) end
  if verb == "switch" then return U.switcher(ui, "switch to", as) end
  if verb == "bg" then return U.switcher(ui, "background this session, and go to", "bg") end
  local id = target == "" and ui.session or resolve(ui, target)
  if id == ui.session then return U.switcher(ui, verb .. " this session, and go to", verb) end
  end_other(ui, id, verb)
end

-- :Maic in the interface: focus this tab's input (a waiting float first), showing its windows again if hidden, or
-- start one with `args`.
function U.open(args)
  local ui = U.here()
  if not ui then return U.start(args) end
  show(ui)
  if ui.float and vim.api.nvim_win_is_valid(ui.float.win) then vim.api.nvim_set_current_win(ui.float.win) end
end

-- :MaicToggle: hides the conversation and the input (the engine keeps running), or shows them.
function U.toggle()
  local ui = U.here()
  local wins = {}
  for _, buf in ipairs({ ui.conv, ui.input }) do
    for _, w in ipairs(vim.fn.win_findbuf(buf)) do
      if vim.api.nvim_win_get_tabpage(w) == vim.api.nvim_get_current_tabpage() then wins[#wins + 1] = w end
    end
  end
  if #wins > 0 and #wins < #vim.api.nvim_tabpage_list_wins(0) then
    close_float(ui)
    for _, w in ipairs(wins) do vim.api.nvim_win_hide(w) end
  else
    U.open()
  end
end

-- `maic --ui nvim`: the interface in the whole of the first tab; quitting nvim ends the engine.
function U.main(o)
  if not maic().opts then maic().setup({}) end
  if o.cmd then maic().config.cmd = o.cmd end
  return U.start(o.args, { win = vim.api.nvim_get_current_win(), session = o.session })
end

-- For tests and statuslines: the interface of this tab as plain values.
function U.state(ui)
  ui = ui or U.here()
  if not ui then return nil end
  return { job = ui.conn.job, session = ui.session, response = ui.response, paused = ui.paused, activity = ui.activity, mode = ui.mode, model = ui.model,
    conversation = ui.conv, input = ui.input, float = ui.float and { kind = ui.float.kind, win = ui.float.win, buf = ui.float.buf } or nil }
end

return U
