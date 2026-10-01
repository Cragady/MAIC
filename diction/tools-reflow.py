#!/usr/bin/env python3
"""Reflow diction's markdown to one sentence per line. **Under the hood; no user runs this.**

**There is no implementation in this file, deliberately.** It carries none and
maintains none. Owner, 2026-09-03, asking for a way to satisfy both worlds:

    diction must not require `cai`     inverting that dependency is exactly what
                                       the diction/cai split exists to prevent
    two copies of one operation        is the drift this suite spends its life
                                       closing, and this file WAS the second copy

**A dated snapshot is neither.** One canonical implementation lives in `cai`; a
frozen copy is carried here, stating what it copied and when. So there is a
single source of truth, no dependency, and **staleness is measured rather than
guessed** -- a hand-maintained fallback can only be checked by running it, while
a snapshot's age is a fact printed on its face.

THE LADDER, same shape as `cai.source` uses for data
----------------------------------------------------
    1. `cai reflow --mode sentence`   canonical and live, if cai is installed
    2. the newest snapshot            frozen, dated, carried in sync-frozen/
    3. refuse                         and say which rung failed

**Refusing is the third rung on purpose.** The previous version's third rung was
its own divergent copy, which is the thing being removed; a fallback that quietly
does something slightly different is worse than one that stops.

    python3 tools-reflow.py FILE...            reflow
    python3 tools-reflow.py FILE... --check    do rungs 1 and 2 AGREE?
    cai sync reflow-lines                      take a fresh snapshot

**--check exists because a snapshot can drift from its source silently.** It runs
both rungs on a copy and compares. Searching for the guard tests the text; only
running the hazard tests the guard.
"""
import glob
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = os.path.join(HERE, "sync-frozen")


def newest_snapshot():
    """The most recent frozen copy, or None. Dated names sort chronologically."""
    found = sorted(glob.glob(os.path.join(FROZEN, "reflow-lines-*.py")))
    return found[-1] if found else None


def load_snapshot(path):
    spec = importlib.util.spec_from_file_location("reflow_lines_frozen", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def provenance(path):
    """What this snapshot says about itself, read off its header."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            if ":" in line:
                k, _, v = line[1:].partition(":")
                out[k.strip()] = v.strip()
    return out


def via_cai(paths):
    exe = shutil.which("cai")
    if not exe:
        return False
    try:
        r = subprocess.run([exe, "reflow", "--mode", "sentence"] + list(paths),
                           capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0


def via_snapshot(paths):
    snap = newest_snapshot()
    if not snap:
        return None
    mod = load_snapshot(snap)
    changed = 0
    for arg in paths:
        p = Path(arg)
        before = p.read_text(encoding="utf-8")
        after, rep = mod.apply(before, mode="sentence")
        if not rep["signature_held"]:
            print("  REFUSED %s: content signature changed" % p)
            continue
        if after != before:
            p.write_text(after, encoding="utf-8")
            changed += 1
            print(f"  reflowed {p}")
    return changed, snap


def check(paths):
    """Do the two rungs agree? Reports; never writes to the originals."""
    exe, snap = shutil.which("cai"), newest_snapshot()
    if not exe or not snap:
        print("cannot compare: %s" % ("cai not installed" if not exe else "no snapshot"))
        return 2
    mod = load_snapshot(snap)
    bad = 0
    for arg in paths:
        src = Path(arg).read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as d:
            b = Path(d) / "b.md"
            b.write_text(src, encoding="utf-8")
            subprocess.run([exe, "reflow", "--mode", "sentence", str(b)],
                           capture_output=True, text=True, timeout=120)
            mine, _ = mod.apply(src, mode="sentence")
            same = mine.rstrip("\n") == b.read_text(encoding="utf-8").rstrip("\n")
        print("%s  %s" % (arg, "AGREE" if same else "DIVERGED"))
        bad += 0 if same else 1
    if bad:
        print("the snapshot has drifted from `cai`. Take a fresh one: cai sync reflow-lines")
    return 1 if bad else 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--check" in sys.argv[1:]:
        sys.exit(check(args))
    if via_cai(args):
        print("reflowed via `cai reflow --mode sentence` (canonical)")
        sys.exit(0)
    got = via_snapshot(args)
    if got is None:
        print("no implementation available: `cai` is not installed and sync-frozen/ holds no "
              "snapshot. Run `cai sync reflow-lines` where cai is present, or install cai.",
              file=sys.stderr)
        sys.exit(1)
    changed, snap = got
    pv = provenance(snap)
    print("%d file(s) changed  (frozen snapshot %s, synced %s)"
          % (changed, os.path.basename(snap), pv.get("synced", "?")))
