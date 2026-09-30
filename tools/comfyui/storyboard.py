#!/usr/bin/env python3
"""Merge a story JSON (characters, setting, panels) into a MAIC manga workflow, one panel at a time.

The workflow has, per panel N, a prompt node ("Panel N prompt"), a narration overlay ("Panel N narration")
and a dialogue overlay ("Panel N dialogue"). The story has `panels[]` with a caption, a visual
description, a mood, sometimes a dialogue line, and which characters are in the panel; `characters[]`
with an appearance each; and a `setting`. Two kinds of work follow from that:

  mechanical, done by this script:  caption -> narration text, dialogue -> dialogue text, verbatim
  judgement, done by the agent:      the prompt: the workflow's quality baseline, kept exactly, followed by
                                     Danbooru-style tags for the characters present, the scene, camera, mood

Commands:
    storyboard.py fill STORY.json WORKFLOW.json [--out PATH] [--dry-run]
        Writes every caption and dialogue line into the overlays and prints the panel list. Run once.
    storyboard.py plan STORY.json WORKFLOW.json --panel N
        One panel's work order: the story values it has (shown in full), the target node, the baseline,
        and the exact maic-workflow-edit command to run once the tags are written. Small enough for a
        small model's context; do one panel per turn.
    storyboard.py plan STORY.json WORKFLOW.json --all
        The same for every panel, for a person.
    storyboard.py check STORY.json WORKFLOW.json
        Which panels still have a baseline-only prompt, and which story fields this tool does not map.

Character presence comes from the keys of `character_positions` / `character_details`, matched to
`characters[].id` or name (case-insensitive). The baseline is the longest run of leading tags shared by
every panel prompt in the workflow (the quality and style block); the script never changes it.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

PROMPT_RE = re.compile(r"^Panel (\d+) prompt$")


def load(path, what):
    if not os.path.isfile(path):
        sys.exit(f"no such {what}: {path}")
    with open(path, encoding="utf-8") as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as e:
            sys.exit(f"{path} is not valid JSON: {e}")


def panel_nodes(wf):
    """{N: {"prompt": node, "narration": node, "dialogue": node}} by title."""
    out = {}
    for n in wf["nodes"]:
        m = re.match(r"^Panel (\d+) (prompt|narration|dialogue)$", n.get("title", ""))
        if m:
            out.setdefault(int(m.group(1)), {})[m.group(2)] = n
    if not out:
        sys.exit("the workflow has no nodes titled 'Panel N prompt' / 'Panel N narration' / 'Panel N dialogue'; this tool expects MAIC's manga layout")
    return out


def baseline_of(wf, nodes):
    """Leading tags common to every panel prompt, at comma boundaries."""
    prompts = [nodes[k]["prompt"]["widgets_values"][0] for k in sorted(nodes) if "prompt" in nodes[k]]
    split = [[t.strip() for t in p.split(",") if t.strip()] for p in prompts]
    common = []
    for tags in zip(*split):
        if len(set(tags)) == 1:
            common.append(tags[0])
        else:
            break
    return ", ".join(common)


def characters_by_key(story):
    by = {}
    for c in story.get("characters", []):
        for key in (c.get("id"), c.get("name")):
            if key:
                by[str(key).lower()] = c
    return by


def present(panel):
    keys = []
    for field in ("character_positions", "character_details"):
        if isinstance(panel.get(field), dict):
            keys += list(panel[field].keys())
    seen = []
    for k in keys:
        if k not in seen:
            seen.append(k)
    return seen


def story_panels(story):
    panels = story.get("panels")
    if not isinstance(panels, list) or not panels:
        sys.exit("the story has no `panels` list")
    out = {}
    for i, p in enumerate(panels):
        n = p.get("panel_number", i + 1)
        try:
            out[int(n)] = p
        except (TypeError, ValueError):
            out[i + 1] = p
    return out


def write(wf, path, out, dry_run, before):
    if dry_run:
        print("(dry run: nothing written)")
        return
    target = out or path
    if os.path.exists(target):
        shutil.copy2(target, target + ".bak")
    with open(target, "w", encoding="utf-8") as f:
        json.dump(wf, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {target} (previous contents in {target}.bak)" if before else f"wrote {target}")


def cmd_fill(args):
    story, wf = load(args.story, "story"), load(args.workflow, "workflow")
    nodes, panels = panel_nodes(wf), story_panels(story)
    changed = 0
    for n in sorted(panels):
        p, target = panels[n], nodes.get(n)
        if not target:
            print(f"panel {n}: the workflow has no panel {n}; skipped")
            continue
        caption, dialogue = str(p.get("caption", "") or ""), str(p.get("dialogue", "") or "")
        if "narration" in target and target["narration"]["widgets_values"][0] != caption:
            target["narration"]["widgets_values"][0] = caption
            changed += 1
        if "dialogue" in target and target["dialogue"]["widgets_values"][0] != dialogue:
            target["dialogue"]["widgets_values"][0] = dialogue
            changed += 1
        print(f"panel {n:>2}: narration <- caption ({len(caption)} chars)" + (f", dialogue <- {len(dialogue)} chars" if dialogue else ", no dialogue"))
    missing = [n for n in nodes if n not in panels]
    if missing:
        print(f"workflow panels with no story panel: {missing} (left as they are)")
    print(f"{changed} overlay value(s) changed. Next: `storyboard.py plan STORY WORKFLOW --panel 1` and write the prompts, one panel per turn.")
    write(wf, args.workflow, args.out, args.dry_run, os.path.exists(args.out or args.workflow))


def plan_one(story, wf, nodes, panels, baseline, n, workflow_path):
    p, target = panels.get(n), nodes.get(n)
    if not p:
        return f"PANEL {n}: the story has no panel {n}\n"
    if not target or "prompt" not in target:
        return f"PANEL {n}: the workflow has no prompt node for panel {n}\n"
    chars = characters_by_key(story)
    lines = [f"PANEL {n}  ->  prompt node #{target['prompt']['id']} \"{target['prompt']['title']}\""]
    lines.append("  story values:")
    for k in ("caption", "visual_description", "mood", "dialogue"):
        if p.get(k):
            lines.append(f"    {k}: {json.dumps(p[k], ensure_ascii=False)}")
    for field in ("character_positions", "character_details"):
        if isinstance(p.get(field), dict):
            for who, text in p[field].items():
                lines.append(f"    {field}.{who}: {json.dumps(text, ensure_ascii=False)}")
    who = present(p)
    if who:
        lines.append("  characters in this panel (their appearance, from characters[]):")
        for w in who:
            c = chars.get(str(w).lower())
            if c:
                lines.append(f"    {w}: {json.dumps(c.get('appearance', ''), ensure_ascii=False)}" + (f"  (age {c['age']})" if c.get("age") is not None else ""))
            else:
                lines.append(f"    {w}: not in characters[]; describe from the panel text")
    setting = story.get("setting", {})
    if isinstance(setting, dict) and (setting.get("location") or setting.get("environment") or setting.get("atmosphere")):
        lines.append("  setting: " + "; ".join(str(setting[k]) for k in ("location", "environment", "atmosphere") if setting.get(k)))
    style = story.get("story_metadata", {}).get("style") if isinstance(story.get("story_metadata"), dict) else None
    if style:
        lines.append(f"  story style: {json.dumps(style, ensure_ascii=False)}  (the workflow's baseline below already fixes the render style; add nothing that contradicts it)")
    current = target["prompt"]["widgets_values"][0].strip()
    rest = current[len(baseline):].strip(" ,") if current.startswith(baseline) else current
    if not rest:
        lines.append("  current prompt: baseline only, needs tags")
    else:
        lines.append(f"  current tags after the baseline: {json.dumps(rest, ensure_ascii=False)}")
        lines.append("    (if these describe this story's characters and scene, keep what fits; if they are the template's own character, replace them)")
    lines.append("  done for you: narration and dialogue overlays hold the caption and dialogue verbatim (storyboard.py fill).")
    lines.append("  YOUR JOB: write this panel's prompt as Danbooru-style tags, comma separated: the characters present (count, then")
    lines.append("  each one's appearance as tags), then the scene from visual_description, camera/framing, and the mood. Keep it")
    lines.append("  under about 60 tags. Do not repeat the baseline; the command below already starts with it. No quotes inside.")
    lines.append(f"  baseline (keep exactly): {json.dumps(baseline, ensure_ascii=False)}")
    lines.append(f"  command:  maic-workflow-edit set {shell_quote(workflow_path)} \"Panel {n} prompt\".text \"{baseline}, YOUR TAGS HERE\"")
    lines.append(f"  then:     maic-workflow-edit inspect {shell_quote(workflow_path)} --json | grep -A3 '\"Panel {n} prompt\"'   (to confirm)")
    return "\n".join(lines) + "\n"


def shell_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def cmd_plan(args):
    story, wf = load(args.story, "story"), load(args.workflow, "workflow")
    nodes, panels = panel_nodes(wf), story_panels(story)
    baseline = baseline_of(wf, nodes)
    if args.panel is None and not args.all:
        sys.exit("give --panel N (one panel, for an agent) or --all")
    targets = sorted(set(panels) | set(nodes)) if args.all else [args.panel]
    for n in targets:
        print(plan_one(story, wf, nodes, panels, baseline, n, args.workflow))


def cmd_check(args):
    story, wf = load(args.story, "story"), load(args.workflow, "workflow")
    nodes, panels = panel_nodes(wf), story_panels(story)
    baseline = baseline_of(wf, nodes)
    print(f"baseline: {json.dumps(baseline, ensure_ascii=False)}")
    todo, done = [], []
    for n in sorted(nodes):
        cur = nodes[n]["prompt"]["widgets_values"][0].strip().rstrip(",").strip() if "prompt" in nodes[n] else ""
        (todo if cur == baseline or not cur else done).append(n)
    print(f"prompts still baseline-only: {todo if todo else 'none'}")
    print(f"prompts with tags: {done if done else 'none'}")
    for n in sorted(nodes):
        t = nodes[n]
        if "narration" in t and (panels.get(n, {}).get("caption") or "") != t["narration"]["widgets_values"][0]:
            print(f"panel {n}: narration differs from the story caption (run `fill`)")
    mapped = {"panels", "characters", "setting", "story_metadata"}
    extra = [k for k in story if k not in mapped]
    if extra:
        print(f"story fields this tool does not map (for a human, not the workflow): {extra}")
    pextra = sorted({k for p in panels.values() for k in p} - {"panel_number", "caption", "visual_description", "mood", "dialogue", "character_positions", "character_details"})
    if pextra:
        print(f"panel fields not mapped: {pextra}")
    if len(panels) != len(nodes):
        print(f"story has {len(panels)} panels, workflow has {len(nodes)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fill", help="captions and dialogue into the overlays")
    p.add_argument("story")
    p.add_argument("workflow")
    p.add_argument("--out")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=cmd_fill)
    p = sub.add_parser("plan", help="a panel's work order for the prompt")
    p.add_argument("story")
    p.add_argument("workflow")
    p.add_argument("--panel", type=int)
    p.add_argument("--all", action="store_true")
    p.set_defaults(func=cmd_plan)
    p = sub.add_parser("check", help="what is left to do")
    p.add_argument("story")
    p.add_argument("workflow")
    p.set_defaults(func=cmd_check)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
