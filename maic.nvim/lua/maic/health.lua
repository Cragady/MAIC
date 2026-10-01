-- :checkhealth maic: every key maic.nvim sets, the keys MAIC's terminal input needs and llama.vim's keys,
-- against your mappings (lua/maic/keymaps.lua does the looking).
local M = {}

function M.check()
  local report = require("maic.keymaps").check()
  for _, s in ipairs(report.sections) do
    vim.health.start(s.name)
    for _, it in ipairs(s.items) do
      if it.level == "ok" or it.level == "info" then
        vim.health[it.level](it.text)
      else
        vim.health[it.level](it.text, it.hint and { it.hint } or nil)
      end
    end
  end
end

return M
