# Bans: what the model must not say

Three tools, which combine. All of them are configured in `settings.lua` under `bans`, on the command line, or live with `:ban` (see `:h ban`), and XTC with `:sampling` (see `:h sampling`).

| Tool | Works with | How |
| :--- | :--- | :--- |
| String bans (`bans.strings`, `--ban`, `:ban add`) | every provider | MAIC filters the streamed reply, cuts before the phrase reaches the screen, tells the model, re-asks; after `retries` it replaces the phrase |
| Regex bans (`bans.patterns`, `--ban-pattern`, `:ban pattern`) | every provider | the same, with a POSIX extended regex; the last `window` characters are held back while a match could still grow |
| Token bans (`bans.tokens`, `:ban token`) | OpenAI-compatible servers (llama.cpp, vLLM, LM Studio, ...) | `logit_bias` at minus infinity: the token can never be chosen, so the model takes another path |
| XTC (`sampling.xtc_probability`, `:sampling xtc`) | llama.cpp-based servers | a sampler that throws away the top choices, so stock phrases lose their head start |

## Files

Wherever a ban is given, `@path` stands for a file: one entry per line, blank lines and lines starting with `#` skipped, `~` expanded. `--ban @~/bans/phrases.txt`, `--ban-pattern @~/bans/tics.re`, `:ban add @file`, `:ban pattern @file`, `:ban token @file` (numeric lines are ids), and in settings `strings = { "@~/bans/phrases.txt" }`, `patterns = { "@~/bans/tics.re" }`. A missing file is an error at the point it is named.

## Why not constrained decoding

Grammar-guided or regex-guided decoding (GBNF on llama.cpp, guided regex on vLLM and friends) constrains the reply to *match* a grammar. Those engines have no complement and no negative lookahead, so "anything except a reply containing X" cannot be written down for them. That is why MAIC bans after the fact: the filter is provider-independent, it sees the exact text, and the model is told what it did so the retry can be different. The cost is one or more extra calls when a ban fires, which `retries` bounds.

## Writing the regex

One expression holding every tic a model is known for is the intended use. POSIX extended syntax: alternation `|`, groups `( )`, classes `[ ]`, `?` `*` `+` `{n,m}`; no `\b`, no lookahead. `:ban case off` (or `ignore_case = true`) makes it case-insensitive. Keep `window` longer than the longest phrase a pattern can match (64 characters by default); a match longer than the window can be partly on screen before the cut.

A starter set, meant to be edited down to what the model in front of you actually says:

```lua
bans = {
  ignore_case = true,
  patterns = {
    "^(certainly|sure|absolutely|of course)[!,.]",
    "as an ai( language model)?",
    "i('m| am) (sorry|afraid)[, ]",
    "i (cannot|can't|am unable to) (help|assist|provide|comply)",
    "(it('s| is) (important|worth) (to note|noting)|please note that)",
    "(i hope this helps|let me know if you (have|need))",
    "in (conclusion|summary),",
  },
  retries = 3,
  replacement = "",
}
```

`:ban list` shows what is in force; a pattern that does not compile is reported once at the next model call and left out.

## What the model sees

When a ban fires the reply so far (the clean part) is kept as the assistant's turn, followed by a system line: the phrase it started, the full list of bans, and the instruction to continue from exactly where it stopped. After `retries` cuts the filter switches to replace mode for that reply and the text streams through with the match replaced. The transcript records the clean text only.
