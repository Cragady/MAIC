"""MAIC's template shelf for ComfyUI.

Adds no nodes. Its only job is the `example_workflows/` folder, which ComfyUI serves in the template browser
under "comfyui-maic-templates". That folder is a link to ~/.local/state/maic/templates/comfyui, so workflows
kept there are your originals: opening one in ComfyUI creates a new workflow, and saving lands in
user/default/workflows (MAIC's workflows artifact), never back into the template.
"""

NODE_CLASS_MAPPINGS = {}
NODE_DISPLAY_NAME_MAPPINGS = {}
WEB_DIRECTORY = None
