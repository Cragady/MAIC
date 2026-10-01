"""Named sync jobs: what cai wants, from where, into what.

**The purpose is disaster and isolation.** `cai` should work from a single
download, with no SOPIA reachable and no network -- which means it must carry a
copy of what SOPIA defines. **That copy is a snapshot, not a mirror**, and the
difference matters: a mirror is whatever the source says now, while a snapshot is
what it said at a stated instant.

**Frozen AND synced, which are usually opposites.** A synced copy tracks a source
and is updated; a frozen file is immutable and carries exactly one commit. **Each
sync resolves that by writing a NEW dated file rather than editing one** -- so
every snapshot keeps its freeze, the set tracks the source, and the old ones stay
valid. Append rather than mutate, the same shape as re-freezing a file under a
new name.

**The jobs are data so that sync is reusable.** Adding a thing to carry is an
entry here, not a code change.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SPECS = os.path.join(HERE, "specs.json")


def load(path=None):
    with open(path or SPECS, encoding="utf-8") as fh:
        return json.load(fh)


def jobs(doc=None):
    return (doc or load()).get("jobs", {})
