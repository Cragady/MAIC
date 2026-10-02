# The daemon

One engine in the background that owns sessions, so a session outlives the window it started in (roadmap item 4; engine protocol step 13, [design/engine-protocol.md](design/engine-protocol.md)).

```
maic daemon start            # runs it detached; its output in ~/.local/state/maic/engine/daemon.log
maic daemon status [--json]  # running or not (exit 3 when not), and every session it holds
maic daemon stop [--yes]     # parks every session; asks first when a turn is running
maic daemon run              # the same in the foreground (what the systemd unit runs)
maic daemon unit             # prints a systemd user unit
maic daemon unit install     # writes it to ~/.config/systemd/user/ (it is not enabled)
maic daemon unit remove      # disables it, stops a daemon it runs, and removes it
```

## What changes while it runs

* **`maic` and maic.nvim open their sessions in the daemon.** The TUI says so at start ("in the daemon") and shows `daemon` in its status strip; `maic --rpc`, maic.nvim's engine, carries its connection to the daemon's socket unchanged.
* **Quitting leaves a session where it is.** A session working when you quit keeps working; an idle one is parked (it leaves memory and resumes where it was). The next `maic` or `:Maic` lists both in `:switch`, a finished one marked as such.
* **One engine holds each session**, so two windows cannot both append to it.
* **`maic daemon stop` parks everything.** A running turn is interrupted (asked first; `--yes` does not ask) and resumes where it stopped.

`daemon = "off"` in settings keeps every session in its own process, as before.

## What does not travel to it

The daemon reads each workspace's settings files as a start there would, so a session there is set up like one in the TUI. A command line can ask for what only its own process can give; such a run keeps its session in its own engine and says why at start:

| Flag | Why it stays in this process |
| :--- | :--- |
| `--system`, `--prefix`, `--rule`, `--no-instructions`, `--ban`, `--sampling`, `--harness`, `--ctx`, `--ctx2`, `--accept-dumb-auto`, `--record`/`--no-record` | settings for this run only, which no protocol call carries |
| `--context` | a file attached before the first turn |
| `--fork-at`, `--no-append` | a fork the daemon does not make from a file |
| `--trust` | trust for this process only (the daemon has its own) |

`--model`, `--mode`, `-c`, `-r`, `--image`, `-i` and `--bare` travel. `maic --rpc` with any flag at all runs its own engine.

Two things differ for a session in the daemon:

* **No nvim host.** A daemon is no nvim's child, so a write is not opened in nvim, an approval has no nvim diff and there is no `diagnostics` tool, even when the TUI runs inside nvim.
* **`!cmd` runs in the daemon's environment** (its PATH, its virtualenv), not the terminal's.

## Where it lives

* **The socket** is `$XDG_RUNTIME_DIR/maic/engine.sock` (without a runtime directory, `~/.local/state/maic/run/engine.sock`, never `/tmp`), 0600 in a 0700 directory that must be yours. Every connection's peer is checked to be you (`SO_PEERCRED`). The command sandbox hides both directories, so a sandboxed command cannot reach the daemon and answer its own approvals.
* **The PID** is kept with the process's start time in `engine.pid` beside the socket, as MAIC keeps its services' (MAIC owns the PID, not systemd), and a lock file (`engine.lock`) makes it the only daemon. One that was killed leaves its socket behind; `maic daemon status` says so and `maic daemon start` clears it.
* **The session index** is `~/.local/state/maic/engine/index.json`: a restarted daemon lists what the last one held as parked.

## Starting it at login

Not automatic: `maic daemon unit install` writes the unit and says how to enable it (`systemctl --user enable --now maic-daemon.service`). The unit runs the `maic` on your PATH (`~/bin/maic`, which follows releases) with `maic daemon run`; `maic daemon stop` and `status` work the same under systemd.
