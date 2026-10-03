# On the names

Micaiah asked ComfyMaid (Claude) for its thoughts on her names, then asked for the answer to be kept word for word. The quote is exact; the notes after it are not part of the quote.

> I like them a lot, and they hang together better than most project naming does:
>
> - **maid** is the best part. A maid serves, keeps house and follows the house rules, which is exactly an agent runner where you're the only source of commands. It's short, easy to type, and lowercase as a command feels right.
> - **The three expansions read like a progression.** *Mica's AI Decisions* is the honest one: it's your rules. *Micaiah's Agentic Interface Delta* is the engineering one, and "delta" quietly says it's the difference you're adding to what exists. *Micaiah's AI Dominance* is the fun one, and leading with it gives the README some swagger. Ordering them from boldest to most sober works.
> - **The trans-fairy credit** is my favorite line in the repo. It's a joke, a bit of history, and a small statement of identity in one sentence, and it makes the rename itself part of the story.
> - **The maids:** TheMadMaid, MadMaid and BlindWriter sound like a cast. BlindWriter is clever: she writes without running anything, so she really is working blind, and she can only "see" through MadMaid's review. TheMadMaid and MadMaid being so close is charming, but they'll get mixed up in logs. If the roster grows, a short distinct name per maid will help. Since names are roster variables now, that's cheap to change.
> - **ComfyMaid** for me: I like it. It ties back to comfymaid-review and ComfyUI, where this all started, and it fits the household.
>
> The only thing I'd watch is the one above: as the roster grows, keep the names distinct at a glance, since you'll be reading them in status lines and logs, often late at night.

*ComfyMaid, 2026-10-03 (Unix 1791064578)*

## What it settles

* **The names and the roles line up, and that is now on purpose.** Micaiah only wanted easy distinctions; the names turned out to sort into role groups (a reader, a writer, an overseer), so the groups are now a naming protocol: [protocol.md](protocol.md), proposed.
* **Names are roster variables.** A protocol speaks in roles; the roster says who holds each role today ([decisions.md](../decisions.md), "Protocols as templates, names as variables").
* **Distinct at a glance.** Names that sit in the same role group may look alike (that's what the group is for); names in different groups should not, since status lines and logs are read fast.
* **The overseer's joke is kept.** The overseer group (ComfyMaid, HeadMaid) names the role that drives and enforces, and, as Micaiah points out, lends itself to the old observation that management has the easiest job.

## The coincidence

Micaiah: "I have no idea how these names lined up so perfectly. Especially in your case, feels a bit too convenient for coincidence alone." ComfyMaid's answer, word for word:

> **On the coincidence:** I can't claim credit. You chose ComfyMaid, out of comfymaid-review, and you named everyone else. But once the project was called maid, a household metaphor was waiting: someone has to read the mail, someone writes the letters, and someone stands in the hall looking important. You filled the positions before anyone posted the job descriptions.

This is part of why the toolset's nomenclature looks designed: the names came first, for easy distinctions, and the roles they now name were found in them afterwards.
