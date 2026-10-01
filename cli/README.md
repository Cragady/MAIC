# cli

The `maic` command. With no arguments it starts the agent in the current directory, which becomes the workspace. `maic help` lists the rest.

```sh
maic vendor add llamacpp           # once: build llama.cpp (docs/llamacpp.md)
maic vendor use llamacpp /path/to/model.gguf   # once per model: the GGUF it serves
cd ~/some/project
maic up llamacpp                   # once per boot
maic up llamacpp-2                 # optional: the side server on 8082, a second resident model (docs/llamacpp.md, Two servers)
maic                               # the agent, on llamacpp/current
maic --model anthropic/claude-opus-5-5 --mode auto-read
maic -c                            # continue the last session started in this directory
maic -r                            # pick an earlier session from a list (maic -r ID for one you know, maic -r PATH for any transcript file)
maic -p "explain main.cpp"         # one turn, no UI, no transcript; --record keeps one; -c / -r load an old one; --json for events
cat dialog.txt | maic -p -         # the prompt from stdin
maic -p "summarise these" -C notes.md -C log.txt   # files attached as context before the prompt (also for the interactive maic)
maic -p "let's plan the refactor" -i               # an interactive session that opens with that prompt sent
cat prompt.txt | maic -pi -        # short flags cluster (-p -i); a value-taking flag (-m, -C) goes last in a cluster
maic --system @~/prompts/reviewer.md --no-instructions   # front-load behaviour; ignore every MAIC.md / AGENTS.md
maic --prefix "Sure thing! "                              # every reply starts with these words, guaranteed (:h prefix)
maic --rule "Always answer in French"                      # a standing instruction, reminded every turn (:h rule)
maic --model opus-5.5                                      # a preset: model, 1M context, Sonnet 5 as the reviewer, thinking on
maic --xtc 0.5,0.1 --sampling min_p=0.05 --ban-pattern @~/bans/tics.re   # samplers and bans for one run
maic help headless                 # the verbose page for all of the above (same as :h headless in a session)
maic help | grep vendor            # help goes to stdout, so it pipes; maic help topics lists every page
maic lua                           # a LuaJIT REPL in this directory with the maic table loaded
maic sessions                      # every session, with a preview
maic artifacts                     # where MAIC and its services keep transcripts, logs, outputs
maic settings init                 # a documented settings file (docs/settings.md)
maic doctor                        # the machine, the tools MAIC needs, installed models, a recommended setup
maic setup                         # the first run as yes/no questions: settings, llama.cpp, ComfyUI, a model, the tripwire
```

## Screen

```
 conversation window          (markdown rendered; Ctrl-W k to move into it)
 ⏵ manual (shift-tab to cycle)        qwen3.5:4b local · harness armed · 1 queued (:w now)
 ───────────────────────────────────────────────────────────────────────────────────────
 ❯ your input                 (markdown highlighted as you type)
  INSERT   ↑12 (G follows)                                       Ctrl-W k: conversation · :help
```

The strip above the input shows the agent mode, the model, whether it is local or `REMOTE`, the harness state, queued messages and whether the agent is working. The bottom line shows the vim mode, the focused window, your position, and the last message.

## Keys

The input starts in normal mode, like opening vim: `i` to type. Cursor: a bar in insert mode, a block in normal mode. **Alt+Enter** (or `:w`) sends from any mode; **Enter** is a newline. nvim has no default Alt mappings, so nothing is lost.

| Where | Keys |
| :--- | :--- |
| insert | type; **Enter** new line; **Esc** to normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register, **Ctrl-R** `{reg}` a named one; **Ctrl-O** runs one normal-mode command and comes back; ↑ ↓ or Ctrl-P / Ctrl-N prompt history, which persists across sessions (the last 500 prompts) |
| normal (input) | `i a I A o O s S R` insert (a count repeats what you type: `3ix<Esc>`); `h j k l w b e ge 0 ^ $ gg G` move (Enter = down a line); `f{c} F{c} t{c} T{c}` to a character, `;` `,` repeat; `}` `{` paragraphs, `)` `(` sentences; `x X D C J r{c} ~` edit; operators `d c y > < gq gu gU g~` + motion, doubled for the line (`dd cc yy >> gqq guu gUU g~~`), `Y`; text objects after an operator or in visual mode: `iw aw iW aW ip ap is as i" a" i' i` i( a( ib i[ a[ i{ a{ iB i< a<` (so `ciw`, `dap`, `gqip`, `di"`, `ya(`, `viw`); `.` repeats the last change (with its count, or `5.`); `m{a-z}` marks, `'a` / `` `a `` jump, `''` back; `"a`..`"z` name a register, `"A` appends; `v V` select; `p P` paste (lines go on their own line); `u` undo, **Ctrl-R** redo (multi-level; an insert session is one step); counts everywhere (`3w`, `2d3w`, `3fa`, `2p`, `3J`); `:e` or **Ctrl-X Ctrl-E** opens the input in `$VISUAL` / `$EDITOR` / nvim as markdown and loads it back when you quit |
| visual | `y d x c s` on the selection; `> <` shift, `J` join, `~ u U` case, `r{c}` replace every character, `gq` re-wrap; `o` swaps the ends; text objects (`vip` goes linewise) |
| clipboard | `"+y` / `"*y` before any yank sends it to the system clipboard; `"+p` pastes from it; the **leader** (Space by default, `leader` in settings) then `y` yanks the line (normal) or the selection (visual) to the clipboard, leader then `p`/`P` pastes from it. In the conversation window every yank reaches the clipboard; `yiw`, `yw`, `y$`, `Y`, `yy` work there |
| normal (input empty) | `j k` Ctrl-D/U Ctrl-F/B `G` scroll the conversation without leaving the input; `v` / `V` jump into the conversation window selecting |
| conversation window | **Ctrl-W k** enters, **Ctrl-W j** (or Esc, `i`, Enter) returns; `j k h l w b e 0 $ gg G` Ctrl-D/U/F/B move; `f t F T ; ,` on the line; `}` / `{` next / previous message, `]]` / `[[` your messages only; `v` / `V` select; `y` yanks (to the register **and** the system clipboard); `yy` a line; `/pattern` then `n` / `N` search (smart case); `o` swaps selection ends |
| anywhere | **Shift-Tab** cycles the mode; **Ctrl-Z** suspends to the shell (`fg` resumes, like vim); **Ctrl-C** interrupts the agent, else stops a `!command`, else clears the input, else (twice) quits; scroll wheel scrolls the conversation (in insert mode: prompt history) |
| approval prompt | **y** yes · **n** no · **N** no, then type a sentence the model receives as the reason · **a** always allow this file / program for the session · **t** trip the harness. Edits show the lines that would change |

Ctrl-W in insert mode deletes a word, as in vim; the window chord works from insert mode only when the input is empty, otherwise press Esc first.

The system clipboard is reached through `wl-copy` or `xclip` when present, and always through the terminal (OSC 52), which also works over ssh. Your terminal's own copy (Ctrl-Shift-C) keeps working on text you select with the mouse; with the scroll wheel enabled, that selection needs Shift+drag.

## Commands

`:` in normal mode, or from the conversation window. Every command also works typed as a message beginning with `/`. As you type after `:`, a palette lists the matching commands with a one-line description (arguments too, for `:mode`, `:set`, `:h`, `:model`, `:up`); **Tab** completes to the highlighted one and cycles on repeat, Shift-Tab goes back. A unique prefix runs the command (`:inst` is `:instructions`), as in vim.

| Command | Does |
| :--- | :--- |
| `:w` | send the input (same as Alt+Enter). `:w now` or `:ww` sends immediately even while the agent is working (see below) |
| `:e` | edit the input in nvim (`$VISUAL`, then `$EDITOR`, then `nvim`); a non-zero exit leaves the input unchanged |
| `:mode manual\|auto-read\|edit\|auto\|plan` | set the agent mode |
| `:model NAME` | switch model (when idle): `llamacpp/current`, `llamacpp/Qwen3.5-9B-Q4_K_M` (any GGUF under the models directory), `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, ... `:model` alone lists providers |
| `:models` | models the current provider serves (llama.cpp: every GGUF under the models directory, by file name) |
| `:think on\|off` | let the model reason before answering |
| `:set markdown\|mouse on\|off` | rendering and scroll-wheel toggles |
| `:!cmd` or `!cmd` | run a command in **your** shell, unsandboxed, in the workspace; output shows in the conversation and is passed to the model as context (Ctrl-C stops it) |
| `:status` | harness, every service with where it runs (`[host]` pid or `[docker]` container) and what it holds (resident model; ComfyUI's VRAM and queue), model and whether it is remote, session file, queue. See `:h status` |
| `:up NAME` / `:down NAME` | start / stop a service |
| `:init` | scaffold `MAIC.md` and `.maic/settings.json` here, then have the agent draft the `MAIC.md` from the project |
| `:settings` | which settings files are in effect and where this session's transcript lives |
| `:instructions` | the MAIC.md / AGENTS.md files in effect |
| `:session` / `:artifacts` | where this transcript is; where everything is kept, with sizes |
| `:path [NAME] [copy]` / `:open NAME` | every place maic knows by a short name (`workspace`, `session`, `sessions`, `models`, `workflows`, `templates`, `vendor/llamacpp`, `comfyui/outputs`, ...); show one, copy it to the clipboard, or open it in the file manager. `maic path`, `maic open`, and `eval "$(maic shell-init)"` for `mcd NAME` in your shell. See `:h path` |
| `:gpu [free [llamacpp\|llamacpp-2\|comfyui]]` | who holds the card (each llama server's resident model, ComfyUI's VRAM view) and one sentence on whether the two models fit it; `free` unloads without stopping anything. `maic gpu` in the shell. A failed `maic up` explains a CUDA out of memory in plain words |
| `:ctx [N]` / `:ctx2 [N]` | the context window of the main llama server (`--ctx`, `context`) and of the side server `llamacpp-2` (`--ctx2`, `context_2`); setting one restarts that server when it runs with another size. See `:h ctx` |
| `:open NAME folder` / `maic cd NAME [--subshell]` | the containing folder in the file manager; the place's directory printed for `cd "$(maic cd NAME)"`, or with `--subshell` a shell there (`exit` returns) |
| `:open SERVICE [firefox\|chrome]` | a service's URL in the browser (`browser` in settings picks the default one; `remote` in settings opens a subscribed maic-server's copy) |
| `:image [FILE\|clear]` | a picture for the next message; `![alt](path)` in the text or a file dragged onto the terminal is attached on send (a plain path inside a sentence stays text). `--image FILE` on the command line. Vision models only. See `:h image` |
| `:forbid [TERM]` | terms no tool call may contain; a search, command or path with one is halted in every mode, under every harness. See `:h forbid` |
| `:allow [PATTERN]` | commands pre-approved for every mode: no asking, no reviewer. MAIC's helpers are on it by default. See `:h allow`; the full `permission` block (allow / ask / deny over `tool:pattern`) lives in settings, see `:h permission` |
| `:reg` | the yank register and the named registers `"a`..`"z` |
| `:undo [N]` | restore the file(s) the agent changed last; every write saves the previous content first, a delete keeps the file, a move is moved back |
| `:copy` / `:export [FILE]` | copy the last reply to the clipboard; write the transcript as markdown |
| `:stash` / `:pop` | park the input draft and bring it back (survives restarts). `:q` with an unsent draft stashes it for you |
| `:wq` | send, then quit when the reply is in (Ctrl-C while waiting stays) |
| `:rename TITLE` | title the session (`maic sessions` shows it); `title_model` in settings auto-titles after the first turn |
| `:ban add TEXT` / `:ban pattern REGEX` / `:ban token ID` / `:ban list` | phrases and regexes the model must not say (cut before they show, re-asked, then replaced) and token bans (`logit_bias` on OpenAI-compatible providers). `--ban`, `--ban-pattern`, `bans` in settings. See docs/bans.md |
| `:sampling [KEY VALUE\|xtc P T]` | temperature, top_k, min_p, seed, ... for this session; `xtc` (exclude top choices) on llama.cpp-style servers. See `:h sampling` |
| `:harness [smart\|dumb]` | the model reviewer on (default) or off. Auto under a dumb harness warns once and asks; see `:h harness` |
| `:budget [N\|off]` | tokens used; a per-session budget that stops the agent when reached |
| `:set timestamps on` | a time beside each message (also `timestamps` in settings) |
| `:lua [CODE]` / `:luafile PATH` / `:chat` | run Lua (vendored LuaJIT) in the workspace; an expression shows its value. `:lua` alone enters **Lua mode**: the input becomes a REPL (`lua❯`) until `:chat`. Globals persist; output goes to the conversation and to the model as context. Outside a session `maic lua` is a REPL, `maic lua FILE [args]` runs a file. See `:h lua` |
| `:compact [prune\|head\|all]` | free context. Default order: stub old tool results (dialog untouched), then, only if still needed, summarise the oldest turns into a handover note. `all` is a whole-conversation summary. Runs automatically at `compact_at` (75%) |
| `:clear` | start a new conversation (the session file keeps both) |
| `:trip REASON` / `:unlock` | trip the harness now; reset it without leaving the session (asks for your sudo password) |
| `:h [TOPIC]` | vim-style help. `:h` alone is an index; `:h w`, `:h u`, `:h f`, `:h .`, `:h gq`, `:h Ctrl-W`, `:h Alt+Enter`, `:h modes`, `:h harness`, `:h sessions`; a unique prefix is enough and an ambiguous one lists the candidates |
| `:q` | quit |

### After every turn

A footer line shows the model, how long the turn took and how many tools ran (`▣ qwen3.5:4b · 12.3s · 3 tool calls`, `· interrupted` when you stopped it). `run_shell` takes a `workdir` argument, so the approval prompt shows `pytest` in `services/api` rather than a `cd` chain; a workdir outside the workspace is asked about. Reading a file under a directory with its own `AGENTS.md` (or any name in `instruction_files`) attaches those instructions to the result once.

### Messages while the agent works

Typing and sending while the agent is busy queues the message; it reaches the model at its next step in the current turn (a notice says so, and the status strip counts it). `:w now` delivers it immediately: the model's current output is abandoned and it is asked again with your message included. If a tool is running, the message lands the moment the tool returns. Messages still queued when a turn ends start the next turn by themselves.

## Modes

| Mode | Reads in workspace | Read-only commands | Edits in workspace | Other commands | Outside workspace |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **manual** (default) | yes | ask | ask | ask | ask |
| **auto-read** | yes | yes (read-only sandbox) | ask | ask | reads yes, writes ask |
| **edit** | yes | ask | yes | ask | ask |
| **auto** | yes | yes | yes | yes (sandboxed) | reads yes, writes ask |
| **plan** | yes | yes (read-only sandbox) | no | no | reads ask |

"Read-only commands" are ones MAIC recognises as only looking (`ls`, `cat`, `grep`, `git log`, `find` without `-delete`/`-exec`, ...) with no redirection or substitution. They run with the workspace mounted read-only as well, so a wrong guess still cannot change anything.

Whatever the mode: secrets are never read, system paths are never written, startup files and MAIC's own harness are always asked about, dangerous commands trip the harness, and a request that did not come from this terminal is always asked. See [docs/harness.md](../docs/harness.md).

## Sessions

Every session is a JSONL file under `~/.local/state/maic/sessions/`, readable only by you: each message as sent to the model, every tool call with the harness's decision, and the displayable transcript. `maic -c` resumes the newest session started or last opened in the current directory; `maic -r` lists them; `maic -r ID` (a unique prefix is enough) resumes one; `maic -r PATH` resumes any transcript file by path, which is how a temporary `--no-record` transcript (never listed) comes back; when a session ends, `maic` prints the transcript's path and that exact command. The model is told it resumed and what the current mode and instructions are. A session that ended mid tool call resumes from the last complete step.

**Homes.** A project (a workspace with a `MAIC.md`, its own or inherited from a parent directory) keeps its transcripts under `sessions/projects/<encoded workspace path>/`, the Claude Code layout; everything else goes to `sessions/general/`. `sessions_home` in any settings layer overrides this (`general`, `project`, or a name for `sessions/<name>/`), so a project can opt back into `general`. `maic init` or `:init` turns a directory into a project. Every open (the first start and each resume) records the workspace, host and pid, so `maic sessions` shows where a transcript was started and, when different, where it was last opened and how many times. `maic sessions rehome ID [project|general|NAME]` moves a transcript to another home (default: its own project's directory); forks keep working because they find their parent by id. `maic sessions path ID` prints a path.

Where the continuation is written depends on `--append` / `--no-append`:

* **append** (the interactive default): the old file keeps growing; one conversation, one file.
* **no-append**: a new file whose first record points at the old one and says how many records were loaded; the old messages are not copied. Loading follows that pointer, so the new file resumes and lists normally (`maic sessions` shows "resumed from"). This is also how a session forks: two continuations of the same past never touch each other or the original.
* **`--fork-at N`** (with `-c` or `-r`, interactive or `-p`): the same kind of new file, but pointing at the old session's first N records only, so the conversation continues from an earlier point; nothing after record N is loaded and the old file is never changed. `maic sessions path ID` finds the file to count records in; a `user` record is a natural cut. It cannot be combined with `--append`.

**Import and redact.** `maic sessions import FILE` turns a claude.ai export (JSON; `--conversation UUID` picks one out of a full export) or a Claude Code transcript (JSONL from `~/.claude/projects/`) into a session here and prints its id; the format is detected from the content, `--as claude-ai|claude-code` overrides it, `--home general|project|NAME` says where it goes. `maic sessions redact ID` writes `./<id>.redacted.jsonl` with credential material replaced by `[REDACTED:kind]` and reports the count per kind; `-o FILE` and `--in-place` choose the destination, and an existing file is never overwritten. The record types, the import rules and every redaction kind: [docs/sessions.md](../docs/sessions.md).

**Context for a turn.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several files, and `-C -` reads stdin (then the prompt itself has to be an argument). It combines with `-c`/`-r`: `maic -p "compare these" -c -C new-draft.md` puts the file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maic`.

**Interactive from a prompt.** `maic -p "…" --interactive` (`-i`) opens the normal session with the prompt already sent, context files attached and `-c`/`-r` honoured. Because it is an interactive session, interactive transcript rules apply no matter where the flags appear: a transcript is always kept and resumes append unless `--no-append`. `--record` is redundant there and `--json` is ignored with a note.

**Unrecorded sessions** still get a transcript, but in the runtime directory: `$XDG_RUNTIME_DIR/maic/sessions/` (a tmpfs the system clears at logout), or `/tmp/maic-<uid>/sessions/` without one; on Windows this will be `%TEMP%\maic\sessions`. Nothing there is listed by `maic sessions` or found by `-c`. `maic --no-record` does this for one interactive session, `"record": false` in settings makes it the default, and `--record` turns it back on. `maic -p` is unrecorded unless `--record` (or `--transcript`); with `-c` / `-r` that keeps a pointer-style fork, and `--append` writes into the old file instead. So `cat big-context.md | maic -p - -r ID` runs a large context against an old conversation and leaves nothing behind, `... --record` keeps that as a fork, and `maic -c -p "..." --append` extends the old one in place.

## Tools the model gets

`read_file` (`grep` returns only the matching lines of a big file), `list_dir` (`depth` for a tree), `glob` (files by name pattern), `search_files` (grep -E syntax), `write_file`, `edit_file`, `multi_edit` (several replacements in one file, all or none), `apply_patch` (a unified diff over one or more files, all or none), `move_file`, `copy_file`, `delete_file`, `make_dir`, `run_shell`, `question` (asks you something, with options to pick by number), `todo` (the model's plan; `:todo` shows it, the status strip counts it) and `delegate` (a subagent, see below). Every call goes through the harness in `core/`: the file tools are judged as writes to every path they touch, so a move or copy out of the workspace asks, a delete under `~/.ssh` trips, and a patch is refused whole if one of its files is. The model is briefed at the start of the conversation about MAIC, the tools, the modes, the harness and what a denial means (`core/src/agent.cpp`, `system_prompt`).

**Subagents.** `delegate {profile, task, context}` runs a second agent in the same workspace under a profile that only narrows what it may do: `scout` (auto-read, read-only tools and commands, 50k tokens) for a long read or search the model does not want in its own context, `reviewer` (plan mode, reads inside the workspace only) for a review of its own change, `builder` (edit mode) for a self-contained piece of work, or a profile from `profiles` in settings. The child's mode is capped by the session's, so in a manual session its commands are asked about like anything else; approvals come to you through the parent with the profile named, its tool calls show indented under the delegate call (`↳ scout: ...`), its final answer is the tool result with its steps and tokens, and it has no `delegate`, `question` or `todo` of its own. Each child has its own transcript (kind `sub`) that `maic sessions` lists under the parent. See `:h delegate`, `:h profile`, [docs/tools.md](../docs/tools.md).

Your own tools are Lua files in `.maic/tools/` or `~/.config/maic/tools/`, each call in its own sandboxed LuaJIT state whose `maic.read` / `write` / `list` / `search` / `shell` go through the same authorisation step as the built-ins. `:tools` and `maic tools` list them. Format and an example: [docs/tools.md](../docs/tools.md).
