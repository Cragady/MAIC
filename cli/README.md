# cli

The `maic` command. With no arguments it starts the agent in the current directory, which becomes the workspace. With a command it manages services and the harness (`maic status`, `maic up ollama`, `maic trip`, ...; `maic help` lists them).

```sh
cd ~/some/project
maic up ollama        # once per boot
maic                  # MAIC_MODEL=qwen3.5:9b maic  for the bigger model
```

## Keys

Starts in insert mode. The cursor is a bar in insert mode and a block in normal mode.

| Mode | Keys |
| :--- | :--- |
| insert | type; **Enter** sends; **`\` then Enter** starts a new line; **Esc** to normal; Ctrl-W / Ctrl-U delete word / line; ↑ ↓ or Ctrl-P / Ctrl-N prompt history |
| normal: conversation | **j / k** scroll a line; **Ctrl-D / Ctrl-U** half page; **Ctrl-F / Ctrl-B** page; **gg / G** top / bottom (G follows new output); counts work (`5k`) |
| normal: input | `i a I A` insert; `h l w b e 0 ^ $` move; `x X D C S` edit; `dd dw de db d$ cc cw` operators; `u` undo; **Enter** sends |
| anywhere | **Shift-Tab** cycles the agent mode; **Ctrl-C** interrupts the agent, else clears the input, else (twice) quits |
| approval prompt | **y** yes · **n** no · **a** always allow this file / program for the session · **t** trip the harness |

`:` commands in normal mode (or `/command` typed as a message):

| Command | Does |
| :--- | :--- |
| `:mode manual\|edit\|auto\|plan` | set the agent mode |
| `:model NAME` | switch model (when idle), e.g. `qwen3.5:9b` |
| `:think on\|off` | let the model reason before answering (slower) |
| `:clear` | start a new conversation |
| `:status` | harness and services |
| `:up NAME` / `:down NAME` | start / stop a service |
| `:trip REASON` | trip the harness now |
| `:unlock` | reset the harness without leaving the session (asks for your sudo password) |
| `:help`, `:q` | |

## Modes

| Mode | Reads in workspace | Edits in workspace | Commands (sandboxed) | Outside workspace |
| :--- | :--- | :--- | :--- | :--- |
| **manual** (default) | yes | ask | ask | ask |
| **edit** | yes | yes | ask | ask |
| **auto** | yes | yes | yes | reads yes, writes ask |
| **plan** | yes | no | no | reads ask |

Whatever the mode, secrets are never read, system paths are never written, startup files and MAIC's own harness are always asked about, and dangerous commands trip the harness. See [docs/harness.md](../docs/harness.md).

## Tools the model gets

`read_file`, `list_dir`, `search_files`, `write_file`, `edit_file`, `run_shell`. Every call goes through the harness in `core/`.
