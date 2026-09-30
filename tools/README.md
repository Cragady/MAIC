# tools

`examples/` holds user-defined tools written in Lua: copy one into `.maic/tools/` in a workspace (or `~/.config/maic/tools/`) and the model can call it in the next session. The format, the `maic` table a tool gets and the harness in front of every call it makes are in [docs/tools.md](../docs/tools.md).

Planned beyond Lua: one folder per runtime (`shell/`, `perl/`, `python/`, `typescript/`, `go/`, `wasm/`), each tool a JSON manifest (OpenAI function-calling schema plus `runtime`, and `network: true` if it needs the network) and a script. The core starts the script, sends the arguments as JSON on stdin, and reads the JSON result from stdout, through the same authorisation step. See [docs/harness.md](../docs/harness.md).

Design background: [docs/programming-lang-for-agentic-cli.md](../docs/programming-lang-for-agentic-cli.md).
