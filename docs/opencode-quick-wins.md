# opencode quick wins for MAIC

Read-only audit for Micaiah, a follow-up to [opencode-comparison.md](opencode-comparison.md). That page compared the two designs and picked ten large items (resume, compaction, arity keys, reject with feedback, doom loop, a `permission` block, diff at the approval prompt, snapshots, `question` and `glob`, custom commands). None of those is repeated here. This page is finer-grained: small, concrete things opencode does that one person can put into MAIC in well under a day each, ranked by value for the effort.

opencode was read at commit `2fa3363c` (2026-09-29) in `~/dev2/tools-and-things/opencode`; paths below are relative to `packages/` in that repo (`opencode/src/...` and `tui/src/...`). MAIC paths are relative to `~/dev2/MAIC`, at the tree as of 2026-09-30 (resume, `-c` / `-r`, forks and the help system already exist).

Rules that take precedence over anything here: the root-owned tripwire with sudo unlock; bubblewrap for every model-run command; the manual / auto-read / edit / auto / plan ladder; secrets never readable; no network from the core beyond the chosen model endpoint; session files append-only; and the cleanroom policy in [cleanroom.md](cleanroom.md). opencode is MIT, so reading it is fine; every item below describes behaviour to reimplement, not code to copy. Where an item touches the harness, the caveat says how it stays inside those rules.

## Under two hours

### 1. Tell the model where it is in a file

**Effort:** 1 hour. **User gets:** fewer wasted reads. Small local models re-read files from the top because they cannot tell whether a truncated read ended at the end of the file or in the middle.

**opencode:** the read tool ends every result with one of three lines: `(Showing lines 1-2000 of 5312. Use offset=2001 to continue.)`, `(Output capped at 50 KB. Showing lines 1-812. Use offset=813 to continue.)`, or `(End of file - total 140 lines)` (`opencode/src/tool/read.ts:341-350`). It caps by bytes as well as lines, 50 KB (`read.ts:16`, `read.ts:164-173`), so one file with long lines cannot eat the context. The description tells the model to use a larger window rather than tiny repeated slices (`opencode/src/tool/read.txt:13`). Directory reads end with `(N entries)` (`read.ts:279-281`).

**In MAIC:** `read_file` in `core/src/tools.cpp:63-88` already numbers lines and says `... (more lines; read again with offset=N)`; add the total line count when the read finishes, the byte cap, and the end-of-file line. `list_dir` (`tools.cpp:90-104`) gets an entry count. Adjust the description in `tool_schemas()` (`tools.cpp:179-183`).

### 2. Edit tool hygiene: identical strings, empty old_string, replace_all, clearer errors

**Effort:** 1 to 2 hours. **User gets:** fewer failed edits and fewer "old_string not found" loops.

**opencode:** rejects `oldString === newString` with "No changes to apply" (`opencode/src/tool/edit.ts:75-77`); rejects an empty `oldString` on an existing file and points at the write tool for a full replacement (`edit.ts:90-96`, `edit.ts:686-690`); has a `replaceAll` parameter for renames (`edit.ts:53-55`, `edit.ts:714-716`); normalises CRLF in the search and writes the file's own line ending back (`edit.ts:22-33`, `edit.ts:129-131`); and its not-found message says what exactly must match: "It must match exactly, including whitespace, indentation, and line endings" (`edit.ts:723-728`). The description tells the model the two failure messages and what to do about each (`edit.txt:8-10`). Its one cheap fallback matcher compares lines with trailing and leading whitespace trimmed (`LineTrimmedReplacer`, `edit.ts:248-286`); the fuzzier ones are in the skip list.

**In MAIC:** `edit_file` in `core/src/tools.cpp:152-164` and its schema at `tools.cpp:191-193`. Add the identical-string and empty-string checks, an optional `replace_all` boolean, the line-trimmed retry when the exact search misses, and the fuller messages. `tool_action` (`tools.cpp:207`) is unchanged, so the harness sees the same write.

### 3. Show additions and deletions in the edit result

**Effort:** 1 hour. **User gets:** the transcript line `edited src/foo.cpp (+3 -1)` instead of `edited src/foo.cpp`, which is enough to spot a runaway rewrite at a glance.

**opencode:** counts added and removed lines from the diff and stores them beside the patch (`opencode/src/tool/edit.ts:175-186`); the TUI shows `+12 -3` in green and red per file (`tui/src/routes/session/index.tsx:1246-1250`).

**In MAIC:** in `edit_file` (`core/src/tools.cpp:152-164`) count the lines of `old_s` and `new_s`, and for `write_file` (`tools.cpp:224-228`) the old and new file line counts; append `(+a -d)` to the result text. Both the TUI and the JSONL transcript get it for free through `on_tool_result`.

### 4. Tool descriptions that head off small-model mistakes

**Effort:** 1 hour. **User gets:** fewer approval prompts in manual mode and fewer broken edits, because the model reaches for `read_file` and `search_files` instead of `cat` and `grep` through `run_shell`, and stops pasting line-number prefixes into `old_string`.

**opencode:** the edit description spells out the line-number prefix format from the read tool and says never to include it in `oldString` (`opencode/src/tool/edit.txt:5`). The bash description says in capitals that the tool is for git, npm, docker and the like, not for reading, writing or searching files, and lists the replacement for each: file search, content search, read, edit, write, and "Communication: Output text directly (NOT echo/printf)" (`opencode/src/tool/shell/shell.txt:9`, `opencode/src/tool/shell/prompt.ts:100-106`). The read and glob descriptions say to call tools in parallel when several files are wanted (`read.txt:12`, `glob.txt:6`). The system prompt adds "Never use tools like Bash or code comments as means to communicate with the user" (`opencode/src/session/prompt/default.txt:14`).

**In MAIC:** the schemas in `core/src/tools.cpp:177-200` and the `# Tools` paragraph of `Agent::system_prompt` (`core/src/agent.cpp:82-86`). MAIC's read output uses `N<tab>line`, so the edit_file description should say "everything after the tab is file content; never include the number or the tab". Keep it to a few lines: the whole point is a 4B model reading it.

### 5. "Did you mean" on a missing file, and "that is a directory"

**Effort:** 1 hour. **User gets:** one failed call instead of three when the model misremembers a filename.

**opencode:** on a missing file the read tool lists up to three entries of the parent directory whose names contain the requested basename (or vice versa, case-insensitive) under "Did you mean one of these?" (`opencode/src/tool/read.ts:76-99`). The edit tool answers "Path is a directory, not a file" (`edit.ts:125`) instead of a raw open error. The earlier comparison listed this in one line; this is the shape.

**In MAIC:** `read_file` returns `can't open <path>` (`core/src/tools.cpp:66-69`) and `edit_file` throws from `read_whole` (`tools.cpp:39-47`). Add the sibling scan in the read path only, after the harness has allowed the read (the directory listing is a read of the same directory, so it needs no extra check inside the workspace; outside the workspace, only run it when the read itself was allowed).

### 6. Refuse binary files in read_file

**Effort:** under 1 hour. **User gets:** no more 2000 lines of garbage in the context when the model reads a `.o`, a `.png` or `build/` output.

**opencode:** a fixed extension list plus a 4 KB sample check (a NUL byte, or more than 30 % non-printable bytes, means binary) and the error "Cannot read binary file" (`opencode/src/tool/read.ts:182-227`, `read.ts:327-329`).

**In MAIC:** `read_file` in `core/src/tools.cpp:63-88`. `search_files` already skips lines with NUL (`tools.cpp:133-135`), so the same test moved into a helper serves both.

### 7. Shell result wording: no output, timeout advice, cancelled

**Effort:** under 1 hour. **User gets:** a model that retries a long build with a longer timeout instead of concluding the build is broken.

**opencode:** an empty result becomes `(no output)` (`opencode/src/tool/shell.ts:577`); a timeout appends "shell tool terminated command after exceeding timeout N ms. If this command is expected to take longer and is not waiting for interactive input, retry with a larger timeout value" (`shell.ts:562-565`); a user interrupt appends "User aborted the command" (`shell.ts:566`); these notes go in a `<shell_metadata>` block after the output (`shell.ts:582-584`), and for shell output opencode keeps the tail rather than the head, because errors are at the end (`shell.ts:575`).

**In MAIC:** `run_shell` in `core/src/tools.cpp:166-173` builds `status + "\n" + output`. Add `(no output)`, extend the timeout sentence with the retry advice and the current maximum (600 s, `tools.cpp:23`), and keep MAIC's head-plus-tail trim in `core/src/sandbox.cpp` (`trim_output`) as it is; it already covers the tail case.

### 8. Repair a miscased or unknown tool name instead of erroring

**Effort:** 1 hour. **User gets:** local models that emit `Read_File` or `readFile` still work; an unknown name gets a useful answer.

**opencode:** if the tool name only differs by case from a real tool, the call is rewritten to the real name; otherwise the call is redirected to a hidden `invalid` tool whose result is "The arguments provided to the tool are invalid: <error>" so the model sees a normal tool result and corrects itself (`opencode/src/session/llm.ts:296-311`, `opencode/src/tool/invalid.ts:14-19`).

**In MAIC:** `tool_action` throws `unknown tool: NAME` (`core/src/tools.cpp:209`) and `run_tool_call` turns it into `error: unknown tool` (`core/src/agent.cpp:285-290`). Lower-case the name before matching, and when it still misses, answer with the list of real tool names in the same result. This runs before the harness check, so it is still gated exactly as before once the name is resolved.

### 9. The approval prompt says exactly what "always" will cover

**Effort:** under 1 hour. **User gets:** she knows, before pressing `a`, whether it means "this file", "git", or "rm".

**opencode:** choosing always first shows a confirmation listing the patterns that will be allowed and how long that lasts ("This will allow the following patterns until OpenCode is restarted", one line per pattern) (`tui/src/routes/session/permission.tsx:138-163`).

**In MAIC:** `render_approval` prints `[a] always allow this program this session` (`cli/src/tui.cpp:463-477`) and the headless prompt prints `[a] always this session` (`cli/src/headless.cpp`). Add the key to `ApprovalRequest` (`core/include/maic/agent.hpp:21-26`), filled from `Harness::approval_key` (`core/src/harness.cpp:192-204`) in `run_tool_call` (`core/src/agent.cpp:299`), and print `[a] always: git (this session)`. When the arity keys from the earlier comparison land, this line shows them automatically.

### 10. A footer after every reply: model and how long the turn took

**Effort:** 1 to 2 hours. **User gets:** a feel for how slow each model is, and a clear end-of-turn marker in the scrollback; "interrupted" is marked the same way.

**opencode:** after the last part of an assistant message it prints `▣ Build · claude-opus · 12.3s`, the duration measured from the user message that started the turn (`tui/src/routes/session/index.tsx:1481-1486`, `index.tsx:1556-1566`), and `· interrupted` when aborted. Durations format as `850ms`, `12.3s`, `1m 5s`, `1h 2m` (`tui/src/util/locale.ts:39-59`).

**In MAIC:** the worker lambda in `App::start_turn` (`cli/src/tui.cpp:693-714`) wraps `agent_.submit`; take a `steady_clock` timestamp before and after and post a `Kind::Notice` (`cli/src/view.cpp:16-29`) such as `qwen3.5:9b · 41.2s · 3 tool calls`. Count tool calls in `on_tool_call`. No core change; the headless printer can print the same line to stderr.

### 11. Export a session to markdown; copy the last reply

**Effort:** 1 to 2 hours. **User gets:** a readable transcript to paste into an issue or a note, without reading JSONL.

**opencode:** `formatTranscript` writes `# title`, session id and dates, then `## User` / `## Assistant (agent · model · 12.3s)` sections, `_Thinking:_` blocks if enabled, and `**Tool: name**` with input and output in fenced blocks if tool details are enabled (`tui/src/util/transcript.ts:26-114`). It is bound to `<leader>x` (export to the editor) and there is a `/copy` slash command for the clipboard (`tui/src/config/keybind.ts:86-87`, `tui/src/routes/session/index.tsx:918-960`); `<leader>y` copies just the last assistant message (`keybind.ts:146`, `index.tsx:895-912`).

**In MAIC:** `load_session` already produces `TranscriptEntry` records (`core/src/session.cpp:142-178`, `core/include/maic/session.hpp:82-94`). Add `maic sessions export ID [FILE]` in `cli/src/main.cpp` (beside `print_sessions`, `main.cpp:160-167`) that renders those entries as markdown, and `:export FILE` plus `:copy` in `commands.cpp` (`cli/src/commands.cpp:108-139`) and `run_command` (`cli/src/tui.cpp:784-909`). `:copy` puts the last `Kind::Assistant` entry in the register and calls `copy_to_clipboard` (`core/include/maic/clipboard.hpp`). Caveat: the markdown is written by the user into a path she names; a tool never calls it, so it is outside the harness.

### 12. `maic sessions` grouped by day, with the workspace shown

**Effort:** 1 hour. **User gets:** a list she can scan when there are fifty sessions.

**opencode:** the session picker groups entries under "Today" or the date, and shows the project directory as a footer on each row when it differs from the current one (`tui/src/component/dialog-session-list.tsx:229-236`, `dialog-session-list.tsx:259-266`); a busy session shows a spinner in the gutter.

**In MAIC:** `print_sessions` in `cli/src/main.cpp:160-167` and `list_sessions` in `core/src/session.cpp:69-113`. `SessionInfo.started` is already `YYYYMMDD-HHMMSS`; print a `Today` / date header when the date changes, the time, the basename of the workspace, turns, and the first-prompt preview. Same list for the `maic -r` picker (`main.cpp:181-186`).

### 13. Jump between messages in the conversation window

**Effort:** 1 hour. **User gets:** `}` and `{` move to the next and previous message, so she can read a long turn one message at a time instead of by line.

**opencode:** has bindable `messages_next`, `messages_previous`, `messages_last_user`, `messages_first` and `messages_last` actions (`tui/src/config/keybind.ts:141-145`).

**In MAIC:** `View::handle` in `cli/src/view.cpp:262-383`. Every laid-out `Line` records its entry index and `first` flag (`view.cpp:136`); `}` moves `cur_line_` to the next line with `first == true` whose entry differs, `{` the reverse; with a count. Add `[[` / `]]` for user messages only (`Kind::User`). Document them in the `conversation` topic of `cli/src/commands.cpp:87-89`.

### 14. Per-entry timestamps, off by default

**Effort:** 1 hour. **User gets:** `:set timestamps on` shows `14:02` beside each message, useful after resuming a session from yesterday.

**opencode:** a "timestamps" toggle stored per user, default hidden, in the command palette as `/timestamps` (`tui/src/routes/session/index.tsx:262`, `index.tsx:277`, `index.tsx:695-700`).

**In MAIC:** `View::Entry` (`cli/src/view.hpp`) gets a `time_t`; `View::append` (`cli/src/view.cpp:57-61`) stamps it; the prefix column in `View::render` (`view.cpp:421-424`) grows by six characters when the toggle is on. Wire `timestamps` into `:set` (`cli/src/tui.cpp:828-835`) and `complete_argument` (`cli/src/commands.cpp:161`). The JSONL already has a `time` field on every record, so resumed entries can carry it too.

### 15. A `workdir` argument for run_shell

**Effort:** 1 to 2 hours. **User gets:** in a monorepo the model runs `pytest` in `services/api` without `cd services/api && ...`, which keeps the command text short and readable at the approval prompt.

**opencode:** the bash tool takes an optional `workdir` and the description says to use it instead of `cd` (`opencode/src/tool/shell/prompt.ts:19-21`, `prompt.ts:112-118`, `prompt.ts:260-261`).

**In MAIC:** add `workdir` to the `run_shell` schema (`core/src/tools.cpp:194-197`), resolve it with `Harness::resolve` in `tool_action` (`tools.cpp:208`) and pass it to `run_sandboxed` (`core/src/sandbox.cpp`) as a `--chdir` argument to bwrap. Harness caveat: a `workdir` outside the workspace must be treated like a write outside the workspace (`Ask`), and the read-only classifier is unaffected because the command text is unchanged. Show it in `tool_summary` (`tools.cpp:212-216`) as `$ (in services/api) pytest`.

### 16. Stash and pop the input draft

**Effort:** 1 hour. **User gets:** `:stash` parks a half-written prompt so she can ask something else first, `:pop` brings it back; it survives restarts.

**opencode:** a prompt stash of up to 50 entries kept in `prompt-stash.jsonl` under the state directory, with push, pop and list commands (`tui/src/prompt/stash.tsx:1-60`, `tui/src/component/prompt/index.tsx:736-780`).

**In MAIC:** `Editor::text()` and `replace_text` (`cli/src/editor.cpp:52-61`); a JSONL under `state_dir()` (`core/include/maic/paths.hpp`). Two commands in `commands.cpp` and `run_command`. Lowest value on this list, but it is genuinely an hour.

## Half a day

### 17. Retry with backoff on provider errors

**Effort:** half a day. **User gets:** a 429 or a 503 from OpenRouter or DeepSeek, or a dropped connection, no longer ends the turn with an error she has to re-send; the status strip shows `retrying in 4s (attempt 2/5)`.

**opencode:** up to 5 retries; 2 s initial delay, factor 2, 25 % jitter, capped at 30 s when the response carries no `Retry-After`; `retry-after-ms` and `retry-after` (seconds or HTTP date) are honoured when present (`opencode/src/session/retry.ts:26-31`, `retry.ts:47-83`). Retryable means any 5xx, or a message matching 429 / rate limit / overloaded / connection reset / timeout patterns; context-overflow errors are never retried (`retry.ts:33-41`, `retry.ts:85-98`). The prompt footer shows the truncated error plus `[retrying in 4s attempt #2]` with a live countdown (`tui/src/component/prompt/index.tsx:1567-1574`), and "Overloaded" is shown as "Provider is overloaded" (`retry.ts:145`).

**In MAIC:** wrap the `chat(...)` call in `Agent::submit` (`core/src/agent.cpp:208-241`) in a retry loop that sleeps in 50 ms slices checking `cancel` and `deliver_now_`, and reports through `events.on_notice`. `HttpResult` in `core/src/llm_http.hpp:15-18` has `status` and `error_body` but no headers; add an optional `retry_after` seconds field filled by `stream_post`. Only 429 and 5xx retry; 400, 401, 403 and 404 never do. Harness caveat: every retry against a REMOTE provider is another outbound request carrying the same prompt; keep the count in the notice and log each attempt to the session file so the record shows how many times the data left the machine.

### 18. Token usage and context percentage in the status strip

**Effort:** half a day. **User gets:** `9.8k / 16k (61%)` beside the model name, so on a 16k local context she sees the wall coming before the model starts forgetting. It is also the first step towards the planned per-session budgets in [harness.md](harness.md).

**opencode:** reads the last assistant message's input, output, reasoning and cache tokens, divides by the model's context limit, and prints `12,345 (45%)` and the session cost under the prompt (`tui/src/component/prompt/index.tsx:264-281`); when a provider reports nothing it estimates at four characters per token (`core/src/util/token.ts:1-5`).

**In MAIC:** `Message` (`core/include/maic/llm.hpp:21-32`) gets `input_tokens` and `output_tokens`; `ollama.cpp` fills them from the final chunk's `prompt_eval_count` / `eval_count`, `openai.cpp` from `usage` (with `stream_options.include_usage` in the request), `anthropic.cpp` from `message_start` and `message_delta`. `Agent` keeps the last values; `render_top_status` (`cli/src/tui.cpp:415-433`) prints them, with the percentage against `ChatOptions::num_ctx` (`llm.hpp:62`) for Ollama and against a per-provider `context` option in settings for the rest. Adding fields to `Message` is additive: `message_to_json` writes two more keys, old session files load unchanged. No cost figure (see the skip list).

### 19. Collapsed tool output that can be expanded

**Effort:** half a day. **User gets:** the conversation stays readable (a few lines per tool result, as now) but the full output is still there: `za` on a result unfolds it, `:set tooldetails off` hides results entirely.

**opencode:** tool output is cut to a line and character budget with a trailing `…` and a "Click to expand" hint; each block toggles individually (`tui/src/util/collapse-tool-output.ts:1-19`, `tui/src/routes/session/index.tsx:1802-1830`). A global "tool details" toggle hides completed tool blocks altogether (`index.tsx:263`, `index.tsx:725-741`).

**In MAIC:** `App::on_tool_result` posts `preview(text)` and drops the rest (`cli/src/tui.cpp:49-61`, `tui.cpp:198`). Keep the full text on the `View::Entry` with a `folded` flag and let `View::layout` (`cli/src/view.cpp:97-148`) lay out either the first eight lines plus `… +N lines` or everything. `za` in the conversation window toggles the entry under the cursor (`View::handle`, `view.cpp:262-383`); `zR` / `zM` open and close all, vim style. Resumed sessions already have the full result text in the JSONL (`core/src/session.cpp:172`), capped at 64 KB per result (`core/src/agent.cpp:13`).

### 20. Attach nested instruction files when a file under them is read

**Effort:** half a day. **User gets:** a `services/api/AGENTS.md` takes effect the moment the model reads something in `services/api/`, without being in the context of every session in that repo.

**opencode:** after a successful read, it walks from the file's directory up to the project root; every `AGENTS.md` found on the way that is not already in the system prompt and has not been attached earlier in the conversation is appended to the read result as `Instructions from: <path>` inside a system-reminder block, once (`opencode/src/session/instruction.ts:178-217`, `opencode/src/tool/read.ts:300`, `read.ts:355-357`).

**In MAIC:** `load_instructions` (`core/src/instructions.cpp`, header `core/include/maic/instructions.hpp:18-19`) only walks from `$HOME` down to the workspace. In `Agent::run_tool_call` (`core/src/agent.cpp:329-330`), after a successful `read_file`, check the directories between the workspace and the file for the configured `instruction_files` names, skip any already in `instructions_` or in a per-conversation `attached_` set (cleared in `clear()` and `restore()`), and append `# Instructions from <path>` to the result text. Harness caveat: only directories inside the workspace are walked, and the file goes through `Harness::is_secret` like any read. Cleanroom: the walk-up-and-attach-once idea is described here; MAIC's `instruction_files` setting already names the files.

### 21. Session titles from a small model, and `:rename`

**Effort:** half a day. **User gets:** `maic sessions` shows "Fix tripwire race in service.cpp" instead of the first 72 characters of her first prompt; `:rename TITLE` sets it by hand.

**opencode:** after the first real user message it runs a no-tool call with a "small" model (the provider's designated cheap model, else the session model) and the prompt in `opencode/src/agent/prompt/title.txt`: one line, at most 50 characters, no tool names, keep exact technical terms and filenames, drop articles, never answer the question. The reply is stripped of `<think>...</think>`, the first non-empty line is taken and capped at 100 characters (`opencode/src/session/prompt.ts:193-253`); the default title is `New session - <ISO time>` until then (`opencode/src/session/session.ts:48-54`); `ctrl+r` renames (`tui/src/config/keybind.ts:93`).

**In MAIC:** a `title` record in the JSONL (`SessionLog::write`, `core/src/session.cpp:188-194`); `list_sessions` (`session.cpp:84-107`) prefers it over `first_prompt`. The generation call is `chat()` with an empty tools array (`core/include/maic/llm.hpp:77-78`) after the first `submit` returns, on the worker thread, behind a `title_model` setting that defaults to off. Caveats: a remote `title_model` is another outbound call with the first prompt in it, so it must show the REMOTE label like any model; the record is appended, never edited, so a rename is a second `title` record and the last one wins.

### 22. DeepSeek: replay reasoning on assistant turns

**Effort:** half a day, most of it verification. **User gets:** `deepseek/deepseek-reasoner` keeps working through multi-step tool turns.

**opencode:** for any model whose id contains `deepseek`, every replayed assistant message must carry a reasoning part, empty if none was kept (`opencode/src/provider/transform.ts:303-317`), and OpenAI-compatible providers get the reasoning text back as `reasoning_content` on the assistant message (`transform.ts:329-347`).

**In MAIC:** `core/src/openai.cpp` replays `content` and `tool_calls` only. Before changing anything, confirm the requirement against DeepSeek's public API reference (public docs are allowed input under [cleanroom.md](cleanroom.md)); if it holds, store the streamed `reasoning_content` in `Message::raw` with `raw_kind = "openai"` (`core/include/maic/llm.hpp:28-31`) and send it back for the deepseek provider only. Do not add it for every OpenAI-compatible endpoint; llama.cpp and vLLM reject unknown fields in some versions.

## A day

### 23. A manual `:compact` using opencode's summary template

**Effort:** one day. **User gets:** a way to keep going on a 16k local context when the conversation fills up, on her command, with the old conversation intact in the session file. The earlier comparison chose the two-stage design; this is the concrete prompt and record shape for the manual half.

**opencode:** the conversation is serialised as `[User]: ...`, `[Assistant]: ...`, `[Assistant tool call]: name({...})`, `[Tool result]: ...` with each tool result cut to 2000 characters (`opencode/src/session/compaction.ts:51-85`). The summariser gets the system prompt in `opencode/src/agent/prompt/compaction.txt` and a user prompt that demands a fixed markdown template: Objective, Important Details, Work State (Completed / Active / Blocked), Next Move, Relevant Files, with rules to keep every section, use terse bullets, preserve exact paths and commands, and never mention that context was compacted (`core/src/session/compaction.ts:16-46`). A second compaction is told to merge the prior summary and that the prior summary is discarded afterwards (`compaction.ts:47-55`). After compaction the agent is nudged with "Continue if you have next steps, or stop and ask for clarification if you are unsure how to proceed" (`opencode/src/session/compaction.ts:531`). The summariser runs with no tools (`compaction.ts:429`). `<leader>c` triggers it by hand (`tui/src/config/keybind.ts:99`).

**In MAIC:** `Agent::compact()` serialises `messages_`, calls `chat()` with no tools, then replaces `messages_` with a fresh system prompt plus one user message holding the summary, and writes a `compact` record with the summary text to the session log; `load_into` (`core/src/session.cpp:142-178`) treats `compact` like `clear` followed by the summary message. `:compact` joins the idle-only commands in `run_command` (`cli/src/tui.cpp:794-797`). Caveats: this is a new conversation, not an edit of the old one, so the Anthropic replay rule and the append-only file both hold; the summary goes to the same provider as the session, so nothing new leaves the machine; the summariser gets no tools, so the harness is not involved.

## Skip these even though they look small

* **Fuzzy edit matching** (block-anchor, whitespace-normalised, indentation-flexible and escape-normalised replacers, `opencode/src/tool/edit.ts:288-644`). They let a model edit a span that merely resembles what it asked for; opencode itself needs a "disproportionate match" guard on top (`edit.ts:731-737`). Line-trimmed matching (item 2) is the only one with a clear failure mode.
* **Automatic pruning of old tool results** (`opencode/src/session/compaction.ts:271-317`, replacing outputs with "[Old tool result content cleared]"). It rewrites history in place, which MAIC's append-only session file and its Anthropic replay rule both refuse. `:compact` (item 23) is the version that fits.
* **Cost accounting and `stats`** (`opencode/src/cli/cmd/stats.ts`). Prices come from the models.dev catalog fetched at runtime; MAIC would need a hand-kept price table in settings for a number that is wrong the day a provider changes prices. Token counts (item 18) give her the signal without the fiction.
* **Paste summaries** (`[Pasted ~N lines]` placeholders, `tui/src/component/prompt/index.tsx:1207-1213`). FTXUI 5 has no bracketed-paste event, so MAIC cannot tell a paste from fast typing without a heuristic. `:e` into nvim already handles big inputs.
* **`@file` mentions with autocompletion** (`tui/src/component/prompt/autocomplete.tsx`). A day of editor work in `cli/src/editor.cpp` for something `!cat path` already does, with the output logged as context.
* **Formatter runs after edits** (`opencode/src/tool/edit.ts:112`, `edit.ts:156`) and **LSP diagnostics in the edit result** (`edit.ts:197-201`). Both spawn programs outside the sandbox on the model's behalf.
* **Persistent "always" replies** (the `save` patterns and the permission table in `packages/core/src/permission/`). Session-scoped `always_allowed_` is deliberate.
* **Session pinning and quick slots** (`tui/src/config/keybind.ts:107-116`). MAIC runs one session per process; `maic -r` with a prefix already gets there.
* **The retry upsell branches** (`opencode/src/session/retry.ts:99-144`) and anything that shares, fetches or updates over the network.
* **Reading `~/.claude/CLAUDE.md` by default** (`opencode/src/session/instruction.ts:60-68`). Already rejected in the earlier comparison; `instruction_files` in settings is the explicit list.
