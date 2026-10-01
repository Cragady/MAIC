"""SOPIA as it is published, read through a local mirror.

**The priority, owner 2026-09-02: hosted, then SOPIA REMOTE, then SOPIA local,
then the sync-frozen snapshot.** The remote sits above the working copy
deliberately -- **a definition is a shared decision, and the pushed state is the
agreed one.** A local working tree may hold an experiment nobody else has seen,
and reading policy out of it would let one machine's half-finished edit answer
as though it were settled.

**Consequence worth stating plainly: a local edit to a definition does not take
effect until it is pushed.** That is the intended behaviour and it will surprise
somebody, so it is written here rather than discovered.

**The remote is read through a MIRROR, not fetched per call.** A network round
trip on every lookup would make `cai grant check` -- which runs in a git hook --
depend on the network being up, and a gate that fails when the network does is a
gate that gets turned off. The mirror is refreshed on a TTL or on demand, and
**every failure falls through to the next tier rather than raising**.

**Where the remote comes from.** It is derived from this repository's own origin:
same host, same owner, sibling repository. Nothing is configured, so nothing can
be configured wrongly.
"""
import os
import re
import subprocess
import time

MIRROR = os.path.expanduser("~/.local/state/cai/mirror/SOPIA.git")
REFRESH_SECONDS = 900
SIBLING = "SOPIA"
#: This package's own repository origin, recorded when it moved into MAIC
#: (tools/cai/PROVENANCE.md). The repository it sits in now is MAIC's, whose origin
#: has a different owner, so the sibling is derived from the recorded one.
ORIGIN = "git@github.com:CascadeRefiningInc/cai-tools.git"


def _run(args, cwd=None, timeout=30):
    try:
        p = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.SubprocessError) as e:
        return 1, "", str(e)


def sibling_url(repo=None):
    """The SOPIA remote, derived from this repository's origin.

    Same host, same owner, sibling name. Derived rather than configured, because
    a configured URL is one more thing that can point somewhere wrong and say so
    only when it matters.
    """
    if repo:
        rc, out, _ = _run(["git", "remote", "get-url", "origin"], cwd=repo)
        if rc != 0 or not out.strip():
            return None
        origin = out.strip()
    else:
        origin = ORIGIN
    m = re.match(r"^(git@[^:]+:|https://[^/]+/)([^/]+)/(.+?)(?:\.git)?$", origin)
    if not m:
        return None
    return "%s%s/%s.git" % (m.group(1), m.group(2), SIBLING)


def mirror_age():
    marker = os.path.join(MIRROR, "FETCH_HEAD")
    for p in (marker, MIRROR):
        try:
            return time.time() - os.path.getmtime(p)
        except OSError:
            continue
    return None


def ensure_mirror(refresh_seconds=REFRESH_SECONDS, force=False, timeout=30):
    """Make the mirror exist and be fresh enough. **Never raises.**"""
    url = sibling_url()
    if not url:
        return {"ok": False, "note": "could not derive a SOPIA remote from this origin"}
    if not os.path.isdir(MIRROR):
        os.makedirs(os.path.dirname(MIRROR), exist_ok=True)
        rc, _, err = _run(["git", "clone", "--bare", "--depth", "1", url, MIRROR],
                          timeout=timeout)
        if rc != 0:
            return {"ok": False, "url": url, "note": "clone failed: %s" % err.strip()[:200]}
        return {"ok": True, "url": url, "action": "cloned"}
    age = mirror_age()
    if force or age is None or age > refresh_seconds:
        rc, _, err = _run(["git", "fetch", "--depth", "1", "origin", "HEAD"],
                          cwd=MIRROR, timeout=timeout)
        if rc != 0:
            return {"ok": True, "url": url, "action": "stale",
                    "age_seconds": int(age or 0),
                    "note": ("fetch failed (%s); serving the mirror as it stands. A gate "
                             "that fails when the network does is a gate that gets turned "
                             "off." % err.strip()[:120])}
        return {"ok": True, "url": url, "action": "fetched"}
    return {"ok": True, "url": url, "action": "fresh", "age_seconds": int(age)}


def read(path, refresh_seconds=REFRESH_SECONDS, timeout=30):
    """A file from the published SOPIA, or None. **Never raises.**"""
    state = ensure_mirror(refresh_seconds=refresh_seconds, timeout=timeout)
    if not state.get("ok"):
        return None, state
    for ref in ("FETCH_HEAD", "HEAD", "refs/heads/main"):
        rc, out, _ = _run(["git", "show", "%s:%s" % (ref, path)], cwd=MIRROR, timeout=timeout)
        if rc == 0:
            return out, dict(state, ref=ref, path=path)
    return None, dict(state, note="%s is not in the published SOPIA" % path)
