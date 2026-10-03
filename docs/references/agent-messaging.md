# How agents message each other (opencode, DeepSeek harness, OpenAI API)

Clean-room, read 2026-10-03; no code copied.

## Sources (all read 2026-10-03)

- opencode at `1ddb087` (2026-10-01): HTTP server routes, `tool/task.ts`, `acp/`, `packages/plugin`, `SECURITY.md`.
- deepseek-ai/deepseek-harness at `639ed01` (2026-09-29): `packages/subagent`, `agent-team`, `webhook`, `acp`, `docs/subsystems/approval.md`.
- OpenAI OpenAPI 2.3.0 (`protocol/openai/openapi.json`).
- https://code.claude.com/docs/en/channels-reference.md, for context.

## opencode

- Entry points: an opt-in HTTP server. `POST /session/:id/message` and `/prompt_async` put a user message into a running session; `/event` streams events; `/permission/:id/reply` answers prompts; `/tui/*` drives the live terminal UI; `/sync/*` replays history.
- Gating: one shared Basic Auth password (`OPENCODE_SERVER_PASSWORD`), else open with a warning. Whoever holds it can prompt and approve. `SECURITY.md` calls permissions a UX aid, not a sandbox.
- Subagents: the `task` tool makes a child session. The child inherits the parent's deny rules and cannot spawn tasks by default. Background results return to the parent as a synthetic user message in task tags. The tool text says agent output "should generally be trusted".
- Plugins: hooks for events, messages, tool calls and `permission.ask`, which can turn a prompt into allow. Plugins are trusted code.
- ACP: opencode is an ACP agent; permission prompts become ACP requests, rejected if the client cannot answer.
- No data-versus-instruction marking and no sender identity on these paths.

## DeepSeek harness

- Agent to agent: `send_message` reaches only a direct parent or direct child, never siblings. Text is framed with the sender's id, and the docs say attribution is never authority. Delivery lands at the next step. Team mode adds a durable, deduplicated mailbox with limits; only the lead may spawn or interrupt.
- Subagents over ACP return only the child's final text. Child permission prompts are auto-answered, and `reject` is the default.
- Webhooks: the GitHub adapter checks an HMAC signature before parsing and returns 202. Only trusted code rules may open a session; each rule must label outside text, and the example marks PR fields as untrusted JSON.
- Approval is closed and fails closed: allow-once, reject, cancel or unavailable, with a `never` policy for unattended runs. `SAFETY.md` claims no audit and no isolation.

## OpenAI API

- Webhooks carry only an event id, type and object id (for example a response id); the receiver fetches details. Endpoints are HTTPS, subscribe to chosen event types, and have rotatable signing secrets (scheme not in the spec).
- Into a running session: `POST /agents/sessions/{id}/events` accepts exactly four input kinds: user message, cancel, tool result, approval result. No system or developer text can enter this way.
- MCP: `mcp_approval_request` names the server, tool and arguments; `mcp_approval_response` must cite the request id. MCP tools take `allowed_tools` and `require_approval`.

## Claude Code (context)

Channels are MCP servers that push `notifications/claude/channel`. Servers must be allowlisted or loaded with a development flag, and organizations can disable them. The reference calls an ungated channel an injection vector: gate on sender, not room. Relay accepts verdicts only for issued request ids. A headless `claude -p` session reached an interactive one via an undocumented local socket (tested 2026-10-03), with no approved channel involved.

## Lessons for MAIC

1. Hash-pin protocols. Neither tool verifies what a notifier may say. Receivers refuse any protocol whose hash Micaiah has not approved.
2. Events carry ids and types, never instructions (OpenAI's webhook shape). The receiver fetches content and labels it data.
3. Authority comes from the approved protocol and Micaiah's own words, never from sender attribution or an agent message ("generally trusted" is the wrong default).
4. Limit input kinds as OpenAI does, allow only adjacent agents as DeepSeek does, and bind approvals to an issued request id.
5. Fail closed: unattended means reject; unsigned or unknown senders are dropped.
