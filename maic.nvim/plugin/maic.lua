-- maic.nvim: the commands. Everything else loads on first use; keymaps come from require("maic").setup().
if vim.g.loaded_maic then return end
vim.g.loaded_maic = true

local function maic() return require("maic") end

vim.api.nvim_create_user_command("Maic", function(o) maic().open(o.fargs) end,
  { nargs = "*", complete = "file", desc = "Open or focus MAIC: nvim as its interface, or its TUI in a terminal (one per tab)" })
vim.api.nvim_create_user_command("MaicTerminal", function(o) maic().open_terminal(o.fargs) end,
  { nargs = "*", complete = "file", desc = "Open or focus MAIC's own TUI in a terminal" })
vim.api.nvim_create_user_command("MaicToggle", function() maic().toggle() end,
  { desc = "Show or hide this tab's MAIC window" })
vim.api.nvim_create_user_command("MaicSend", function(o) maic().send_range(o.range, o.line1, o.line2) end,
  { range = true, desc = "Send the buffer's path, or the range as a fenced snippet, into MAIC's input" })
vim.api.nvim_create_user_command("MaicDiagnostics", function(o) maic().send_diagnostics(o.bang) end,
  { bang = true, desc = "Send the buffer's LSP diagnostics (! for every buffer) into MAIC's input" })
vim.api.nvim_create_user_command("MaicInterrupt", function() maic().interrupt() end,
  { desc = "Interrupt MAIC's running turn, as its first Ctrl-C does" })
vim.api.nvim_create_user_command("MaicSteer", function(o) require("maic.ui").steer_command(o.fargs) end,
  { nargs = "+", complete = function() return { "steer", "drop", "further", "interrupt", "keep", "halt" } end,
    desc = "Steer the interface's running or paused turn: steer|drop|further|interrupt|keep|halt [note]" })
vim.api.nvim_create_user_command("MaicOlder", function() require("maic.ui").older() end,
  { desc = "Load the conversation's earlier history above what is shown" })
vim.api.nvim_create_user_command("MaicQuickfix", function() maic().send_quickfix() end,
  { desc = "Send the quickfix list into MAIC's input" })
local function session(verb, desc)
  vim.api.nvim_create_user_command("Maic" .. verb:sub(1, 1):upper() .. verb:sub(2), function(o) require("maic.ui").session_command(verb, o.fargs) end,
    { nargs = "*", complete = function() return { "--bg", "--park", "--stop" } end, desc = desc })
end
session("new", "Start another session in this tab's engine and go to it: [--bg|--park|--stop] [DIR]")
session("switch", "Go to another session, or pick one: [--bg|--park|--stop] [ID|TITLE]")
session("fork", "Fork this session into a second one and go to it: [--bg|--park|--stop]")
session("bg", "Send this session to the background (it keeps working) and pick another")
session("park", "End this session (or ID) for now; it resumes where it was")
session("stop", "End this session (or ID); it stays a transcript")
