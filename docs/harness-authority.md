# Whose harness judges an action

MAIC's harness exists to judge what runs on this machine. When another tool with its own harness is in the loop, exactly one of them judges each action, never both, and the other is told so. This page is the rule; [harness.md](harness.md) is the harness itself and [design/engine-protocol.md](design/engine-protocol.md) carries the events below.

## With the Anthropic API (or any model API)

The plain Messages API has no harness for actions. The model only proposes a tool call; MAIC executes it. Nothing on Anthropic's side reviews what MAIC runs: Anthropic's safeguards apply to what the model says, not to MAIC's actions. So with an API model MAIC's harness is the only judge, and it stays smart (a reviewer model reads each command and write before it runs).

The exception is Anthropic's own server-side tools (web search, code execution). They run on Anthropic's machines, outside MAIC's harness, so they stay off unless a setting enables them, and enabling them prints a notice.

## With other agentic tools

| Level | Whose harness judges actions | MAIC's local harness |
| :--- | :--- | :--- |
| 1: `claude -p` as a text-only provider | nobody: its built-in tools and MCP servers are off, so it can take no actions | unchanged |
| 2: Claude Code driving MAIC's tools over MCP | MAIC's: Claude Code's built-in tools are off and MAIC's tools are pre-allowed in Claude Code, so it neither prompts for nor classifies them | smart |
| 3: an external agent using its own tools (an explicit opt-in per agent) | the external agent's own | dumb by default when the other side is smart (Claude Code in auto mode, with its action classifier): no local reviewer call, so no doubled review and no doubled cost, and no auto-mode warning, since a model already reviews on the other side. If that agent runs in a bypass mode with no smart harness of its own, MAIC keeps its smart harness, or warns as it does for dumb plus auto. |

In every case "dumb" still means MAIC's fixed rules hold underneath as the floor: forbidden terms, trip patterns, trust, the sandbox, and self-protection. Those are rules, not a second judgement, so they never create a double-harness effect. Even the dumbest harness catches them.

## The two streaming paths

Both are supported, and a session uses whichever its harness authority calls for.

**The local round trip (MAIC's harness judges).** The model's reply streams in to MAIC. When it proposes an action, MAIC sends that action out again, in a second stream, to the reviewer model, waits for the verdict (allow, ask, deny), and only then runs it or asks the user. The reviewer may be local or a remote API. The interface shows both streams: the reply as it arrives, then the review in flight and its verdict, then the action's own output as it runs. Cost and latency: one extra model round trip per reviewed action (reads are never reviewed).

**All remote (the other side's harness judges).** The external agent decides and runs its actions itself; MAIC receives one stream, remote to local: what the agent says, which actions it took, their results, and whose harness approved them. There is no second stream and no local review; MAIC records and displays, confines the agent to its sandbox, and its fixed rules still stop anything they match.

Each action in a transcript and on the event stream names its judge (`judged_by: "maic"`, `"claude-code"`, or `"rules"` for the fixed rules alone), so it is always visible which path an action took.
