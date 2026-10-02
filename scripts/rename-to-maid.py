#!/usr/bin/env python3
"""Rename maic to maid across the tracked tree: file contents and paths, keeping case (maic, Maic, MAIC).

    scripts/rename-to-maid.py            rename in place, staging moves with git mv
    scripts/rename-to-maid.py --dry-run  print what would change
    scripts/rename-to-maid.py --check    list what is left (exit 1 if anything is)

Re-runnable: run it on any branch before merging that branch into a renamed main; a tree with nothing
left to rename is left as it is. Not renamed:
  * records of the past: docs/releases/, docs/transcripts/ (git history is never touched);
  * OpenAI's pinned spec and its licence (protocol/openai/openapi.json, LICENSE) and the submodules under vendor/;
  * what ComfyUI and saved workflows know by name: the custom node folders comfyui-maic-llamacpp and
    comfyui-maic-templates and the node types MaicLlmServer and MaicLlmChat;
  * the GitHub URL (the repository's own rename is Micaiah's);
  * this script and scripts/migrate-to-maid.sh, which name the old paths on purpose.
"""
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
SKIP_PREFIXES = ("docs/releases/", "docs/transcripts/")
SKIP_FILES = {"scripts/rename-to-maid.py", "scripts/migrate-to-maid.sh", "protocol/openai/openapi.json", "protocol/openai/LICENSE"}
KEEP = ["comfyui-maic-llamacpp", "comfyui-maic-templates", "MaicLlmServer", "MaicLlmChat", "github.com/Cragady/MAIC"]
CASES = (("maic", "maid"), ("Maic", "Maid"), ("MAIC", "MAID"))
PATTERN = re.compile("|".join(re.escape(k) for k in KEEP) + "|maic|Maic|MAIC")
README_LINE = "*Mica's AI Decisions*, or *Micaiah's Agentic Interface Delta*."


def rename(text):
    return PATTERN.sub(lambda m: m.group(0) if m.group(0) in KEEP else dict(CASES)[m.group(0)], text)


def tracked():
    out = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    for entry in filter(None, out.split("\0")):
        mode, _, _, path = entry.split(None, 3)
        if mode == "160000" or path in SKIP_FILES or path.startswith(SKIP_PREFIXES):
            continue
        yield mode, path


def text_of(path):
    with open(os.path.join(ROOT, path), "rb") as f:
        data = f.read()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def readme(dry_run):
    path = os.path.join(ROOT, "README.md")
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    if any(README_LINE in line for line in lines):
        return False
    at = next((i for i, line in enumerate(lines) if line.startswith("# ")), None)
    if at is None:
        return False
    if not dry_run:
        lines[at + 1:at + 1] = ["", README_LINE]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
    return True


def main():
    dry_run, check = "--dry-run" in sys.argv, "--check" in sys.argv
    edited, moved, left = [], [], []
    for mode, path in tracked():
        full = os.path.join(ROOT, path)
        if mode == "120000":
            target = os.readlink(full)
            if rename(target) != target:
                edited.append(path)
                if not (dry_run or check):
                    os.remove(full)
                    os.symlink(rename(target), full)
        else:
            text = text_of(path)
            if text is not None and rename(text) != text:
                edited.append(path)
                if check:
                    left += ["%s:%d" % (path, n) for n, line in enumerate(text.split("\n"), 1) if rename(line) != line]
                elif not dry_run:
                    with open(full, "w", encoding="utf-8") as f:
                        f.write(rename(text))
        if rename(path) != path:
            moved.append((path, rename(path)))
            if check:
                left.append(path)
    if check:
        print("\n".join(left) if left else "nothing left to rename")
        return 1 if left else 0
    for old, new in moved:
        print("move %s -> %s" % (old, new))
        if not dry_run:
            os.makedirs(os.path.dirname(os.path.join(ROOT, new)) or ROOT, exist_ok=True)
            subprocess.run(["git", "mv", "-k", old, new], cwd=ROOT, check=True)
    if not dry_run:
        for old, _ in moved:
            d = os.path.dirname(os.path.join(ROOT, old))
            while d != ROOT and os.path.isdir(d) and not os.listdir(d):
                os.rmdir(d)
                d = os.path.dirname(d)
    if readme(dry_run):
        edited.append("README.md (the name's two readings)")
    print("%s %d files, %s %d paths" % ("would edit" if dry_run else "edited", len(edited), "would move" if dry_run else "moved", len(moved)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
