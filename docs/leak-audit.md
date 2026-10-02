# Leak audit

`maic-leak-audit` answers one question about your session transcripts: did any agent reach for a host socket from the command sandbox? Until v0.3.1 the sandbox left them reachable ([releases/v0.3.1.md](releases/v0.3.1.md), [harness.md](harness.md#layers) rule 6): the session D-Bus, through which `systemd-run --user`, `busctl`, `gdbus` or `dbus-send` start processes outside the sandbox; your nvim socket; `docker.sock`; libvirt, screen, snapd, systemd, mysqld and cups sockets; ssh-agent and gpg-agent; and the environment variables naming them. The source is `tools/audit/leak_audit.py`.

```
maic-leak-audit [--model PRESET] [--dry-run] [--root DIR ...] [--timeout S]
```

## What it checks

1. **Phase 1, deterministic.** Every `.jsonl` file under MAIC's sessions directory (`$XDG_STATE_HOME/maic/sessions`, backups under `.backups/` included) and the runtime one (`$XDG_RUNTIME_DIR/maic/sessions`) is read leniently: a line that is not a JSON object is counted and skipped. `--root DIR` (repeatable) reads those directories instead. A `tool` record is a candidate when its arguments, or the actions a Lua or script tool was judged on, name an escape route: `dbus-send`, `busctl`, `gdbus`, `qdbus`, `systemd-run`, `systemctl --user`, `nvim --server`/`--remote` or an `nvim.PID.0` socket, `socat`, `nc -U`/`ncat -U`, `docker`, `podman`, `virsh`, `screen -x`/`-r`/`-ls`, mysql with a socket, `/run/`, `/var/run/`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`, `SSH_AUTH_SOCK` or `ssh-add`, `GPG_AGENT_INFO` or `gpg-connect-agent`, `/nix/var/nix/daemon-socket`, `AF_UNIX`, `curl --unix-socket` and `UNIX-CONNECT`. Each keeps its file and line, the tool, the call, the harness decision (with the user's answer and the reviewer's verdict when there were any), whether it ran, and how it ended.
2. **Phase 2, local judges.** Each candidate goes, with the exchange around it (the user message that started the turn, the records before the call and two after), to a local model, by default the `qwen-9b` preset (`--model` picks another). It answers `reached` (an attempt to use a host socket or get out of the sandbox, allowed or not, run or not; probing for one counts), `mentioned` (the term is only in text, a file's content or a harmless use) or `unclear`, with one sentence.

## Privacy

* **Local judges only.** The model is resolved with `maic model resolve`. The audit refuses to run unless the result is a local provider (`remote` false) whose `base_url` is a loopback address, and speaks the OpenAI-compatible chat API to it with the standard library, with proxies from the environment and redirects both turned off, so a request cannot leave the machine. No flag overrides this. When the local server is not answering it says to start it (`maic up llamacpp`) and exits 2 without judging anything.
* **One line out.** The full report (every candidate, its excerpt, the verdict and sentence, a summary table) is written only to `<state>/maic/audits/leak-audit-<UTC time>.md`, 0600 in a 0700 directory. Standard output gets exactly one line, whatever happens:

  ```
  leak audit complete: report at PATH. Something was reached for: YES
  ```

  `NO` when nothing was judged reached or unclear, `UNCLEAR` when nothing was reached but something was unclear, and `UNKNOWN` (in a "leak audit not completed" line) when the audit could not finish. No counts, session names or paths, and no transcript text appear on standard output or standard error, errors included. So an agent that runs the audit for you learns one word, and only you read the report.
* `--dry-run` runs phase 1 only and prints the same line with `(phase 1 only)`; any candidate then makes it `UNCLEAR`.

Exit codes: 0 when the audit completed, 2 when it could not run (usage, a model that is not local, no judge answering), 1 for an internal error.

## Limits

The keyword filter is the floor: a call that reaches a socket without naming any of these terms (a socket in another directory, a path built at run time) is not a candidate. Only `tool` records are read, so a socket named only in conversation is not judged. A small local model can misjudge; the report shows the call and its context so you can check every verdict yourself.
