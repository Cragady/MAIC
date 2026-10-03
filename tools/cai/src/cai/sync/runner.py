"""Running a sync: read the source, write a dated frozen snapshot.

**Nothing is overwritten.** A snapshot names the instant it was taken, so a
second sync sits beside the first rather than replacing it -- which is what lets
each one keep the single-commit freeze while the set still tracks the source.
"""
import json
import os

#: This repository's root, for jobs whose source or destination is not SOPIA.
#: Inside MAID that is MAID's root, where diction lives (tools/cai/src/cai/sync -> ../../../../..).
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", ".."))

#: Provenance for a snapshot that cannot hold JSON keys. Same fields as the JSON
#: payload carries, in a form a reader and a parser both reach.
TEXT_HEADER = """# frozen: %(frozen)s
# synced: %(synced)s
# name: %(name)s
# syncedFrom: %(from)s
# syncedFromRoot: %(root)s
#
# Frozen snapshot, synced at the instant above. IMMUTABLE: a later sync writes a
# NEW dated file beside this one rather than editing it, so this keeps its
# single-commit freeze while the set tracks the source. Do not edit -- fixes go
# to the source and a fresh sync carries them.
"""

from cai import source
from cai.sync import spec

# `sync-frozen`, not `defaults-frozen`, and the distinction is real: a default
# never had a source, while these are dated snapshots OF one. Owner, 2026-09-02.
FROZEN_DIR = "sync-frozen"


def target_dir(into):
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        into, FROZEN_DIR)


def snapshots(into, prefix):
    d = target_dir(into)
    if not os.path.isdir(d):
        return []
    return sorted(f for f in os.listdir(d)
                  if f.startswith(prefix + "-") and f.endswith(".json")
                  and not f.endswith(".frozen"))


def newest(into, prefix):
    snaps = snapshots(into, prefix)
    return os.path.join(target_dir(into), snaps[-1]) if snaps else None


def run(name, doc=None, dry_run=False, stamp=None):
    """Take one snapshot. Reports; refuses rather than guessing."""
    jobs = spec.jobs(doc)
    job = jobs.get(name)
    if not job:
        return {"job": name, "ok": False,
                "note": "no sync job named %r. Known: %s" % (name, ", ".join(sorted(jobs)))}
    # **A job may name its ROOT.** Everything carried until 2026-09-03 came from
    # SOPIA, so the root was assumed. Code that lives here can be snapshotted the
    # same way -- the snapshot's value is provenance and a date, and neither
    # depends on which repository the source sat in.
    root = REPO if job.get("root") == "self" else source.SOPIA
    src = os.path.join(root, job["from"])
    if not os.path.exists(src):
        return {"job": name, "ok": False, "source": src,
                "note": ("the source is not reachable, so there is nothing to snapshot. "
                         "This is the case the snapshots exist FOR -- it is reported, not "
                         "worked around.")}
    kind = job.get("kind", "json")
    if kind == "json":
        try:
            with open(src, encoding="utf-8") as fh:
                payload = json.load(fh)
        except (OSError, ValueError) as e:
            return {"job": name, "ok": False, "source": src,
                    "note": "source unreadable (%s); nothing written" % e}
    else:
        try:
            with open(src, encoding="utf-8") as fh:
                text = fh.read()
        except OSError as e:
            return {"job": name, "ok": False, "source": src,
                    "note": "source unreadable (%s); nothing written" % e}

    from cai.timekeeping import cli as timecli
    at = stamp or timecli.stamp()
    ext = ".json" if kind == "json" else job.get("ext", ".txt")
    fname = "%s-%s%s" % (job["prefix"], at.replace(":", "").replace("-", ""), ext)
    # `into` names a cai package by default; a job carrying to somewhere else in
    # this repository says so with a path.
    dest_dir = (os.path.join(REPO, job["into"]) if "/" in job["into"]
                else target_dir(job["into"]))
    dest = os.path.join(dest_dir, fname)

    if kind == "json":
        payload = {k: v for k, v in payload.items() if not k.startswith("_")}
        payload["frozen"] = at
        payload["synced"] = at
        payload["name"] = fname
        payload["syncedFrom"] = job["from"]
        payload["note"] = ("Frozen snapshot, synced from SOPIA at the instant above. It is "
                           "immutable: a later sync writes a NEW dated file beside this one "
                           "rather than editing it, so this keeps its single-commit freeze "
                           "while the set tracks the source. Carried so cai works from a "
                           "single download with nothing else reachable.")
    else:
        # **A text snapshot carries its provenance in a header, not a sidecar.**
        # A sidecar can be orphaned by a rename -- that is how the freeze check's
        # bypass was found -- and a header travels with the bytes it describes.
        payload = None
        header = TEXT_HEADER % {"frozen": at, "synced": at, "name": fname,
                                "from": job["from"], "root": job.get("root", "SOPIA")}
        body = header + text

    if dry_run:
        return {"job": name, "ok": True, "dry_run": True, "source": src,
                "would_write": dest, "at": at}
    os.makedirs(dest_dir, exist_ok=True)
    with open(dest, "w", encoding="utf-8") as fh:
        if kind == "json":
            json.dump(payload, fh, ensure_ascii=False, indent=2)
            fh.write("\n")
        else:
            fh.write(body)
    return {"job": name, "ok": True, "source": src, "written": dest, "at": at,
            "snapshots": snapshots(job["into"], job["prefix"])}
