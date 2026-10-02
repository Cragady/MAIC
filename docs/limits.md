# Known limits

What MAIC does not do, or does only partly, in one place. Each entry has a status:

* **accepted**: a deliberate choice; it stays unless the reason changes.
* **expected**: how the thing underneath works; nothing in MAIC can change it.
* **open**: worth fixing; what would fix it is named.
* **addressed**: fixed; kept here for the record, with where.

The feature's own section is linked from each entry. Add an entry when a limit is found, and change its status rather than deleting it.

## Trust and settings Lua

Feature: [harness.md, Directory trust and restricted settings Lua](harness.md#directory-trust-and-restricted-settings-lua-built), [settings.md, Project layers, trust and the chain](settings.md#project-layers-trust-and-the-chain).

* **Custom instruction-file names are not hashed.** *Addressed* (2026-10-01): the names come from `instructions.files` in the global settings, which MAIC reads before it asks about trust, and every one of them, with its `.local.md` variant, is part of the directory's hash ([instructions.md, Trust](instructions.md#trust)).
* **Nested `AGENTS.md` files are not hashed.** *Addressed* (2026-10-01): a project directory's hash covers the instruction files in its subdirectories (and the files they import inside it), and on-demand loading attaches a nested file only when a trusted directory on the chain hashes it, so a new or edited one asks again as the tier says ([instructions.md, Trust](instructions.md#trust)).
* **Nested instruction files past the first 4096 directories are not read.** *Accepted.* The search below a project directory is breadth first in name order, skips hidden directories, `node_modules` and links, and stops after 4096 directories, so it costs little at every start; a file it never reaches is neither hashed nor attached. Would address it: a larger bound, at the cost of start-up time in a very large tree. ([instructions.md, Trust](instructions.md#trust))
* **An import outside the importing directory is not in its hash.** *Open.* An `@path` import that lands in your config directory, or in another trusted directory, is read but hashed by neither, so editing it does not ask again; imports inside the importing directory are hashed with it. Why: the target belongs to a directory with its own trust (or to you). Would address it: hashing every followed import with the directory whose file imports it. ([instructions.md, Imports](instructions.md#imports))
* **The standard tier counts any uncommitted local edit as yours.** *Accepted.* Under `standard`, an uncommitted change in a git working tree passes, whoever made it on this machine: a build script or a postinstall hook that rewrites `.maic/settings.lua` looks like you. Why: git cannot tell who edited a working tree, and asking about every edit of your own is what `strict` is for. Would address it: `maic trust PATH --level strict` for such a repository, or sandboxed trust so the file's Lua cannot reach the system either way.
* **Server sessions take their settings from the server's startup.** *Open.* A maic-server session gets the settings `maic server start` loaded where it was started; a remote session's workspace contributes its trusted instructions and tools but not its project settings layers. Why: one settings object is shared by every session the server hosts. Would address it: loading settings per session from its workspace, through the same trust check.
* **Restricted Lua had no memory cap.** *Addressed* (2026-10-01): the sandbox level runs a data file in a child process under RLIMIT_AS, RLIMIT_CPU and RLIMIT_NOFILE, and the restricted level refuses early in process (heap checks in the count hook, `string.rep` and `table.concat` checked before allocating); `lua_memory_mb` sets the cap ([harness.md](harness.md#directory-trust-and-restricted-settings-lua-built)).
* **A fully trusted directory runs its settings Lua as you.** *Accepted.* That is what trusting fully means, the same as your own dotfiles; the exploit that started this work runs when trusted fully. Would address it for a given repository: trust it sandboxed (`s` at the prompt, `maic trust PATH --lua sandbox`).
* **Lua tools have no memory cap and keep a few base functions.** *Open.* A user's Lua tool runs in its own state per call with base, string, table, math and bit, no `load`, `require`, `dofile` or `loadfile`, the JIT off and a count hook for Ctrl-C and its 60 s limit, every `maic.*` call authorised by the harness; but nothing caps its heap, and `collectgarbage`, `getfenv`, `setfenv`, `newproxy` and `string.dump` are still there. Why: tools predate the restricted environment. Would address it: building tool states from the restricted environment and its count-hook memory check, keeping the tool's own `maic` table. ([tools.md](tools.md))
* **The restricted level's memory check is not a hard cap.**
 *Expected.* In MAIC's own process the count hook sees the heap only every 1000 instructions, so one allocation between two checks can pass the limit; only the sandbox's child has a kernel limit behind it. Would address it: choose `sandbox`, the default for partly trusted directories.

## The command sandbox

Feature: [harness.md, Layers, rule 6](harness.md#layers).

* **Host sockets reached the sandbox.** *Addressed* (2026-10-01, v0.3.1): the read-only bind of `/` left `$XDG_RUNTIME_DIR` and `/run` in sight and the environment named the session D-Bus, so a sandboxed command could ask systemd to run something outside it. All of `/run`, the runtime directory and the agent socket directories are empty now, and the environment is an allow-list ([releases/v0.3.1.md](releases/v0.3.1.md)).
* **A socket in some other directory is still reachable.** *Open.* The sandbox hides `/tmp`, `/run`, the runtime directory and the directories of the sockets `SSH_AUTH_SOCK`, `GPG_AGENT_INFO` and `NVIM` name; a path socket anywhere else that you can read stays reachable through the read-only bind, for example one in your home directory or under `/var/lib`, or an nvim started with `--listen PATH` that MAIC was not given as `$NVIM`. Why: a read-only bind does not stop `connect()`, and MAIC cannot know every socket on the machine. Would address it: a seccomp filter that refuses `connect()` on `AF_UNIX` sockets (bubblewrap's `--seccomp`), with a way to allow one a tool needs.
* **Programs kept under `/run` are missing inside.** *Open.* All of `/run` is replaced, so a system that keeps programs there (NixOS's `/run/current-system`, `/run/wrappers`) finds them gone in the sandbox; only `/run/media` is bound back. Would address it: binding those paths back read-only when they exist.

## Sessions and the workspace

* **`:cd` does not reload Lua or script tools.** *Accepted.* `:cd` moves the harness root, shell commands, the project settings layers and the instruction files; the user's Lua and script tools stay those loaded at start. A `:reload` is planned. ([cli/README.md, Commands](../cli/README.md#commands))
* **`maic sessions rehome` leaves subagent sessions behind unless asked.** *Accepted* (by design). Only the named sessions move; `--subagent` moves their `sub` sessions too, `--subagent-only` only those. A subagent's transcript still finds its parent by id. ([sessions.md, Homes](sessions.md#homes))
* **`:init` moving a session takes only the `sub` sessions in its own home.** *Accepted* (by design). When `:init` (or a relocate) moves the running session into the project's home, its subagent sessions that a default `maic sessions rehome` left in another home stay there; `maic sessions` and `maic sessions state` still find them by the parent's id. `maic sessions rehome ID --subagent` moves them when wanted. ([sessions.md, Homes](sessions.md#homes))
* **The `:init` ask path is not in the TUI suite.**
 *Open.* The question `:init` asks when a session worked outside the project ("move it into the project's home anyway?") is covered by the core tests (`init_move_check`) but not driven through the TUI. Would address it: a `tests/test_tui.py` case that reads and writes outside a project, runs `:init` and answers. ([sessions.md, Homes](sessions.md#homes))

## Local models and services

* **Side threads and a running turn take turns on a single-slot local server.** *Expected.* A llama-server with one slot serves one request at a time, so a side thread asked while a main turn runs waits for it. Would address it: a second slot (`--parallel`) where the card has room, or the side server. ([roadmap.md](roadmap.md), [llamacpp.md](llamacpp.md))
* **whisper-server's ready pattern is read from stderr.** *Expected.* whisper-server says it is ready only in its log, so `maic up whisper` waits for `compute buffer (decode)` in its stderr; a whisper.cpp release that changes that line makes the start wait out its timeout. Would address it: a health endpoint in whisper-server, then polling it like the llama servers. ([diction.md, Setup](diction.md#setup))

## nvim

* **llama.vim maps normal-mode `<Tab>` and `<Esc>` globally while enabled.** *Expected.* Not only while an instruction runs; a normal-mode `<Esc>` of your own and llama.vim's replace each other in load order. MAIC's spec turns them off. ([models.md, Code completion](models.md#code-completion))
* **The nvim input highlighter sees only nvim's bundled parsers.** *Accepted.* `highlight = "nvim"` starts `nvim --embed --headless -u NONE`, so treesitter parsers your config installs are not there; fenced code in other languages is left plain. Would address it: starting it with your config, at the cost of your plugins' start-up time on the first keystroke. ([settings.md](settings.md))
* **`bare = true` in a settings file connects to the host nvim, then drops it.** *Accepted.* The host connection comes before the settings are read (so the settings Lua can use `maic.nvim`), so `bare = true` there takes effect only after; `--bare` and `MAIC_BARE=1` never connect. ([nvim.md, Bare](nvim.md#bare))
* **The lazy-lock hash file writes the path relative to `$HOME`.** *Expected.* `sha256sum` cannot expand `~`, so the line is `<hex>  .config/nvim/lazy-lock.json` and is checked from your home directory (`cd ~ && sha256sum -c ...`). ([lazy-lock.md, Files](lazy-lock.md#files))
* **`maic nvim setup llama-vim` refuses when any config file merely mentions llama.vim.** *Addressed* (2026-10-01): only lazy.nvim's resolved spec (its plugins and disabled lists, queried from the headless nvim) refuses, when it has llama.vim from somewhere other than MAIC's own file; a file that only mentions it is a warning (`init.lua mentions llama.vim; it is not loaded as a plugin, continuing`). ([nvim.md, maic nvim setup llama-vim](nvim.md#maic-nvim-setup-llama-vim))
