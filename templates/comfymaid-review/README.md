# comfymaid-review

A page for answering a long stretch of conversation at your own pace: the open decisions first, then every message, with a reply box under each paragraph and each list item. Adopted 2026-10-02 (Micaiah). The name comes from its first use, the ComfyMaid review, and now names the kind: a catch-up page for everything after a pointer in a conversation that has gone unaddressed.

## Use

```sh
python3 build.py example.json review.json     # split each message's text into blocks
python3 -c "import sys; p=open('page.html').read(); d=open('review.json').read().replace('</','<\\\\/'); open('out.html','w').write(p.replace('__REVIEW_DATA__', d))"
```

`example.json` shows the input: `issues` (each with `fields`; more than one field makes a form group) and `messages` (`who` is `you` or `claude`, `addressed` collapses and checks it off). `build.py` splits a message's text on blank lines, keeps ``` fences whole, and turns bullet lines plus the line above them into a list group whose header and items each get a box.

## Behaviour

* `misc` (optional, same shape as a message plus `at`, an ISO time) fills the Misc / Uncovered / Ungrouped section, sorted oldest first. An After Prompt box and a second Submit close the page, so the keyboard reaches Submit by tabbing from the last box.
* Every save carries `key`: a one-off definition of the data's layout (field id patterns, merges and their marker lines, `kept`, `addressed`, `submitted`). It holds until the next submit, which carries its own key, the same one unless the data is applied differently.
* Every box saves 500 ms after the last keystroke, one write at a time. Submit waits for any save in flight, then writes everything at once and marks the review submitted; editing afterwards clears the mark.
* State is one document, `review/<id>`: `answers` by field id, `addressed` overrides, `merges`, `kept`, `submitted`, `submittedAt`, `savedAt`. It is plain JSON for an agent to read.
* Combine boxes: pick boxes within one card and combine them. The combined text marks each part with a `── <field id> ──` line. Back to default splits it along those lines, edits included; if the lines were damaged, the boxes return to what they held before combining and the whole combined text is kept in a box beside the first one. Nothing typed is ever dropped.
* The store hands back frozen objects: the page copies anything it loads before using it, and on load it recovers any text the viewer restored into boxes that its state never received.
* Storage today is the claude.ai artifact runtime (`db`, with `comments` for the Submit notice) and falls back to the browser's own storage. MAIC's artifact creation will give it a file backend.

* Status: every card is open (amber), addressed (blue) or resolved (green), stored in `status`. A topic is resolved only after it is addressed and after the document is marked addressed; the document (`doc_status`) is resolved by the user or an agent once every topic is. When everything is addressed and a topic has grown new depth, it gets a new artifact.
* Every box collapses to a one-line preview, per box, per card or for the whole page. Boxes under text that asks nothing (the user's own messages, the agent's reports and lists) start collapsed; `build.py` marks each block `respond: true` only when the agent's text asks a question.
* Data Refresh: loads the latest answers and replies into the page without a reload (targeted hydration), never saving over newer data first; with no store, or if the load fails, it reloads the whole page. The History links each short reply to its full version in the page.
* One editor at a time: the editing view holds a 15 s lease (`review-lock/<id>`, renewed while visible); every other view is a live read-only viewer that never writes. Take over editing writes `review-lock-want/<id>`; the editor saves, shortens its lease and steps back. A hidden editor lets its lease lapse so the visible view takes over. A view that becomes the editor loads the latest data first. Found by Micaiah testing two views at once: whole-document saves from two editors overwrite each other.
* Sending notifies by default: Send to Claude saves a side prompt and tells Claude in one click (`comments.sendToClaude`); Save only leaves it unanswered by choice; Send all unanswered (N) tells Claude about every side prompt without a reply in one notice. When the view can't reach a Claude session, sending still saves and says why. The overall Submit notifies the same way when it can. Needs the full `comments` capability.
* Saves go through a queue: each waits for the one before it.
* Side Prompt floats: a fixed panel beside the page on wide screens, a chat button that opens a sheet on narrow ones. Its own Submit sends only it into `side_prompts`; one left unsent goes out with the overall Submit. The ↪ beside a box adds a `[[field id]]` reference.
* Feedback where the user is: the Side Prompt panel reports every action (references, sends, saves, failures), mirrors the save status, and shows a banner for any page error. A side prompt clears only once the store confirms it; otherwise its text returns to the box. Saves time out after 10 seconds so one stuck write cannot block the queue. `events` keeps the last 30 actions for diagnosis, and the page shows its version.
* References toggle (↪ reference / ↪ referenced), refuse an empty box, and show as chips with go and remove. History shows, per side prompt, the reply it followed, the prompt, and its reply, by id.
* Most Recent Reply: the agent sorts its answer to a side prompt into the page first, then writes it to `review-replies/<id>` (a document only the agent writes); the side panel shows it with a link to where it was sorted. The overall Submit shows no reply: it stages the document for resolution (`doc_status: "staged"`) and says so there instead.
* Mark for resolution: a checkbox before the final Submit; ticking it opens a confirm dialog (keyboard: Space, Confirm, then Enter or Space on Submit). It sets `resolve_requested`.
* The user's prompts and the agent's messages are told apart by card color and label.
* `improvements` and `todo` in the data render on the page itself, so each artifact carries its own improvement list.

## Splitting a page

The user ticks Pick for split on cards, chooses a relationship and requests the split; the request lands in the saved state's `splits` with `status: "requested"`. The agent then:

1. Builds a new page from the picked cards (same template, a new `id`), with `lineage.parent` set to this page and the relationship.
2. Republishes this page with `lineage.children` gaining the new page and `moved` mapping each moved card to the new page's URL, and sets that split's status to `done` in the store.
3. Records task and topic changes across the two in `lineage.deltas` whenever it syncs them; a page cannot read another page's store, so the agent keeps the deltas.

Relationships: `tight` (resolving the topics there resolves them here), `linked` (the parent cannot be resolved until the child is), `loose` (related only; each resolves on its own).

## Future

* Adding textareas (more boxes than the blocks give).
* More expressive references from the Side Prompt (ranges, excerpts, whole cards).
* The Ctrl+G bridge, behind a switch the page already shows and records (`flags.ctrl_g_bridge`, with "not built yet" beside it): sync on demand with a file under MAIC's state directory; flipping the switch clears the linked sections on both sides. The full version needs MAIC's own artifact system, where a watched file syncs both ways.
* A view of an artifact in MAIC's TUI with keychords for the same functions; nvim makes moving, yanking and editing several fields easier, and the `--bare` TUI may not reach full parity.
* `notes` in the data renders as a Notes list among the page's own docs (`~~strike~~` supported).

## Notes

* ~~Affair Prompt~~ `After Prompt`: a misreading worth keeping, given the tool is about to be renamed `maid`.
