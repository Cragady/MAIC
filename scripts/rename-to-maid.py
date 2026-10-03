#!/usr/bin/env python3
"""Rename maic to maid across the tracked tree: file contents and paths, keeping case (maic, Maic, MAIC).

    scripts/rename-to-maid.py            rename in place, staging moves with git mv
    scripts/rename-to-maid.py --dry-run  print what would change
    scripts/rename-to-maid.py --check    list what is left, matching maic in any case (exit 1 if anything is)

Re-runnable: run it on any branch before merging that branch into a renamed main; a tree with nothing
left to rename is left as it is. Everything that says maic goes, inside identifiers and paths too: the
binaries, maic.nvim, the maic. protocol namespace, MAIC_* variables, ~/.config/maic, <state>/artifacts and
reviews, .maic/, MAIC.md, .maic-artifact.json, .maic-notify-protocol.json, X-Maic-Artifact-Token, the
maic-artifact-token meta, the maic_artifacts cookie, the MCP server maic (mcp__maic__), the systemd units.
The tunnel's HKDF salt (maic-tunnel-v1) is renamed with the rest, so its test vectors are swapped for the
ones derived under maid-tunnel-v1 (VECTORS). Not renamed:
  * records of the past: docs/releases/, docs/transcripts/ (git history is never touched);
  * OpenAI's pinned spec and its licence (protocol/openai/openapi.json, LICENSE), the vendored Vue build and
    its licence (vendor/vue, checked by hash), and the submodules under vendor/;
  * what ComfyUI and saved workflows know by name: the custom node folders comfyui-maic-llamacpp and
    comfyui-maic-templates and the node types MaicLlmServer and MaicLlmChat;
  * the GitHub URL (the repository's own rename is Micaiah's), and README's line on how MAIC became MAID;
  * words that merely contain the letters (NOT_THE_NAME), which --check lists;
  * this script and scripts/migrate-to-maid.sh, which name the old paths on purpose.
"""
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
SKIP_PREFIXES = ("docs/releases/", "docs/transcripts/")
SKIP_FILES = {"scripts/rename-to-maid.py", "scripts/migrate-to-maid.sh", "protocol/openai/openapi.json", "protocol/openai/LICENSE",
              "vendor/vue/vue.global.prod.js", "vendor/vue/LICENSE"}
KEEP = ["comfyui-maic-llamacpp", "comfyui-maic-templates", "MaicLlmServer", "MaicLlmChat", "github.com/Cragady/MAIC",
        "made MAIC transition to MAID"]
NOT_THE_NAME = ["aramaic", "jamaica", "ptolemaic", "romaic"]
CASES = (("maic", "maid"), ("Maic", "Maid"), ("MAIC", "MAID"))
NOT_THE_NAME_RE = "(?i:" + "|".join(NOT_THE_NAME) + ")"
PATTERN = re.compile("|".join(re.escape(k) for k in KEEP) + "|" + NOT_THE_NAME_RE + "|maic|Maic|MAIC")
LOOSE = re.compile("|".join(re.escape(k) for k in KEEP) + "|" + NOT_THE_NAME_RE + "|(?i:maic)")
# server/tests/relay_test.cpp and tunnel_js_test.mjs: the session keys and frames under maic-tunnel-v1, then under
# maid-tunnel-v1, from an independent derivation (cryptography's X25519 and HKDF, HChaCha20 from the XChaCha draft).
VECTORS = {
    "4e9588819afb1555ae99e31bfb04279d0bff0bea31178470fc72ef93f0805c17": "e41686e59b8e259961fcc23c36258b02d8cae64c20ba4a4a1b51e4fef4b515cb",
    "4bb80b6df72d6a086ed7769fee3105cae2e39298f563bb7c44b52211edca1eae": "dc8fc06e7c9de424952ff10629baf6c47bf3cc48232390f2ccfce1bcb0edfd01",
    "89af5f3f097bb2e3dfb40f1a891728bb9bcb3f41e849cff2727a9d7398324b3c1da03816178d50b37403149bd5173e66ba556b1dde27654226d4cd633e2d5d86f229e71358d2cefe":
        "7f9bfdaeb7a8ed68c0417a6081d73dd2aaf4a73465436aef509690504bc4b7f9e98d2f2f9a9b98ebedb613f893dbb821bf1c6ede8c098d3d7e1b447747178a1616f4200b03d9cedf",
    "1239476723a0194ad68846b9cfc3fced83e59588f2": "331f10cd8e3884fd28071c028270b7a45ebf9ab53d",
}
README_LINES = ["*Mica's AI Decisions*, or *Micaiah's Agentic Interface Delta*.",
                "Why maid? Blame trans-fairy: once it arrived, it earned its name three times over, because it made MAIC transition to MAID."]


def kept(word):
    return word in KEEP or word.lower() in NOT_THE_NAME


def rename(text):
    text = PATTERN.sub(lambda m: m.group(0) if kept(m.group(0)) else dict(CASES)[m.group(0)], text)
    for old, new in VECTORS.items():
        text = text.replace(old, new)
    return text


def left_in(text):
    """What --check reports: maic in any case that the rename leaves (an odd case such as mAiC), and old vectors."""
    return rename(text) != text or any(not kept(m.group(0)) for m in LOOSE.finditer(text))


def inside_words(text):
    """maic with a lowercase letter against it, and the NOT_THE_NAME words: where a false positive would be."""
    for m in LOOSE.finditer(text):
        a, b = text[m.start() - 1:m.start()], text[m.end():m.end() + 1]
        if m.group(0).lower() in NOT_THE_NAME or (m.group(0) not in KEEP and (a.islower() or b.islower())):
            yield re.search(r"[A-Za-z_]*$", text[:m.start()]).group(0) + m.group(0) + re.match(r"[A-Za-z_]*", text[m.end():]).group(0)


def tracked():
    out = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    for entry in filter(None, out.split("\0")):
        mode, _, _, path = entry.split(None, 3)
        if mode == "160000" or path in SKIP_FILES or path.startswith(SKIP_PREFIXES):
            continue
        yield mode, path


def read(path):
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read()


def text_of(data):
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
    if any(README_LINES[0] in line for line in lines):
        return False
    at = next((i for i, line in enumerate(lines) if line.startswith("# ")), None)
    if at is None:
        return False
    if not dry_run:
        lines[at + 1:at + 1] = ["", README_LINES[0], "", README_LINES[1]]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
    return True


def main():
    dry_run, check = "--dry-run" in sys.argv, "--check" in sys.argv
    edited, moved, left, words = [], [], [], {}
    for mode, path in tracked():
        full = os.path.join(ROOT, path)
        if mode == "120000":
            target = os.readlink(full)
            if left_in(target):
                edited.append(path)
                left.append("%s -> %s" % (path, target))
                if not (dry_run or check):
                    os.remove(full)
                    os.symlink(rename(target), full)
        else:
            data = read(path)
            text = text_of(data)
            if text is None:
                if re.search(rb"(?i)maic", data):
                    left.append("%s (not UTF-8 text; rename by hand)" % path)
            else:
                for w in inside_words(text):
                    words[w] = words.get(w, 0) + 1
                if left_in(text):
                    edited.append(path)
                    left += ["%s:%d" % (path, n) for n, line in enumerate(text.split("\n"), 1) if left_in(line)]
                    if not (dry_run or check):
                        with open(full, "w", encoding="utf-8") as f:
                            f.write(rename(text))
        if left_in(path):
            left.append(path)
        if rename(path) != path:
            moved.append((path, rename(path)))
    if check:
        if words:
            print("maic inside a longer word (the name, unless it is in NOT_THE_NAME):")
            print("\n".join("  %s (%d)" % (w, n) for w, n in sorted(words.items())))
        if readme(True):
            left.append("README.md: the name's two readings")
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
