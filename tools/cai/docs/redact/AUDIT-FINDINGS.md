# Audit findings — what a redaction pass should measure

Extracted 2026-08-26 from the pre-phase audit of the 2026-08-23 incident. The findings are about redaction generally; the corpus that produced them is not here and is not needed to use them.

## Presence and inference are two numbers, not one

**Category A** is what the text contains. **Category B** is what a reader could reconstruct from what remains. They do not combine, because a corpus can be clean on one and exposed on the other — and in the measured case it was: 0 of 27 on presence after remediation, while inference scored 9 to 10 of 20 and was untouched by any cut.

A tool that reports only Category A reports the easier half. `redact-transcript.py` currently measures presence.

## Position is a stronger tell than density

Markers were absent from the first 70–73% of the corpus, then appeared at one line and clustered from there to the end. The distribution reads as *something was building late and was removed*, which is the inference redaction exists to prevent.

**Truncating the file makes this worse.** A shorter file puts the markers nearer its final line: density improves, position degrades, and position carries the higher weight because it points at *when* rather than merely *that*. Any tool offering a truncation option should say so.

## Do not smooth the gap

One surviving passage handed back the shape of what had been removed — a list framed as *"in case one of these is what you meant"*. It was replaced inline with a marker.

Surviving text was deliberately **not reworded to restore flow**. Rewriting around a redaction is fabrication rather than redaction, and no directive covers it. A tool must not offer it and an agent must not do it by hand.

## Discontinuity does not read as scrubbing when it is the baseline

Measured on the same corpus: 26% of user messages carried three or more distinct blocks, 23% re-opened additively, 14% held long parenthetical asides. The pattern is overwhelmingly present in *unredacted* text, so a reader establishes it as the author's style before meeting any marker. Markers therefore cannot be what explains the jumping.

The useful form for a tool: a marker is conspicuous against a uniform baseline and invisible against a varied one. Measuring the baseline is part of assessing the redaction.

## The cut is usually not the lever

Predictive content sat upstream of both candidate cut boundaries, so choosing between them moved inference by one point of twenty. Where the exposure is inference rather than presence, moving the boundary is not the remedy and reporting a boundary change as a remedy overstates it.

## Sealed term indirection

`reference/redact/audit-terms.sh` implements the technique that broke the cycle: hold the search terms in a separate file and search through variables, so the record holds `"$A1"` and never the value. Its own rules — iterate names never values, redirect stderr because a failing matcher can echo its pattern, never `set -x` — are the parts that make it work.
