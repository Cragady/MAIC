"""One write doctrine, reachable from every family that writes.

**Built 2026-09-03, after an audit found four `--to`/`--out` flags that clobbered
their destination silently.** The board had carried this as unbuilt since a peer
session pointed out that the reasoning already existed in `redact` -- *"in-place
rewrite truncates, so a concurrent append is destroyed and a backup cannot
recover it"*, and *"never `cp -n`, which declines silently and returns success"* --
and was not reachable from the families that needed it.

**That is not the two-homes failure but its opposite.** The doctrine had exactly
one home and was correct there. What it lacked was any mechanism making it apply
to the next writer, so every tool added afterwards started from zero. A rule that
lives in one tool is a property of that tool, not of the suite.

TWO KINDS OF WRITE, AND THEY FAIL DIFFERENTLY
---------------------------------------------
**A NEW file** -- `--to`, `--out`, a snapshot. The hazard is that the path is not
new. `--to` reads as *non-destructive* and is only non-destructive **about the
source**; nothing about the destination is implied by that word, and four flags
in this suite silently proved it.

**An IN-PLACE rewrite** -- `cai edit`, `reflow lines`. The hazard is that there
is nothing to recover from. These are content-guarded -- an anchored replacement
that must parse, a reflow whose content signature must hold -- so the CONTENT is
safe. The FILE is not: a correct-but-unwanted result overwrites the original with
no copy anywhere. **Git is the backup, which is fine until the file is not
tracked**, and nothing was checking.
"""
import os
import subprocess


def stat_token(path):
    """A cheap fingerprint of a file's identity and state, or None if absent.

    **mtime alone is not enough**, and neither is size: a rewrite can land on the
    same second, and an edit can preserve length. Together with the inode these
    catch appending, in-place rewriting, and replacement-by-rename, which are the
    three ways a file changes underneath a reader.
    """
    try:
        st = os.stat(path)
    except OSError:
        return None
    return {"mtime_ns": st.st_mtime_ns, "size": st.st_size, "inode": st.st_ino}


def unchanged_since(path, token):
    """Has this file stayed as it was? Returns `(ok, why)`.

    **This detects the race that `liveness` only predicts.** A liveness check asks
    *does this look like something a client is holding open* -- an mtime
    heuristic, and it cannot see whether the client actually wrote. Taking a
    token before the read and checking it immediately before the write asks the
    answerable question instead: *did anything change while I was working?*

    **It is the check the backup cannot substitute for.** A backup holds the
    pre-read state, so a write that races a client destroys the client's write
    and the backup does not contain it -- that is exactly what the in-place
    hazard is. Refusing on a moved token is the only thing that prevents it
    rather than recording it.

    **Absence of a session record is not evidence a session ended** -- this
    session's own vanished from `~/.claude/sessions` while it was still running,
    which is why identity registries are the weaker signal and file metadata is
    the stronger one.
    """
    now = stat_token(path)
    if token is None or now is None:
        return False, "the file could not be stat'd before or after; refusing rather than guessing"
    if now == token:
        return True, "unchanged since it was read"
    moved = [k for k in token if token[k] != now[k]]
    return False, ("%s changed while this was working (%s). Something else wrote to it, and "
                   "a backup taken before that does not contain what it wrote."
                   % (path, ", ".join(moved)))


def write_new(path, data, force=False):
    """Write to a path that **must not exist**.

    Refusing is the whole function. `--to` and `--out` are the flags an operator
    reaches for *to avoid* destroying something, so a clobber there is the
    opposite of what was asked for.
    """
    if os.path.exists(path) and not force:
        raise FileExistsError(
            "%s exists. Refusing rather than overwriting -- pick a new path, or pass "
            "--force if replacing it is what you meant." % path)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(data)
    return path


def tracked(path):
    """Is this file tracked by git, and therefore recoverable after a bad write?"""
    d = os.path.dirname(os.path.abspath(path)) or "."
    try:
        r = subprocess.run(["git", "-C", d, "ls-files", "--error-unmatch",
                            os.path.basename(path)],
                           capture_output=True, text=True, timeout=10)
        return r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def recoverable(path, backup=None, force=False):
    """May this file be rewritten in place? Returns (ok, why).

    **Recoverable means there is a copy somewhere after the write goes wrong**:
    git history, or a backup the caller took. Content guards do not count -- they
    prove the new content is right, not that the old content still exists.
    """
    if force:
        return True, "forced by the operator"
    if backup and os.path.exists(backup):
        return True, "a backup exists at %s" % backup
    if tracked(path):
        return True, "tracked by git, so the previous content is recoverable"
    if not os.path.exists(path):
        return True, "the file does not exist yet, so there is nothing to lose"
    return False, (
        "%s is not tracked by git and no backup was given, so a rewrite here is "
        "unrecoverable. Take a backup, or pass --force if losing the previous "
        "content is what you meant." % path)
