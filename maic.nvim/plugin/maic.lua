-- maic.nvim: the commands. Everything else loads on first use; keymaps come from require("maic").setup().
if vim.g.loaded_maic then return end
vim.g.loaded_maic = true

local function maic() return require("maic") end

vim.api.nvim_create_user_command("Maic", function(o) maic().open(o.fargs) end,
  { nargs = "*", complete = "file", desc = "Open or focus MAIC in a terminal (one per tab)" })
vim.api.nvim_create_user_command("MaicToggle", function() maic().toggle() end,
  { desc = "Show or hide this tab's MAIC window" })
vim.api.nvim_create_user_command("MaicSend", function(o) maic().send_range(o.range, o.line1, o.line2) end,
  { range = true, desc = "Send the buffer's path, or the range as a fenced snippet, into MAIC's input" })
vim.api.nvim_create_user_command("MaicDiagnostics", function(o) maic().send_diagnostics(o.bang) end,
  { bang = true, desc = "Send the buffer's LSP diagnostics (! for every buffer) into MAIC's input" })
vim.api.nvim_create_user_command("MaicInterrupt", function() maic().interrupt() end,
  { desc = "Interrupt MAIC's running turn, as its first Ctrl-C does" })
vim.api.nvim_create_user_command("MaicQuickfix", function() maic().send_quickfix() end,
  { desc = "Send the quickfix list into MAIC's input" })
