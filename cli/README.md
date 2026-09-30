# cli

The `maic` command. With no arguments it starts the agent in the current directory, which becomes the workspace. `maic help` lists the rest.

```sh
cd ~/some/project
maic up ollama                     # once per boot
maic                               # the agent
maic --model anthropic/claude-opus-5-5 --mode auto-read
maic -c                            # continue the last session started in this directory
maic -r                            # pick an earlier session from a list (maic -r ID for one you know)
maic -p "explain main.cpp"         # one turn, no UI, no transcript; --record keeps one; -c / -r load an old one; --json for events
maic sessions                      # every session, with a preview
maic artifacts                     # where MAIC and its services keep transcripts, logs, outputs
maic settings init                 # a documented settings file (docs/settings.md)
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
| insert | type; **Enter** new line; **Esc** to normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register; ↑ ↓ or Ctrl-P / Ctrl-N prompt history |
| normal (input) | `i a I A o O` insert; `h j k l w b e 0 ^ $` move (Enter = down a line); `x X D C S` edit; `d c y` + motion, `dd cc yy`; `v V` select; `p P` paste; `u` undo, **Ctrl-R** redo (multi-level; an insert session is one step); counts (`3w`); `:e` or **Ctrl-X Ctrl-E** opens the input in `$VISUAL` / `$EDITOR` / nvim as markdown and loads it back when you quit |
| normal (input empty) | `j k` Ctrl-D/U Ctrl-F/B `G` scroll the conversation without leaving the input; `v` / `V` jump into the conversation window selecting |
| conversation window | **Ctrl-W k** enters, **Ctrl-W j** (or Esc, `i`, Enter) returns; `j k h l w b e 0 $ gg G` Ctrl-D/U/F/B move; `v` / `V` select; `y` yanks (to the register **and** the system clipboard); `yy` a line; `/pattern` then `n` / `N` search (smart case); `o` swaps selection ends |
| anywhere | **Shift-Tab** cycles the mode; **Ctrl-C** interrupts the agent, else stops a `!command`, else clears the input, else (twice) quits; scroll wheel scrolls the conversation (in insert mode: prompt history) |
| approval prompt | **y** yes · **n** no · **a** always allow this file / program for the session · **t** trip the harness |

Ctrl-W in insert mode deletes a word, as in vim; the window chord works from insert mode only when the input is empty, otherwise press Esc first.

The system clipboard is reached through `wl-copy` or `xclip` when present, and always through the terminal (OSC 52), which also works over ssh. Your terminal's own copy (Ctrl-Shift-C) keeps working on text you select with the mouse; with the scroll wheel enabled, that selection needs Shift+drag.

## Commands

`:` in normal mode, or from the conversation window. Every command also works typed as a message beginning with `/`. As you type after `:`, a palette lists the matching commands with a one-line description (arguments too, for `:mode`, `:set`, `:h`, `:model`, `:up`); **Tab** completes to the highlighted one and cycles on repeat, Shift-Tab goes back. A unique prefix runs the command (`:inst` is `:instructions`), as in vim.

| Command | Does |
| :--- | :--- |
| `:w` | send the input (same as Alt+Enter). `:w now` sends immediately even while the agent is working (see below) |
| `:e` | edit the input in nvim (`$VISUAL`, then `$EDITOR`, then `nvim`); a non-zero exit leaves the input unchanged |
| `:mode manual\|auto-read\|edit\|auto\|plan` | set the agent mode |
| `:model NAME` | switch model (when idle): `qwen3.5:9b`, `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, ... `:model` alone lists providers |
| `:models` | models the Ollama server has |
| `:think on\|off` | let the model reason before answering |
| `:set markdown\|mouse on\|off` | rendering and scroll-wheel toggles |
| `:!cmd` or `!cmd` | run a command in **your** shell, unsandboxed, in the workspace; output shows in the conversation and is passed to the model as context (Ctrl-C stops it) |
| `:status` | harness, every service with where it runs and what to do about it, model and whether it is remote, session file, queue |
| `:up NAME` / `:down NAME` | start / stop a service |
| `:instructions` | the MAIC.md / AGENTS.md files in effect |
| `:session` / `:artifacts` | where this transcript is; where everything is kept, with sizes |
| `:reg` | the yank register |
| `:clear` | start a new conversation (the session file keeps both) |
| `:trip REASON` / `:unlock` | trip the harness now; reset it without leaving the session (asks for your sudo password) |
| `:h [TOPIC]` | vim-style help. `:h` alone is an index; `:h w`, `:h u`, `:h Ctrl-W`, `:h Alt+Enter`, `:h modes`, `:h harness`, `:h sessions`; a unique prefix is enough and an ambiguous one lists the candidates |
| `:q` | quit |

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

Every session is a JSONL file in `~/.local/state/maic/sessions/`, readable only by you: each message as sent to the model, every tool call with the harness's decision, and the displayable transcript. `maic -c` resumes the newest session from the current directory; `maic -r` lists them; `maic -r ID` (a unique prefix is enough) resumes one. The model is told it resumed and what the current mode and instructions are. A session that ended mid tool call resumes from the last complete step.

Where the continuation is written depends on `--append` / `--no-append`:

* **append** (the interactive default): the old file keeps growing; one conversation, one file.
* **no-append**: a new file whose first record points at the old one and says how many records were loaded; the old messages are not copied. Loading follows that pointer, so the new file resumes and lists normally (`maic sessions` shows "resumed from"). This is also how a session forks: two continuations of the same past never touch each other or the original.

`maic -p` writes no transcript at all unless asked: `--record` (or `--transcript`) keeps one, which with `-c` / `-r` is the pointer-style new file; `--append` writes into the old file and implies recording. So `cat big-context.md | maic -p - -r ID` runs a large context against an old conversation and leaves nothing behind, `... --record` keeps that as a fork, and `maic -c -p "..." --append` extends the old one in place.

## Tools the model gets

`read_file`, `list_dir`, `search_files` (grep -E syntax), `write_file`, `edit_file`, `run_shell`. Every call goes through the harness in `core/`. The model is briefed at the start of the conversation about MAIC, the tools, the modes, the harness and what a denial means (`core/src/agent.cpp`, `system_prompt`).
