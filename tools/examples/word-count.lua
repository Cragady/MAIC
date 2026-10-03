-- word_count: a MAID tool written in Lua. See docs/tools.md for the format.
--
-- Install it by copying this file to .maid/tools/word_count.lua in a workspace (or to
-- ~/.config/maid/tools/ for every workspace). MAID loads it when a session starts, the model sees it
-- beside the built-in tools, and every maid.* call below goes through the harness exactly as the
-- matching built-in would: maid.read is a read_file, so in manual mode inside the workspace it just
-- runs, and a path outside the workspace is asked about first.
--
-- The state this runs in has only the base, string, table, math and bit libraries: no io, os or
-- require. A call is stopped after 60 seconds, and its result is capped at 64 KB.
return {
  name = "word_count",
  description = "Counts the words and lines in a text file of the workspace. Use it instead of wc in run_shell.",
  parameters = {
    type = "object",
    properties = {
      path = { type = "string", description = "File to count, relative to the workspace" },
    },
    required = { "path" },
  },
  run = function(args)
    local text = maid.read(args.path)
    local words, lines = 0, 0
    for _ in text:gmatch("%S+") do words = words + 1 end
    for _ in text:gmatch("\n") do lines = lines + 1 end
    if #text > 0 and text:sub(-1) ~= "\n" then lines = lines + 1 end
    return string.format("%s: %d words, %d lines", args.path, words, lines)
  end,
}
