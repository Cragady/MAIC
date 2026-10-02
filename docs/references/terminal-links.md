# Terminal links opening twice

Observed 2026-10-02 by Micaiah in Konsole with Firefox: a Ctrl+click on a link printed by Claude Code opened two tabs, both for a link she typed and for one the agent echoed. The same Ctrl+click on a link shown by nvim opened one tab. A click test in the browser showed no double-firing mouse (0 double-clicks, about one second between clicks).

**Cause.** Claude Code prints links as real terminal hyperlinks (the OSC 8 escape sequence). Konsole also detects URLs in plain text on its own. A Ctrl+click on an OSC 8 link is handled by both, so it opens twice. nvim prints plain text, so only Konsole's own detection fires.

**Fix on the terminal side.** In the Konsole profile, under Mouse, turn off "Allow escape sequences for links" (the label varies by version) and keep plain-text URL detection, which nvim's links need. Turning off plain-text detection instead keeps real hyperlinks but makes nvim's links unclickable.

**For MAIC.** MAIC's TUI prints no OSC 8 links today. If it starts to, it gets the `hyperlinks` setting described in the [roadmap](../roadmap.md), so a user whose terminal already detects URLs can keep links opening once.
