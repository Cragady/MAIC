"""The reader's map -- one per repository, live, and never a second dictionary.

**Owner's design, 2026-09-01, across a long thread.** An audit that meets a
symbol which will not resolve builds a map of what it must have meant, so a
stretch of work that outlived its vocabulary stays readable.

FOUR RULINGS, IMPLEMENTED HERE
------------------------------
**Map first, then the tables.** Current-tables-first would return the PRESENT
meaning for PAST material: the symbol resolves, nothing announces a problem, and
the reader gets a confident wrong answer. Map-first means a miss degrades to
*unknown* instead of *silently wrong* -- the same fail-toward-the-safe-reading
shape as an envelope failing long.

**Symbol to symbol, not symbol to definition, wherever the meaning already
lives in the tables.** A restated definition is a snapshot and goes stale the
moment the real one moves. **An alias follows its target**, so a later
refinement reaches everything that ever used the old symbol, for free. It also
names a rename AS a rename instead of leaving two suspiciously similar
definitions for someone to reconcile.

**One map.** Not a pile of dated stamps. A frozen rating is fixed at its instant
and nothing will ever revise it -- it can be wrong for as long as it is kept,
with nothing saying so. **A live rating is falsifiable**: the next audit confirms
or degrades it, and a degrading rating IS the signal to re-derive. So counts move
in both directions and there is exactly one artifact. **A map that fails an audit
is discarded**, because a map that no longer reads the material is not a map.

**A nulled pointer means the vocabulary moved**, not that one link broke. So the
answer is a full audit around that symbol and a fresh derivation -- never
re-pointing the alias at whatever looks closest, which would be a guess dressed
as a fix.

THE STAMP, AND WHERE IT CAN BE VALIDATED
----------------------------------------
**Owner: the map notes where its timestamp can be validated, IF it can be.** An
internal stamp is written by the same party it describes, so on its own it is a
claim. A tracked file has a second, independent witness in its commit time, and
the map records how to reach it. **An untracked map says plainly that its stamp
cannot be validated** rather than presenting an unverifiable time as though it
were evidence -- the same distinction as a locally-signed grant against a
server-side signer.

WHAT IS NOT HERE
----------------
**Nothing promotes anything into the dictionary.** The map reports difference;
adoption is the operator's act. That is what stops a guess becoming a definition
by attrition, and it is the same discipline `state --audit` follows: report,
never repair.
"""
import json
import os
import subprocess

from cai.notation import store

NAME = ".cai-readers-map.json"

#: Where the material stands, which changes what the map is FOR.
#:
#: **archive** -- restore readability and leave the material alone. A finished
#: artifact records what was done; rewriting its symbols alters the record.
#: **live** -- restore readability, then update the symbols, because carrying
#: stale notation forward in worked-in material only defers the same audit.
#:
#: The map still never edits anything. In a live repository it reports that
#: updating is warranted; adoption remains the operator's act.
ARCHIVE, LIVE = "archive", "live"
STANCES = (ARCHIVE, LIVE)

#: The three ways a finding resolves. **Owner, 2026-09-01**, with the third
#: added on the observation that a definition which keeps producing apparent
#: misuse is more likely wrong than the material is.
UNKNOWN_SYMBOL = "unknown-symbol"
USAGE_AT_FAULT = "usage-at-fault"
DEFINITION_AT_FAULT = "definition-at-fault"


def classify(entry):
    """Which of the three a finding is, from the confidence tuple.

    **Owner, 2026-09-02, on why derivation is confident rather than a guess:**
    the re-root runs measured that *"even if details change, or if a fabrication
    around a detail is present, the overall shape still wins, because the shape
    is what produced what came after."* Run 2 arm 3 removed a ruling AND its
    reasoning and the subjects derived it anyway, from the artifacts the decision
    had shaped. Arm 4 planted an operator turn asserting the opposite and both
    subjects REJECTED it -- one diagnosing it by noticing the record's actions
    contradicted its words.

    **The same structure holds for a symbol.** Its meaning is evidenced by every
    place it was applied, not by its definition alone, so a large body of
    consistent usage is stronger evidence than the one line that claims to define
    it. That is what makes the third resolution tractable instead of a matter of
    taste:

        scattered deviation     many readings disagreeing with each other
                                -> the MATERIAL is sloppy
        consistent deviation    many readings agreeing with each other and
                                disagreeing with the definition
                                -> the DEFINITION is wrong

    **Both raise the error count identically**, which is why the tuple carries an
    arrow and not only a magnitude. A count alone cannot tell them apart, and
    they call for opposite responses.
    """
    conf = {cls: count for count, cls in entry.get("confidence", [])}
    if not entry.get("resolution") and not conf:
        return {"resolution": UNKNOWN_SYMBOL,
                "why": "a lookup miss -- cheap, certain, mechanical. No reading required, and "
                       "nothing here claims to know what it meant.",
                "needs": "derivation from the surrounding material"}
    consistent = conf.get(2, 0) + conf.get(4, 0)
    scattered = conf.get(3, 0)
    agreeing = conf.get(1, 0) + conf.get(5, 0)
    # **Agreement is weighed first, or a single stray occurrence indicts the
    # material.** `9 1  1 3` -- nine readings agreeing, one scattered -- is a
    # healthy entry, and an earlier version of this called it usage-at-fault
    # because it compared the deviations only against each other. Deviation is
    # a finding relative to how much agreement it sits in, not in isolation.
    if agreeing >= consistent + scattered:
        return {"resolution": None,
                "why": "the usage agrees with the inferred reading more than it departs "
                       "from it. Isolated departures are noise, not evidence of a second "
                       "meaning.",
                "needs": None}
    if consistent > scattered:
        return {"resolution": DEFINITION_AT_FAULT,
                "why": "the deviations agree with each other and disagree with the "
                       "definition. Many independent applications landing on the same "
                       "reading outweigh the one line that claims to define it -- the "
                       "same evidence structure that let a re-rooted session reject a "
                       "planted contradiction.",
                "needs": "a corrected definition, proposed to the operator"}
    if scattered >= consistent and scattered > 0:
        return {"resolution": USAGE_AT_FAULT,
                "why": "the deviations disagree with each other, so no alternative reading "
                       "is being evidenced. Inconsistency points at the material rather "
                       "than at the definition.",
                "needs": "the sites flagged, the definition left alone"}
    return {"resolution": None,
            "why": "the usage agrees with the inferred reading; there is no finding here.",
            "needs": None}

#: How an entry resolved. `alias` is preferred wherever the meaning already
#: exists in the tables, because it tracks its target instead of snapshotting it.
ALIAS, DEFINITION, UNRESOLVED = "alias", "definition", "unresolved"


def path(repo):
    return os.path.join(repo, NAME)


def _git(args, repo):
    try:
        r = subprocess.run(["git"] + args, cwd=repo, capture_output=True, text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def validation(repo):
    """Where this map's stamp can be checked -- or that it cannot be.

    Returns a dict, never None. **Absence of a witness is itself reported**, so a
    reader is never left to assume one exists.
    """
    tracked = _git(["ls-files", "--error-unmatch", NAME], repo)
    if not tracked:
        return {"validatable": False,
                "why": "the map is not tracked here, so nothing outside the file "
                       "witnesses its stamp. The time it states is a claim."}
    return {"validatable": True,
            "how": "git log -1 --format=%%cI -- %s" % NAME,
            "witness": "the commit that last touched the map",
            "why": "an internal stamp is written by the party it describes; the commit "
                   "time is recorded by something with no stake in it."}


def new(repo, audit_result, at=None, stance=LIVE):
    """A map from an audit. One per repository -- writing replaces."""
    at = at or store.now()
    return {
        "schema": 1,
        "repo": os.path.abspath(repo),
        "taken_at": at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "stance": stance,
        "stamp_validation": validation(repo),
        "legend": audit_result["legend"],
        "legend_taken_at": audit_result["legend_taken_at"],
        "entries": audit_result["entries"],
        "note": "One live map. It reports difference and never adopts; a rating that "
                "degrades on a later audit is the signal to re-derive, which a frozen "
                "stamp could never give.",
    }


def load(repo):
    """Read the map, **recomputing where its stamp can be validated.**

    Trackedness is a fact about the repository right now, not about the map's
    content, so storing it makes a snapshot that goes stale -- and it did: a map
    written before being committed kept reporting that nothing witnessed its
    stamp, long after a commit did. That is the very defect this design exists to
    close, reached from inside it.
    """
    try:
        with open(path(repo), encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError):
        return None
    m["stamp_validation"] = validation(repo)
    return m


def save(repo, mapping):
    """Write THE map. There is one, so this replaces rather than accumulating."""
    mapping["stamp_validation"] = validation(repo)
    with open(path(repo), "w", encoding="utf-8") as fh:
        json.dump(mapping, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    return path(repo)


def alias(mapping, symbol, target, confidence=None, inferred=True):
    """Point a symbol at another symbol whose definition already exists.

    **Preferred over copying a definition**, which snapshots and goes stale.
    Marked inferred by default: an alias derived by reasoning is a proposal, and
    a wrong alias is worse than an unresolved symbol because it resolves
    confidently with nothing downstream announcing that the match was a judgement.
    """
    for e in mapping["entries"]:
        if e["symbol"] == symbol:
            e["resolution"] = target
            e["kind"] = ALIAS
            e["inferred"] = bool(inferred)
            if confidence:
                e["confidence"] = confidence
            e["note"] = ("aliased to a symbol whose definition is live, so it follows "
                         "that definition rather than restating it")
            return e
    raise KeyError("no entry for %r in this map" % symbol)


def resolve(mapping, doc, symbol, at=None):
    """**Map first, then the tables.** A miss returns unknown, never a present meaning.

    The order is the whole safety argument. Tables-first hands back what the
    symbol means NOW for material written when it meant something else, and
    nothing in that answer announces a problem.
    """
    if mapping:
        for e in mapping.get("entries", []):
            if e["symbol"] != symbol:
                continue
            if e.get("kind") == ALIAS and e.get("resolution"):
                target = store.resolve(doc, e["resolution"], at)
                # Falsy, not None: `store.resolve` returns an EMPTY LIST for an
                # absent symbol. An `is None` test here never fired, so a nulled
                # pointer resolved as healthy -- the confident wrong answer this
                # map exists to prevent, inside the map. Caught by a control.
                if not target:
                    return {"via": "map", "kind": ALIAS, "target": e["resolution"],
                            "definition": None, "inferred": e.get("inferred", True),
                            "nulled": True,
                            "note": "the alias target no longer resolves. The vocabulary "
                                    "moved; re-derive from the material rather than "
                                    "re-pointing at whatever looks closest."}
                return {"via": "map", "kind": ALIAS, "target": e["resolution"],
                        "definition": target, "inferred": e.get("inferred", True),
                        "nulled": False}
            if e.get("kind") == DEFINITION and e.get("resolution"):
                return {"via": "map", "kind": DEFINITION, "definition": e["resolution"],
                        "inferred": e.get("inferred", True), "nulled": False}
            return {"via": "map", "kind": UNRESOLVED, "definition": None,
                    "inferred": False, "nulled": False,
                    "note": "carried by the map as unresolved, which is a finding rather "
                            "than a gap -- it was met and could not be read."}
    live = store.resolve(doc, symbol, at)
    if not live:
        return {"via": None, "kind": UNRESOLVED, "definition": None,
                "inferred": False, "nulled": False}
    return {"via": "tables", "kind": DEFINITION, "definition": live,
            "inferred": False, "nulled": False}


def verify(mapping, doc, at=None):
    """Does this map still read the material? **A map that fails is discarded.**

    Two failures, and they are different problems. A NULLED pointer means the
    vocabulary moved and a full audit around that symbol is warranted. A map
    whose entries no longer correspond to anything is not repaired -- there is
    one map, and a broken one is replaced by auditing again.
    """
    nulled, resolved_since = [], []
    for e in mapping.get("entries", []):
        if e.get("kind") == ALIAS and e.get("resolution"):
            if not store.resolve(doc, e["resolution"], at):
                nulled.append(e["symbol"])
        if e.get("kind") in (None, UNRESOLVED) and store.resolve(doc, e["symbol"], at):
            resolved_since.append(e["symbol"])
    ok = not nulled
    return {
        "ok": ok,
        "nulled_pointers": nulled,
        "resolved_since": resolved_since,
        "verdict": ("the map holds" if ok else
                    "the map FAILS -- discard it and audit again"),
        "why": ("a nulled pointer means the vocabulary moved rather than one link "
                "breaking, so the symbol needs a full audit and a fresh derivation"
                if nulled else
                "entries that resolve against the live tables again are candidates for "
                "dropping at dissolution; they are reported, not removed"),
    }


def findings(mapping):
    """Every entry, classified. **Reported; nothing is adopted or edited.**"""
    out = []
    for e in mapping.get("entries", []):
        c = classify(e)
        out.append(dict(c, symbol=e["symbol"], occurrences=e.get("occurrences", 0),
                        inferred=e.get("inferred", False),
                        confidence=e.get("confidence", [])))
    return out


def dissolution(mapping):
    """What the map is for once reading ends -- **and it differs by stance.**

    **Owner's distinction, 2026-09-01.** The map was one thing in my reading and
    is two in hers:

        archive   restore readability, LEAVE THE MATERIAL ALONE
                  a finished artifact records what was done, and rewriting its
                  symbols alters what it records

        live      restore readability, THEN update the symbols
                  carrying stale notation forward in worked-in material only
                  defers the same audit

    **Neither is performed here.** In a live repository this reports that
    updating is warranted and names the sites; the edit is the operator's act,
    which is the same rule that stops an inferred alias becoming a definition by
    attrition.
    """
    stance = mapping.get("stance", LIVE)
    found = findings(mapping)
    pending = [f for f in found if f["resolution"]]
    if stance == ARCHIVE:
        return {
            "stance": ARCHIVE,
            "action": "restore readability only",
            "may_edit_material": False,
            "why": "the material is a finished record. Its symbols are part of what it "
                   "records, so updating them would alter the artifact rather than "
                   "translate it. The map carries the meaning; the material stays as it is.",
            "findings": pending,
            "then": "drop the map, or adopt definition changes it proposes -- the operator's "
                    "act either way",
        }
    return {
        "stance": LIVE,
        "action": "restore readability, then update the symbols",
        "may_edit_material": True,
        "why": "the material is still being worked. Leaving stale notation in place defers "
               "this same audit to whoever reads it next, and the map that would have "
               "explained it is gone by then.",
        "findings": pending,
        "sites_to_update": sum(f["occurrences"] for f in pending),
        "then": "the update is the operator's act. This names what and where; it does not "
                "edit, for the same reason an inferred alias is never promoted.",
    }


def discard(repo):
    """Remove the map. Dissolution and failure both end here."""
    p = path(repo)
    if os.path.exists(p):
        os.remove(p)
        return p
    return None
