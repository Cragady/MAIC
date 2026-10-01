"""Short-lived state that does not belong in a tracked repository.

**A grant instance is ephemeral. Its shape is not.** *This session, this window,
these three hours* is a fact about right now; committing it means every expired
grant accumulates in git history forever, and the repository records permissions
nobody holds any more. **The shape of a grant is documentation and belongs in
SOPIA; the instances belong here.**

**Items expire after a TTL, default 24 hours, and expiry is enforced on read.**
A stale item is not served, which for a grant means it fails closed -- an
out-of-date file can never answer *yes*. Deletion is a separate sweep, so a read
does not mutate the store and two reads never disagree about it.

**This is scaffolding until the data is hosted.** When it is, the hosted store
becomes the first candidate in `cai.source` and this stops being consulted --
without any caller changing, because callers ask `cai` rather than reading a
file.
"""
import datetime
import json
import os
import time

ROOT = os.environ.get("CAI_TMP") or os.path.expanduser("~/.local/state/cai/tmp")
DEFAULT_TTL_HOURS = 24


def path_for(name):
    return os.path.join(ROOT, name)


def age_hours(path):
    try:
        return (time.time() - os.path.getmtime(path)) / 3600.0
    except OSError:
        return None


def is_stale(path, ttl_hours=DEFAULT_TTL_HOURS):
    a = age_hours(path)
    return a is None or a > ttl_hours


def load(name, ttl_hours=DEFAULT_TTL_HOURS, empty=None):
    """Read an item, or the empty value if it is missing or past its TTL.

    **Never raises into a caller.** A tmp read that threw would be a read a
    caller might mishandle, and for a grant that mishandling is permission.
    """
    p = path_for(name)
    d = dict(empty or {})
    if not os.path.exists(p):
        d.update({"_path": p, "_tmp": True, "_note": "no local item at %s" % p})
        return d
    a = age_hours(p)
    if a is not None and a > ttl_hours:
        d.update({"_path": p, "_tmp": True, "_stale": True, "_age_hours": round(a, 1),
                  "_note": ("local item is %.1f hours old, past the %d-hour TTL, so it is "
                            "not served. Deletion is a separate sweep." % (a, ttl_hours))})
        return d
    try:
        with open(p, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        d.update({"_path": p, "_tmp": True, "_note": "unreadable (%s)" % e})
        return d
    doc.update({"_path": p, "_tmp": True, "_age_hours": round(a, 1) if a else 0.0})
    return doc


def save(name, doc):
    p = path_for(name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
    return p


def sweep(ttl_hours=DEFAULT_TTL_HOURS, dry_run=False):
    """Delete what has aged out. **Separate from reading, on purpose.**"""
    removed, kept = [], []
    if not os.path.isdir(ROOT):
        return {"root": ROOT, "removed": [], "kept": [], "note": "no tmp directory"}
    for name in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, name)
        if not os.path.isfile(p):
            continue
        if is_stale(p, ttl_hours):
            if not dry_run:
                try:
                    os.remove(p)
                except OSError:
                    continue
            removed.append({"name": name, "age_hours": round(age_hours(p) or 0, 1)})
        else:
            kept.append({"name": name, "age_hours": round(age_hours(p) or 0, 1)})
    return {"root": ROOT, "removed": removed, "kept": kept, "dry_run": dry_run}
