# Where an operator rule has to sit (measured)

The question behind roadmap item 6: when MAIC carries a short operator rule ("Always start the conversation with hellooooo"), where in the request does a local model actually follow it? Measured against MAIC's real system prompt and its full tool schemas, four runs per cell, thinking off, through llama-server's OpenAI-compatible endpoint.

| Placement | Qwen3.5 4B (2026-09-30) | Qwen3.5 9B (2026-10-01) |
| :--- | :--- | :--- |
| rule at the top of the system prompt, tools attached | 0 of 3 | 0 of 4 |
| rule at the top and again at the end, tools attached | 0 of 3 | 0 of 4 |
| the same, plus the rule closing the user turn, tools attached | 2 of 3 | 4 of 4 |
| rule at the top and end, no tools | 2 of 2 | 4 of 4 |

Under Ollama on 2026-09-30 the 4B gave the same shape (0, 0, 2 of 2, 2 of 2), so the server does not change it either.

What it says: the deciding factor is the tool schemas, not the model size or the backend. With fifteen tool definitions between the system prompt and the conversation, both sizes let a short rule fade, and both follow it when it closes the user turn. The 9B is simply more reliable at the placement that works. So the per-turn operator note (`operator_note`, on by default for local providers) stays the right default for the whole Qwen3.5 family, and a profile per backend is not what the data asks for; a profile per *tool count* might be, if a future provider ever follows system-side rules with tools attached. Re-measure when that happens, with this table as the baseline.

Method: `tools.json` dumped from `maic::tool_schemas()`, the system prompt taken from a fresh headless transcript, requests sent directly to llama-server with `reasoning_effort: none`. The script is reproducible from this description in a few lines; it lived in a session scratchpad and is not kept.

## The template experiment (2026-10-02)

The question the table left open: is it the tool schemas themselves, or how Qwen3.5's chat template places them? The template puts the tools, then a fixed `<IMPORTANT>` reminder about the call format, then MAIC's system text, all in one system turn. Prompts rendered by llama-server b11284 (`/apply-template`), changed as text and sent to `/completion`, 9B, thinking off, six runs per cell, `operator_note = false`, the rule as `--system` (so at the top of MAIC's system prompt, under its operator header).

| Cell | "Hi, what can you do?" | "List the files in this directory." |
| :--- | :--- | :--- |
| as shipped: 16 tools, reminder, then system text | 5 of 6 | 0 of 6 |
| A: the reminder removed | 6 of 6 | 0 of 6 |
| B: the system text before the tools | 6 of 6 | 0 of 6 |
| A and B | 6 of 6 | 4 of 6 |
| C: 4 tools instead of 16 | 6 of 6 | 0 of 6 |
| no tools | 6 of 6 | 6 of 6 |
| shipped default, the rule also closing the user turn (chat endpoint) | | 6 of 6, and the tool call still follows |

What it says: the rule fades when the turn asks for a tool. On a turn that only talks, the 9B follows a system-side rule with all the tools attached; on a turn that wants a tool, it goes straight to `<tool_call>`. The number of schemas is not it; only taking out the reminder *and* putting the system text before the tools brings the rule partly back. A replacement template would buy 4 of 6 where the per-turn note already gives 6 of 6, so the note stays the default and no template ships. The 2026-10-01 row of 0 of 4 matches the task-shaped turn.

## Per model, not general (yet)

Every measurement on this page is Qwen3.5 (the 4B and the 9B, one family, one chat template). The tool-turn finding is recorded as a fact about the model, in the catalog entry's `notes` (`maic models info qwen3.5-9b-text`): "system rule ignored on tool turns: Qwen3.5 9B, measured 2026-10-02". It must be measured again on other families (and other templates) before anything general is built on it. If it holds for some families and not others, that pattern is the thing to look for, and the per-model record is where it goes.
