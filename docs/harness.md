# Harness

The harness is what stops a MAIC agent from damaging the machine it runs on. It lives in the C++ core, and every action an agent takes goes through it. Nothing a model or tool says can switch it off.

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

Every agent tool call goes through `Agent::run_tool_call` (`core/src/agent.cpp`), in this order: tripwire check, harness policy, approval, then execution. No tool path skips it.

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
10. **Session transcripts** (`core/src/session.cpp`): every message as sent, every tool call with the harness's decision, 0600 in a 0700 directory. `maic sessions`, `maic artifacts` and `maic artifacts clean` manage them.
11. **Remote models are labelled.** Switching to a provider off this machine prints what leaves the machine, and the status strip shows `REMOTE`.

12. **Repeated calls.** The same call three times in a row is refused with a message telling the model to do something different; five times trips the lock. Three denials by the user in one turn end the turn.
13. **Deny with a reason.** `N` at the approval prompt takes a sentence that reaches the model as the tool result ("DENIED by the user, who says: ...").
14. **A preview before a write.** The approval prompt shows the lines an edit would remove and add (or the head of a new file).
15. **Undo points.** Every file a tool changes is saved first; `:undo` restores, the model is told, the transcript records it.
16. **A budget.** `budget_tokens` (or `:budget N`) stops the agent when the session's tokens reach it.
17. **`workdir` for commands** is resolved by the harness; outside the workspace it is asked about like a write there.

**Planned:**

* **Landlock** as a second filesystem fence applied by the core itself, and **resource limits** (memory, process count).
* **An additive `permission` block in settings** (allow / ask / deny per tool or command pattern) that can only add restrictions or pre-approve harmless commands, never touch trip patterns, secrets or system paths.
* **Per-tool network grants**: some future tools will need the network, declared in their manifest.
* **Permission profiles by role** (orchestrator, builder, scout, reviewer) for sessions and future subagents: mode, write paths, network and budgets per profile, narrowing only. See [cleanroom.md](cleanroom.md).
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
