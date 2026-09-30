#!/usr/bin/env python3
"""Edit the tunable fields of a ComfyUI workflow JSON without touching its wiring.

A workflow is nodes, links and widget values. Links are structure; widget values (prompts, seeds, steps,
file prefixes, caption text, colours) are what you tune. This script reads a workflow by exact path, shows
every node and every widget slot with a name, and changes values on request. Links, node ids, positions
and everything else are left exactly as they were. Writing keeps a `<file>.bak` of the previous contents.

Interactive (a person at a terminal):
    workflow_edit.py edit PATH [--only TYPE ...] [--nodes ID ...] [--fields NAME ...] [--out PATH]
        For each node and field: [k]eep, [r]eplace, [a]ppend, [p]repend, [s]kip node, [w]rite and stop, [q]uit.
        Text fields take several lines; a line holding only "." ends the input.

Non-interactive (an agent through run_shell, or a script):
    workflow_edit.py inspect PATH [--json]                 every node, its fields and current values
    workflow_edit.py set PATH NODE.FIELD VALUE [...]       replace a field (VALUE "-" reads stdin)
    workflow_edit.py append PATH NODE.FIELD VALUE [...]    add to the end of a text field
    workflow_edit.py prepend PATH NODE.FIELD VALUE [...]   add to the start of a text field
    workflow_edit.py replace-all PATH OLD NEW [--fields NAME ...] [--only TYPE ...]
                                                           substring replacement across text fields
    workflow_edit.py apply PATH EDITS.json                 a list of {"node": 10, "field": "text", "op": "set"|"append"|"prepend", "value": "..."}
    every writing command takes --dry-run (show the diff, write nothing) and --out PATH (write elsewhere)

NODE is the node id (`10`) or, when unique, its title (`"Panel 1 prompt"`). FIELD is the slot name shown by
inspect (`text`, `seed`, `steps`, `filename_prefix`, ...) or its index (`w0`). Numbers stay numbers: setting
`steps` to "30" stores 30. The exit code is 0 when the file was written (or the dry run fits), 1 on a bad
argument, 2 when a node or field cannot be found; every message says which.
"""

import argparse
import difflib
import json
import os
import shutil
import sys

# Widget slot names per node type, in the order ComfyUI stores widgets_values. Unknown types show w0, w1, ...
FIELDS = {
    "CLIPTextEncode": ["text"],
    "KSampler": ["seed", "control_after_generate", "steps", "cfg", "sampler_name", "scheduler", "denoise"],
    "KSamplerAdvanced": ["add_noise", "noise_seed", "control_after_generate", "steps", "cfg", "sampler_name", "scheduler", "start_at_step", "end_at_step", "return_with_leftover_noise"],
    "EmptyLatentImage": ["width", "height", "batch_size"],
    "CheckpointLoaderSimple": ["ckpt_name"],
    "LoraLoader": ["lora_name", "strength_model", "strength_clip"],
    "VAELoader": ["vae_name"],
    "SaveImage": ["filename_prefix"],
    "PreviewImage": [],
    "LoadImage": ["image", "upload"],
    "TextOverlay": ["text", "font_size", "color", "vertical_align", "horizontal_align", "shadow"],
    "ImageStitch": ["direction", "match_image_size", "spacing_width", "spacing_color"],
    "Note": ["text"],
    "MarkdownNote": ["text"],
    "PrimitiveNode": ["value"],
    "MaicLlmServer": ["base_url", "model", "timeout"],
    "MaicLlmChat": ["system", "prompt", "think", "format", "temperature", "top_k", "top_p", "min_p", "seed", "control_after_generate", "extra_json", "keep_context", "session_id", "reset"],
}
TEXT_FIELDS = {"text", "prompt", "system", "filename_prefix", "extra_json", "value"}


def load(path):
    if not os.path.isfile(path):
        sys.exit(f"no such file: {path}")
    with open(path, encoding="utf-8") as f:
        try:
            wf = json.load(f)
        except json.JSONDecodeError as e:
            sys.exit(f"{path} is not valid JSON: {e}")
    if not isinstance(wf, dict) or "nodes" not in wf:
        sys.exit(f"{path} does not look like a ComfyUI workflow (no 'nodes')")
    return wf


def field_names(node):
    values = node.get("widgets_values") or []
    if isinstance(values, dict):  # a few nodes store a dict; treat keys as names
        return list(values.keys())
    names = list(FIELDS.get(node["type"], []))
    while len(names) < len(values):
        names.append(f"w{len(names)}")
    return names[: len(values)]


def get_value(node, index):
    values = node.get("widgets_values") or []
    if isinstance(values, dict):
        return values[list(values.keys())[index]]
    return values[index]


def set_value(node, index, value):
    values = node["widgets_values"]
    if isinstance(values, dict):
        values[list(values.keys())[index]] = value
    else:
        values[index] = value


def coerce(old, text):
    """Keep the field's type: a number stays a number, a bool a bool, text is text."""
    if isinstance(old, bool):
        if text.strip().lower() in ("true", "yes", "on", "1"):
            return True
        if text.strip().lower() in ("false", "no", "off", "0"):
            return False
        raise ValueError(f"expected true or false, got {text!r}")
    if isinstance(old, int) and not isinstance(old, bool):
        try:
            return int(text.strip())
        except ValueError:
            return float(text.strip())  # a float is fine where an int was; ComfyUI accepts both for cfg-like slots
    if isinstance(old, float):
        return float(text.strip())
    return text


def is_text(node, index, name):
    return isinstance(get_value(node, index), str) and (name in TEXT_FIELDS or node["type"] in ("CLIPTextEncode", "Note", "MarkdownNote"))


def find_node(wf, key):
    """A node by id, or by unique title."""
    by_id = {str(n["id"]): n for n in wf["nodes"]}
    if key in by_id:
        return by_id[key]
    matches = [n for n in wf["nodes"] if n.get("title") == key]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        sys.exit(f"node title {key!r} is not unique (ids {[n['id'] for n in matches]}); use the id")
    sys.exit(f"no node with id or title {key!r}; `inspect` lists them")


def find_field(node, key):
    names = field_names(node)
    if key in names:
        return names.index(key), key
    if key.startswith("w") and key[1:].isdigit() and int(key[1:]) < len(names):
        return int(key[1:]), names[int(key[1:])]
    sys.exit(f"node {node['id']} ({node['type']}) has no field {key!r}; it has {names}")


def describe(wf, as_json=False):
    rows = []
    for n in sorted(wf["nodes"], key=lambda n: (n["pos"][1], n["pos"][0]) if n.get("pos") else (0, 0)):
        fields = []
        for i, name in enumerate(field_names(n)):
            fields.append({"index": i, "name": name, "value": get_value(n, i), "text": is_text(n, i, name)})
        rows.append({"id": n["id"], "type": n["type"], "title": n.get("title", ""), "mode": n.get("mode", 0), "fields": fields})
    if as_json:
        print(json.dumps({"file_nodes": len(wf["nodes"]), "links": len(wf.get("links", [])), "nodes": rows}, indent=2, ensure_ascii=False))
        return
    print(f"{len(wf['nodes'])} nodes, {len(wf.get('links', []))} links (links are never changed by this tool)")
    for r in rows:
        muted = "  [muted]" if r["mode"] == 2 else "  [bypassed]" if r["mode"] == 4 else ""
        print(f"\n#{r['id']} {r['type']}" + (f'  "{r["title"]}"' if r["title"] else "") + muted)
        for f in r["fields"]:
            v = f["value"]
            shown = json.dumps(v, ensure_ascii=False)
            if len(shown) > 110:
                shown = shown[:107] + "..."
            print(f"    {f['name']:<24} = {shown}")


def diff_text(before, after, path):
    a = json.dumps(before, indent=2, ensure_ascii=False).splitlines(keepends=True)
    b = json.dumps(after, indent=2, ensure_ascii=False).splitlines(keepends=True)
    return "".join(difflib.unified_diff(a, b, fromfile=path, tofile=path + " (edited)", n=2))


def write(wf, original, path, out, dry_run):
    if dry_run:
        d = diff_text(original, wf, path)
        print(d if d else "no changes")
        return
    target = out or path
    if os.path.exists(target):
        shutil.copy2(target, target + ".bak")
    with open(target, "w", encoding="utf-8") as f:
        json.dump(wf, f, indent=2, ensure_ascii=False)
        f.write("\n")
    changed = sum(1 for x, y in zip(json.dumps(original, sort_keys=True), json.dumps(wf, sort_keys=True)) if x != y)
    print(f"wrote {target}" + (f" (previous contents in {target}.bak)" if os.path.exists(target + ".bak") else "") + ("" if changed else "; nothing had changed"))


def apply_edit(node, index, name, op, value):
    old = get_value(node, index)
    if op == "set":
        new = coerce(old, value) if not isinstance(old, str) else value
    elif op in ("append", "prepend"):
        if not isinstance(old, str):
            raise ValueError(f"{op} only applies to text fields; {name} on node {node['id']} is {type(old).__name__}")
        new = old + value if op == "append" else value + old
    else:
        raise ValueError(f"unknown op {op!r} (set, append, prepend)")
    set_value(node, index, new)
    return old, new


def read_value(arg):
    return sys.stdin.read() if arg == "-" else arg


def cmd_inspect(args):
    describe(load(args.path), args.json)


def cmd_op(args, op):
    wf = load(args.path)
    original = json.loads(json.dumps(wf))
    if len(args.pairs) % 2:
        sys.exit("give NODE.FIELD VALUE pairs")
    for key, value in zip(args.pairs[0::2], args.pairs[1::2]):
        if "." not in key:
            sys.exit(f"{key!r}: use NODE.FIELD, for example 10.text or 'Panel 1 prompt'.text")
        node_key, field_key = key.rsplit(".", 1)
        node = find_node(wf, node_key)
        index, name = find_field(node, field_key)
        try:
            old, new = apply_edit(node, index, name, op, read_value(value))
        except ValueError as e:
            sys.exit(str(e))
        print(f"#{node['id']} {name}: {json.dumps(old, ensure_ascii=False)[:60]} -> {json.dumps(new, ensure_ascii=False)[:60]}")
    write(wf, original, args.path, args.out, args.dry_run)


def cmd_replace_all(args):
    wf = load(args.path)
    original = json.loads(json.dumps(wf))
    hits = 0
    for n in wf["nodes"]:
        if args.only and n["type"] not in args.only:
            continue
        for i, name in enumerate(field_names(n)):
            if args.fields and name not in args.fields:
                continue
            v = get_value(n, i)
            if isinstance(v, str) and args.old in v:
                set_value(n, i, v.replace(args.old, args.new))
                hits += v.count(args.old)
                print(f"#{n['id']} {name}: {v.count(args.old)} occurrence(s)")
    print(f"{hits} replacement(s)")
    write(wf, original, args.path, args.out, args.dry_run)


def cmd_apply(args):
    wf = load(args.path)
    original = json.loads(json.dumps(wf))
    with open(args.edits, encoding="utf-8") as f:
        edits = json.load(f)
    if not isinstance(edits, list):
        sys.exit("EDITS.json must hold a list of {node, field, op, value}")
    for e in edits:
        node = find_node(wf, str(e["node"]))
        index, name = find_field(node, str(e["field"]))
        try:
            old, new = apply_edit(node, index, name, e.get("op", "set"), str(e["value"]) if not isinstance(e["value"], str) else e["value"])
        except ValueError as err:
            sys.exit(str(err))
        print(f"#{node['id']} {name}: {e.get('op', 'set')}")
    write(wf, original, args.path, args.out, args.dry_run)


def read_multiline(prompt):
    print(prompt + ' (end with a line holding only ".")')
    lines = []
    while True:
        try:
            line = input()
        except EOFError:
            break
        if line == ".":
            break
        lines.append(line)
    return "\n".join(lines)


def cmd_edit(args):
    if not sys.stdin.isatty():
        sys.exit("edit is interactive; without a terminal use set, append, prepend, replace-all or apply")
    wf = load(args.path)
    original = json.loads(json.dumps(wf))
    nodes = sorted(wf["nodes"], key=lambda n: (n["pos"][1], n["pos"][0]) if n.get("pos") else (0, 0))
    for n in nodes:
        if args.only and n["type"] not in args.only:
            continue
        if args.nodes and str(n["id"]) not in args.nodes and n.get("title") not in args.nodes:
            continue
        names = field_names(n)
        if args.fields:
            names_shown = [x for x in names if x in args.fields]
            if not names_shown:
                continue
        else:
            names_shown = names
        if not names_shown:
            continue
        print(f"\n== #{n['id']} {n['type']}" + (f'  "{n.get("title")}"' if n.get("title") else ""))
        skip_node = False
        for name in names_shown:
            if skip_node:
                break
            i = names.index(name)
            v = get_value(n, i)
            shown = json.dumps(v, ensure_ascii=False)
            print(f"  {name} = {shown if len(shown) <= 200 else shown[:197] + '...'}")
            text_field = isinstance(v, str)
            choices = "[k]eep [r]eplace" + (" [a]ppend [p]repend" if text_field else "") + " [s]kip node [w]rite and stop [q]uit without saving"
            while True:
                try:
                    c = input(f"  {choices}: ").strip().lower()[:1]
                except EOFError:
                    c = "q"
                if c in ("", "k"):
                    break
                if c == "s":
                    skip_node = True
                    break
                if c == "w":
                    write(wf, original, args.path, args.out, False)
                    return
                if c == "q":
                    print("nothing written")
                    return
                if c == "r" or (text_field and c in ("a", "p")):
                    op = {"r": "set", "a": "append", "p": "prepend"}[c]
                    value = read_multiline(f"  new text to {op}") if text_field and "\n" in v or (text_field and c in ("a", "p")) or (text_field and len(v) > 80) else input(f"  value to {op}: ")
                    try:
                        apply_edit(n, i, name, op, value)
                    except ValueError as e:
                        print(f"  {e}")
                        continue
                    print(f"  {name} = {json.dumps(get_value(n, i), ensure_ascii=False)[:200]}")
                    break
                print("  ?")
    write(wf, original, args.path, args.out, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, writes=True):
        p.add_argument("path")
        if writes:
            p.add_argument("--out", help="write here instead of in place")
            p.add_argument("--dry-run", action="store_true", help="show the diff, write nothing")

    p = sub.add_parser("inspect", help="list nodes, fields and values")
    common(p, writes=False)
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_inspect)
    for op in ("set", "append", "prepend"):
        p = sub.add_parser(op, help=f"{op} NODE.FIELD VALUE pairs")
        common(p)
        p.add_argument("pairs", nargs="+", metavar="NODE.FIELD VALUE")
        p.set_defaults(func=lambda a, op=op: cmd_op(a, op))
    p = sub.add_parser("replace-all", help="substring replacement across text fields")
    common(p)
    p.add_argument("old")
    p.add_argument("new")
    p.add_argument("--fields", nargs="*", default=[])
    p.add_argument("--only", nargs="*", default=[], help="node types")
    p.set_defaults(func=cmd_replace_all)
    p = sub.add_parser("apply", help="apply a JSON list of edits")
    common(p)
    p.add_argument("edits")
    p.set_defaults(func=cmd_apply)
    p = sub.add_parser("edit", help="interactive walk over every field")
    common(p)
    p.add_argument("--only", nargs="*", default=[], help="node types")
    p.add_argument("--nodes", nargs="*", default=[], help="node ids or titles")
    p.add_argument("--fields", nargs="*", default=[], help="field names")
    p.set_defaults(func=cmd_edit)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
