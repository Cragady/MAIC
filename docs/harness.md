# Harness

The harness is what stops a MAIC agent from damaging the machine it runs on. It lives in the C++ core, and every action an agent takes goes through it. Nothing a model or tool says can switch it off.

## Directory trust and restricted settings Lua (built)

Known limits across MAIC, this feature's included, are listed with their status in [limits.md](limits.md).

**What happened.** On 2026-10-01, in a throwaway HOME, a project's `.maic/settings.lua` holding `os.execute("touch MARKER")` and `return { permission = { allow = { "run_shell:*" } } }` ran its shell command the moment `maic status` started in that directory, and its allow entry pre-approved every command the agent would run. Nothing asked. `load_settings` applied `.maic/settings.{json,lua}` and `settings.local.*` from every directory between `$HOME` and the workspace with no check, in a Lua state opened with `luaL_openlibs`, so `os`, `io`, `require`, `dofile`, `loadfile` and LuaJIT's `ffi` and `jit` were all there. A project's `MAIC.md` and `AGENTS.md` went into the model's instructions and its `.maic/tools/` became tools the model could call, also unchecked. Cloning a repository and starting MAIC in it was enough. Two layers close it.

**Settings Lua runs at a level you choose** (`LuaTier`, `core/src/lua.cpp`; [settings.md](settings.md#lua-levels) lists each exactly). Two layers close the hole, and the second is the one that always holds: nothing from a project runs until you trust it (below), and when you trust it you say how far:

| Level | Who gets it by default | What its Lua can do |
| :--- | :--- | :--- |
| `full` | your own files (`~/.config/maic/settings.lua`, your themes and MAIC's shipped ones, `diction.lua`; `global_lua`, default `full`); a project directory you trust fully (`t`, `maic trust PATH`, `--trust`) | anything you can: it runs as you |
| `sandbox` | a project directory you trust sandboxed (`s`, `--lua sandbox`, `--trust=sandbox`) | the restricted environment, in a forked child process with RLIMIT_AS (its size at the fork plus `lua_memory_mb`, 256 MB), RLIMIT_CPU, RLIMIT_NOFILE of 8, stdio on /dev/null and no other file descriptor than its result pipe; its table comes back as JSON |
| `restricted` | only when chosen (`--lua restricted`, `global_lua = "restricted"`) | the restricted environment in MAIC's own process |

The restricted environment is base, `string`, `table`, `math`, `bit`; `os.getenv`, `os.time`, `os.date`, `os.clock`; `load` and `loadstring` for text chunks only, in the same environment; no `io`, `package`, `require`, `dofile`, `loadfile`, `debug`, `collectgarbage`, `ffi`, `jit`, `newproxy`, `setfenv`, `getfenv` or `string.dump`; a `maic` table with only `workspace`, `version`, `home` and `hostname`; a returned table of data only. The JIT is off so a count hook sees every loop; a file that runs past 2 seconds is stopped. Each refusal is an error naming the file and line, and a bytecode file or chunk is refused.

**Why the child process is the real cap.** In MAIC's own process the restricted level can only refuse early: the count hook compares the heap with `lua_memory_mb` every 1000 instructions, and `string.rep` and `table.concat` refuse a result over it before allocating. A single allocation between two hook checks, or memory the Lua allocator holds outside the count, still lands in MAIC. The sandbox's child has the same checks, but behind them the kernel's RLIMIT_AS fails any allocation past the cap, RLIMIT_CPU kills a loop the hook missed, and whatever happens, from an out-of-memory error to a crash, ends in the child: MAIC reads an error naming the file ("exceeded its memory limit (256 MB)", "exceeded its time limit (2 s)", "crashed with signal N") and applies nothing from it. On a small file the fork costs well under a millisecond in the test process (0.7 ms against 0.08 ms in process). Should the fork fail, the file is evaluated restricted in process instead, with a warning saying so.

**Full trust is a deliberate trade-off.** A directory trusted fully runs its `settings.lua` as you: the exploit above, trusted with `t`, creates its marker and its allow entry applies (`trust_test`, "trusted fully: it runs as the user"). That is what trusting fully means, the same trust you give your own dotfiles. For a repository you only partly trust, answer **s**: its Lua then runs in the sandbox, where `os.execute` is an error at its line and nothing from the file applies.

**Nothing from a project is used until you trust it** (`core/src/trust.cpp`). MAIC reads project files along a chain: from the workspace up to the project root (the nearest directory holding `.git`, `.maic/` or `MAIC.md`; `instructions.project_markers` and `instructions.bound` in the global settings change this), or up to just under `$HOME` when there is no project. A directory on the chain holding `.maic/`, `MAIC.md` or `AGENTS.md` is a project directory. Until it is trusted its `.maic/settings.*` are not applied, its instruction files are not given to the model, its `.maic/tools/` are not loaded, and an `AGENTS.md` below the workspace is attached only when the workspace itself is trusted. A notice at start names what was skipped and how to trust it.

* **Asked once, before anything runs.** The first time MAIC starts with an untrusted project directory, it asks on the terminal before the screen is drawn and before any project file is read, listing its settings, instruction and tool files: **t** trust fully ("its Lua runs as you"), **s** trust sandboxed ("its Lua runs in a child process that cannot reach the system"), **n** not now (untrusted this session), **v** never (remembered). The Lua level is kept with the answer. `:cd` into a directory asks the same about its chain, in a modal inside the TUI, before its settings are re-read, and `:init` says when the directory it scaffolded is not trusted yet. A directory MAIC used before this check existed is not trusted for it: the prompt says "MAIC used this directory before this check existed", and each of your repositories is asked about once.
* **Remembered with its contents.** `<state>/trust.json` (0600) keeps each answer per absolute path, with a SHA-256 over the path and content of every settings, instruction and tool file, sorted, each file's own hash, the git HEAD and what the directory could do (its permission entries, harness keys, providers, tools). When the files change, the directory's tier decides whether to ask again, naming the changed files and why.
* **Tiers**, chosen only by you (`trust_strictness`, `trust_levels`, `maic trust PATH --level L`):

  | Tier | Passes | Asks again |
  | :--- | :--- | :--- |
  | `strict` | nothing | any change |
  | `standard` (default) | your own changes: an uncommitted edit in a git working tree, or commits whose author email is one of `trust_identities` (else `git config --global user.email`) | a commit by anyone else (a pull, a merge), a checkout or reset that moved the history, a new untracked file, any change outside a git working tree |
  | `relaxed` | edits that widen nothing | a new allow entry, a removed ask or deny entry, a looser mode, `harness = "dumb"`, a looser tripwire, `allow_isolated`, `dumb_auto_ok`, a new or changed provider, a new tool or a manifest whose run, reads or writes changed, a new instruction file |

  A change that passes is named in a one-line notice and becomes the record. Every prompt and notice says the directory's tier and how to move it. Git is read locally (`rev-parse`, `ls-files`, `log`, `merge-base` on those paths only) with no `GIT_*` from the environment and with fsmonitor, hooks, pager and signature checks off, so a repository's own config cannot run anything. Your identities never come from a repository.
* **Headless and off a terminal nothing is asked:** an untrusted directory stays untrusted unless `--trust` (fully) or `--trust=sandbox` (or `=restricted`) is given, for that run only. `--bare` does not change trust.
* **`$HOME` and `/` are never projects;** with either as the workspace their files are ignored with a notice. Your global config is always trusted.
* **A project cannot loosen any of this.** `trust_*`, `global_lua`, `lua_memory_mb`, `instructions.project_markers` and `instructions.bound` in a project's file are ignored, each with a warning naming the file.
* **Commands:** `:trust [PATH] [--lua L] [--level L]`, `:untrust [PATH]`, `maic trust [PATH] [--lua full|sandbox|restricted] [--level strict|standard|relaxed]`, `maic trust --list`, `maic untrust PATH`. The Lua level and the tier are separate axes: what its Lua may do, and how often it is asked about again.

**Who can change trust.** Only you, at this machine. The agent cannot: a write to `<state>/trust*`, or a `maic trust`, `maic untrust` or `maic ... --trust` command, trips the smart harness and is refused outright by the dumb one, with no prompt for an approval to slip through (`touches_trust`, `core/src/agent.cpp`); this covers a Lua tool's inner actions too. A remote session (maic-server) uses only directories already trusted here and cannot grant trust through any session path. A paired device may change trust only through `POST /api/trust` (`path`, `action` = trust, untrust, never, level or lua, `level`, `lua`, `step_up`) with a step-up proof that a registered verifier accepts (`set_step_up_verifier`, `core/include/maic/trust.hpp`). That verifier is the hook the accounts work ([design/accounts.md](design/accounts.md)) will fill with a fresh TOTP code; until then none is registered and every request is refused with "step-up verification is not available until accounts land (docs/design/accounts.md)". Every remote request, done or refused, is a line in `<state>/trust-audit.log` naming the device. A message from another session or agent has no route to any of it.

**Known limits** of all this, with their status, are in [limits.md](limits.md#trust-and-settings-lua).

Tests: `core/tests/trust_test.cpp` (the exploit as a regression: untrusted, sandboxed and restricted refuse it, trusted fully runs it; every refused library and the loop, at both levels; `string.rep('x', 2^31)`, a growing table, `table.concat` and `string.format` against the memory limit; a function in the table; the child's memory, CPU, crash and file descriptor hygiene and its timing; your own files and themes at `global_lua`; the prompt's four answers; the hash and the tiers over real git repositories; the chain; enrollment of the tier and the Lua level; the remote hook; the agent's guard), `core/tests/agent_test.cpp` (the agent running `maic trust` or writing trust.json), `server/tests/server_test.cpp` (`POST /api/trust` with and without a verifier, and the audit), `tests/cli_smoke.py` (`maic status` on the exploit untrusted, sandboxed, restricted and fully trusted, headless with and without `--trust`, `maic trust`, `--list`, `untrust`) and `tests/test_tui.py` (the prompt before the screen).

## The tripwire (built)

A lock that stops all actions the moment something looks wrong. It survives restarts, and only a sudo password clears it. **The session keeps running while tripped.** You can keep talking to the agent, work out what happened, unlock, and carry on in the same session.

| Piece | Where | Owner |
| :--- | :--- | :--- |
| Lock file (exists = tripped) | `/var/lib/maic/tripwire` | root, 0644 |
| Lock helper | `/usr/local/sbin/maic-lock` (source: `harness/src/maic_lock.cpp`) | root, 0755 |
| Sudo rule | `/etc/sudoers.d/maic` | root, 0440 |

**Why it's shaped like this:**

* **Root owns the lock**, so nothing running as your user can delete it: not a tool, not a compromised agent, not a script it wrote.
* **Tripping is passwordless.** The only thing `/etc/sudoers.d/maic` allows without a password is exactly `maic-lock trip` (no other arguments). The reason is read from stdin, so there's nothing to inject into the command line. Tripping can only *add* restrictions.
* **Unlocking always needs your password.** `maic unlock` runs `sudo -k` first, which throws away any cached sudo login, and then `sudo maic-lock reset`.
* **`maic-lock` is a root-owned copy in `/usr/local/sbin`, never a symlink into the build tree.** A sudo rule that points at a user-writable file lets anything running as you become root.
* **`maic-lock` is deliberately tiny:** standard library only, a fixed lock path, and nothing the caller sends is used as a path.
* **`trip` and `unlock` run before anything else in the CLI loads**, so a broken config can't block the panic button.

**While tripped:** actions are refused: every agent tool call, and `maic up`. Stopping things (`maic down`) still works, because stopping is how you calm the system down.

### Install (once)

```sh
cd ~/dev2/MAIC
cmake --preset default && cmake --build --preset default
sudo ./harness/install-tripwire.sh
maic-lock status          # -> armed
```

### Use

```sh
maic trip "saw it try to rm my home dir"   # instant, no password
maic status                                # shows TRIPPED, with time, user and reason
maic unlock                                # asks for your sudo password
```

### Uninstall

```sh
sudo rm /etc/sudoers.d/maic /usr/local/sbin/maic-lock
sudo rm -rf /var/lib/maic
```

## Layers

Every agent tool call goes through `Agent::run_tool_call` (`core/src/agent.cpp`), in this order: tripwire check, harness policy, approval, then execution. No tool path skips it. Policy, the session's "always" answers and the approval prompt are one step, `Agent::authorise`; a user-defined Lua tool ([tools.md](tools.md)) hands every `maic.read` / `write` / `list` / `search` / `shell` call it makes to that same step, so a tool of yours can do exactly what a built-in could and nothing more.

**Built:**

1. **Tripwire check** before every call. If it's tripped, the call is refused and the model is told to stop.
2. **Automatic trips** (`core/src/harness.cpp`, `trip_patterns`). These deny the call and trip the lock, in every mode including plan, before any approval prompt:
   * privilege escalation: `sudo`, `su`, `doas`, `pkexec`, `run0`
   * `rm` on `/`, `~` or `$HOME`; disk tools (`mkfs`, `wipefs`, `shred`, `fdisk`, `dd of=/dev/...`, writes to raw disks); fork bombs
   * recursive `chmod`/`chown` on `/` or home; `curl`/`wget` piped into a shell
   * changing scheduled jobs or services (`crontab FILE`, `systemctl enable/start/...`), shutting down
   * anything mentioning `.ssh`, `.gnupg`, `sudoers`, `/var/lib/maic` or `maic-lock`
   * writes to secret paths (`~/.ssh`, `~/.gnupg`, `~/.aws`, `~/.kube`, `~/.docker`, `~/.password-store`, keyrings, `~/.ollama`, `/var/lib/maic`) or system paths (`/etc`, `/usr`, `/boot`, `/var`, ...)
3. **Policy by mode: allow / ask / deny.** The table is in [cli/README.md](../cli/README.md#modes). Also, in every mode:
   * secrets are never read (denied), and `search_files` skips them
   * startup files (`~/.zshrc`, `~/.bashrc`, `~/.profile`, ...), `~/bin`, `~/.local/bin`, autostart/systemd user folders and MAIC's own harness sources are always asked about before writing
   * paths are resolved with symlinks followed, so a link inside the workspace can't smuggle a write out
4. **Approval gate** in the CLI. It shows the exact command or file and why it's asking. Answers: yes, no, always (for this session, per file or per program), or **trip**.
5. **Remote origin is always asked.** A request marked `Origin::Remote` (the future server) is downgraded from allow to ask in every mode.
6. **Sandboxed commands** (`core/src/sandbox.cpp`). `run_shell` runs `bash -c` inside bubblewrap:
   * the workspace is the only writable path; `/` is read-only
   * secret directories are replaced by empty ones
   * `--unshare-all`: no network, and its own PID/IPC/UTS namespaces
   * `--new-session`: no keystroke injection into your terminal
   * `no_new_privs`: `sudo` and setuid programs do nothing
   * killed at its timeout (default 120 s, max 600 s); output capped at 32 KB
7. **Tests**: `build/core/harness_test` (also `ctest --test-dir build`). It checks every policy rule above and makes real escape attempts against the sandbox: writing to home and `/var/tmp`, reading `~/.ssh`, the network, `sudo`, `no_new_privs`, timeouts, and symlink tricks.

8. **The model is briefed.** The system prompt tells it where it is, what the tools do, what each mode allows, that DENIED/BLOCKED results are final and not to be worked around, that a trip stops everything until the user resets it, and that it cannot escalate. Briefing a model on the rules is not enforcement (the layers above are), but it cuts down on wasted retries.
9. **Read-only sandbox** for commands the harness recognises as read-only (auto-read and plan modes): the workspace itself is mounted read-only, so a misjudged command still changes nothing.
10. **Session transcripts** (`core/src/session.cpp`): every message as sent, every tool call with the harness's decision, 0600 in a 0700 directory. `maic sessions`, `maic artifacts` and `maic artifacts clean` manage them. MAIC appends to them and never rewrites one, except `maic sessions redact --in-place` (a copy under `sessions/.backups/<id>/` first, as `trans-fairy-write` takes one, then a temporary file and a rename, and a `rewritten` record). cai's tools ([cai.md](cai.md)) that rewrite one in place: `trans-fairy-write` and its `restore` (a copy under `sessions/.backups/<id>/` first, and a `rewritten` record), `redact` without `--to` and `reflow context --replace` (each a copy to its required `--backup` first), `reflow FILE --rewrite-session` (refused without it for a MAIC session or a Claude projects file; with it, a terminal confirmation, a copy to `.backups/<id>/` first and a validity check), and `edit --force`, which keeps no copy ([sessions.md](sessions.md)).
11. **Remote models are labelled.** Switching to a provider off this machine prints what leaves the machine, and the status strip shows `REMOTE`.

12. **Repeated calls.** The same call three times in a row is refused with a message telling the model to do something different; five times trips the lock. Three denials by the user in one turn end the turn.
13. **Deny with a reason.** `N` at the approval prompt takes a sentence that reaches the model as the tool result ("DENIED by the user, who says: ...").
14. **A preview before a write.** The approval prompt shows the lines an edit would remove and add (or the head of a new file), every edit of a `multi_edit`, the patch for `apply_patch`, and what a `delete_file` would remove (the file's lines, or how many files a directory holds).
15. **Undo points.** Every file a tool changes is saved first; `:undo` restores, the model is told, the transcript records it. A `delete_file` keeps the file's content, a `move_file` keeps the reverse move; a deleted directory's contents are not kept.
16. **File tools are judged like writes.** `move_file`, `copy_file`, `delete_file`, `make_dir`, `multi_edit` and `apply_patch` exist so a model reaches for a tool instead of `mv`, `cp`, `rm` or `mkdir` in `run_shell` (which asks). They get no shortcut: each path a call touches is a Write action (the source of a copy is a Read) and every one goes through the same policy, reviewer and approval before anything runs. A move whose destination is outside the workspace asks on that path; a delete under `~/.ssh` or `/etc` trips; a patch with one file the harness refuses is refused whole, before any file is written.
17. **A budget.** `budget_tokens` (or `:budget N`) stops the agent when the session's tokens reach it.
18. **`workdir` for commands** is resolved by the harness; outside the workspace it is asked about like a write there.
19. **User-defined tools stay inside.** A Lua tool runs in a state with no `io`, `os`, `require` or `load`; its only way to the machine is the `maic` table, whose calls are authorised one by one like built-in calls, logged with the tool call, and refused with a Lua error the model reads. A tool is stopped after 60 s or on Ctrl-C. A script tool ([tools.md](tools.md)) declares in its manifest what it reads and writes; each declaration is judged here as a read or write before the script starts (a write glob outside the workspace is refused, not asked), the arguments are checked against its schema, and the script then runs in the same bubblewrap sandbox as `run_shell`, the workspace writable only when it declared writes, killed at its `timeout_s`. `network: true` in a manifest is refused when it loads.

20. **A second reader.** With the default `harness = "smart"`, a model reads the last few user messages, the agent's last words and the action before any command or write that the rules would let through without asking (auto and edit modes), and answers ALLOW, ASK or DENY. ASK turns into an approval prompt, DENY refuses with the reason in the tool result, and a reviewer that fails or gives no clear verdict means ASK. Reads and actions the user already approved are not reviewed. The review is a separate, tool-less call. Its model, first match wins: `reviewer_model` (the user's pin), the preset's `reviewer` (the local presets review with themselves, which is free and already loaded), `small_model`, then the family's small model, the cheapest non-limited preset on the preset's `subagents` list (haiku-4.5 for the Anthropic presets: a one-line verdict needs no more), then the model itself; the transcript's `review` object records `model` and `model_reason`. The reviewer runs on every command and write in auto and edit modes, so its cost and its limits are held to account: its tokens count toward `budget_tokens`, and `reviewer_budget_tokens` caps them on their own. A reviewer that reports a usage limit is never called again that session: the next review goes to the failed preset's `on_limit` or the cheapest non-limited preset no higher in tier, with one notice, and when none is left, or the cap is reached, the reviewer is off and every action it would have reviewed is asked (fail closed: never silently allowed). A subagent's reviewer follows the same rule from the child's own model. `:harness` shows the model, why, and what it has spent. The reviewer can run on Claude Code with your own plan instead of API billing (`small_model` or `reviewer_model` = `claude-haiku-cli`, [settings.md](settings.md#claude-code-as-a-provider)): the review prompt becomes the CLI's system prompt, its usage limit sends reviews to the API's `haiku-4.5` like any other limit, and a CLI that cannot be run means ASK. A `cli` provider (claude -p) is text only: its tools and MCP servers are off, so it takes no actions and MAIC's harness remains the only judge of what runs.

21. **An allow list.** `allow` in settings (and `:allow` in a session) holds command patterns the user pre-approved; they run in every mode but plan without an approval prompt and without the second reader. MAIC's own helpers are on it by default. An entry, a helper's read-only shape and a session's "always" for a program match only one simple command: a command with `;`, `&`, `|`, a line break, a backtick, `$(`, `<(`, `>(`, `>` or `<` anywhere matches none of them (`is_simple_command`), so `maic path && rm -rf .` is not `maic path*`; it gets the mode's own decision, asked in manual and reviewed in auto. Trip patterns are checked first and still win; the sandbox still applies.

22. **Forbidden terms.** `forbid` in settings (and `:forbid` in a session) lists terms no tool call may contain, in any letter case. The check runs first, on the call's name and its whole argument object, and again inside `Harness::check` on commands and paths so a Lua tool's inner actions are covered. A hit is a denial the model reads and a notice the user sees; nothing runs. Being a rule, it does not depend on the reviewer and holds under the dumb harness.

**Repeats, refined.** The doom-loop rule now tells harm from confusion: a repeated write or a repeated command that could change something trips the lock at five, as before; a repeated read, read-only command or allow-listed command is refused at three and ends the turn at five, with a note to the user, because a small model re-running `maic-storyboard` is stuck, not dangerous, and a root-owned lock is the wrong answer to stuck.

21. **The harness protects itself.** Writing MAIC's settings, its lock files, the server's tokens, the trust record, or running the lock helper or `maic trust` is refused as the agent's business (trust is refused outright under either harness; see [Directory trust](#directory-trust-and-restricted-settings-lua-built)): under the smart harness it trips the machine lock, since a request from another agent that tries it is exactly what the global lock exists for; the dumb harness asks. The machine lock outranks every session: locked, unlocked or session-scoped, a session sees it, unless it is isolated (below).

23. **The additive `permission` block.** `permission = { allow, ask, deny }` in settings holds patterns over `tool:argument` (`run_shell:pytest *`, `write_file:src/**`, `read_file:/etc/**`; `write:` and `read:` for any such tool). It runs in `Harness::check` after every rule above: a trip pattern, a write to a secret or system path, a forbidden term, plan mode's refusals and an isolated session's fence have already decided, and the block only sees what is left as allow or ask. `deny` wins over `ask` wins over `allow`: a deny entry refuses with a reason the model reads, an ask entry turns an action the mode would run silently into an approval prompt, and an allow entry runs it without a prompt and without the second reader, in every mode but plan. `allow` entries are ignored for `Origin::Remote`, which stays asked. An allow entry for `run_shell` matches only a simple command (`is_simple_command`), while a deny or ask entry matches the whole line and each command in it (`command_segments`: the line split on `;`, `&`, `|` and line breaks, the insides of `$(...)`, `(...)` and backticks as commands of their own, each trimmed of a leading `{`, `!`, shell keyword or `VAR=value`; quotes are not honoured, so a separator inside a string only adds a segment), so a prefix such as `true; git push` does not dodge `run_shell:git push*`. Forbidden terms and trip patterns were always searched over the whole line. The allow list above is the block's `run_shell:` allow entries (the old `allow` key still works and means those); `:allow` edits them at run time. Layers of settings add up, so a project can add restrictions or pre-approve its test command without touching your global file.

24. **Agents and subagents.** An agent (`core/include/maic/agent_def.hpp`, `AgentDef`; opencode's term, and MAIC's profiles before) is a named narrowing: a mode (the most it allows), a `role` (who may run it: `primary` the session, `subagent` the `task` tool, `all` both), `write_paths` (globs under the workspace; empty means the whole workspace and never outside it), `read_outside`, a token budget, a step limit, a tool allow-list (one with no write tool makes the agent read-only) and whether the second reader applies; no agent has the network. Built in, as in opencode: `build` (primary: everything the session has), `plan` (all: plan mode, reads inside the workspace only, 50k tokens), `general` (subagent: edit mode), `explore` (subagent: auto-read, read-only tools plus read-only commands, 50k tokens); `agents` in settings adds or narrows, and widening a built-in is an error when settings load. The model's `task` tool runs a child `Agent` in the same workspace as an agent whose role allows it: `Harness::set_agent_def` makes `check()` deny a write outside the agent's paths, a read outside the workspace when it may not, and anything that could write under a read-only agent, each with the agent named; the agent loop denies a tool off the list. The child's mode is its agent's capped by the parent's (`narrower_mode`), so a manual session's children are asked about through the parent like the session itself; its approvals and notices reach the parent's front end with the agent's name, its tool calls show indented under the task call, its tokens count against the parent's budget, and it stops at its own budget, on Ctrl-C with the parent, and the moment the tripwire trips. A child has no `task`, `question` or `todo`: one level, reporting to the parent in its final answer, which becomes the parent's tool result. Its model is chosen from the session's preset (an agent's `model` is a pin; a limited model hands subagents to a non-limited tier; see [settings.md](settings.md#model-presets-and-tiers)), and after a usage limit it continues once on the preset's `on_limit`. It has its own transcript (kind `sub`, the parent's id, the agent and `model_reason` in its `start` record) that `maic sessions` lists under the parent. Everything above applies to a child unchanged: trip patterns, secrets, system paths, `permission`, `forbid`, the sandbox, the repeat guard.

25. **The host nvim** ([nvim.md](nvim.md)). Inside nvim MAIC connects to `$NVIM` only when it is a Unix socket owned by the user whose listening nvim (SO_PEERCRED) runs as the user and is one of MAIC's ancestors; `MAIC_NVIM_TRUST_SOCKET=1` skips the ancestry part for tests and is ignored without `MAIC_TESTING=1`. The model gets one thing from the host, the `diagnostics` tool, an Action of kind Read on the file or the workspace judged like `read_file` (a secret is never read, a path outside the workspace asks in manual and plan modes); Lua tools get a read-only `maic.nvim.diagnostics` / `buffers`, each authorised as a read. Arbitrary Lua in the host (`maic.nvim.exec`) belongs to the user's own Lua only. A model's edit is always a file write through this harness, with its preview, undo point and approval; the host learns of it through `:checktime`.

**Lock scope.** `tripwire = "session"` in settings makes a trip from that session land in a user-owned file beside its transcript instead of the root-owned machine lock: the session stops exactly as before, resumes still see it, and `:unlock` removes it without sudo. The machine lock is honoured in both scopes, and the more specific settings file wins, so a scratch project can run session-scoped while the default stays machine-wide. `tripwire = "isolated"` is the opt-out: the machine lock is ignored, which is allowed only with `allow_isolated = true` and costs the session its reach: reads stay inside its directory, commands run only there, remote requests are refused and the server will not host it. `maic unlock` lists the machine lock and every session's lock (task, directory, model, running or not) and unlocks what you pick; `maic up`, `maic down` and `maic open` offer the same kind of menu when given no name.

**The root moves with `:cd`.** `:cd PATH` (a user command, refused while the agent works and for `Origin::Remote`; no tool can do it) sets the harness's workspace: what counts as inside, the sandbox's writable directory, relative paths and permission patterns relative to the workspace all follow the new directory, and so does the process directory shell commands start in. The tripwire, the forbid list, the harness choice and the locks are not touched, and a directory's settings cannot change them mid-session. A confined session (`tripwire = "isolated"`) may only move within the directory it was started in; anything else is refused with that reason.

**The dumb harness.** `harness = "dumb"` (`:harness dumb`, `--harness dumb`) turns the reviewer off: nothing reads the conversation, the rule list alone decides. That is Micaiah's explicit option, not a fallback. Because auto mode then runs any well-formed command the rules do not catch, entering auto under a dumb harness shows a warning once and asks for a yes; after that the session settles in and does not ask again. `dumb_auto_ok = true` or `--accept-dumb-auto` skips the warning; a headless run refuses dumb + auto without one of them. Remote sessions are unaffected: a remote request is always asked, so the reviewer never sees one.

**Planned:**

* **Landlock** as a second filesystem fence applied by the core itself, and **resource limits** (memory, process count).
* **Per-tool network grants**: some future tools will need the network, declared in their manifest; a script tool's `network: true` is refused when the manifest loads ("per-tool network grants are not implemented yet") and an agent's `network` field is reserved for them and is an error today.
* **Primary agents for sessions** (a session running as `plan` or an agent of your own rather than `build`) and **nested tasks**: today an agent applies to a subagent, one level deep.
* **Token and cost budgets** per session, from the usage figures providers return.
* **Forkable transcripts**: `maic -r ID --fork-at N` continues from an earlier point in a new session file; the parent is never edited.
* **Docker as a service runtime** (`"runtime": "docker"` in `services/*.json`): the service manager would start and stop containers and `:status` would show them the same way.

The comparison with opencode that produced several of these is in [opencode-comparison.md](opencode-comparison.md).

Existing protections in the service manager (`core/src/service.cpp`):

* MAIC only signals processes it started. Each PID file stores the PID **and** the process's start time from `/proc`, so a reused PID never matches.
* It refuses to stop anything on a service's port that it didn't start.
* It signals a whole process group only if the group is still the one MAIC created.

## Windows version (not built; design notes)

Same idea as Linux: tripping is instant and needs no credentials, resetting needs admin credentials, and the session survives. Windows actually makes one part easier, because NTFS permissions can separate "create" from "delete", which Unix directory permissions can't.

### Tripwire

Two ways to build it. Option B is preferred.

**Option A: ACL-only lock folder (no service)**

* Lock folder `C:\ProgramData\MAIC\tripwire\`, owned by `SYSTEM`, with inheritance from the parent turned off.
* ACL on the folder:
  * `SYSTEM`, `Administrators`: Full control.
  * `Users`: *Create files / write data* + *Read*, applied to this folder only. **No** *Delete* and **no** *Delete subfolders and files*. So users can create the lock file but not remove it.
  * `CREATOR OWNER`: removed, so the person who creates the lock file doesn't automatically get full control of it.
  * `OWNER RIGHTS` (`S-1-3-4`): *Read* only, inherited by files. Without this, a file's owner always keeps the right to rewrite its permissions (`WRITE_DAC`) and could give themselves delete rights. The `OWNER RIGHTS` entry removes that.
* Trip: write `tripwire` into the folder (no elevation needed). Reset: an elevated `maic-lock reset` deletes it.
* Downside: getting the ACL exactly right is fiddly, and one mistake quietly makes the lock deletable. Check it with `icacls` after install.

**Option B: a small SYSTEM service (mirrors the sudo helper)**

* `maic-lockd`: a tiny C++ Windows service running as `LocalSystem`. It owns `C:\ProgramData\MAIC\tripwire`, whose ACL is SYSTEM/Administrators only, with Users read-only.
* It listens on a named pipe (`\\.\pipe\maic-lock`). The pipe's security descriptor lets Users connect. It accepts only two messages: `trip <reason>` from anyone, and `status`.
* **Reset is not on the pipe.** `maic unlock` starts an elevated `maic-lock reset` (`ShellExecuteEx` with the `runas` verb), which deletes the file directly.
* The service verifies nothing about the caller except that the message is a trip request, the same as the passwordless `maic-lock trip` sudo rule. Tripping can only add restrictions.

**What makes unlocking actually need a password:**

* By default, an admin account on Windows gets a UAC *consent* prompt: one click, no password. That's weaker than Linux, where unlocking needs your sudo password.
* Fix: set UAC to ask admins for credentials (`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\ConsentPromptBehaviorAdmin = 1`, or the Local Security Policy "Behavior of the elevation prompt for administrators" = *Prompt for credentials on the secure desktop*). Even better: run MAIC from a standard (non-admin) account, so elevation always needs the admin password.
* UAC's secure desktop also stops a process from clicking its own elevation prompt.

**Checking it:** the core checks whether `C:\ProgramData\MAIC\tripwire` exists before every action, the same as `/var/lib/maic/tripwire` on Linux. The path is fixed at compile time.

### Other harness layers on Windows

| Linux | Windows equivalent |
| :--- | :--- |
| PID + `/proc` start time identity | PID + `GetProcessTimes` creation time |
| Process group kill (`setsid`, `kill(-pgid)`) | Job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then `TerminateJobObject` |
| bubblewrap filesystem/network sandbox | AppContainer (`CreateAppContainerProfile` + `SECURITY_CAPABILITIES`): no network unless a capability is granted, access only to granted paths. Heavier option: Windows Sandbox |
| Landlock | AppContainer ACLs on the workspace folder only |
| `no_new_privs` | Restricted token (`CreateRestrictedToken`, drop admin group, `LUA_TOKEN`) plus AppContainer, so the tool can't elevate |
| `prlimit` | Job object limits: memory, CPU rate, active process count |
| Shell tool (`sh -c`, PTY) | `cmd.exe /c` / `pwsh -NoProfile -Command`; ConPTY (`CreatePseudoConsole`) for persistent sessions |
| `~/.local/state/maic` | `%LOCALAPPDATA%\MAIC\` (PIDs, logs, audit log) |

Things to watch on Windows:

* Blocklists need Windows forms too: `Remove-Item -Recurse`, `rd /s`, `format`, `diskpart`, `bcdedit`, `vssadmin delete shadows`, `reg delete HKLM`, `takeown`/`icacls` on system paths, `Set-ExecutionPolicy`, and anything touching `C:\Windows`.
* Also protect `C:\ProgramData\MAIC`, the `maic-lockd` service settings (`sc config`/`sc delete` need admin anyway), and the user's `%USERPROFILE%\.ssh`.
