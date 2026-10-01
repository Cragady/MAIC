# Conventions

Rules that span tools rather than belonging to one.

**Project-wide rules already stated inside a tool's design document stay there and are pointed at, not copied.** Two sources of truth for one fact is the failure `transfairy/DESIGN.md` rejects by name, and a conventions file is the easiest place to create it by accident.

Already stated there, and in force everywhere: no multi-lettered short forms, and **an agent never answers a `y/N`** — a prompt under `--agent` becomes a hard stop that prints the exact re-invoke command.

## Agentic mode is a forced read

**`--agent` is the prime, and on its own it does not run.** It throws.

**The throw carries the question and the token to answer it.** The message ends by naming a second flag, and the run only proceeds when that flag is passed alongside `--agent`. An agent that did not read the message cannot produce the flag, so the question cannot be skipped, defaulted through, or answered from a guess about what the tool probably wanted.

**Every `--agent` invocation without the second flag throws.** Not the first one only. There is no state to remember and nothing that expires into a silent success.

For the salt tool the question is exposure — whether the caller has been exposed or suspects a leak — and each answer routes to different steps. The message names those steps and what the tool can do about each.

**Human mode needs none of this.** A person shown a prompt has already read it. The gate exists because an agent can emit a plausible next command without having read anything, and the flag is the cheapest available proof that it did.

**The rule is the shape rather than a request.** Nothing here asks an agent to read carefully. The only path forward runs through the text.

## The shared surface — duplicated deliberately

> **This section breaks the rule at the top of this file, on purpose.** Owner instruction, 2026-08-28: *"I want duplication if we put it in conventions. We can use the duplication as a todo list. I do not want to risk losing these again."*
>
> **The duplication is temporary and git is what retires it.** Once these are committed, history holds a copy that cannot be lost, and the duplication can collapse back to a pointer. Until then it is insurance against the material going missing a second time.
>
> **Do not tidy this away as an accident.** It is an exception with a reason and an author.

**Every rule below is stated in `transfairy/DESIGN.md` and applies to every tool in this suite.** Trans-fairy is the worked instance, not the owner — nothing in this table is specific to transcripts.

| Rule | trans-fairy | redact | diction | claude.nvim |
| --- | --- | --- | --- | --- |
| Single entry point, sub-commands | designed | applied | unknown | not applied |
| `flag → env → derived default → prompt` | designed | n/a | unknown | not applied |
| Non-TTY never prompts; fails with the hint | designed | applied | unknown | not applied |
| No multi-lettered short forms | designed | applied | unknown | not applied |
| Help family, most-verbose-wins silently | designed | n/a | unknown | not applied |
| Help flag plus non-help flag is a hard drop | designed | n/a | unknown | not applied |
| Empty-value flag prints its sub-command's help | designed | applied | unknown | not applied |
| `--yes` suppresses confirmation, never selection | designed | n/a | unknown | not applied |
| `--agent`: one stage per invocation | designed | n/a | unknown | not applied |
| `--agent`: exit 0 at each boundary | **recovered** | n/a | not applied | not applied |
| `--agent`: print the exact re-invoke command | designed | n/a | unknown | not applied |
| An agent never answers a `y/N` | designed | applied | unknown | not applied |
| Unsafe thing unreachable, not forbidden | designed | applied | unknown | not applied |
| Protection is destination-based, not path-name | designed | n/a | not applied | not applied |
| A backup copies and never consumes the original | designed | applied | unknown | not applied |
| Over-commit backups, hand-pick; filesystem counts | designed | applied | unknown | not applied |
| Working area and injection target are separate | designed | n/a | not applied | not applied |

**`unknown` is not `applied`.** It means nobody has checked, and it is on the list for exactly that reason.

**`n/a` is not `not applied`.** It means the rule does not apply to that tool at all — redact is a single-purpose script, not a sub-command suite, so the help family, the `--agent` staging, and working-area-vs-injection-target have nothing to govern there. Marking them `not applied` would read "should but hasn't"; `n/a` says "nothing to do here." Added 2026-08-28 after reading redact against every row.

**Every row that is not `applied` is the todo list.** No separate checklist exists and none should — a parallel list drifts out of step with the rules it tracks, which is the failure this whole table is already an exception to.

### Two rows worth saying out loud

**`exit 0 at each boundary` is marked `recovered`** because it was absent from `DESIGN.md` until 2026-08-28 and was pulled back out of a session transcript. It is the line the entire agentic flow rests on: without it a harness reads a stage boundary as a failure and either retries or aborts.

**`--yes` suppresses confirmation and never selection** is the row most likely to be got wrong by a later tool, because the distinction stays invisible until a wrong default does damage. Confirming an action the operator already chose is safe to suppress. Choosing which object to act on is not.
