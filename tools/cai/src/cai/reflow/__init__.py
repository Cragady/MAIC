"""cai reflow -- **operating on data to produce a certain result.** An OPEN family.

**Owner, 2026-09-02, in three corrections, each wider than the last:**

*"It's two different modes of a subset of what I would call `reflow`"* -- the
sentence and paragraph rules are modes of a SUBSET, not the whole of it.

*"It's not that an implementation missing under `reflow` means a proposal was
refused, it means that I want to keep reflow open to mean more than just
formatting lines"* -- absence is a fact about an installation, never a ruling on
what belongs.

*"`reflow` may mean operating on DATA to produce a certain result. Which does
contain the shape of formatting lines, but constricting it to just formatting
lines takes away its versatility."*

**So this is not a text-rewrapping package.** Text line-breaking is one shape of
the thing. A member may reshape JSON, records, a table, a transcript, a
directory listing -- anything where the *representation* changes and what the
data MEANS does not.

THE ONE INVARIANT
-----------------
**A reflow changes the SHAPE of data, not its CONTENT.** That is what separates
it from an edit, and it is the only thing every member must satisfy:

    apply(data, **opts) -> (data, report)
    report["signature_held"]   did the content survive the reshaping?
    report["changed"]          did the representation actually move?

**Each member defines its own signature; the family requires that one exists.**
For text it is whitespace- and blockquote-marker-insensitive, because a marker
is markup. For JSON it would be equality of the parsed structure. For records,
the record set. **A member that cannot say whether content survived is not a
reflow** -- it is an edit wearing the name, and one of the two originals here
was exactly that: it rewrote files with no check at all. **On a held signature
being false, a member returns its input unchanged.** Refusing by reporting
rather than by raising is deliberate: a caller reshaping many items needs to
know which one refused, not to lose the batch.

EXTENSION
---------
**Members are DISCOVERED, not enumerated** -- the same mechanism `cai` uses for
its own tools. A package registers one from its own distribution, so reflow can
come to mean more without anyone editing this file:

    [project.entry-points."cai.reflow.operations"]
    columns = "mypkg.columns"

`register()` does the same in-process, for a caller holding a module and no
distribution. A member may also expose `read(path)` / `write(path, data)` if its
data is not UTF-8 text; the default reader treats it as text, which is a
convenience of this CLI rather than a constraint of the family.

MEMBERS HERE
------------
    lines      how text is BROKEN into lines
               modes: sentence (one per line) | paragraph (one per line)

**`context` LEFT this family, 2026-09-02, and that is the fix rather than a
tidy-up.** Projecting a transcript is a READ, and filing it here handed it a
write path it should never have had -- which destroyed a live transcript.
It is `cai read` now, a tool with no way to write over what it reads.
**Every member remaining here transforms data into the same kind of data**,
which is what makes writing the result back a coherent thing to do at all.
"""
import sys
from importlib.metadata import entry_points

from cai.reflow import lines

GROUP = "cai.reflow.operations"
DEFAULT = "lines"

#: Members added at runtime, by `register`. Discovery reaches installed
#: packages; this reaches a caller that has one in hand and no distribution.
_REGISTERED = {"lines": lines}


def register(name, module):
    """Add a member in-process. Extension without a package, and without an edit here."""
    if not hasattr(module, "apply"):
        raise ValueError(
            "a reflow member must expose apply(data, **opts) -> (data, report); "
            "%r does not. The report must say whether the content signature held -- "
            "reshaping data without being able to prove the content survived is an "
            "edit, not a reflow." % name)
    _REGISTERED[name] = module
    return module


def discover():
    """name -> module, for every member reachable from this installation."""
    found = dict(_REGISTERED)
    eps = list(entry_points(group=GROUP))
    if not eps:
        # No installed distribution: the registry in pyproject.toml, as `cai` itself reads it.
        from cai import dispatch
        eps = list(dispatch.pyproject_entry_points(GROUP).values())
    for ep in eps:
        if ep.name in found:
            continue
        try:
            found[ep.name] = ep.load()
        except Exception as e:                                   # noqa: BLE001
            print("cai reflow: %s failed to load: %s" % (ep.name, e), file=sys.stderr)
    return found


def named_elsewhere():
    """Names notation records that this installation cannot provide.

    **Reported, never refused.** A name here is one the vocabulary knows and this
    machine has no code for -- the same thing `cai` says about an uninstalled
    tool. Whether it belongs to reflow was decided by whoever recorded it; this
    function only reports that it is not available to run.
    """
    try:
        from cai.notation import store as nstore
        for e in nstore.live(nstore.load()):
            if e["id"] == "reflow-operation":
                return tuple(s for s in e["symbols"] if s not in discover())
    except Exception:                                            # noqa: BLE001
        pass
    return ()


def read(module, path):
    """Load data for a member, letting it say how if its data is not text."""
    if hasattr(module, "read"):
        return module.read(path)
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def in_place(module):
    """May this member's output be written back over its input?

    **Default FALSE, deliberately, after the default TRUE destroyed a
    transcript.** A member that transforms text in place says so; a member that
    PROJECTS -- producing a different kind of data from what it read -- says
    nothing and is refused. **The safe answer has to be the one you get by
    saying nothing**, because the member that needed the guard was the one whose
    author had not thought about it.
    """
    return bool(getattr(module, "IN_PLACE", False))


def write(module, path, data):
    """Store data for a member. **Refuses to overwrite a source it may not.**"""
    if hasattr(module, "write"):
        return module.write(path, data)
    if not in_place(module):
        raise ValueError(
            "this reflow member projects rather than transforming in place, so its "
            "output may not be written over %s. Use --to PATH, or -n." % path)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(data)


def operation(name):
    """Resolve a member, or raise `NotAvailable` -- which is not a rejection."""
    found = discover()
    if name in found:
        return found[name]
    raise NotAvailable(name, sorted(found), name in named_elsewhere())


class NotAvailable(LookupError):
    """This installation has no code for that member. **Not a ruling on the name.**

    Carries `known`: whether notation records the name. Either way the remedy is
    to install or write the member, never to argue that reflow does not extend
    that far -- the family is open by construction and this exception says so.
    """

    def __init__(self, name, available, known):
        self.name, self.available, self.known = name, available, known
        where = ("recorded in notation, but no implementation is installed here"
                 if known else "not implemented here, and not recorded in notation")
        super().__init__(
            "reflow member %r: %s. reflow is an OPEN family -- it operates on data to "
            "produce a result, and line formatting is one shape of that. Add a member "
            "under the %r entry-point group, or call cai.reflow.register(). "
            "available here: %s" % (name, where, GROUP, ", ".join(available)))
