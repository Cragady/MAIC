-- nvim as MAIC's interface, against the real engine (`maic --rpc`) and a fake model. Run by MAIC's
-- tests/cli_smoke.py, which starts the fake, makes a throwaway home and sets MAIC_UI_TEST_BIN to the maic to run:
-- nvim --headless -u NONE -i NONE -n -l maic.nvim/tests/ui_test.lua. Exit 0 when every check passes.
vim.g.mapleader = " "
local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h")
vim.opt.rtp:prepend(root)
vim.cmd("runtime plugin/maic.lua")
local maic = require("maic")
local ui = require("maic.ui")

local failures = 0
local function expect(ok, what)
  io.write((ok and "  ok    " or "  FAIL  ") .. what .. "\n")
  if not ok then failures = failures + 1 end
end

local fired = {}
vim.api.nvim_create_autocmd("User", { pattern = "Maic*", callback = function(ev) fired[#fired + 1] = { ev.match, ev.data } end })
local function count(pattern, pred)
  local n = 0
  for _, f in ipairs(fired) do
    if f[1] == pattern and (not pred or pred(f[2])) then n = n + 1 end
  end
  return n
end

-- A FileType autocmd of the user's holding <C-q> in the input: maic.nvim leaves it alone and says so.
local notes = {}
local real_notify = vim.notify
vim.notify = function(msg) notes[#notes + 1] = msg end
vim.api.nvim_create_autocmd("FileType", { pattern = "maic-input", callback = function(ev) vim.keymap.set("n", "<C-q>", "<cmd>echo 'mine'<cr>", { buffer = ev.buf, desc = "my C-q" }) end })

io.write("starting\n")
maic.setup({ cmd = os.getenv("MAIC_UI_TEST_BIN"), open = "vsplit" })
vim.cmd("Maic")
local s
vim.wait(30000, function()
  s = ui.state()
  return s and s.session ~= nil
end, 20)
expect(s and s.session, ":Maic starts the engine and opens a session: " .. vim.inspect(s and s.session))
local conv, input = s.conversation, s.input
expect(vim.bo[conv].filetype == "maic" and vim.bo[input].filetype == "maic-input", "the conversation has the filetype maic, the input maic-input")
expect(vim.api.nvim_get_current_buf() == input and #vim.fn.win_findbuf(conv) == 1, "the input has the focus, the conversation is shown above it")
local function nmap(buf, lhs, mode)
  for _, m in ipairs(vim.api.nvim_buf_get_keymap(buf, mode or "n")) do
    if m.lhs == lhs then return m end
  end
end
expect(nmap(input, "<CR>") and nmap(input, "<M-CR>", "i") and nmap(input, "<C-S>") and nmap(input, "<C-S>", "i") and nmap(input, "<C-C>")
  and nmap(conv, "<C-S>") and nmap(conv, "<C-Q>") and nmap(conv, "<C-C>"), "the buffer keymaps are set in the interface's buffers")
expect(nmap(input, "<C-Q>").desc == "my C-q" and table.concat(notes, "\n"):find("<C-q> (normal, MAIC's buffer) skipped, held by \"my C-q\"", 1, true) ~= nil,
  "a key the user's FileType autocmd holds in the input is left alone and reported")
vim.notify = real_notify
vim.api.nvim_clear_autocmds({ event = "FileType", pattern = "maic-input" })

local function text() return table.concat(vim.api.nvim_buf_get_lines(conv, 0, -1, false), "\n") end
local function occurrences(needle)
  local n, at = 0, 1
  while true do
    local i = text():find(needle, at, true)
    if not i then return n end
    n, at = n + 1, i + 1
  end
end
local function wait_for(needle, times, ms)
  vim.wait(ms or 30000, function() return occurrences(needle) >= (times or 1) end, 20)
  return occurrences(needle) >= (times or 1)
end
local function idle() return vim.wait(30000, function() return ui.state().response == nil end, 20) end
local function keys(k) vim.api.nvim_feedkeys(vim.keycode(k), "x", false) end
local function focus(buf)
  vim.cmd("stopinsert")
  vim.api.nvim_set_current_win(vim.fn.win_findbuf(buf)[1])
end
local function send(t)
  focus(input)
  vim.api.nvim_buf_set_lines(input, 0, -1, false, vim.split(t, "\n"))
  keys("<CR>")
end
local function float_kind(kind)
  vim.wait(30000, function()
    local f = ui.state().float
    return f and f.kind == kind
  end, 20)
  local f = ui.state().float
  return f and f.kind == kind and f or nil
end

io.write("a turn\n")
send("ping")
expect(wait_for("echo: ping") and wait_for("▣ fake/fake"), "<CR> in the input sends it; the reply streams in, the footer follows")
expect(occurrences("❯ ping") == 1 and #vim.api.nvim_buf_get_lines(input, 0, -1, false) == 1 and vim.api.nvim_buf_get_lines(input, 0, 1, false)[1] == "", "the message is shown and the input emptied")
expect(count("MaicTurnStart", function(d) return d.session == s.session end) == 1 and count("MaicTurnEnd", function(d) return d.tool_calls == 0 end) == 1,
  "MaicTurnStart and MaicTurnEnd fire with the session")
expect(vim.wo[vim.fn.win_findbuf(conv)[1]].winbar:find("fake/fake", 1, true) ~= nil, "the winbar names the model")
idle()
focus(input)
keys("ipong<M-CR>")
expect(wait_for("echo: pong"), "<M-CR> sends from insert mode")
idle()

io.write("approvals and folds\n")
send("shell:echo nv-$((40+2))")
local f = float_kind("approval")
expect(f and vim.api.nvim_get_current_win() == f.win and table.concat(vim.api.nvim_buf_get_lines(f.buf, 0, -1, false), "\n"):find("$ echo nv-$((40+2))", 1, true),
  "an approval opens a focused float naming the call")
keys("y")
expect(wait_for("nv-42") and wait_for("ran it"), "y approves: the command runs and its output shows under its line")
expect(count("MaicApproval", function(d) return d.verdict == "pending" end) == 1 and count("MaicApproval", function(d) return d.verdict == "yes" end) == 1
  and count("MaicToolCall", function(d) return d.tool == "run_shell" end) == 1, "MaicApproval fires pending and answered, MaicToolCall for the call")
idle()
send("shell:seq 100 111")
float_kind("approval")
keys("y")
wait_for("ran it", 2)
local cw = vim.fn.win_findbuf(conv)[1]
local line = vim.fn.search("^  111$", "nw")
vim.api.nvim_win_call(cw, function() line = vim.fn.search("^  111$", "nw") end)
local closed = line > 0 and vim.api.nvim_win_call(cw, function() return vim.fn.foldclosed(line) end) or -1
expect(line > 0 and closed > 0, "an output of fold_output lines or more is folded closed (line " .. line .. ", fold " .. closed .. ")")
idle()
send("shell:echo never")
float_kind("approval")
keys("n")
expect(wait_for("ran it", 3) and not text():find("\n  never", 1, true), "n refuses: the command never runs")
idle()

io.write("questions\n")
send("ask:Which colour?|red|blue")
f = float_kind("question")
expect(f and table.concat(vim.api.nvim_buf_get_lines(f.buf, 0, -1, false), "\n"):find("[2] blue", 1, true), "a question opens a float with its options")
keys("2")
expect(wait_for("❯ blue") and wait_for("ran it", 4), "a number picks an option and the turn goes on")
idle()

io.write("steering\n")
send("hold on")
wait_for("holding")
focus(conv)
keys("<C-s>")
f = float_kind("pause")
expect(f and ui.state().paused and wait_for("↯ interrupt"), "<C-s> pauses the turn: the pause menu opens")
expect(vim.wo[cw].winbar:find("PAUSED", 1, true) ~= nil, "the winbar says it is paused")
keys("k")
expect(idle() and not ui.state().paused and not ui.state().float, "k keeps the reply so far and ends the turn")
send("hold two")
wait_for("holding", 2)
focus(input)
keys("<C-c>")
expect(idle() and wait_for("· interrupted"), "<C-c> cancels the running turn")
send("hold three")
wait_for("holding", 3)
vim.cmd("MaicSteer interrupt")
float_kind("pause")
keys("<C-q>")
expect(vim.wait(30000, function() return not ui.state().paused end, 20) and idle(), "<C-q> resumes the paused turn, which then ends")
expect(count("MaicTurnEnd", function(d) return d.interrupted end) >= 1, "MaicTurnEnd says when a turn was interrupted")
send("slow: one")
vim.wait(30000, function() return ui.state().response ~= nil end, 20)
send("two")
expect(wait_for("queued: it reaches the model at its next step") and wait_for("echo: two"), "a message sent mid-turn goes to the running turn")
idle()

io.write("commands and the shell\n")
send("/mode plan")
expect(vim.wait(30000, function() return ui.state().mode == "plan" end, 20) and vim.wo[cw].winbar:find("plan", 1, true) ~= nil, "/mode plan runs the engine's :mode")
send("/nosuchcommand")
expect(wait_for("unknown command"), "an unknown command says so")
send("!echo bang-$((1+1))")
expect(wait_for("  bang-2"), "!cmd runs a shell command and shows its output")

io.write("sending from nvim\n")
local tmp = vim.fn.tempname()
vim.fn.writefile({ "alpha", "beta" }, tmp)
focus(conv)
vim.cmd("wincmd p")
vim.cmd("aboveleft split " .. vim.fn.fnameescape(tmp))
vim.cmd("2MaicSend")
local got = table.concat(vim.api.nvim_buf_get_lines(input, 0, -1, false), "\n")
expect(got:find("```\nbeta\n```", 1, true) ~= nil and occurrences("beta") == 0, ":MaicSend puts the snippet into the input, unsent")
vim.cmd("bwipe!")

io.write("toggling\n")
vim.cmd("MaicToggle")
expect(#vim.fn.win_findbuf(conv) == 0 and #vim.fn.win_findbuf(input) == 0, ":MaicToggle hides the conversation and the input")
vim.cmd("MaicToggle")
expect(#vim.fn.win_findbuf(conv) == 1 and #vim.fn.win_findbuf(input) == 1 and ui.state().session == s.session, ":MaicToggle shows them again, the same session")
cw = vim.fn.win_findbuf(conv)[1]
vim.api.nvim_win_call(cw, function() line = vim.fn.search("^  111$", "nw") end)
expect(vim.api.nvim_win_call(cw, function() return vim.fn.foldclosed(line) end) > 0, "the new window gets the folds again")

io.write("the end\n")
local job = ui.state().job
vim.fn.chanclose(job, "stdin")
local code = vim.fn.jobwait({ job }, 30000)[1]
expect(code == 0 and wait_for("the engine exited (code 0)"), "closing the engine's stdin ends it cleanly (exit " .. code .. ")")

io.write("resuming\n")
vim.cmd("tabnew")
local again = ui.start({}, { session = s.session })
vim.wait(30000, function()
  local st = ui.state()
  return st and st.model ~= nil
end, 20)
expect(ui.state() and ui.state().session == s.session and again.conv ~= conv, "a new engine resumes the session (what maic --ui nvim -c does)")
conv, input = again.conv, again.input
expect(wait_for("echo: two") and text():find("⋯ earlier history", 1, true) == 1 and not text():find("echo: ping", 1, true),
  "the resumed session shows its last exchanges, and a line at the top for the earlier ones")
focus(conv)
vim.api.nvim_win_set_cursor(0, { 1, 0 })
vim.api.nvim_exec_autocmds("CursorMoved", { buffer = conv }) -- a headless -l run never fires it on its own
expect(wait_for("❯ hold on"), "moving to that line loads the page before them")
vim.wait(30000, function()
  if not text():find("⋯ earlier history", 1, true) then return true end
  vim.cmd("MaicOlder")
  return false
end, 50)
local ping, nv, two = text():find("echo: ping", 1, true), text():find("nv-42", 1, true), text():find("echo: two", 1, true)
expect(ping and nv and two and ping < nv and nv < two and not text():find("⋯ earlier history", 1, true),
  ":MaicOlder loads the rest, oldest at the top, until there is nothing before it")
send("again")
expect(wait_for("echo: again"), "and runs a turn in it")
idle()
job = ui.state().job
vim.fn.chanclose(job, "stdin")
expect(vim.fn.jobwait({ job }, 30000)[1] == 0, "and ends cleanly")

io.write(failures == 0 and "all passed\n" or (failures .. " FAILED\n"))
if failures > 0 then io.write("---- conversation ----\n" .. text() .. "\n") end
os.exit(failures == 0 and 0 or 1)
