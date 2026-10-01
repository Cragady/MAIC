# lazy-lock tracking

MAIC keeps a SHA-256 of nvim's `lazy-lock.json` (lazy.nvim's plugin pins) and tells you when the file no longer
matches it: a plugin was updated, added or removed, or lazy.nvim updated itself. It never runs nvim, never writes
the lock file and uses no network. Nothing is said on a machine with no lock file and nothing recorded.

## Files

| File | What |
| :--- | :--- |
| `lazy-lock.json` | `lazy_lock` in settings when set (`~` expands), else `$XDG_CONFIG_HOME/$NVIM_APPNAME/lazy-lock.json`: `~/.config/nvim/lazy-lock.json` by default. |
| `~/.config/maic/nvim-lazy-lock.sha256` | The recorded hash, one `sha256sum` line (`<hex>  .config/nvim/lazy-lock.json`, the path relative to `~`). Under `$XDG_CONFIG_HOME/maic` when that is set. Commit it with your dotfiles, beside `lazy-lock.json`. |
| `~/.local/state/maic/lazy-lock/recorded.json` | A copy of the lock file at record time, for per-plugin diffs. Machine-local, not for git. |

The hash file is written in place, so a symlink into a dotfiles repository stays a symlink. Because the path in it
is relative to your home directory, `cd ~ && sha256sum -c ~/.config/maic/nvim-lazy-lock.sha256` checks it without
MAIC, on any machine.

## Commands

`maic lazy-lock` in the shell, `:lazylock` in a session (`maic help lazy-lock`, `:h lazylock`).

| Command | What |
| :--- | :--- |
| `maic lazy-lock` | `in sync`; `changed since DATE: N updated, N added, N removed`; `not recorded yet (maic lazy-lock record)`; or `no lazy-lock.json at PATH`. |
| `maic lazy-lock record` | Writes the hash file and the snapshot, and prints the old and the new hash. |
| `maic lazy-lock diff` | Per plugin: `added`, `removed`, `updated` with `commit old..new` (short hashes) or `branch old..new`. lazy.nvim's own entry is marked `(package manager updated)`. |

Exit codes, for scripts: 0 in sync, 1 changed or not recorded, 2 no lock file or an error.

On a fresh machine where the hash file came with your dotfiles there is no snapshot yet. `diff` says so and still
reports whether the hashes match; `record` there starts a snapshot.

## Being told

- A notice at the start of a session when the file is out of sync or not recorded, for example
  `nvim's lazy-lock.json changed since it was recorded: 3 updated, lazy.nvim itself among them. maic lazy-lock diff / record`.
- `lock≠` in the status strip while it is out of sync (or the lock file is gone after a record). It is re-checked
  at the end of each turn and on `:status`, by the files' mtime and size; the file is hashed again only when those change.
- A line in `maic status` (`nvim lazy-lock: ...`) and in `maic doctor`.

`lazy_lock_notice = false` in settings turns off the notice and the marker. `maic status`, `maic doctor` and the
command still report.
