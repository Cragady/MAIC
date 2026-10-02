# Review form

A page for answering a long stretch of conversation at your own pace: the open decisions first, then every message, with a reply box under each paragraph and each list item. Adopted 2026-10-02 (Micaiah), first used for the ComfyMaid review.

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
* Storage today is the claude.ai artifact runtime (`db`, with `comments` for the Submit notice) and falls back to the browser's own storage. MAIC's artifact creation will give it a file backend.

## Future

* Adding textareas (more boxes than the blocks give).
