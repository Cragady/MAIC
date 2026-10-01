-- maic.nvim's own tests: nvim --headless -u NONE -i NONE -n -l maic.nvim/tests/maic_test.lua
-- Exit 0 when every check passes. Run by MAIC's ctest `nvim` (cli/tests/nvim_test.cpp).
vim.g.mapleader = " "
local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h")
vim.opt.rtp:prepend(root)
vim.cmd("runtime plugin/maic.lua")
vim.cmd("filetype on")
local maic = require("maic")

local failures = 0
local function expect(ok, what)
  io.write((ok and "  ok    " or "  FAIL  ") .. what .. "\n")
  if not ok then failures = failures + 1 end
end

local tmp = vim.fn.tempname()
vim.fn.mkdir(tmp, "p")
vim.cmd.cd(tmp)

io.write("commands\n")
for _, c in ipairs({ "Maic", "MaicSend", "MaicDiagnostics", "MaicQuickfix", "MaicToggle" }) do
  expect(vim.fn.exists(":" .. c) == 2, ":" .. c .. " is defined")
end

io.write(":MaicSend formatting\n")
expect(maic.format_snippet("src/a.lua", 3, 4, { "local x = 1", "return x" }, "lua")
  == "`src/a.lua` lines 3-4:\n```lua\nlocal x = 1\nreturn x\n```", "a range is a fenced snippet with the path and line numbers")
expect(maic.format_snippet("a.md", 7, 7, { "use ```code``` here" }, "markdown")
  == "`a.md` line 7:\n````markdown\nuse ```code``` here\n````", "the fence outgrows backticks inside, one line says line")

local sent
local real_send = maic.send_text
maic.send_text = function(text) sent = text return "test" end
vim.fn.writefile({ "one", "two", "three" }, tmp .. "/f.txt")
vim.cmd.edit("f.txt")
vim.cmd("2,3MaicSend")
expect(sent == "`f.txt` lines 2-3:\n```text\ntwo\nthree\n```", ":2,3MaicSend sends those lines: " .. tostring(sent))
vim.cmd("MaicSend")
expect(sent == "`f.txt`", ":MaicSend without a range sends the path")

io.write("diagnostics and quickfix\n")
local ns = vim.api.nvim_create_namespace("maic_test")
vim.diagnostic.set(ns, 0, {
  { lnum = 2, col = 0, severity = vim.diagnostic.severity.WARN, message = "unused", source = "lint" },
  { lnum = 0, col = 4, severity = vim.diagnostic.severity.ERROR, message = "bad\nthing" },
})
vim.cmd("MaicDiagnostics")
expect(sent == "Diagnostics from nvim for `f.txt`:\nf.txt:1:5: error: bad thing\nf.txt:3:1: warn: unused [lint]", ":MaicDiagnostics: " .. tostring(sent))
vim.fn.setqflist({ { filename = tmp .. "/f.txt", lnum = 2, col = 3, type = "E", text = "boom" }, { text = "a note" } }, "r")
vim.fn.setqflist({}, "a", { title = "make" })
vim.cmd("MaicQuickfix")
expect(sent == "Quickfix list (make):\nf.txt:2:3: error: boom\na note", ":MaicQuickfix: " .. tostring(sent))
maic.send_text = real_send

io.write("the bracketed-paste fallback\n")
expect(maic.bracketed("hi\nthere") == "\27[200~hi\nthere\27[201~", "the text is wrapped in bracketed-paste markers")
local out = tmp .. "/pasted"
maic.setup({ keymaps = false, cmd = { "sh", "-c", "stty raw -echo; exec cat > " .. out }, open = "split" })
vim.cmd("Maic")
vim.wait(2000, function() return vim.fn.filereadable(out) == 1 end)
expect(maic.channel() == nil, "with no MAIC connected there is no channel")
vim.cmd.wincmd("p")
vim.cmd("1MaicSend")
local want = "\27[200~`f.txt` line 1:\n```text\none\n```\27[201~"
local got = ""
vim.wait(3000, function()
  got = table.concat(vim.fn.readfile(out, "b"), "\n")
  return got == want
end)
expect(got == want, ":MaicSend goes into the terminal as a bracketed paste: " .. vim.inspect(got))

io.write("one MAIC per tab, toggled\n")
local before = #vim.api.nvim_list_bufs()
vim.cmd("Maic")
expect(#vim.api.nvim_list_bufs() == before and vim.bo.buftype == "terminal", ":Maic again focuses the same terminal")
vim.cmd("MaicToggle")
expect(vim.bo.buftype ~= "terminal" and #vim.api.nvim_tabpage_list_wins(0) == 1, ":MaicToggle hides it")
vim.cmd("MaicToggle")
expect(vim.bo.buftype == "terminal" and #vim.api.nvim_list_bufs() == before, ":MaicToggle shows the same one again")

io.write("keymaps\n")
local function mapped(lhs, mode) return vim.fn.maparg(lhs, mode or "n") ~= "" end
maic.setup({ keymaps = false })
expect(not mapped("<leader>ms"), "keymaps = false sets none")
maic.setup({})
expect(mapped("<leader>mm") and mapped("<leader>ms") and mapped("<leader>ms", "x") and mapped("<leader>md") and mapped("<leader>mq"),
  "setup() maps under <leader>m")
maic.setup({ keymaps = { send = "<leader>xs", quickfix = false }, prefix = "<leader>z" })
expect(mapped("<leader>xs") and mapped("<leader>zm") and not mapped("<leader>zq"), "single keys can be moved or dropped, the prefix changed")
expect(not mapped("<leader>mm") and not mapped("<leader>ms"), "a second setup() removes the keys the first one set")

io.write("the defaults table\n")
expect(type(maic.defaults) == "table" and maic.defaults.keymaps.open == "<leader>mm" and maic.defaults.open == "vsplit",
  "require('maic').defaults holds every option")
maic.setup({ open = "float", keymaps = { send = "<leader>xs" } })
expect(maic.config.open == "float" and maic.config.cmd == "maic" and maic.config.keymaps.send == "<leader>xs" and maic.config.keymaps.toggle == "<leader>mt",
  "setup(opts) deep-merges opts over the defaults")
expect(maic.defaults.keymaps.send == "<leader>ms" and maic.defaults.open == "vsplit", "and leaves the defaults table as it was")
maic.setup({ keymaps = true })
expect(mapped("<leader>mm") and mapped("<leader>ms"), "keymaps = true is the defaults")

io.write("never overwrite a mapping\n")
local notes = {}
local real_notify = vim.notify
vim.notify = function(msg, level) notes[#notes + 1] = { msg = msg, level = level } end
maic.setup({ keymaps = false })
vim.keymap.set("n", "<leader>mq", "<cmd>echo 'mine'<cr>", { desc = "my quickfix" })
vim.keymap.set("n", "<leader>mdx", "<cmd>echo 'longer'<cr>")
maic.setup({})
vim.wait(500, function() return #notes > 0 end)
expect(vim.fn.maparg("<leader>mq", "n", false, true).desc == "my quickfix", "a key someone else holds keeps their mapping")
expect(not mapped("<leader>md") and mapped("<leader>mD") and mapped("<leader>mm"), "a key that is a prefix of another mapping is skipped too, the rest are set")
expect(#notes == 1 and notes[1].level == vim.log.levels.WARN, "the skipped keys are reported in one warning")
local msg = notes[1] and notes[1].msg or ""
expect(msg:find("<leader>mq (normal) skipped, held by \"my quickfix\"", 1, true) ~= nil and msg:find("<leader>md (normal) skipped, held by <Space>mdx", 1, true) ~= nil,
  "naming the key, the mode and what holds it: " .. msg)
maic.setup({})
vim.wait(200)
expect(#notes == 1, "once per session: a second setup() says nothing new")
maic.setup({ keymaps = { quickfix = "<leader>mq" } })
vim.wait(500, function() return #notes > 1 end)
expect(vim.fn.maparg("<leader>mq", "n", false, true).desc == "MAIC: send the quickfix list", "a key the user names in opts is set anyway")
expect(#notes == 2 and notes[2].msg:find("<leader>mq (normal), your keymaps.quickfix, shadows \"my quickfix\"", 1, true) ~= nil,
  "and reported as shadowing what held it: " .. (notes[2] and notes[2].msg or ""))
vim.g.maic_keymap_check = 1
maic.setup({ keymaps = { toggle = "<leader>zt" } })
expect(not mapped("<leader>zt") and not mapped("<leader>mm"), "with g:maic_keymap_check set, setup() plans the keys and sets none")
vim.g.maic_keymap_check = nil
pcall(vim.keymap.del, "n", "<leader>mq")
vim.keymap.del("n", "<leader>mdx")
vim.notify = real_notify

for _, j in ipairs(vim.api.nvim_list_chans()) do
  if j.mode == "terminal" then pcall(vim.fn.jobstop, j.id) end
end
vim.fn.delete(tmp, "rf")
io.write(failures == 0 and "all passed\n" or (failures .. " FAILED\n"))
os.exit(failures == 0 and 0 or 1)
