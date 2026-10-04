# Naming ledger

Changes to the base naming protocol ([protocol.md](protocol.md)), oldest first. Time is a bare Unix timestamp (seconds since the epoch); nothing else.

| Unix time | Role | Change (`old` -> `new`) | Reasoning |
| :- | :- | :- | :- |
| 1791064578 | reader | `NULL` -> TheMadMaid, MadMaid; active MadMaid | The first DeepSeek maid and the reviewer, grouped as the base reader. |
| 1791064578 | writer | `NULL` -> BlindWriter, BlindMaid; active BlindWriter | The writer works without running anything; BlindMaid as the household form. |
| 1791064578 | overseer | `NULL` -> ComfyMaid, HeadMaid; active ComfyMaid | ComfyMaid as introduced by Micaiah; HeadMaid names the rank. |
| 1791065137 | all | `NULL` -> role-prefixed names `MM_`, `BM_`, `CM_` plus a personal name (proposed convention) | Several maids per role stay distinct, and the prefix carries the role; loose or no protocol is respected. |
| 1791065289 | all | `NULL` -> rule: a role spelled out in a name implies a single carrier | A second carrier of that role needs an inclusive name or a rename, reassignment or decommission of the first. |
| 1791065410 | writer | `NULL` -> compound names: qualifiers around a base role (BlindWriter = Blind + Writer) | The base role is the center; qualifiers allow or restrict around it; a name describes the profile, never grants it, and a mismatch is flagged. |
| 1791065491 | writer | `NULL` -> candidate RunningBlindWriter | Running + Blind + Writer: may run builds and tests, still reviewed by the reader; and the idiom says it too. |
| 1791065519 | writer | `NULL` -> candidate RunningBlindMaid | The household form of RunningBlindWriter, as BlindMaid is of BlindWriter. |
| 1791075572 | forks | `NULL` -> fork names and an independence symbol (proposed; symbol to choose) | `maid proto name` covers forks; a fork grown independent shows both its independence and its fork origin. |
