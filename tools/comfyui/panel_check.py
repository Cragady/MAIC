#!/usr/bin/env python3
"""One manga panel on one screen, with the usual prompt mistakes flagged. Offline; reads only.

    panel_check.py WORKFLOW.json N          panel N: prompt, negative, sampler settings, captions, then the checks
    panel_check.py WORKFLOW.json all        every panel, one after the other
    options: --max-tags N (default 75), --caption-lines N (default 2), --tags FILE (the local Danbooru set)

The workflow is MAID's manga layout: a "Panel N prompt" CLIPTextEncode wired into a KSampler whose negative and
latent_image inputs lead to the negative prompt and the image size, and TextOverlay nodes titled "Panel N narration"
and "Panel N dialogue". The checks, each a FLAG line when it fires:

  * no character count tag (1girl, 2girls, 1boy, 2boys, no_humans, ...), or two of them for the same subject
  * a tag repeated inside the prompt
  * solo beside a count of more than one person
  * a tag that is in both the prompt and the negative (the negative cancels it)
  * more than --max-tags tags (CLIP weighs roughly 75 tokens; later tags count for less)
  * the YOUR TAGS HERE slot still in the prompt, or an empty prompt
  * a tag the local Danbooru set does not know (when `maid-danbooru-tags fetch` has run, or --tags names a file);
    an alias is noted with Danbooru's own name. The baseline, the leading tags every panel prompt shares (the
    quality and style block the storyboard never changes), is left out of this check
  * a caption longer than its overlay can show: TextOverlay's font_size is a percent of the image height, a glyph is
    about 0.55 of that wide, the text gets 90% of the width and --caption-lines lines (an estimate, for a 4B to act on)

Exit code 0 when nothing is flagged, 1 when something is, 2 for a bad argument or a workflow without that panel.
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import danbooru_tags  # noqa: E402

COUNT_RE = re.compile(r"^(?:(\d+)\+?(girl|boy|other)s?|multiple_(girl|boy|other)s|(no)_humans)$")
SAMPLER_FIELDS = ["seed", "control_after_generate", "steps", "cfg", "sampler_name", "scheduler", "denoise"]
GLYPH_WIDTH = 0.55  # of the font size, for a typical sans face
TEXT_WIDTH = 0.9  # the share of the image width an overlay line gets


def die(message):
    print(message, file=sys.stderr)
    sys.exit(2)


def load(path):
    if not os.path.isfile(path):
        die(f"no such workflow: {path}")
    with open(path, encoding="utf-8") as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as e:
            die(f"{path} is not valid JSON: {e}")


def title(node):
    return node.get("title") or ""


def split_tags(text):
    """Comma-separated tags, weights and escapes removed, in Danbooru spelling (lowercase, underscores)."""
    out = []
    for raw in text.split(","):
        t = raw.strip()
        m = re.match(r"^\(+(.*?)(?::[\d.]+)?\)+$", t)
        if m:
            t = m.group(1)
        t = t.replace("\\(", "(").replace("\\)", ")")
        t = danbooru_tags.norm(t)
        if t and t != "break":
            out.append(t)
    return out


def source_of(wf, node, input_name):
    """The node wired into `node`'s input of that name, or None."""
    link_id = next((i.get("link") for i in node.get("inputs", []) if i.get("name") == input_name), None)
    if link_id is None:
        return None
    for link in wf.get("links", []):
        if link[0] == link_id:
            return next((n for n in wf["nodes"] if n["id"] == link[1]), None)
    return None


def panel_parts(wf, n):
    """The nodes that make panel N: prompt, sampler, negative, latent, narration, dialogue (None where not wired)."""
    by_title = {title(node): node for node in wf["nodes"]}
    prompt = by_title.get(f"Panel {n} prompt")
    if prompt is None:
        return None
    sampler = None
    for link in wf.get("links", []):
        if link[1] == prompt["id"]:
            target = next((x for x in wf["nodes"] if x["id"] == link[3]), None)
            if target and target["type"].startswith("KSampler"):
                sampler = target
                break
    if sampler is None:
        sampler = by_title.get(f"Panel {n} sampler")
    negative = source_of(wf, sampler, "negative") if sampler else by_title.get(f"Panel {n} negative")
    latent = source_of(wf, sampler, "latent_image") if sampler else None
    return {"prompt": prompt, "sampler": sampler, "negative": negative, "latent": latent,
            "narration": by_title.get(f"Panel {n} narration"), "dialogue": by_title.get(f"Panel {n} dialogue")}


def baseline_of(wf):
    """Leading tags common to every panel prompt, in Danbooru spelling; empty with one panel (nothing to compare)."""
    prompts = [str(widget(node, 0, "") or "") for node in wf["nodes"] if re.match(r"^Panel \d+ prompt$", title(node))]
    if len(prompts) < 2:
        return []
    common = []
    for tags in zip(*(split_tags(p) for p in prompts)):
        if len(set(tags)) != 1:
            break
        common.append(tags[0])
    return common


def panel_numbers(wf):
    return sorted(int(m.group(1)) for node in wf["nodes"] for m in [re.match(r"^Panel (\d+) prompt$", title(node))] if m)


def widget(node, index, default=None):
    values = node.get("widgets_values") or []
    return values[index] if index < len(values) else default


def image_size(latent):
    if latent and isinstance(widget(latent, 0), (int, float)) and isinstance(widget(latent, 1), (int, float)):
        return int(widget(latent, 0)), int(widget(latent, 1))
    return None


def caption_capacity(font_size, size, lines):
    """How many characters an overlay can show: glyphs GLYPH_WIDTH of the font size wide, TEXT_WIDTH of the image."""
    width, height = size
    glyph = GLYPH_WIDTH * font_size / 100.0 * height
    per_line = int(TEXT_WIDTH * width / glyph) if glyph > 0 else 0
    return per_line, per_line * lines


def check_panel(tags, negative_tags, captions, size, known, max_tags, caption_lines, baseline=()):
    """The FLAG lines for one panel, and the ok lines worth showing. Returns (flags, notes)."""
    flags, notes = [], []
    if not tags:
        flags.append("the prompt is empty")
        return flags, notes
    if "your_tags_here" in tags:
        flags.append("the YOUR TAGS HERE slot is still in the prompt")
    counts = {}
    people = 0
    for t in tags:
        m = COUNT_RE.match(t)
        if not m:
            continue
        subject = m.group(2) or m.group(3) or "humans"
        counts.setdefault(subject, []).append(t)
        people += int(m.group(1)) if m.group(1) else 2 if m.group(3) else 0
    if not counts:
        flags.append("no character count tag (1girl, 2girls, 1boy, 2boys, no_humans, ...)")
    for subject, found in counts.items():
        if len(found) > 1:
            flags.append(f"count tag given twice for {subject}: {', '.join(found)}")
    if counts and not any(len(v) > 1 for v in counts.values()):
        notes.append("count tag: " + ", ".join(t for v in counts.values() for t in v))
    if "solo" in tags and (people > 1 or any(t.startswith("multiple_") for t in tags)):
        flags.append("solo beside " + ", ".join(t for v in counts.values() for t in v if t != "solo"))
    seen, repeated = set(), []
    for t in tags:
        if t in seen and t not in repeated:
            repeated.append(t)
        seen.add(t)
    if repeated:
        flags.append("tag repeated: " + ", ".join(repeated))
    both = [t for t in dict.fromkeys(tags) if t in negative_tags]
    if both:
        flags.append("in both the prompt and the negative: " + ", ".join(both))
    if len(tags) > max_tags:
        flags.append(f"{len(tags)} tags (over {max_tags}; the encoder weighs about 75 tokens, later tags count for less)")
    else:
        notes.append(f"{len(tags)} tags")
    if known is None:
        notes.append("no local Danbooru set, tag names not checked (maid-danbooru-tags fetch, outside the agent)")
    else:
        unknown, aliases = [], []
        for t in dict.fromkeys(tags):
            if COUNT_RE.match(t) or t in baseline or t == "your_tags_here":
                continue
            status, tag, extra = danbooru_tags.check_one(known, t)
            if status == "unknown":
                near = [x for x in extra if len(x) >= 3]
                unknown.append(tag + (f" (near: {', '.join(near)})" if near else ""))
            elif status == "alias":
                aliases.append(f"{tag} (Danbooru's name is {extra})")
        for u in unknown:
            flags.append("unknown tag: " + u)
        for a in aliases:
            notes.append("alias: " + a)
        if not unknown:
            notes.append("every tag is in the local Danbooru set" + (f" (the {len(baseline)}-tag baseline not checked)" if baseline else ""))
    for kind, text, font_size in captions:
        if not text:
            continue
        if size is None or not isinstance(font_size, (int, float)):
            notes.append(f"{kind}: {len(text)} chars (no image size wired in, so its fit is not checked)")
            continue
        per_line, capacity = caption_capacity(font_size, size, caption_lines)
        if len(text) > capacity:
            flags.append(f"{kind} is {len(text)} chars; its overlay shows about {capacity} ({per_line} per line, {caption_lines} lines at font_size {font_size})")
        else:
            notes.append(f"{kind}: {len(text)} chars of about {capacity}")
    return flags, notes


def load_known(path):
    """The local Danbooru set when it exists, else None (the check is then skipped, not failed)."""
    path = path or danbooru_tags.store_path()
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def show_panel(wf, n, args, known):
    parts = panel_parts(wf, n)
    if parts is None:
        return None
    prompt_text = str(widget(parts["prompt"], 0, "") or "")
    tags = split_tags(prompt_text)
    negative_text = str(widget(parts["negative"], 0, "") or "") if parts["negative"] else ""
    negative_tags = split_tags(negative_text)
    size = image_size(parts["latent"])
    lines = [f"PANEL {n}  {os.path.basename(args.workflow)}"]
    lines.append(f"prompt    #{parts['prompt']['id']} \"{title(parts['prompt'])}\"  ({len(tags)} tags)")
    lines.append("  " + (prompt_text.strip() or "(empty)"))
    if parts["negative"]:
        lines.append(f"negative  #{parts['negative']['id']} \"{title(parts['negative'])}\"  ({len(negative_tags)} tags)")
        lines.append("  " + (negative_text.strip() or "(empty)"))
    else:
        lines.append("negative  (none wired into the sampler)")
    if parts["sampler"]:
        s = parts["sampler"]
        values = {name: widget(s, i) for i, name in enumerate(SAMPLER_FIELDS)}
        lines.append(f"sampler   #{s['id']} \"{title(s)}\"  seed {values['seed']} ({values['control_after_generate']}), steps {values['steps']}, "
                     f"cfg {values['cfg']}, {values['sampler_name']} / {values['scheduler']}, denoise {values['denoise']}"
                     + (f", image {size[0]}x{size[1]}" if size else ", image size not wired in"))
    else:
        lines.append("sampler   (no KSampler wired to the prompt)")
    captions = []
    for kind in ("narration", "dialogue"):
        node = parts[kind]
        if node is None:
            lines.append(f"{kind:<9} (no overlay)")
            continue
        text = str(widget(node, 0, "") or "")
        font_size = widget(node, 1)
        captions.append((kind, text, font_size))
        lines.append(f"{kind:<9} #{node['id']}  font_size {font_size}  {widget(node, 3, '?')}/{widget(node, 4, '?')}  " + (json.dumps(text) if text else "(empty)"))
    flags, notes = check_panel(tags, negative_tags, captions, size, known, args.max_tags, args.caption_lines, baseline_of(wf))
    lines.append("checks")
    for note in notes:
        lines.append("  ok    " + note)
    for flag in flags:
        lines.append("  FLAG  " + flag)
    lines.append(f"{len(flags)} flag{'s' if len(flags) != 1 else ''}")
    print("\n".join(lines))
    return len(flags)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("workflow")
    ap.add_argument("panel", help="a panel number, or all")
    ap.add_argument("--max-tags", type=int, default=75)
    ap.add_argument("--caption-lines", type=int, default=2)
    ap.add_argument("--tags", help="the local Danbooru tags.json (default: where maid-danbooru-tags keeps it)")
    args = ap.parse_args()
    wf = load(args.workflow)
    if "nodes" not in wf:
        die(f"{args.workflow} has no nodes: not a ComfyUI workflow")
    known = load_known(args.tags)
    numbers = panel_numbers(wf)
    if not numbers:
        die("the workflow has no node titled 'Panel N prompt'; this tool expects MAID's manga layout")
    if args.panel == "all":
        wanted = numbers
    else:
        try:
            wanted = [int(args.panel)]
        except ValueError:
            die(f"panel must be a number or all, not {args.panel!r}")
        if wanted[0] not in numbers:
            die(f"no panel {wanted[0]}: the workflow has panels {', '.join(map(str, numbers))}")
    flagged = 0
    for i, n in enumerate(wanted):
        if i:
            print()
        flagged += show_panel(wf, n, args, known)
    sys.exit(1 if flagged else 0)


if __name__ == "__main__":
    main()
