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
| `task` | hands one job to a subagent running as an agent; its report is the result, or with `background: true` it runs beside the turn as a session of its own | nothing itself: every call the child makes goes through the harness under the child's agent |
| `task_result` | how a background task stands, or its report; `wait: true` waits for it | nothing: it reads the engine's record of the task |
| `diagnostics` | only while MAIC is connected to the nvim it runs inside ([nvim.md](nvim.md)): the LSP diagnostics nvim has for `path`, or for every open file in the workspace when `path` is left out, one `path:line:col: severity: message` line each | a read of the file, or of the workspace |

`diagnostics` comes from `vim.diagnostic.get()` in the host: only files nvim has open have any, and the workspace-wide call drops files outside the workspace. It is offered while the host is connected and disappears when it goes, to subagents too (the built-in `plan` and `explore` list it among their tools); a Lua or script tool of the same name takes precedence. The model has no other way into nvim: it cannot run Lua there or change a buffer, and its edits stay file writes through the harness (the host reloads them with `:checktime` after they are made).

`question` shows up like an approval box in the interactive session (a number picks an option, typed text is a free answer, Esc gives no answer) and as a prompt on stderr in `maic -p` when there is a terminal; without one the answer is empty and the model is told so. `todo` shows as `todo n/m done` in the status strip and `:todo` lists it; `maic -p` prints it to stderr.

### Live output

A command shows its output while it runs. Under its tool call the TUI keeps the last 12 lines of what `run_shell` prints, inside the call's fold, and when the command ends its result takes their place, as it always looked. A script tool shows its stderr this way (its stdout is its result), and a Lua tool what its `maic.shell` commands print. A subagent's command shows under its `↳` call. `maic -p` prints nothing new; maic-server sends the output to its clients as it comes, and the web client shows it under the call the same way.

The live view is a copy for you to watch: the model gets only the result, exactly as before (24 KiB of head and 8 KiB of tail with the omitted bytes counted, [harness.md](harness.md)). The command is never slowed for it. Output arrives in chunks every 100 ms or 16 KiB, whichever comes first; a screen or client that cannot keep up loses chunks rather than holding the command back, and the TUI says where (`[N bytes not shown: the screen fell behind]`). A remote client gets at most 64 KiB of output a second per session; what is over that is counted (`maic.skipped`) and not sent. The protocol events are in [design/engine-protocol.md](design/engine-protocol.md#4-streaming-tool-output); what a slow client misses is in [limits.md](limits.md#the-command-sandbox).

When a command's output is longer than what the model gets, the whole of it is kept beside the session, every byte as it was read with a timing index, and labelled display only wherever it is shown: `maic sessions output ID CALL` prints it and `--replay` plays it back as it ran ([sessions.md, Full output](sessions.md#full-output)).

### task

`task {agent, prompt, context?, model?}` (opencode's name and meaning; MAIC called it `delegate {profile, task}`, and older transcripts with that name still read) starts a second agent in the same workspace and runs `prompt` to completion. The child gets none of the parent's conversation: `prompt` and `context` are all it knows, and its final answer comes back as the tool result (capped at 16 KB), ending with `(the subagent used N steps, M tokens)`. The model is briefed to use `explore` for long reads or searches it does not want in its own context (asked for a short report with paths and line numbers), and `plan` for a read-only review of its own change before it calls the work done; `general` takes a self-contained piece of editing.

`agent` names an agent whose `role` is `subagent` or `all`: the built-in `explore` (auto-read, read-only tools plus read-only commands, 50k tokens), `plan` (plan mode, reads inside the workspace only, 50k tokens) and `general` (edit mode, writes anywhere in the workspace), or one from `agents` in settings ([settings.md](settings.md)). `build` is primary, the session itself, and a call naming it (or an unknown name) is an error that lists the agents task can run; the older names `scout`, `reviewer`, `builder` and `orchestrator` find `explore`, `plan`, `general` and `build`. What the child may do is its agent's, capped by the session: a child never gets a wider mode than the session is in, so under a manual session even explore's `git log` is asked about. Approvals come to you through the parent with the agent named (`explore: $ git log`); the child's tool calls show indented under the task call (`↳ explore: read_file src/a.cpp`). A child has no `task`, `task_result`, `question` or `todo` tool: one level only, and it reports to the parent in its answer. Ctrl-C and a trip stop the child along with the parent; a child stops at its agent's step and token budget, and its tokens count against the session's budget.

**Background tasks.** `task {..., background: true}` returns at once with a task id (the child's session id) and the parent's turn goes on; the child runs in parallel as a session of its own (kind `sub`, under its parent in the switcher, its own stream and transcript, steered and stopped by its own id) with everything above unchanged: its agent's narrowing capped by the session's mode, the harness, the sandbox, its budget, its model. When its first turn ends, its report reaches the parent's conversation as a note marked as coming from that session (data, not your instruction; recorded as a `context` record, never a user turn): at the parent's next step while a turn runs, or as context for the next turn when it is idle, or at its next load when it was parked. `task_result {task, wait?}` reads how it stands or its report; with `wait: true` it waits for the end (Ctrl-C or a steer ends the wait), and the report then is the tool's result and no note repeats it. Its approvals are raised on its own stream and on its parent's, naming the task, and either answer settles both. At most `max_tasks` (4) run at once per session; past that the call is refused, so the model can wait for one or run the job in the foreground. A front end that runs no background tasks runs the job in the foreground and says so in the result. The task's tokens join the session's (and its budget) when it ends. Details: [the engine protocol, step 14](design/engine-protocol.md#16-build-order).

**The child's model**, in this order: the agent's `model` in settings (your pin; the call cannot override it); the call's `model`, which may name any preset on the session preset's `subagents` list, higher or lower in tier (anything else is an error listing the allowed ones with their tiers); the preset's own pick (the same model, or the strongest non-limited tier below it when the model is `limited`: Fable hands subagents to Opus by default); the session's model when it is not a preset. The chosen preset sets the child's model, context window and thinking as `:model` would, on its own provider; a local session handing work to a remote model is told so. The tool's description lists the presets with their tiers, the default first and why, and tells the model to choose lower for wide reads, searches and mechanical work and higher only for a hard reasoning subtask. The call shows as `↳ explore on opus-5.5 (fable-5.1 is limited)`. When the child's model hits its plan's usage limit, it continues the same job, conversation and all, on the preset's `on_limit` (`explore: fable-5.1 hit its usage limit; continuing on opus-5.5`); a second limit ends it with an error naming both models. A Claude Code preset (`claude-haiku-cli`, `claude-sonnet-cli`: your Claude plan's usage) is a subagent's model only when asked for (the agent's `model`, the call's `model`) or under a parent on a Claude model; the automatic pick and `on_limit` pass it over otherwise. See [settings.md](settings.md#model-presets-and-tiers).

Every child has its own transcript, kind `sub`, in the parent's home, with the parent's id, the agent and `model_reason` in its `start` record and a `model` record for a switch ([sessions.md](sessions.md)); `maic sessions` lists it indented under the parent and `maic -r ID` opens it like any session. The parent's `tool` record for the call names the agent, the child's file, its model and `model_reason`, its steps and its tokens.

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
| `maic.nvim.diagnostics(path)` | only inside a connected host nvim ([nvim.md](nvim.md)), else `maic.nvim` is nil: a list of `{path, line, col, severity, message, source}` for `path`, or for every open file in the workspace without one | `diagnostics path` (a read; the workspace without a path) |
| `maic.nvim.buffers()` | the same condition: the host's file buffers in the workspace, `{bufnr, path, modified, loaded}` | `buffers` (a read of the workspace) |
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
