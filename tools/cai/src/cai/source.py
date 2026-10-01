"""Where cai fetches data from, and in what order.

**SOPIA defines; cai enforces and fetches.** The same resolution applies to every
data file the tools read, so it is written once here rather than three times:

**The priority, owner 2026-09-02:**

    hosted  ->  SOPIA  ->  local sync-frozen snapshot

Two more exist and are NOT peers of those three, which is why they are named
separately rather than folded into the list:

    an ENVIRONMENT VARIABLE sits above everything as an override, for tests and
    for pointing at something else deliberately. It is not a source of truth; it
    is a way to say "use this instead", and treating it as a tier would make an
    accident indistinguishable from an instruction.

    the EMBEDDED FALLBACK sits below everything as bootstrap. It is what ships
    before `cai sync` has ever run, so a fresh install answers at all. Once a
    snapshot exists the snapshot is preferred, because both are copies and only
    one states when it was taken.

**The fallback answers only when the source cannot be reached**, which is the case
an outside adopter runs in permanently. It is stamped so a reader knows what it
is, and `stale()` reports how far behind it is -- *stale* without an age is not
actionable, since a copy a day behind and one a year behind warrant different
confidence.

**This order is also what a hosted store slots into.** It becomes the first
candidate, everything below it stays as written, and no caller changes -- because
callers ask `cai` rather than reading a file. That is what makes the migration a
no-op rather than a rewrite.
"""
import datetime
import json
import os

SOPIA = os.path.expanduser("~/dev2/Cascade/SOPIA")


def _parse(ts):
    if not ts:
        return None
    return datetime.datetime.strptime(ts.replace("Z", ""), "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=datetime.timezone.utc)


def newest_snapshot(package):
    """The most recent frozen snapshot `cai sync` took for this package, if any.

    **Snapshots sit between the source and the embedded fallback.** They are what
    a single download carries: dated, immutable, and stating the instant they
    were taken -- so when the source is gone they answer with something whose age
    is knowable, rather than with a fallback nobody can date.
    """
    if not package:
        return None
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), package,
                     "sync-frozen")
    if not os.path.isdir(d):
        return None
    snaps = sorted(f for f in os.listdir(d)
                   if f.endswith(".json") and not f.endswith(".frozen"))
    return os.path.join(d, snaps[-1]) if snaps else None


def resolve(env_var, source_candidates, fallback, package=None):
    """(path, is_source). A missing everything returns (None, False).

    Order: override, [hosted], the source, the newest snapshot, bootstrap.

    **The hosted store is not built and its slot is already correct** -- it goes
    ahead of `source_candidates`, everything below stays as written, and no
    caller changes, because callers ask `cai` rather than reading a file.
    """
    env = os.environ.get(env_var)
    if env and os.path.exists(env):
        return env, True
    for c in source_candidates:
        if os.path.exists(c):
            return c, True
    snap = newest_snapshot(package)
    if snap:
        return snap, False
    if fallback and os.path.exists(fallback):
        return fallback, False
    return None, False


def remote_payload(remote_path):
    """SOPIA as published, above the local working copy. Returns text or None.

    **A definition is a shared decision, so the pushed state is the agreed one.**
    A working tree may hold an experiment nobody else has seen, and reading
    policy out of it would let one machine's half-finished edit answer as though
    it were settled.

    **A local edit therefore does not take effect until it is pushed.** That is
    intended, and it will surprise somebody, so it is said here rather than
    discovered.
    """
    if not remote_path or os.environ.get("CAI_NO_REMOTE"):
        return None, {"skipped": True}
    try:
        from cai import remote as remotemod
        return remotemod.read(remote_path)
    except Exception as e:                                       # noqa: BLE001
        return None, {"error": str(e)}


def load(env_var, source_candidates, fallback, empty=None, package=None,
         remote_path=None):
    """Fetch, preferring the source. **A read failure is never permission.**

    Returns the parsed document with `_path` and `_is_source` attached, or the
    `empty` value with a note explaining why -- never an exception a caller might
    forget to handle into an affirmative.
    """
    # The remote sits above the local working copy, per the owner's priority.
    # It is consulted only when nothing overrides it, and every failure falls
    # through rather than raising -- a gate that breaks when the network does is
    # a gate that gets turned off.
    if remote_path and not os.environ.get(env_var):
        text, meta = remote_payload(remote_path)
        if text:
            try:
                doc = json.loads(text)
                doc["_path"] = "SOPIA@remote:%s" % remote_path
                doc["_is_source"] = True
                doc["_remote"] = {k: v for k, v in (meta or {}).items() if k != "url"}
                return doc
            except ValueError:
                pass
    path, is_source = resolve(env_var, source_candidates, fallback, package=package)
    if not path:
        d = dict(empty or {})
        d.update({"_path": None, "_is_source": False,
                  "_note": "no source or fallback found"})
        return d
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        d = dict(empty or {})
        d.update({"_path": path, "_is_source": False,
                  "_note": "unreadable (%s)" % e})
        return d
    doc["_path"] = path
    doc["_is_source"] = is_source
    return doc


def stale(doc):
    """Whether this copy is the source, and if not, how far behind."""
    if doc.get("_is_source"):
        return {"stale": False, "path": doc.get("_path"),
                "note": "reading the source directly; SOPIA defines, cai fetches"}
    synced = _parse(doc.get("synced"))
    if synced is None:
        return {"stale": True, "path": doc.get("_path"), "age_days": None,
                "note": "embedded fallback, never synced against a source"}
    age = (datetime.datetime.now(datetime.timezone.utc) - synced).days
    return {"stale": True, "path": doc.get("_path"), "age_days": age,
            "note": "embedded fallback, %d days behind its last sync" % age}
