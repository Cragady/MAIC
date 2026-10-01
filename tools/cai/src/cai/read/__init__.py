"""cai read -- project a transcript to what was said. **It cannot write.**

**Owner, 2026-09-02:** *"`cai read` should be the first half of this part of
`reflow` it seems."*

**This exists because filing the projection under `reflow` gave it a writer.**
`reflow` is a family of things that TRANSFORM data, so its CLI writes results
back; the projection is not a transformation and had no business inheriting that.
The result destroyed a live transcript -- 5.27 MB of records replaced by 2.06 MB
of its own projection -- and every gate passed, because each gate was correct
about content surviving and none was asking whether the output belonged where the
input had been.

**Guards were added afterwards and they work. This is better than a guard.** A
reader has no write path to guard: the destructive operation is not refused, it
is unrepresentable. That is the top of the ladder rather than one rung down, and
it is the same move as making `build` unable to claim a root rather than making
it claim carefully.

**What it never emits, at any selection.** Thinking blocks. They are the largest
single component and their signatures do not validate outside the session that
minted them -- a correctness reason, so it does not relax when there is budget to
spare.

**The first half of what `reflow` was doing; the second half stays there.**
Rewrapping markdown is a genuine transformation -- same kind of data in and out,
verified by a content signature -- and keeps its write path honestly.
"""
