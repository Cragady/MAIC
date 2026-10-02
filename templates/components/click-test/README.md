# click-test

A debug component: counts presses, releases, clicks, double-clicks, Ctrl+clicks and other buttons on its own button and test link, shows the time since the previous click, and flags two clicks under 80 ms apart from one press as a double fire. Made 2026-10-02 to check whether Micaiah's mouse double-fires (the double tabs turned out to be Konsole handling a link twice).

* In memory only: nothing is saved.
* × removes it from the page until the page reloads.
* Submit click sends the counts and an optional message to Claude as a comment, then clears the message; Claude's answer arrives in that comment thread. It has no backend of its own; a dev-debug template would give it one.

A page lists the components it carries in its data (`components: [{ id, title, removable }]`) and mounts them from a `COMPONENTS` table; `comfymaid-review` is the first page that does.
