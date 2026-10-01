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
