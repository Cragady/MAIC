# diction — open items

Tracked here rather than in conversation. One sentence per line; see the note at the bottom.

## Next

- [ ] **End-to-end voice mode switch, with a real voice.** Tap detection is characterised, but tap -> "diction normal" -> mode actually flipping has never run in a live session. This is the feature all the tap tuning was for.

## Small

- [ ] **Self-labelling logs.** A stdin reader for `taptest` so typed `TEST:` / `TEST-END` markers land in the log with timestamps. Today they exist only as shell echo in terminal capture, which made two runs ambiguous to read afterwards.
- [ ] **Onset saturation.** One physical event re-triggers every 80ms because the baseline is a 1-second median that stays low during short bursts. Harmless for fires, but it inflates onset counts and makes logs noisier than necessary. Fix is to require a rise against the immediately preceding frame rather than against a running median.

## Micaiah's calls, not mine

- [ ] **LICENSE file.** Without one the repo is "all rights reserved" by default, which matters only if it is ever shared. MIT would match the dependency stack.
- [ ] **Global gitignore** for `diction-logs/` and `.h-diction-logs/`. The repo's own `.gitignore` protects the tool, not the directories you dictate in, and those hold verbatim narration. Deliberately left alone.

## Legacy directories, left alone

Two `.diction/` directories predate the rename to `diction-logs/` and hold real narration from earlier sessions, including a 10.9KB log. Nothing writes to them any more and they are no longer covered by the repo's `.gitignore`. Not deleted, because the content is yours to judge: `NOTES/email-creation-process/another-test/.diction` and `.../other/.diction`.

## Known limitations, not being fixed

- **"Change row N to be verbatim" is ambiguous, and the agent reads it literally.** It replaces the passage with the word "verbatim" rather than restoring what was actually said. Phrase it as "make row N say exactly what I said" instead. The transcript is not stored pre-cleanup per passage, so there is nothing to restore from other than the raw log.


- **Severe wake-word garbling cannot be recovered.** "Dekchin Tago" scores 0.40 against "toggle", and the threshold cannot go that low without real dictation matching. The window stays armed for 20s, so the fix is to say it again rather than tap again.


- **The trailing quiet guard measures audible quiet, not absence of typing.** Keystrokes too soft to clear the onset floor pass straight through it, so a triple followed by quiet typing can still fire. Observed with ring-finger `ll` keystrokes, which never registered as onsets at all. Not worth tuning for: the spoken phrase is the second gate, and the raw logs record enough of the session to reconstruct what should have happened when the stars do align.

## Done

- [x] "Print row 4" prints row 4. It used to print the last four, because SHOW was the only directive and it meant "last n".
- [x] The agent can compose on request (WRITE), which the blanket "never invent content" rule had forbidden outright. Composed passages are marked in the terminal; the rule still forbids volunteering anything.

- [x] Dropped the tap's trailing guard entirely. A false arm is inert, so the guard was duplicating the spoken phrase's job while breaking the gesture it existed to serve.
- [x] Tap now has waiting / success / expired states on a transient status line, rather than a log line that arrived out of order.

- [x] Normal mode recognises commands that name a passage by number without an opening marker, which is how commands are actually spoken.
- [x] The scribe no longer follows its own precedent within a session. A garbled first command used to anchor the rest of the session into treating clean commands as content -- reproduced, then fixed.

- [x] Mode-switch phrases no longer leak into the scribe, where a failed switch could be rewritten as a document edit. Deferred instructions stored in the transcript still work.

- [x] Stable/beta split (`dist/` + `promote`), 2026-08-21.
- [x] Three taps rather than two, after measuring that pair spacings from typing and deliberate tapping overlap completely.
- [x] Trailing quiet guard, which discriminates where rhythm cannot.
- [x] `HF_HUB_OFFLINE=1` in the launcher, so a normal start makes zero outbound connections.
- [x] Insert mode no longer rewrites "diction" to "Dictation".
- [x] `--visual` renamed to `--clean`.
- [x] Markdown reflowed to one sentence per line, with `tools-reflow.py` to keep it that way.

**From 2026-09-03 that script CARRIES NO IMPLEMENTATION.** It resolves through a ladder: `cai reflow --mode sentence` if `cai` is installed, otherwise the newest frozen snapshot in `sync-frozen/`, otherwise it refuses and says which rung failed. **diction still requires nothing** — the snapshot is carried here, taken by `cai sync reflow-lines`, and states what it copied and when. **One canonical implementation, no dependency, and staleness measured rather than guessed.** `python3 tools-reflow.py FILE --check` compares the two rungs and reports AGREE or DIVERGED; verified 2026-09-03 that it reports DIVERGED when the snapshot is altered.

---

Markdown in this repo is written one sentence per line: a sentence that begins at `^` ends at `$`. Wrapped sentences break `grep`, and wrapping is cheap to reapply at read time. Run `python3 tools-reflow.py *.md` after editing.
