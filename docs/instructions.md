# Instruction files

Standing instructions the model sees on every turn, like Claude Code's CLAUDE.md and opencode's AGENTS.md. MAID reads them from a few fixed places, re-reads them at the start of each turn (an edit applies to the next message), caps each at 32 KB, and tells the model that the later ones take precedence where two conflict. `:instructions` lists what is in effect; `:instructions off`, `--no-instructions` or `load_instructions = false` loads none. Code: `core/src/instructions.cpp`, the trust hash in `core/src/trust.cpp`.

## Where MAID looks, in reading order

1. **The system directory**, `/etc/maid/`: a slot for a machine-wide file (`MAID.md` and the other class names). Empty by default; MAID never writes there.
2. **Your config directory**, `~/.config/maid/` (`$XDG_CONFIG_HOME/maid/`): the same names. Always trusted.
3. **Extra directories**, once MAID has them (an `--add-dir` like Claude Code's is not built yet): their instruction files are read only with `instructions.extra_dirs = true`, only from trusted ones, and before the project's.
4. **The project chain**: from the project root (the nearest directory at or above the workspace holding a project marker), or from just under `$HOME` when there is no project, down to the workspace, outermost first. Outside `$HOME` the chain is the workspace alone; `$HOME` and `/` are never on it. Only trusted directories contribute ([settings.md, Project layers, trust and the chain](settings.md#project-layers-trust-and-the-chain)).
5. **Below the workspace, on demand**: when the agent reads a file in a subdirectory of the workspace, the instruction files from that file's directory up to (not including) the workspace are attached to the conversation once, outermost first, as a marked system note naming each file. Only files a trusted directory's hash covers are attached (see [Trust](#trust)).

Inside one directory the classes go lowest priority first (`CLAUDE.md`, `AGENTS.md`, `MAID.md` by default), then that directory's local files (`CLAUDE.local.md`, `AGENTS.local.md`, `MAID.local.md`) in the same order. Across directories the more general comes first and the closer one later. A short header ahead of the files tells the model that later instructions take precedence where they conflict.

An example. The workspace is `~/dev2/app/src` in a git repository `~/dev2/app`, with `instructions` left at its defaults:

```
/etc/maid/MAID.md                  (if an administrator put one there)
~/.config/maid/AGENTS.md
~/.config/maid/MAID.md
~/dev2/app/docs/style.md           (imported by the next file, so read right before it)
~/dev2/app/CLAUDE.md
~/dev2/app/MAID.md
~/dev2/app/MAID.local.md
~/dev2/app/src/AGENTS.md
```

Reading `~/dev2/app/src/net/http.cpp` later attaches `~/dev2/app/src/net/AGENTS.md`, once per conversation (again after `:clear`, which forgets it).

## Options

All of them live in the `instructions` table of your **global** settings file (`~/.config/maid/settings.lua`). A project's `.maid/settings.*` setting any `instructions` key is ignored, with a warning naming the file.

```lua
instructions = {
  files = { "CLAUDE.md", "AGENTS.md", "MAID.md" },  -- the classes, lowest priority first
  read = "all",            -- or "highest"
  local_files = true,      -- MAID.local.md and the like
  imports = { depth = 4 }, -- @path imports; 0 turns them off
  extra_dirs = false,      -- extra directories' instructions, when that feature lands
  project_markers = { ".git", ".maid", "MAID.md" },
  bound = "project",       -- or "home"
},
```

| Key | Default | What |
| :--- | :--- | :--- |
| `files` | `{ "CLAUDE.md", "AGENTS.md", "MAID.md" }` | What counts as an instruction file: plain file names, lowest priority first. Add a name (`"RULES.md"`), drop one, or reorder them. Each name's local variant is the stem plus `.local` (`RULES.md` -> `RULES.local.md`). |
| `read` | `"all"` | `"all"`: every class present in a directory, in priority order. `"highest"`: only the top class present in each directory (and likewise the top local file), so a repository carrying both `CLAUDE.md` and `MAID.md` gives the model its `MAID.md` alone. |
| `local_files` | `true` | Reads `MAID.local.md` and the other local variants, each right after its directory's shared files. They are meant to stay out of version control (personal notes on a shared project). |
| `imports.depth` | `4` | How many hops `@path` imports follow. `0` reads no imports. |
| `extra_dirs` | `false` | Whether extra directories contribute their instruction files. There is no extra-directories feature yet; this is the rule it will follow when it lands. |
| `project_markers` | `{ ".git", ".maid", "MAID.md" }` | What makes a directory the project root, where the chain stops. |
| `bound` | `"project"` | `"project"` stops the chain at the project root; `"home"` reads up to just under `$HOME` everywhere. |

`load_instructions = false` (any layer), `--no-instructions` and `:instructions off` switch all of it off for a session; `system_prompt` and `rules` are separate and still apply.

The old `instruction_files` key is gone: `instructions.files` in the global file replaces it, and a file that still sets it gets a warning saying so.

## Imports

A line in an instruction file may import another file with `@path`, as in Claude Code:

```markdown
Follow the house style in @docs/style.md and the shared notes in @~/.config/maid/notes.md.
```

* The path is relative to the importing file; an absolute path and `~/` work too. An `@` counts only at the start of a line or after a space or `(`, and only when what follows looks like a path (it has a `/` or a `.`, or starts with `~`), so `me@example.com` and `@someone` are left alone. Trailing `.`, `:`, `!` and `?` are not part of the path.
* Never inside a code span (`` `@x.md` ``), a fenced code block, a block quote (`> @x.md`) or double quotes (`"@x.md"`).
* An imported file is read right before the file that imports it, so the importing file's own words come later and take precedence. Imports nest up to `imports.depth` hops (4); the fifth is not followed. Each file is read once, so a cycle ends where it starts.
* **An import may not reach outside the trusted chain and your config directory**, unless the target is in a trusted directory. Allowed: anything under the system directory, under `~/.config/maid/`, under a trusted directory of the chain (or a trusted extra directory), or in a directory you have trusted with `maid trust`. Your own files (the system directory and `~/.config/maid/`) may reach further, once you approve each import: see [Imports from your own files](#imports-from-your-own-files). A refused import is noted in the importing file as the model sees it (`[MAID: @/path was not imported: it is outside the trusted directories and your config directory]`), as is a missing one; neither stops the rest from loading.

## Imports from your own files

An `@path` in one of your own instruction files (`/etc/maid/` and `~/.config/maid/`) may import a file from outside the trusted directories, such as `~/notes/style.md`, but only after you verify it. The first time MAID meets such an import it does not read the file; it asks you once, showing the importing file, the target, the target's size and what approving means:

1. it becomes standing instructions for every agent in every project;
2. its contents are sent to whatever model provider a session uses, cloud included;
3. every session pays its tokens;
4. if an agent or another tool can write that file, it can change future instructions.

In the TUI the question is the confirm modal (**y** approves, **n** leaves it unread this session; `:trust imports --approve` asks again). Headless and off a terminal nothing is asked: the import is not read and a notice names `maid trust imports --approve`, which asks at a terminal.

An approval is kept for good, for every later session and client: the (importing file, target) pair goes into `<state>/trust-imports.json` (0600, beside `trust.json`) with the target's SHA-256. When the target changes, the global `trust_strictness` decides as it does for trust: `strict` asks again, `standard` lets your own uncommitted edit or your own commit pass (anything else, or a file outside a git working tree, asks again), `relaxed` lets it pass. `maid trust imports` lists the approvals, `maid trust imports --remove PATH` forgets every pair whose importing file or target is PATH.

The agent can never approve an import: a write to the list, and any `maid trust` command, are trust actions it is refused, and approving takes the local user. Every approved target is protected too: an agent's write to it (or a command that is not read-only and names it) is asked under the smart harness, even in auto mode, and refused under the dumb one. A project's files keep the ordinary rule above: they cannot import from outside, approved or not. An approved file's own imports follow the ordinary rule as well.

## Trust

A project directory's instructions reach the model only once you trust it, and the trust record keeps a SHA-256 over every file it contributes, so a change asks again as its tier says ([harness.md, Directory trust](harness.md#directory-trust-and-restricted-settings-lua-built)). The hash covers:

* every name in `instructions.files` present in the directory, and with `local_files` their local variants, whatever the names are;
* the same names in its subdirectories, the nested files on-demand loading attaches: searched breadth first in name order, skipping hidden directories, `node_modules` and links to directories, at most 4096 directories in all;
* the files those import that lie inside the directory, as far as `imports.depth` follows them.

The class names come from your global settings, which MAID reads before it asks about trust, so a custom name is hashed like `MAID.md`. Changing `instructions.files` changes what a directory contributes, so a directory holding a file whose name was added or dropped is asked about again. Nested files make the workspace a project directory of its own only when no project directory above it already hashes them; a project root covers its whole tree. On-demand loading attaches a nested file only when a trusted directory on the chain hashes it.

## How it compares

| | Claude Code | opencode | MAID (defaults) |
| :--- | :--- | :--- | :--- |
| How far up | up to the filesystem root | from the working directory up to the git worktree root (findUp) | up to the project root, or just under `$HOME` without one; `bound = "home"` always to `$HOME` |
| Order | root first, so the closer file is read later | the matches found on the way up | general first, the workspace last; within a directory lowest class first, local files after |
| Below the working directory | on demand, when a file there is read | on a read, the instruction files from the read file's directory up to the root, once per message | on demand, from the read file's directory up to the workspace, once per session, trusted directories only |
| Names and precedence | `CLAUDE.md`; `AGENTS.md` only when there is no `CLAUDE.md`, unless a setting reads both | every match of the first filename kind found; `AGENTS.md` beats `CLAUDE.md`, no stacking | `CLAUDE.md`, `AGENTS.md`, `MAID.md` all read, highest last; `read = "highest"` for one per directory; names are configurable |
| Layers | the managed policy file, then `~/.claude/CLAUDE.md`, then the project, then `CLAUDE.local.md` | one global file, `~/.config/opencode/AGENTS.md`, else `~/.claude/CLAUDE.md`, then the project files | `/etc/maid/`, then `~/.config/maid/`, then the project chain, each directory's `*.local.md` right after its shared files |
| Extra directories | `--add-dir` does not load their instructions unless `CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD=1` | not compared here | not loaded unless `extra_dirs = true` (no extra-directories feature yet) |
| Imports | `@path`, four hops, relative to the importing file | not compared here | `@path`, four hops, relative to the importing file; never outside the trusted chain and your config directory unless the target is trusted |
| Source | [code.claude.com/docs/en/memory.md](https://code.claude.com/docs/en/memory.md) | `packages/opencode/src/session/instruction.ts` at `2fa3363c92` | this page; `core/src/instructions.cpp` |

## Why these defaults

* **Stop at the project root.** Every directory above it is one more place a file can come from: one more trust prompt, and one more chance for something you did not write (a checkout in a shared parent, a downloaded archive) to instruct the model. Claude Code reads to the filesystem root; MAID reads what belongs to the project, and `bound = "home"` is there for a layout that needs more.
* **Closer and higher read last.** Models weigh what comes later, and the header says so outright, so the most specific file (the workspace's, the highest class, the local one) wins a conflict, the way a nearer `.gitignore` refines a farther one. Reading every class rather than the first found means a repository's `CLAUDE.md` and `AGENTS.md` both arrive, with your `MAID.md` last; `read = "highest"` keeps one per directory when they repeat each other.
* **Extra directories are not loaded.** A directory added for reading or editing is not one whose rules you chose to follow; as with Claude Code's `--add-dir`, its instructions come only when you say so (`extra_dirs = true`), and then only from trusted directories.
* **Global-only keys.** Which files count, how they are read and what may be imported decide what text reaches the model. If a repository could set them it could rename its way past the trust hash, turn on imports of files outside it, or pull in instructions from somewhere it should not reach. So only your own settings file sets them, and a project's attempt is a warning naming the file.
