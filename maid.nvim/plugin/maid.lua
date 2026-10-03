-- maid.nvim: the commands. Everything else loads on first use; keymaps come from require("maid").setup().
if vim.g.loaded_maid then return end
vim.g.loaded_maid = true

local function maid() return require("maid") end

vim.api.nvim_create_user_command("Maid", function(o) maid().open(o.fargs) end,
  { nargs = "*", complete = "file", desc = "Open or focus MAID: nvim as its interface, or its TUI in a terminal (one per tab)" })
vim.api.nvim_create_user_command("MaidTerminal", function(o) maid().open_terminal(o.fargs) end,
  { nargs = "*", complete = "file", desc = "Open or focus MAID's own TUI in a terminal" })
vim.api.nvim_create_user_command("MaidToggle", function() maid().toggle() end,
  { desc = "Show or hide this tab's MAID window" })
vim.api.nvim_create_user_command("MaidSend", function(o) maid().send_range(o.range, o.line1, o.line2) end,
  { range = true, desc = "Send the buffer's path, or the range as a fenced snippet, into MAID's input" })
vim.api.nvim_create_user_command("MaidDiagnostics", function(o) maid().send_diagnostics(o.bang) end,
  { bang = true, desc = "Send the buffer's LSP diagnostics (! for every buffer) into MAID's input" })
vim.api.nvim_create_user_command("MaidInterrupt", function() maid().interrupt() end,
  { desc = "Interrupt MAID's running turn, as its first Ctrl-C does" })
vim.api.nvim_create_user_command("MaidSteer", function(o) require("maid.ui").steer_command(o.fargs) end,
  { nargs = "+", complete = function() return { "steer", "drop", "further", "interrupt", "keep", "halt" } end,
    desc = "Steer the interface's running or paused turn: steer|drop|further|interrupt|keep|halt [note]" })
vim.api.nvim_create_user_command("MaidOlder", function() require("maid.ui").older() end,
  { desc = "Load the conversation's earlier history above what is shown" })
vim.api.nvim_create_user_command("MaidQuickfix", function() maid().send_quickfix() end,
  { desc = "Send the quickfix list into MAID's input" })
local function session(verb, desc)
  vim.api.nvim_create_user_command("Maid" .. verb:sub(1, 1):upper() .. verb:sub(2), function(o) require("maid.ui").session_command(verb, o.fargs) end,
    { nargs = "*", complete = function() return { "--bg", "--park", "--stop" } end, desc = desc })
end
session("new", "Start another session in this tab's engine and go to it: [--bg|--park|--stop] [DIR]")
session("switch", "Go to another session, or pick one: [--bg|--park|--stop] [ID|TITLE]")
session("fork", "Fork this session into a second one and go to it: [--bg|--park|--stop]")
session("bg", "Send this session to the background (it keeps working) and pick another")
session("park", "End this session (or ID) for now; it resumes where it was")
session("stop", "End this session (or ID); it stays a transcript")
