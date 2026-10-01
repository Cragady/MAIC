# Tools

What the model can call, and how to add a tool of your own: in Lua, or as a script in any language behind a manifest. Every tool, built in or yours, goes through the harness described in [harness.md](harness.md): tripwire check, policy for the mode, your approval when policy says ask, then the sandbox for commands. Nothing a tool does gets around that.

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
| `delegate` | hands one task to a subagent running under a profile; its report is the result | nothing itself: every call the child makes goes through the harness under the child's profile |

`question` shows up like an approval box in the interactive session (a number picks an option, typed text is a free answer, Esc gives no answer) and as a prompt on stderr in `maic -p` when there is a terminal; without one the answer is empty and the model is told so. `todo` shows as `todo n/m done` in the status strip and `:todo` lists it; `maic -p` prints it to stderr.

### delegate

`delegate {profile, task, context?}` starts a second agent in the same workspace, on the same model unless the profile names another, and runs `task` to completion. The child gets none of the parent's conversation: `task` and `context` are all it knows, and its final answer comes back as the tool result (capped at 16 KB), ending with `(the subagent used N steps, M tokens)`. The model is briefed to use it for long reads or searches it does not want in its own context (a `scout`, asked for a short report with paths and line numbers), and for a review of its own change by a `reviewer` before it calls the work done; a `builder` takes a self-contained piece of editing.

What the child may do is its profile's, capped by the session: the built-in `scout` (auto-read, read-only tools plus read-only commands, 50k tokens), `reviewer` (plan mode, reads inside the workspace only, 50k tokens), `builder` (edit mode, writes anywhere in the workspace) and `orchestrator` (everything the session has), or a profile from `profiles` in settings ([settings.md](settings.md)). A child never gets a wider mode than the session is in, so under a manual session even a scout's `git log` is asked about. Approvals come to you through the parent with the profile named (`scout: $ git log`); the child's tool calls show indented under the delegate call (`↳ scout: read_file src/a.cpp`). A child has no `delegate`, `question` or `todo` tool: one level only, and it reports to the parent in its answer. Ctrl-C and a trip stop the child along with the parent; a child stops at its profile's step and token budget, and its tokens count against the session's budget.

Every child has its own transcript, kind `sub`, in the parent's home, with the parent's id and the profile in its `start` record ([sessions.md](sessions.md)); `maic sessions` lists it indented under the parent and `maic -r ID` opens it like any session. The parent's `tool` record for the call names the child's file, its steps and its tokens.

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

## Your own tools, in any language: script tools

A script tool is a directory, `.maic/tools/<name>/` in the workspace or `~/.config/maic/tools/<name>/`, holding `tool.json` and the script it names. Both directories are read when a session starts, after the Lua tools; workspace tools come first, and a name that repeats a built-in, a Lua tool or an earlier script tool is skipped with a notice.

```json
{
  "name": "word_count",
  "description": "Counts the lines, words and characters of a text file in the workspace. Use it instead of wc in run_shell.",
  "parameters": {
    "type": "object",
    "properties": { "path": { "type": "string", "description": "File to count, relative to the workspace" } },
    "required": ["path"],
    "additionalProperties": false
  },
  "run": ["python3", "main.py"],
  "timeout_s": 10,
  "network": false,
  "reads": ["**"],
  "writes": []
}
```

| Field | Meaning |
| :--- | :--- |
| `name` | snake_case: a lowercase letter, then lowercase letters, digits and underscores. Not a built-in's name. This is what the model calls. |
| `description` | What it does and when it applies; the model reads it to decide. |
| `parameters` | A JSON schema for the arguments (`type: object`). Checked when the manifest loads (`maic tools check` reports a bad one) and again on every call: a missing required argument, a wrong type, a value outside an `enum`, an unknown argument under `additionalProperties: false` is an error the model reads, and the script never starts. Leave it out for a tool that takes nothing. |
| `run` | The program and its arguments, as a list. The program must be on `PATH` (or be a path: `./tool` is looked up in the tool's directory). An argument that names a file in the tool's directory is passed as its absolute path, so `["python3", "main.py"]` works whatever the working directory. |
| `timeout_s` | Seconds before the script is killed (its whole process group). Default 60, at most 600. |
| `network` | `false`, or left out. `true` is refused when the manifest loads: "per-tool network grants are not implemented yet". |
| `reads`, `writes` | Globs, relative to the workspace (`docs/**`, `out/*.txt`, `**`) or absolute. What the harness judges before the script starts; see below. |

How a call runs: MAIC checks the arguments against the schema, then builds one action per declared glob, a read at the glob's fixed prefix (`docs/**/*.md` is a read of `docs`, `**` is a read of the workspace, `/etc/hosts` is a read of that file) and a write likewise, and hands each to the same step a built-in call goes through: policy for the mode, the session's "always" answers, your approval, the second reader. A write glob whose prefix is outside the workspace is refused outright ("a script tool writes only inside the workspace"), never asked. So in manual mode a tool that declares `writes` is asked about before it starts, with the tool named and the directory it will write under, and one that declares reads inside the workspace only runs unasked; in auto mode both run; in plan mode a tool with writes is refused. When every action is allowed the script runs inside the same bubblewrap sandbox as `run_shell`: the whole filesystem read-only, secrets hidden, no network, no privilege escalation, the workspace writable only when `writes` is non-empty (read-only otherwise, so a tool that declared nothing cannot change a file even by accident), and the workspace as its working directory. The arguments arrive as one JSON object on stdin. What the script prints to stdout is the result, capped like command output (the head and tail are kept when it is long). A non-zero exit fails the call: the model sees `exit code N`, the output so far and `stderr:` with what the script complained about; on success stderr is dropped. At `timeout_s` the process group is killed and the model is told.

The declarations are what the harness sees, not a fence: inside the sandbox a script can read whatever `run_shell` could, and a tool with any `writes` at all gets the whole workspace writable, like `run_shell`. Declare what the tool actually touches, narrowly, so the approval prompt tells the truth; the model is told the declared reads and writes in its briefing.

### Languages

Anything `run` can name. Two lines each:

* **Python**: `"run": ["python3", "main.py"]`; `args = json.load(sys.stdin)`, `print(...)`. The shipped [tools/examples/word_count](../tools/examples/word_count).
* **Shell**: `"run": ["sh", "main.sh"]`; `args=$(cat)`, pull fields out with `jq -r .field`. The shipped [tools/examples/json_pick](../tools/examples/json_pick), which wraps `jq`.
* **Perl**: `"run": ["perl", "main.pl"]`; `use JSON::PP; local $/; my $args = decode_json(<STDIN>);` (JSON::PP ships with Perl).
* **TypeScript**: `"run": ["deno", "run", "main.ts"]` and `const args = JSON.parse(await new Response(Deno.stdin.readable).text());` (give deno no `--allow-*` flags; the sandbox is MAIC's), or **JavaScript** with `"run": ["node", "main.js"]` reading `process.stdin`.
* **Go**: build the binary into the tool's directory and name it: `"run": ["./tool"]`; `json.NewDecoder(os.Stdin).Decode(&args)`. No interpreter needed at run time.
* **WASM**: `"run": ["wasmtime", "tool.wasm"]` when `wasmtime` is installed; a WASI module reads stdin and writes stdout like any program, and MAIC's sandbox is around the runtime as well.

`maic tools new NAME --lang python|sh|perl|node` scaffolds `.maic/tools/NAME/` with a manifest and a stub that echoes its arguments (`--global` puts it under `~/.config/maic/tools/`); edit the description, the parameters and the script. `maic tools check` validates every manifest here and in the global directory (well-formed schema, `run[0]` on `PATH`, a legal name, no duplicate) and exits 1 when something is wrong. `maic tools` lists script tools with their language, manifest and declared reads and writes; `:tools` does the same in a session.

### Examples

[tools/examples/word_count](../tools/examples/word_count) (`python3`) counts a file; it declares `reads: ["**"]` and nothing else, so it runs unasked in every mode but needs no writable workspace. [tools/examples/json_pick](../tools/examples/json_pick) (`sh` with `jq`) returns one value from a JSON file by a jq filter; `reads: ["**/*.json"]`. Copy either directory into `.maic/tools/` to try it: `maic -p "use word_count on README.md"`.

### In the transcript

A script tool call is logged like any tool call, with its manifest and, under `actions`, each declared read and write with the harness's decision and your answer.

## Where this is going

Per-tool network grants, so a manifest can ask for the network and the harness can judge that too: [roadmap.md](roadmap.md) and the planned list in [harness.md](harness.md). Until then `network: true` is refused when the manifest loads.
