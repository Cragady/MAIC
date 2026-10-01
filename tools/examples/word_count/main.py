#!/usr/bin/env python3
"""word_count: a MAIC script tool in Python. See docs/tools.md for the format.

Install it by copying this directory to .maic/tools/word_count/ in a workspace (or to ~/.config/maic/tools/
for every workspace). MAIC validates the model's arguments against tool.json, judges the declared reads
through the harness, then runs `python3 main.py` inside the command sandbox with the arguments as JSON on
stdin. Whatever is printed is the result; a non-zero exit fails the call and stderr goes to the model.
"""
import json
import sys

args = json.load(sys.stdin)
path = args["path"]
try:
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
except OSError as e:
    sys.exit(f"can't read {path}: {e.strerror}")
lines = text.count("\n") + (1 if text and not text.endswith("\n") else 0)
print(f"{path}: {lines} lines, {len(text.split())} words, {len(text)} characters")
