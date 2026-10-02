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
for _, c in ipairs({ "Maic", "MaicTerminal", "MaicSend", "MaicDiagnostics", "MaicQuickfix", "MaicToggle", "MaicInterrupt", "MaicSteer", "MaicOlder" }) do
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
maic.setup({ keymaps = false, ui = "terminal", cmd = { "sh", "-c", "stty raw -echo; exec cat > " .. out }, open = "split" })
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
expect(maic.interrupt() == "key", "without a connected MAIC, :MaicInterrupt types Ctrl-C into the terminal")
vim.wait(3000, function()
  got = table.concat(vim.fn.readfile(out, "b"), "\n")
  return got == want .. "\3"
end)
expect(got == want .. "\3", "the job gets Ctrl-C: " .. vim.inspect(got:sub(-3)))

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
local function global(lhs, mode)
  for _, m in ipairs(vim.api.nvim_get_keymap(mode)) do
    if m.lhs == lhs then return m end
  end
end
expect(vim.fn.maparg("<leader>mc", "n"):find("MaicInterrupt") ~= nil and not global("<C-C>", "n") and not global("<C-C>", "t"),
  "<leader>mc interrupts; <C-c> is never mapped globally")

io.write("Esc in MAIC's terminal\n")
vim.keymap.set("t", "<Esc>", "<C-\\><C-n>", { desc = "leave the terminal" })
maic.setup({ keymaps = false, ui = "terminal", cmd = { "sh", "-c", "sleep 30" }, open = "split", terminal_escape = "<C-q>" })
vim.cmd("tabnew")
vim.cmd("Maic")
local tbuf = vim.api.nvim_get_current_buf()
local function local_map(lhs, mode)
  for _, m in ipairs(vim.api.nvim_buf_get_keymap(tbuf, mode or "t")) do
    if m.lhs == lhs then return m end
  end
end
local esc = local_map("<Esc>")
expect(vim.bo[tbuf].buftype == "terminal" and vim.bo[tbuf].filetype == "maic", "MAIC's terminal has the filetype maic")
expect(esc and esc.rhs == "<Esc>" and esc.noremap == 1 and esc.nowait == 1, "a buffer-local terminal-mode <Esc> sends Esc to MAIC (noremap, nowait)")
local q = local_map("<C-Q>")
expect(q and q.rhs == "<C-\\><C-N>", "terminal_escape leaves terminal mode there: " .. vim.inspect(q and q.rhs))
local cc, ncc = local_map("<C-C>"), local_map("<C-C>", "n")
expect(cc and cc.rhs == "<C-C>" and ncc and ncc.rhs == "<Cmd>MaicInterrupt<CR>", "<C-c> interrupts in MAIC's buffer: passed through in terminal mode, :MaicInterrupt in normal mode")
expect(vim.fn.maparg("<Esc>", "t", false, true).buffer == 1 and vim.api.nvim_get_keymap("t")[1].desc == "leave the terminal",
  "the user's global <Esc> mapping stays as it was")
maic.setup({ keymaps = false, ui = "terminal", cmd = { "sh", "-c", "sleep 30" }, open = "split" })
local seen = {}
local real_notify2 = vim.notify
vim.notify = function(msg) seen[#seen + 1] = msg end
local au = vim.api.nvim_create_autocmd("TermOpen", { callback = function(ev) vim.keymap.set("t", "<Esc>", "<C-\\><C-n>", { buffer = ev.buf, desc = "termopen esc" }) end })
vim.cmd("tabnew")
vim.cmd("Maic")
tbuf = vim.api.nvim_get_current_buf()
vim.wait(500, function() return #seen > 0 end)
expect(local_map("<Esc>").desc == "termopen esc" and local_map("<C-Q>") == nil, "a buffer-local <Esc> from a TermOpen autocmd is left alone; terminal_escape = nvim's own maps nothing")
expect(#seen == 1 and seen[1]:find("<Esc> (terminal, MAIC's buffer) skipped, held by \"termopen esc\"", 1, true) ~= nil, "and reported: " .. (seen[1] or ""))
vim.api.nvim_del_autocmd(au)
vim.api.nvim_create_autocmd("TermOpen", { callback = function(ev)
  if vim.bo[ev.buf].filetype == "maic" then return end
  vim.keymap.set("t", "<Esc>", "<C-\\><C-n>", { buffer = ev.buf })
end })
vim.cmd("tabnew")
vim.cmd("Maic")
tbuf = vim.api.nvim_get_current_buf()
expect(local_map("<Esc>").rhs == "<Esc>", "a TermOpen autocmd can skip MAIC's terminal by its filetype")
vim.api.nvim_clear_autocmds({ event = "TermOpen" })
vim.notify = real_notify2
vim.keymap.del("t", "<Esc>")

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

io.write("the keymap check (:checkhealth maic)\n")
local K = require("maic.keymaps")
maic.setup({})
local function find(report, section, needle)
  for _, s in ipairs(report.sections) do
    if s.name == section then
      for _, it in ipairs(s.items) do
        if it.text:find(needle, 1, true) then return it end
      end
    end
  end
end
local tabs, bufs = #vim.api.nvim_list_tabpages(), #vim.api.nvim_list_bufs()
local r0 = K.check()
expect(#vim.api.nvim_list_tabpages() == tabs and #vim.api.nvim_list_bufs() == bufs, "the scratch buffers it opens are closed again")
expect(#r0.hash == 64 and find(r0, "maic.nvim's keys", "<leader>mm (normal): MAIC: open or focus").level == "ok", "every key maic.nvim sets is listed, free ones OK")
expect(find(r0, "maic.nvim's keys", "<Esc> (terminal) in MAIC's terminal").level == "ok", "the buffer-local keys of MAIC's terminal are listed too")
expect(find(r0, "MAIC's terminal input", "<C-w> (window moves").level == "ok", "the keys MAIC's input needs reach it in a plain nvim")
expect(find(r0, "llama.vim", "not installed").level == "info", "no llama.vim: an info line")
vim.keymap.set("t", "<C-w>", "<C-\\><C-n><C-w>", { desc = "window from a terminal" })
vim.keymap.set("t", "<Esc>", "<C-\\><C-n>", { desc = "leave the terminal" })
vim.api.nvim_create_autocmd("FileType", { pattern = "maic-input", callback = function() vim.keymap.set("n", "<C-c>", "<cmd>close<cr>", { buffer = true, desc = "close it" }) end })
vim.g.llama_config = { endpoint_fim = "http://127.0.0.1:8084/infill" }
local r1 = K.check()
local cw = find(r1, "MAIC's terminal input", "<C-w>")
expect(cw.level == "error" and cw.hint:find('terminal_passthrough = { ["<C-w>"] = true }', 1, true) ~= nil, "a global terminal-mode <C-w> is an error with the fix: " .. cw.text)
expect(find(r1, "MAIC's terminal input", "<Esc>").level == "ok", "a global terminal-mode <Esc> is fine: it is passed through")
expect(find(r1, "maic.nvim's keys", "<C-c> (normal) in a maic-input buffer").level == "warn", "a FileType maic-input mapping on <C-c> is found in the scratch input buffer")
local llf = find(r1, "llama.vim", "<leader>llf (insert)")
expect(llf and llf.level == "warn" and llf.text:find("typing <Space> in insert mode waits", 1, true) ~= nil, "llama.vim's insert-mode <leader>llf with a Space leader is a warning")
expect(find(r1, "llama.vim", "<Tab> (insert)").text:find("only while a suggestion shows", 1, true) ~= nil, "Tab is llama.vim's only while a suggestion shows")
expect(r1.hash ~= r0.hash, "the collision hash changes with the collisions")
vim.g.llama_config = { keymap_fim_trigger = "<M-f>", keymap_fim_accept_word = "<M-]>" }
local r2 = K.check()
expect(find(r2, "llama.vim", "<M-f> (insert)").level == "ok" and find(r2, "llama.vim", "<M-]> (insert)").level == "ok", "<M-f> and <M-]> are free in a plain nvim")
vim.cmd("checkhealth maic")
local text = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
expect(text:find("maic.nvim's keys", 1, true) ~= nil and text:find("MAIC never gets <C-w>", 1, true) ~= nil, ":checkhealth maic shows the same report")
vim.cmd("bwipe!")
vim.g.llama_config = nil
vim.api.nvim_clear_autocmds({ event = "FileType", pattern = "maic-input" })
vim.keymap.del("t", "<C-w>")
vim.keymap.del("t", "<Esc>")

for _, j in ipairs(vim.api.nvim_list_chans()) do
  if j.mode == "terminal" then pcall(vim.fn.jobstop, j.id) end
end
vim.fn.delete(tmp, "rf")
io.write(failures == 0 and "all passed\n" or (failures .. " FAILED\n"))
os.exit(failures == 0 and 0 or 1)
