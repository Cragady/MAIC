# tools

`examples/` holds user-defined tools: `word-count.lua` in Lua, and two script tools, `word_count/` (Python) and `json_pick/` (shell with jq), each a directory with a `tool.json` manifest and its script. Copy a Lua file into `.maic/tools/` in a workspace (or `~/.config/maic/tools/`), or a tool directory into the same place, and the model can call it in the next session. The Lua format and the `maic` table, the manifest fields (`run` names the interpreter: Python, Perl, shell, Node or Deno, a Go binary, `wasmtime`), the arguments-on-stdin contract, and the harness in front of every call are in [docs/tools.md](../docs/tools.md). `maic tools new NAME --lang python|sh|perl|node` scaffolds a script tool; `maic tools check` validates the manifests.

`comfyui/` holds MAIC's own helpers for the manga workflow, installed beside `maic` as `maic-workflow-edit`, `maic-storyboard`, `maic-danbooru-tags` and `maic-panel-check` ([comfyui/README.md](comfyui/README.md)).

`audit/` holds `maic-leak-audit`, which checks transcripts for agents that reached for a host socket from the sandbox, judged by a local model ([docs/leak-audit.md](../docs/leak-audit.md)).

`agent-kit/` holds `artifact-watch.sh`, `maic artifact watch` in POSIX sh for an agent outside MAIC ([docs/agent-kit.md](../docs/agent-kit.md)).

Still to come: per-tool network grants declared in the manifest (`network: true` is refused today).

Design background: [docs/programming-lang-for-agentic-cli.md](../docs/programming-lang-for-agentic-cli.md).
