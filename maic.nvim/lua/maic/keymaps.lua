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

-- The maps in `mode` that hold `lhs` exactly or share a prefix with it either way (the ones mapcheck() finds):
-- buffer-local ones of `buf` first, then the global ones. `skip(m)` leaves a map out.
function K.holders(mode, lhs, buf, skip)
  local want, found = K.expand(lhs), {}
  local function scan(list, local_)
    for _, m in ipairs(list) do
      local r = m.lhsraw or K.expand(m.lhs)
      if (vim.startswith(r, want) or vim.startswith(want, r)) and not (skip and skip(m)) then
        found[#found + 1] = { map = m, exact = r == want, buffer = local_ }
      end
    end
  end
  if buf then scan(vim.api.nvim_buf_get_keymap(buf, mode), true) end
  scan(vim.api.nvim_get_keymap(mode), false)
  return found
end

-- A mapping as a person names it: its description or right-hand side, and the script and line that set it.
function K.describe(h)
  local m = h.map or h
  local what = (m.desc and m.desc ~= "") and ('"' .. m.desc .. '"') or (m.rhs and m.rhs ~= "") and m.rhs or "a Lua function"
  if h.map and not h.exact then what = vim.fn.keytrans(m.lhsraw or m.lhs) .. " " .. what end
  local where
  if m.sid and m.sid > 0 then
    local ok, info = pcall(vim.fn.getscriptinfo, { sid = m.sid })
    if ok and info[1] then where = vim.fn.fnamemodify(info[1].name, ":~") .. ((m.lnum or 0) > 0 and (":" .. m.lnum) or "") end
  elseif m.sid == -8 then
    where = "Lua (nvim -V1 names the file)"
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

return K
