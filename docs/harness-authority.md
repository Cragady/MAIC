# Whose harness judges an action

MAID's harness exists to judge what runs on this machine. When another tool with its own harness is in the loop, exactly one of them judges each action, never both, and the other is told so. This page is the rule; [harness.md](harness.md) is the harness itself and [design/engine-protocol.md](design/engine-protocol.md) carries the events below.

## With the Anthropic API (or any model API)

The plain Messages API has no harness for actions. The model only proposes a tool call; MAID executes it. Nothing on Anthropic's side reviews what MAID runs: Anthropic's safeguards apply to what the model says, not to MAID's actions. So with an API model MAID's harness is the only judge, and it stays smart (a reviewer model reads each command and write before it runs).

The exception is Anthropic's own server-side tools (web search, code execution). They run on Anthropic's machines, outside MAID's harness, so they stay off unless a setting enables them, and enabling them prints a notice.

## With other agentic tools

| Level | Whose harness judges actions | MAID's local harness |
| :--- | :--- | :--- |
| 1: `claude -p` as a text-only provider | nobody: its built-in tools and MCP servers are off, so it can take no actions | unchanged |
| 2: Claude Code driving MAID's tools over MCP | MAID's: Claude Code's built-in tools are off and MAID's tools are pre-allowed in Claude Code, so it neither prompts for nor classifies them | the session's own: dumb by default, smart when chosen |
| 3: an external agent using its own tools (an explicit opt-in per agent) | the external agent's own | dumb by default when the other side is smart (Claude Code in auto mode, with its action classifier): no local reviewer call, so no doubled review and no doubled cost, and no auto-mode warning, since a model already reviews on the other side. If that agent runs in a bypass mode with no smart harness of its own, MAID keeps its smart harness, or warns as it does for dumb plus auto. |

In every case "dumb" still means MAID's fixed rules hold underneath as the floor: forbidden terms, trip patterns, trust, the sandbox, and self-protection. Those are rules, not a second judgement, so they never create a double-harness effect. Even the dumbest harness catches them.

## The two streaming paths

Both are supported, and a session uses whichever its harness authority calls for.

**The local round trip (MAID's harness judges).** It happens under `harness = "smart"`; the default is `"dumb"`, where the fixed rules alone judge and there is no second stream, because the reviewer's call is too slow on a local model. The model's reply streams in to MAID. When it proposes an action, MAID sends that action out again, in a second stream, to the reviewer model, waits for the verdict (allow, ask, deny), and only then runs it or asks the user. The reviewer may be local or a remote API. The interface shows both streams: the reply as it arrives, then the review in flight and its verdict, then the action's own output as it runs. Cost and latency: one extra model round trip per reviewed action (reads are never reviewed).

**All remote (the other side's harness judges).** The external agent decides and runs its actions itself; MAID receives one stream, remote to local: what the agent says, which actions it took, their results, and whose harness approved them. There is no second stream and no local review; MAID records and displays, confines the agent to its sandbox, and its fixed rules still stop anything they match.

A panel of judges (`checkers`, [harness.md](harness.md#checkers-a-panel-of-judges-built)) is not a second harness: it is MAID's reviewer made of several models, inside MAID's harness, under its fixed rules. Its transcript entry says which judge decided (`review.judged_by`: a judge's name, or `user`).

Each action in a transcript and on the event stream names its judge (`judged_by: "maid"`, `"claude-code"`, or `"rules"` for the fixed rules alone), so it is always visible which path an action took.
