-- maic.nvim's view of the user's mappings: who holds a key, in which scope, from which script. setup() asks it
-- before it sets a key (:h maic-never-overwrite).
local K = {}

local mode_names = { n = "normal", x = "visual", v = "visual", s = "select", o = "operator", i = "insert", c = "command line", t = "terminal" }

function K.mode_name(mode)
  return mode_names[mode] or mode
end

-- The bytes a key stands for, <leader> and <localleader> expanded as nvim does when a mapping is made.
function K.expand(lhs)
  local leader, localleader = vim.g.mapleader or "\\", vim.g.maplocalleader or "\\"
  lhs = lhs:gsub("<[Ll][Oo][Cc][Aa][Ll][Ll][Ee][Aa][Dd][Ee][Rr]>", function() return localleader end)
  lhs = lhs:gsub("<[Ll][Ee][Aa][Dd][Ee][Rr]>", function() return leader end)
  return vim.api.nvim_replace_termcodes(lhs, true, true, true)
end

-- A key sequence as a list of key names (keytrans), so <C-w> typed as \23 and kept as a modifier match.
local function tokens(raw)
  local s, out, i = vim.fn.keytrans(raw), {}, 1
  while i <= #s do
    local j = s:sub(i, i) == "<" and s:find(">", i + 1, true) or i
    out[#out + 1] = s:sub(i, j)
    i = j + 1
  end
  return out
end

-- True when one key sequence starts with the other.
local function overlap(a, b)
  for i = 1, math.min(#a, #b) do
    if a[i] ~= b[i] then return false end
  end
  return true
end

-- The maps in `mode` that hold `lhs` exactly or share a prefix with it either way (the ones mapcheck() finds):
-- buffer-local ones of `buf` first, then the global ones. `skip(m)` leaves a map out.
function K.holders(mode, lhs, buf, skip)
  local want, found = tokens(K.expand(lhs)), {}
  local function scan(list, local_)
    for _, m in ipairs(list) do
      local have = tokens(m.lhsraw or K.expand(m.lhs))
      if overlap(have, want) and not (skip and skip(m)) then
        found[#found + 1] = { map = m, exact = #have == #want, buffer = local_ }
      end
    end
  end
  if buf then scan(vim.api.nvim_buf_get_keymap(buf, mode), true) end
  scan(vim.api.nvim_get_keymap(mode), false)
  return found
end

-- What a mapping does, as a person names it: its description or right-hand side (and its own key, when it only
-- shares a prefix with the one asked about).
function K.what(h)
  local m = h.map or h
  local what = (m.desc and m.desc ~= "") and ('"' .. m.desc .. '"') or (m.rhs and m.rhs ~= "") and m.rhs or "a Lua function"
  if h.map and not h.exact then what = vim.fn.keytrans(m.lhsraw or m.lhs) .. " " .. what end
  return what
end

-- K.what plus the script and line that set it.
function K.describe(h)
  local m = h.map or h
  local what = K.what(h)
  local where
  if m.sid and m.sid > 0 then
    local ok, info = pcall(vim.fn.getscriptinfo, { sid = m.sid })
    if ok and info[1] then
      -- nvim's own Lua (vim/_core/defaults.lua) is named relative to its runtime; a user's file is absolute.
      local name = info[1].name
      if name:sub(1, 1) == "/" then name = vim.fn.fnamemodify(name, ":~") else name = "nvim's " .. name end
      where = name .. ((m.lnum or 0) > 0 and (":" .. m.lnum) or "")
    end
  elseif m.sid == -8 then
    where = "Lua"
  end
  return what .. (where and (" from " .. where) or "") .. ((h.buffer or (m.buffer or 0) ~= 0) and " (buffer-local)" or "")
end

-- maic.nvim's own maps, which every check leaves out.
function K.ours(m)
  return m.desc ~= nil and vim.startswith(m.desc, "MAIC: ")
end

-- Whether a key maic.nvim wants is held: nil when free, else { holder, exact, shadow }. A global key (`buf` nil)
-- is held by any global map sharing a prefix with it. A buffer-local one (in `buf`, set with nowait) is held by a
-- buffer-local map there; a global map under it is only shadowed in that buffer (shadow = true).
function K.clash(mode, lhs, buf)
  local check = function() return vim.fn.mapcheck(lhs, mode) end
  if (buf and vim.api.nvim_buf_call(buf, check) or check()) == "" then return nil end
  local shadowed
  for _, h in ipairs(K.holders(mode, lhs, buf, K.ours)) do
    if not buf or h.buffer then return { holder = K.describe(h), exact = h.exact } end
    shadowed = shadowed or h
  end
  if shadowed then return { holder = K.describe(shadowed), exact = shadowed.exact, shadow = true } end
end

-- ---------- the check behind :checkhealth maic and `maic nvim keymaps` ----------

-- maic.nvim itself: the user's, set up, when require finds it; else the copy beside this file (the CLI loads this
-- file by path, and the user's config may not have maic.nvim at all).
local function plugin()
  local ok, maic = pcall(require, "maic")
  if ok and type(maic) == "table" and maic.planned and maic.buffer_planned then return maic end
  return dofile(debug.getinfo(1, "S").source:sub(2):match("(.*)/") .. "/init.lua")
end

local function term_start(cmd)
  if vim.fn.has("nvim-0.11") == 1 then return vim.fn.jobstart(cmd, { term = true }) end
  return vim.fn.termopen(cmd)
end

-- A tab with three buffers, for the buffer-local maps the user's config gives each kind: a plain file, a scratch
-- buffer of MAIC's input filetype and a terminal of MAIC's terminal filetype. Returns them and a cleanup.
local function scratch(filetypes)
  local back = vim.api.nvim_get_current_tabpage()
  vim.cmd("tabnew")
  local file = vim.fn.tempname() .. ".txt"
  vim.cmd("edit " .. vim.fn.fnameescape(file))
  local b = { plain = vim.api.nvim_get_current_buf(), input = vim.api.nvim_create_buf(false, true), term = vim.api.nvim_create_buf(false, false) }
  vim.bo[b.input].filetype = filetypes.input
  vim.api.nvim_win_set_buf(0, b.term)
  vim.bo[b.term].filetype = filetypes.terminal
  local job = term_start({ "sh", "-c", "sleep 60" })
  return b, function()
    pcall(vim.fn.jobstop, job)
    for _, buf in pairs(b) do pcall(vim.api.nvim_buf_delete, buf, { force = true }) end
    if vim.api.nvim_tabpage_is_valid(back) then vim.api.nvim_set_current_tabpage(back) end
    vim.cmd("stopinsert")
    vim.fn.delete(file)
  end
end

-- MAIC's own input in its terminal: the keys it needs and what for.
local maic_keys = {
  { "<Esc>", "normal mode in the input" },
  { "<C-w>", "window moves (Ctrl-W k / j) and deleting a word" },
  { "<C-p>", "the prompt history" },
  { "<S-Tab>", "cycling the mode" },
  { "<C-z>", "suspending to the shell" },
  { "<C-c>", "interrupting, clearing the input, quitting" },
  { "<M-CR>", "sending" },
}

-- llama.vim's keys (autoload/llama.vim): its defaults, the older option names it still reads, and when each is
-- mapped. "insert": buffer-local in every buffer from an InsertEnter autocmd; "shown": buffer-local only while a
-- suggestion shows (fim_render maps them, fim_hide runs iunmap <buffer>); "always": global from llama#enable().
local llama_defaults = {
  keymap_fim_trigger = "<leader>llf", keymap_fim_accept_full = "<Tab>", keymap_fim_accept_line = "<S-Tab>",
  keymap_fim_accept_word = "<leader>ll]", keymap_fim_next = "<C-L>", keymap_fim_prev = "<C-H>",
  keymap_debug_toggle = "<leader>lld", keymap_inst_trigger = "<leader>lli", keymap_inst_rerun = "<leader>llr",
  keymap_inst_continue = "<leader>llc", keymap_inst_accept = "<Tab>", keymap_inst_cancel = "<Esc>",
}
local llama_old = { keymap_trigger = "keymap_fim_trigger", keymap_accept_full = "keymap_fim_accept_full",
  keymap_accept_line = "keymap_fim_accept_line", keymap_accept_word = "keymap_fim_accept_word", keymap_debug = "keymap_debug_toggle" }
local llama_keys = {
  { "keymap_fim_trigger", "i", "insert", "asks for a suggestion" },
  { "keymap_fim_accept_full", "i", "shown", "accepts the suggestion" },
  { "keymap_fim_accept_line", "i", "shown", "accepts its first line" },
  { "keymap_fim_accept_word", "i", "shown", "accepts its first word" },
  { "keymap_fim_next", "i", "shown", "the next suggestion" },
  { "keymap_fim_prev", "i", "shown", "the previous suggestion" },
  { "keymap_debug_toggle", "n", "always", "the debug pane" },
  { "keymap_inst_trigger", "x", "always", "an instruction for the selection" },
  { "keymap_inst_rerun", "n", "always", "reruns an instruction" },
  { "keymap_inst_continue", "n", "always", "continues an instruction" },
  { "keymap_inst_accept", "n", "always", "accepts an instruction, else feeds <Tab> (Ctrl-I, jump forward)" },
  { "keymap_inst_cancel", "n", "always", "cancels an instruction, else does nothing" },
}

local function llama_own(m)
  local rhs = m.rhs or ""
  return rhs:find("llama#", 1, true) ~= nil or rhs:find("LlamaInstruct", 1, true) ~= nil
end

local function llama_config()
  if vim.g.llama_config == nil and #vim.api.nvim_get_runtime_file("autoload/llama.vim", false) == 0 then return nil end
  local c = vim.deepcopy(llama_defaults)
  for k, v in pairs(vim.g.llama_config or {}) do c[llama_old[k] or k] = v end
  return c
end

local function shares_prefix(a, b)
  return overlap(tokens(K.expand(a)), tokens(K.expand(b)))
end

-- Every key maic.nvim sets, the keys MAIC's terminal input needs and llama.vim's keys, against the user's maps:
-- { sections = { { name, items = { { level = "ok"|"info"|"warn"|"error", text, hint, id } } } }, hash }. A warn or
-- error is a collision; `id` names it without line numbers, and `hash` is the sha256 of the sorted ids.
function K.check()
  local maic = plugin()
  local c = maic.config
  local b, cleanup = scratch(c.filetypes or { input = "maic-input", terminal = "maic" })
  local ok, sections = pcall(function()
    local out = {}
    local function section(name)
      local s = { name = name, items = {} }
      out[#out + 1] = s
      return function(level, text, hint, id)
        s.items[#s.items + 1] = { level = level, text = text, hint = hint, id = id and (name .. "|" .. id) or nil }
      end
    end
    local function key(k) return ("%s (%s)"):format(k.lhs, K.mode_name(k.mode)) end

    local add = section("maic.nvim's keys")
    local planned = maic.planned(maic.opts)
    if #planned == 0 then add("info", "no global keymaps (keymaps = false)") end
    for _, k in ipairs(planned) do
      local hs = K.holders(k.mode, k.lhs, b.plain, K.ours)
      local h = hs[1]
      if not h then
        add("ok", key(k) .. ": " .. k.desc)
      elseif h.buffer then
        add("warn", key(k) .. " is shadowed in ordinary buffers by " .. K.describe(h), "a FileType or BufEnter autocmd sets it; keymaps." .. k.name .. " = '<another key>' in setup()",
          key(k) .. "|" .. K.what(h))
      elseif k.explicit then
        add("warn", key(k) .. ", your keymaps." .. k.name .. ", shadows " .. K.describe(h), "pick another key if you want that mapping back", key(k) .. "|" .. K.what(h))
      else
        add("warn", key(k) .. " is held by " .. K.describe(h) .. ", so maic.nvim leaves it alone", "keymaps." .. k.name .. " = '<another key>' (or false) in setup()",
          key(k) .. "|" .. K.what(h))
      end
    end
    -- MAIC's own buffers: its terminal, and for the normal-mode keys the input buffer of nvim as MAIC's interface.
    for _, k in ipairs(maic.buffer_planned()) do
      local bufs = { { b.term, "MAIC's terminal" } }
      if k.mode ~= "t" then bufs[2] = { b.input, "a " .. c.filetypes.input .. " buffer" } end
      for _, pair in ipairs(bufs) do
        local where = key(k) .. " in " .. pair[2]
        local h = K.holders(k.mode, k.lhs, pair[1], K.ours)[1]
        if h and h.buffer then
          add("warn", where .. " is held there by " .. K.describe(h) .. ", so maic.nvim leaves it alone",
            "skip MAIC's buffers in that autocmd: if vim.bo[ev.buf].filetype == '" .. vim.bo[pair[1]].filetype .. "' then return end", where .. "|" .. K.what(h))
        elseif h then
          add("ok", where .. ": " .. k.desc .. "; your global " .. K.what(h) .. " stays everywhere else")
        else
          add("ok", where .. ": " .. k.desc)
        end
      end
    end

    add = section("MAIC's terminal input")
    local through = {}
    for lhs, on in pairs(c.terminal_passthrough or {}) do
      if on then through[K.expand(lhs)] = true end
    end
    for _, mk in ipairs(maic_keys) do
      local lhs, use = mk[1], mk[2]
      local hs = K.holders("t", lhs, b.term, K.ours)
      local h = hs[1]
      local label = lhs .. " (" .. use .. ")"
      if not h then
        add("ok", label .. " reaches MAIC")
      elseif h.buffer then
        add("error", "MAIC never gets " .. label .. " in its terminal: " .. K.describe(h) .. " holds it there",
          "skip MAIC's terminal in that TermOpen autocmd: if vim.bo[ev.buf].filetype == '" .. c.filetypes.terminal .. "' then return end", lhs .. "|" .. K.what(h))
      elseif through[K.expand(lhs)] then
        add("ok", label .. " reaches MAIC (passed through in its terminal); " .. K.describe(h) .. " stays everywhere else")
      elseif h.exact then
        add("error", "MAIC never gets " .. label .. " in its terminal: " .. K.describe(h) .. " holds it",
          "terminal_passthrough = { [\"" .. lhs .. "\"] = true } in setup() sends it to MAIC there", lhs .. "|" .. K.what(h))
      else
        add("warn", "MAIC gets " .. label .. " only after 'timeoutlen': " .. K.describe(h) .. " starts with it",
          "terminal_passthrough = { [\"" .. lhs .. "\"] = true } in setup() sends it at once", lhs .. "|" .. K.what(h))
      end
    end

    add = section("llama.vim")
    local lc = llama_config()
    if not lc then
      add("info", "llama.vim is not installed (no g:llama_config, no autoload/llama.vim on 'runtimepath')")
    else
      for _, lk in ipairs(llama_keys) do
        local name, mode, when, does = lk[1], lk[2], lk[3], lk[4]
        local lhs = lc[name]
        if type(lhs) == "string" and lhs ~= "" then
          local k = { lhs = lhs, mode = mode }
          local where = key(k) .. ", " .. name .. ", " .. does
          local first = K.expand(lhs):byte(1)
          local hs = K.holders(mode, lhs, b.plain, function(m) return llama_own(m) end)
          local h = hs[1]
          local found = false
          if mode == "i" and first and first >= 0x20 and first < 0x7f then
            found = true
            add("warn", where .. ": it starts with " .. vim.fn.keytrans(string.char(first)) .. ", so typing " .. vim.fn.keytrans(string.char(first))
              .. " in insert mode waits 'timeoutlen' (or the next key) before it appears" .. (when == "insert" and ", in every buffer" or " while a suggestion shows"),
              "a chord that types nothing: " .. name .. " = '" .. (name == "keymap_fim_accept_word" and "<M-]>" or "<M-f>") .. "' (or another free <M-...>) in g:llama_config",
              key(k) .. "|starts with a printable key")
          end
          for _, mk in ipairs(planned) do
            if mk.mode == mode and shares_prefix(mk.lhs, lhs) then
              found = true
              add("warn", where .. " collides with maic.nvim's keymaps." .. mk.name .. " " .. mk.lhs, "move one of them", key(k) .. "|maic.nvim " .. mk.name)
            end
          end
          if h and when == "shown" and h.buffer then
            add("warn", where .. ": while a suggestion shows llama.vim maps it in the buffer, and hiding the suggestion runs iunmap <buffer>, which also removes "
              .. K.describe(h), "map that key globally, or give llama.vim another key", key(k) .. "|" .. K.what(h))
          elseif h and when == "shown" then
            add("ok", where .. ": mapped in the buffer only while a suggestion shows; the rest of the time " .. K.describe(h) .. " has it")
          elseif h then
            add("warn", where .. ": " .. K.describe(h) .. (h.exact and " has it too; " .. (when == "always" and "whichever loads last wins" or "llama.vim's buffer-local map wins in insert mode")
              or " shares a prefix with it, so one of them waits 'timeoutlen'"), name .. " = '<another key>' (or '') in g:llama_config", key(k) .. "|" .. K.what(h))
          elseif not found then
            add("ok", where .. (when == "shown" and " (mapped only while a suggestion shows)" or ""))
          end
        end
      end
    end
    return out
  end)
  cleanup()
  if not ok then error(sections, 0) end
  local ids = {}
  for _, s in ipairs(sections) do
    for _, it in ipairs(s.items) do
      if it.id then ids[#ids + 1] = it.id end
    end
  end
  table.sort(ids)
  local v = vim.version()
  return { sections = sections, hash = vim.fn.sha256(table.concat(ids, "\n")), nvim = ("%d.%d.%d"):format(v.major, v.minor, v.patch) }
end

-- `maic nvim keymaps`: run headless inside the user's config (with g:maic_keymap_check set), after User VeryLazy
-- so lazy-loaded plugins have mapped their keys; writes the report as JSON to `out` and quits.
function K.headless(out)
  pcall(vim.api.nvim_exec_autocmds, "User", { pattern = "VeryLazy", modeline = false })
  vim.defer_fn(function()
    local ok, report = pcall(K.check)
    local f = assert(io.open(out, "w"))
    f:write(vim.json.encode(ok and report or { error = tostring(report) }))
    f:close()
    vim.cmd("qa!")
  end, 100)
end

return K
