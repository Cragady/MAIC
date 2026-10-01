#!/usr/bin/env bash
# Installs the vendored ComfyUI the way MAIC wants it: its own Python via uv (system Python untouched),
# CUDA 13 torch, MAIC's own custom nodes (the llama.cpp chat nodes, the template shelf), workflows kept in
# MAIC's artifact tree. Idempotent: run again to update packages. The model paths (extra_model_paths.yaml)
# are written by `maic vendor wire comfyui` from the manifest's models map, not here.
#
#   vendor/comfyui.sh install|update|wire|check   MAIC_VENDOR=<state>/vendor  MAIC_STATE=<state>  MAIC_ROOT=<repo>
#   install/update fetch packages (network); wire only links the custom nodes and workflows.
#
# The checkout is $MAIC_VENDOR/ComfyUI (a symlink to the submodule, or to an adopted install).
set -euo pipefail
cmd="${1:-install}"
: "${MAIC_VENDOR:?set by maic}"
: "${MAIC_STATE:?set by maic}"
root="$MAIC_VENDOR/ComfyUI"
[ -d "$root" ] || { echo "no ComfyUI at $root (maic vendor add comfyui, or maic vendor adopt comfyui PATH)" >&2; exit 1; }
command -v uv >/dev/null || { echo "uv is required (https://docs.astral.sh/uv/)" >&2; exit 1; }
cd "$root"

wire() {
    # MAIC's chat nodes: they talk to llama-server on 127.0.0.1:8081 (vendor/comfyui-maic-llamacpp/README.md).
    mkdir -p custom_nodes
    if [ ! -e custom_nodes/comfyui-maic-llamacpp ]; then
        ln -s "$MAIC_ROOT/vendor/comfyui-maic-llamacpp" custom_nodes/comfyui-maic-llamacpp
    fi
    # Your own templates (originals, opened as copies) show up in ComfyUI's template browser through a
    # no-op custom node whose example_workflows/ points at the artifact tree.
    tpl="$MAIC_STATE/templates/comfyui"
    mkdir -p "$tpl"
    if [ ! -e custom_nodes/comfyui-maic-templates ]; then
        ln -s "$MAIC_ROOT/vendor/comfyui-maic-templates" custom_nodes/comfyui-maic-templates
    fi
    [ -e "$MAIC_ROOT/vendor/comfyui-maic-templates/example_workflows" ] || ln -s "$tpl" "$MAIC_ROOT/vendor/comfyui-maic-templates/example_workflows"
    echo "templates: $tpl"
    # Workflows belong to MAIC's artifact tree; ComfyUI's user workflow folder points there.
    wf="$MAIC_STATE/workflows/comfyui"
    mkdir -p "$wf" user/default
    if [ -d user/default/workflows ] && [ ! -L user/default/workflows ]; then
        cp -an user/default/workflows/. "$wf"/ 2>/dev/null || true
        rm -rf user/default/workflows
        echo "moved existing workflows to $wf"
    fi
    [ -L user/default/workflows ] || ln -s "$wf" user/default/workflows
    echo "workflows: $wf"
}

case "$cmd" in
wire)
    wire
    ;;
install|update)
    if [ ! -x .venv/bin/python ]; then
        uv venv --python 3.13 .venv
    fi
    # CUDA 13.0 torch is what 20-series and newer need per ComfyUI's README.
    uv pip install --python .venv torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu130
    uv pip install --python .venv -r requirements.txt
    for req in custom_nodes/*/requirements.txt; do
        [ -f "$req" ] && uv pip install --python .venv -r "$req"
    done
    .venv/bin/python -c "import torch; print('torch', torch.__version__, 'cuda', torch.cuda.is_available())"
    wire
    echo "ComfyUI ready at $root"
    ;;
check)
    [ -x .venv/bin/python ] && .venv/bin/python -c "import torch, folder_paths" 2>/dev/null && echo ok || { echo "not installed"; exit 1; }
    ;;
*)
    echo "usage: $0 install|update|wire|check" >&2
    exit 2
    ;;
esac
