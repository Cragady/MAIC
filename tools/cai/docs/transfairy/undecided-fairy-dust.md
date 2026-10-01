# Undecided — fairy dust

Takeaways that surfaced but were not promoted into `BUILDING-OF-TRANSFAIRY-STEPS.md`. Left here rather than dropped: each is real, none survived the selection cap (more than five candidates, so three were picked).

Promote any of these by moving it into the building doc with a number.

## How an entry is structured

Three parts, each a heading:

1. The item itself.
2. Why it was not picked — the deferral.
3. The foreclosure — the argument that closes it, so it does not return without something new behind it.

A foreclosure is not always measurable, and not always effective. It is a standing argument, not a proof.

## Rules governing foreclosures

The governance is `BUILDING-OF-TRANSFAIRY-STEPS.md` lesson 15a, and lives there. In short: an unaccepted foreclosure may be **replaced but never added to**; a human-authored one is modified only after consulting the human author; an agent-authored one an agent may replace on the strength of the replacement. The append prohibition is the load-bearing part — see 15a for why.

---

# Confirmation and selection are different acts

`--yes` means "do not ask me to confirm." It should never be read as "decide for me." Confirming an action the operator already chose is safe to suppress; choosing which object to act on is not.

## Why not picked

Already enforced operationally in `DESIGN.md` — `--yes` requires explicit `--graft-onto`. Lesson 12 covers the underlying move.

## Foreclosure

*Agent-authored, pending acceptance.*

This is an instance of lesson 12, not a peer of it. Making the unsafe selection unreachable is what produces the behavior; the confirmation/selection distinction merely describes the result. Recording the description alongside the cause creates two homes for one fact, which lesson 9 argues against directly.

---

# An error message should be the help text for that command

A required-value flag given no value prints its own sub-command's help as the error. One body of text serves the human and the agent, and it cannot drift from real behavior the way a bespoke error string does.

## Why not picked

A UX pattern already specified in `DESIGN.md`, not a lesson learned the hard way. Nothing in this session went wrong to produce it.

## Foreclosure

*Agent-authored, pending acceptance.*

The building doc records what construction taught. This rule was designed, not discovered — it entered fully formed and cost nothing. A doc that also collects good ideas that were simply had loses the property that makes it worth reading: every entry marking a place where reality pushed back.

---

# Credentials and identifiers do not survive a change of context

Exported `thinking` blocks carry a `signature` minted by claude.ai that will not validate elsewhere. Exported `tool_use` blocks name tools (`bash_tool`, `web_fetch`) that do not exist in the destination and would reach the API as undefined. Both had to be dropped or folded rather than replayed.

## Why not picked

Domain-specific to transcript work, and already recorded where it governs code — `DESIGN.md`, "Build behavior (settled)."

## Foreclosure

*Agent-authored, pending acceptance.*

Generalizing it yields a claim too vague to act on: "data from elsewhere may not apply here." The specific form is the useful form, and the specific form belongs next to the code it constrains. Abstraction would cost the entry its teeth without buying reach.

---

# State explicitly who you are not serving

"This tool does not design around users who will not read a marker." Deciding the boundary out loud is what stops a design from accreting defenses against progressively less plausible users.

## Why not picked

Already stated as a scope boundary in `DESIGN.md`. Promoting it would restate rather than add.

## Foreclosure

*Agent-authored, pending acceptance.*

The rule was already operative before anyone proposed writing it down — the operator used it in-session to reject their own directory-structure proposal. A principle demonstrably in force does not need teaching; recording it as a lesson would document a habit rather than transmit one.

---

# Log refusals as provisional, with the reasoning

A design refusal ("interspersion is merge, so it is refused") should be recorded as provisional and reopenable. This one was rejected by the operator, reopened by the scoped-token proposal, and only then genuinely resolved — and the resolution was better than the original refusal.

## Why not picked

Substantially covered by the "rejected alternatives recorded with their reasoning" device in lesson 10.

## Foreclosure

*Agent-authored, pending acceptance.*

The mechanism already makes refusals provisional by construction. A rejection filed with its reasoning is inherently reopenable, because the reasoning is the thing a counter-argument attaches to. Adding a rule instructing people to do what the mechanism already guarantees is the behavioral control lesson 12 says to prefer structure over.

---

# Check whether you are biased toward your own idea, out loud

The operator twice proposed something and then asked, unprompted, whether it was over-engineering — catching one of the two before any argument had been made against it.

## Why not picked

A property of how the operator worked rather than how the program was built, and lesson 10 already covers the working method. Written as a lesson it would also read as flattery, which is the wrong register for a doc meant to instruct.

## Foreclosure

*Operator-authored. Human precedence applies — consult before modifying.*

Lessons 16 and 16a foreclose this structurally. Self-policing for idea-bias is a behavioral control; routing late ideas into a backlog is a structural one, and lesson 12 says prefer the structural. The backlog does not require anyone to notice they are biased — it catches the late idea regardless of whether the author suspected themselves.

The operator notes this foreclosure is neither measurable nor effective in all cases. Recorded as a standing argument, not a settled one.

> **Incomplete.** The concrete instance behind 16a — which change was nearly proposed, and which protection it would have broken — is still outstanding.
