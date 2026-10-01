# workflow_edit.py: tune a ComfyUI workflow without touching its wiring

Installed as `maic-workflow-edit` beside `maic` (the agent is told about it in its briefing and can run it through `run_shell`; the sandbox sees the whole filesystem read-only and writes only inside the workspace, so start `maic` in the folder that holds the workflow). `tools/comfyui/workflow_edit.py PATH` reads a workflow JSON by exact path and works on its widget values only: prompts, seeds, steps, CFG, sampler, file prefixes, caption text and colours, stitch settings. Links, node ids and positions are never changed, and every write keeps a `PATH.bak`.

## For a person

```sh
python3 tools/comfyui/workflow_edit.py edit ~/.local/state/maic/workflows/comfyui/manga-20panel-noobai-beta-captions.json
python3 tools/comfyui/workflow_edit.py edit FILE --only CLIPTextEncode TextOverlay      # prompts and captions only
python3 tools/comfyui/workflow_edit.py edit FILE --nodes "Panel 5 prompt" "Panel 6 prompt"
```

It walks node by node and field by field: keep, replace, append, prepend, skip the node, write and stop, or quit without saving. Multi-line text ends with a line holding only `.`.

## For an agent

Everything below runs without a terminal, which is how the agent inside MAIC (or any script) drives it through `run_shell`. Start MAIC in the folder that holds the workflow, or pass `workdir`, so the sandbox can write it.

```sh
# 1. learn the structure: node ids, titles, field names, current values
python3 tools/comfyui/workflow_edit.py inspect FILE --json

# 2. change fields by id or unique title; numbers stay numbers
python3 tools/comfyui/workflow_edit.py set FILE 51.steps 30 "Panel 5 prompt".text "masterpiece, ..., rooftop, sunset"
python3 tools/comfyui/workflow_edit.py append FILE "Panel 5 prompt".text ", holding umbrella"
python3 tools/comfyui/workflow_edit.py prepend FILE "Panel 5 dialogue".text "HANA: "
python3 tools/comfyui/workflow_edit.py replace-all FILE "grey hair" "silver hair" --only CLIPTextEncode

# 3. many edits at once, from a file the agent writes
python3 tools/comfyui/workflow_edit.py apply FILE edits.json
#   edits.json: [{"node": 50, "field": "text", "op": "set", "value": "..."},
#                {"node": "Panel 6 sampler", "field": "seed", "op": "set", "value": 106}]

# always available: --dry-run shows the diff and writes nothing; --out OTHER writes elsewhere
```

Long text can come from stdin: `set FILE 50.text -`. Exit codes: 0 written (or the dry run shown), 1 bad argument, 2 node or field not found; every message names the node and lists its fields.

## Field names

Known node types show named slots (`CLIPTextEncode.text`, `KSampler.seed/steps/cfg/sampler_name/scheduler/denoise`, `SaveImage.filename_prefix`, `TextOverlay.text/font_size/color/...`, `ImageStitch.direction/...`, MAIC's `MaicLlmChat.system/prompt/...`); anything else shows `w0`, `w1`, ... in the order ComfyUI stores them. `inspect` always shows the current value beside the name, so a slot is never a guess.

# danbooru_tags.py (`maic-danbooru-tags`): is this a real tag?

Danbooru's tag data is not in its repository; `maic-danbooru-tags fetch` pulls the most-used tags and the active aliases from the JSON API (one request a second, twenty pages of a thousand by default, since scene tags sit well below the top five thousand, `--category all` for characters and copyrights too) into `~/.local/state/maic/references/danbooru/tags.json`. Then, offline: `check TAG ...` or `check --prompt "a, b, c"` (ok, alias with the canonical name, or unknown with near matches; exit 1 when anything is unknown), `search WORD`, `show`. The agent's briefing tells it to check a prompt before writing it and never to fetch; the storyboard plan repeats the command. `groups fetch` downloads the wiki's "Tag groups" index and every group page it links (about a hundred pages, one request a second) as markdown under `~/.local/state/maic/references/danbooru/groups/`; then `groups list`, `groups show posture`, `groups search "one leg"` read them offline, for a person or the agent.

# storyboard.py (`maic-storyboard`): a story JSON into the manga workflow, one panel at a time

The story has `characters[]` (with an `appearance`), a `setting`, and `panels[]` with a `caption`, a `visual_description`, a `mood`, sometimes a `dialogue` line, and which characters are present (`character_positions` / `character_details`). The workflow has, per panel, a prompt node and two caption overlays. The split:

- **Mechanical, done by the script.** `maic-storyboard fill STORY WORKFLOW` writes every caption into the narration overlay and every dialogue line into the dialogue overlay, verbatim. Run once.
- **Judgement, done by the agent, one panel per turn.** `maic-storyboard plan STORY WORKFLOW --panel 7` prints that panel's story values in full, the characters present with their appearance, the setting, what the prompt holds now, the quality baseline the workflow already uses (never changed), and the exact `maic-workflow-edit set ...` command with the baseline filled in and a `YOUR TAGS HERE` slot. The agent writes Danbooru-style tags for that one panel and runs the command; the vocabulary is the Danbooru tag groups, kept in [docs/references/danbooru-tag-groups.md](../../docs/references/danbooru-tag-groups.md). Small enough for a 4B's context.
- `maic-storyboard check STORY WORKFLOW` lists the panels still on the baseline alone, overlays that differ from the story, and story fields the tool does not map (production notes, dialogue summaries: for a person).

`maic-storyboard critique N [--image PATH]` closes the loop with the vision model: it finds panel N's newest render (the clean-panel SaveImage prefix under `maic path comfyui/outputs`, or `--image`), sends it with the panel's prompt and story to llama-server (the 9B when it is listed, loopback only), and prints what matched, what is missing, what is wrong, plus the `maic-workflow-edit set` command that applies the model's proposed tag changes after checking them against the local Danbooru set. For a person, or MAIC itself outside the sandbox: the agent's sandbox has no network.

The brief for the agent (all of it):

```
There is a tool called maic-storyboard. Run it with no arguments and follow what it says.
```

Which is: ask the user for the story file and the destination workflow; `maic-storyboard start STORY TEMPLATE --out DEST` (copies, fills the overlays, prints panel 1); write that panel's tags into the printed command and run it; `maic-storyboard next` (verifies, prints the next panel, or the same one again if it was not written); repeat until it reports every panel done. `maic-storyboard --help` carries the same instructions. A template is never written in place: `start` refuses `--out` equal to the source unless `--in-place` is given.

# panel_check.py (`maic-panel-check`): one panel, one screen, the usual mistakes flagged

`maic-panel-check WORKFLOW N` prints panel N as the model needs to see it to judge its own work: the prompt with its tag count, the negative wired into the panel's KSampler, the sampler settings (seed, steps, cfg, sampler, scheduler, denoise, image size), and the narration and dialogue overlays with their font size and placement. Then the checks, each a `FLAG` line when it fires: no character count tag or two for the same subject, a repeated tag, `solo` beside `2girls`, a tag in both the prompt and the negative, more than `--max-tags` (75) tags, the `YOUR TAGS HERE` slot left in, a tag the local Danbooru set does not know (when `maic-danbooru-tags fetch` has run; the baseline every panel shares is left out of that check, aliases are noted with Danbooru's name), and a caption longer than its overlay can show (an estimate from the image size and `font_size`, `--caption-lines` lines). `all` instead of N does every panel. Exit 0 clean, 1 flagged, 2 bad input. Offline and read-only, so it is allowed in every mode; the agent is briefed to run it after writing a panel.
