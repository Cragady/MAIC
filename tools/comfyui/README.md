# workflow_edit.py: tune a ComfyUI workflow without touching its wiring

`tools/comfyui/workflow_edit.py PATH` reads a workflow JSON by exact path and works on its widget values only: prompts, seeds, steps, CFG, sampler, file prefixes, caption text and colours, stitch settings. Links, node ids and positions are never changed, and every write keeps a `PATH.bak`.

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
