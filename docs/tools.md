# Tools

What the model can call, and how to add a tool of your own in Lua. Every tool, built in or yours, goes through the harness described in [harness.md](harness.md): tripwire check, policy for the mode, your approval when policy says ask, then the sandbox for commands. Nothing a tool does gets around that.

## Built in

| Tool | What it does | What the harness sees |
| :--- | :--- | :--- |
| `read_file` | numbered lines of a text file, in ranges; `grep` returns only the lines matching a regex | a read of the file |
| `list_dir` | a directory listing, with entry counts; `depth` (up to 4) shows a tree, not expanding `.git`, `build` and similar | a read of the directory |
| `glob` | files by name pattern (`*.cpp`, `src/**/*.h`); skips `.git`, `build` and similar | a read of the directory |
| `search_files` | grep -E over file contents | a read of the directory |
| `write_file` | create or overwrite a file (an undo point is saved first) | a write |
| `edit_file` | one exact replacement (undo point saved first) | a write |
| `multi_edit` | several replacements in one file, in order; nothing is written unless every one matches (one undo point) | a write |
| `apply_patch` | a unified diff (`diff -u`, `git diff`) over one or more files; exact context, CRLF kept, nothing written if any hunk fails (an undo point per file) | a write to every file in the patch, each judged on its own |
| `move_file` | rename or move a file or directory; never overwrites (the undo point is the reverse move) | a write at both ends |
| `copy_file` | copy a file or directory; never overwrites | a read of the source and a write of the destination |
| `delete_file` | delete a file or empty directory; a directory with contents needs `recursive: true` (a file's content is kept for undo; a directory's is not) | a write |
| `make_dir` | create a directory with its parents | a write |
| `run_shell` | bash in bubblewrap: workspace writable, no network, no sudo, a timeout | a command |
| `question` | asks you one thing and waits; options are picked by number, or you type an answer | nothing: it changes nothing, so it is only logged |
| `todo` | the model's plan for multi-step work, replaced whole on every call | nothing: logged |

`question` shows up like an approval box in the interactive session (a number picks an option, typed text is a free answer, Esc gives no answer) and as a prompt on stderr in `maic -p` when there is a terminal; without one the answer is empty and the model is told so. `todo` shows as `todo n/m done` in the status strip and `:todo` lists it; `maic -p` prints it to stderr.

## Your own tools, in Lua

A tool is one file, `<name>.lua`, in `.maic/tools/` in the workspace or in `~/.config/maic/tools/` (`$XDG_CONFIG_HOME/maic/tools`). Both directories are read when a session starts; workspace tools come first, and a name that repeats a built-in or an earlier tool is skipped. The file returns a table:

```lua
-- .maic/tools/word_count.lua
return {
  name = "word_count",
  description = "Counts the words in a text file of the workspace.",
  parameters = {
    type = "object",
    properties = {
      path = { type = "string", description = "File to count, relative to the workspace" },
    },
    required = { "path" },
  },
  run = function(args)
    local text = maic.read(args.path)
    local n = 0
    for _ in text:gmatch("%S+") do n = n + 1 end
    return n .. " words in " .. args.path
  end,
}
```

* `name`: lowercase letters, digits and underscores. This is what the model calls.
* `description`: the model reads it to decide when to use the tool, so say what it does and when it applies.
* `parameters`: a JSON schema written as a Lua table. Leave it out for a tool that takes nothing. Leave `required` out when nothing is required (an empty Lua table would read as an object, not a list).
* `run(args)`: `args` is a table with the model's arguments. Return a string, or a table (it is sent back as JSON), or nothing. Anything `print`ed comes first. An error (`error("...")`, or a denial from `maic.*`) fails the call and the model sees the message.

The model sees your tool in its tool list beside the built-ins, with its description and schema, and is told in its briefing that you added it. `:tools` in a session and `maic tools` outside one list what loaded; a file that fails to load is skipped with a notice saying which file and why.

### What a tool can do

Each call runs in its own LuaJIT state with the base, `string`, `table`, `math` and `bit` libraries. `load`, `loadstring`, `dofile`, `loadfile` and `require` are removed; `io`, `os`, `package`, `debug`, `ffi` and `jit` are never opened. Globals do not survive from one call to the next. The only way out of the state is the `maic` table:

| Function | Does | Goes through the harness as |
| :--- | :--- | :--- |
| `maic.read(path)` | returns the file's contents | `read_file path` |
| `maic.write(path, text)` | creates or overwrites the file, directories included; saves an undo point | `write_file path`, with the same preview at the prompt |
| `maic.list(path)` | returns a table of entries, directories ending in `/` | `list_dir path` |
| `maic.search(pattern, path)` | grep -E over `path` (default `.`), returns the `path:line: text` lines | `search_files` |
| `maic.shell(cmd, opts)` | runs `cmd` in the sandbox, returns output and exit code; `opts.workdir`, `opts.timeout` (seconds) | `$ cmd`, read-only sandbox when the mode says so |
| `maic.json_encode(value)`, `maic.json_decode(text)` | JSON in and out | nothing |
| `maic.workspace` | the workspace path | nothing |

Paths resolve against the workspace like a tool argument would. Every `maic.*` call above builds the same action the built-in tool would and passes it through the same step as a built-in call: policy for the current mode, the "always" answers from earlier in the session, then you at the approval prompt (the prompt names your tool). A denial, by policy or by you, is raised as a Lua error whose text is the denial message, so the tool stops there unless it `pcall`s, and the model gets the reason. A trip pattern (`sudo`, `rm -rf /`, a write under `/etc`, ...) trips the lock exactly as it would from `run_shell`.

Limits: a tool is stopped after 60 seconds of Lua time or when you press Ctrl-C (a command it started has its own timeout, `opts.timeout`, default 120 s); its result is capped at 64 KB; printing more than that stops it.

### In the transcript

A Lua tool call is logged like any tool call, with its file and, under `actions`, each `maic.*` call it made with the harness's decision and your answer. `maic sessions export` shows them with the other tool calls.

### Tips

* Keep a tool small and specific. The model already has the built-ins; a tool earns its place by doing something in one call that would take the model several, or by encoding a convention of the project ("run the tests the right way", "count words the way this book is counted").
* Say in `description` what the tool returns. The model plans around it.
* Test a tool from the shell: `maic -p "use word_count on README.md" --mode auto-read` runs it with reads allowed and everything else asked.
* Errors are the tool's friend: `error("expected a .md file")` gives the model something to act on.

A complete example ships in [tools/examples/word-count.lua](../tools/examples/word-count.lua); copy it into `.maic/tools/` to try it.

## Where this is going

Tools in other languages (Perl, Python, TypeScript, Go, WASM, shell) as manifests plus scripts, per-tool network grants, and subagents with their own profiles: [roadmap.md](roadmap.md), "Tools and the polyglot spokes".
