# File naming

**Match first, invent second.** When naming a source, backup, or destination file, match the idioms already present around it — the directory it lands in, the files it sits beside, the scheme its siblings use. Where there is no surrounding idiom, choose a name that is concise, informative, and numbered appropriately.

**While building this, this was determined:**

    <base>-<NNN>_<SS>-<what>.<ext>.bak

e.g. `ping-pong-SOP-000_01-extracted.json.bak`

- `-NNN` is the **source export chunk**, matching the zips it descends from (`conversations-000.zip`, `projects-000.zip`, `light_metadata-000.zip`). It records *which export*, not which step.
- `_SS` is the **pipeline step** that produced the artifact. `_01` = split from `conversations.json`, `_02` = built transcript.
- `-<what>` is a short word for what the step did; the real extension stays visible before `.bak`.
- The number names the step's **output, not the attempt** — a rebuild refreshes `_02` rather than advancing to `_03`.
- Files that never change (original sources) get no number at all.

**Precedence:** match the surrounding idiom -> if none, apply the scheme above -> if it does not fit, concise, informative, numbered.

*This scheme is itself an instance of the rule: `-000` was matched from the export zips, not invented.*

> **Resolved 2026-08-28.** Artifacts that do not descend from an export — re-homed transcripts, session transcripts — take no chunk number, and the scheme that covers them was adopted in practice and recorded in `DESIGN.md` while this file still called it open:
>
> - **plain name** — a living document, maintained
> - **`NN-` prefix** — a point-in-time log, one per step
> - **`NNN-` prefix inside a capture directory** — a frozen snapshot
>
> **The gap was never that the scheme did not exist.** It existed, was in use, and was documented in the file that does not own naming, while the file that does kept advertising the question as unanswered. Recorded here because that is the failure worth noticing, not the convention.
