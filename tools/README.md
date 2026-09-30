# tools

Planned: the agent's tools, one folder per runtime (`shell/`, `lua/`, `perl/`, `python/`, `typescript/`, `go/`, `wasm/`).

Each tool is a JSON manifest (OpenAI function-calling schema plus `runtime`, and `network: true` if it needs the network) and a script. The core starts the script, sends the arguments as JSON on stdin, and reads the JSON result from stdout. Every call goes through the harness (policy, approval, sandbox). See [docs/harness.md](../docs/harness.md).

Design background: [docs/programming-lang-for-agentic-cli.md](../docs/programming-lang-for-agentic-cli.md).
