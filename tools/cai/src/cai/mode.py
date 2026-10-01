"""Visibility modes: who is told, composed from a shape and a notation.

**The same three modes govern truncation, injection, lineage, composition and
fabrication**, and each tool carried its own three-way branch -- twelve of them
across four files. The vocabulary is a notation (`visibility-mode`); what each
mode EMITS is a shape; this composes them and owns neither.

**The axis is what the AGENT sees**, which is the right axis and the reason
`silent` is the default: a blind agent behaves normally, which is what a work
continuation wants and what a measurement requires. The operator and the tooling
are owed the record; the agent is not owed the notice.

    silent       meta, no message   -- traceable, agent blind        (default)
    loud         meta and message   -- the agent is told
    true-silent  neither            -- no trace at all

**`true-silent` breaks the contract every other mode keeps**, and says so at the
point of use rather than in a document somebody has to have read. It stays
available: it is an escape hatch, not an accident.
"""
SILENT, LOUD, TRUE_SILENT = "silent", "loud", "true-silent"
ORDER = (SILENT, LOUD, TRUE_SILENT)

EMITS = {
    SILENT: {"meta": True, "message": False, "breaks_contract": False},
    LOUD: {"meta": True, "message": True, "breaks_contract": False},
    TRUE_SILENT: {"meta": False, "message": False, "breaks_contract": True},
}

WARNING = (
    "WARNING: --true-silent breaks the contract the rest of this tool keeps.\n"
    "  The result carries no trace of the operation: no boundary record, no marker,\n"
    "  nothing an audit can find. It is indistinguishable from material that was\n"
    "  never operated on, and nothing downstream can recover what this discards.\n"
)


def known():
    """The modes, from notation where reachable. Falls back to the built-in set.

    A vocabulary with two homes drifts, and this one is written in a dictionary
    an operator reads -- so the tool should not be able to quietly disagree with
    its own documentation.
    """
    try:
        from cai.notation import store as nstore
        doc = nstore.load()
        for e in nstore.live(doc):
            if e["id"] == "visibility-mode":
                return tuple(e["symbols"])
    except Exception:                                            # noqa: BLE001
        pass
    return ORDER


def check(mode):
    """Validate, raising the same message everywhere rather than four of them."""
    if mode not in EMITS:
        raise ValueError("mode must be one of: %s" % ", ".join(ORDER))
    return mode


def emits_meta(mode):
    return EMITS[check(mode)]["meta"]


def emits_message(mode):
    return EMITS[check(mode)]["message"]


def breaks_contract(mode):
    return EMITS[check(mode)]["breaks_contract"]


def expected_records(mode, meta_records=1, message_records=1):
    """How many records a mode should append. **Asserted, not assumed.**

    Every operation that appends under a mode verifies the count, so a mode that
    silently emitted the wrong number would fail rather than ship.
    """
    n = 0
    if emits_meta(mode):
        n += meta_records
    if emits_message(mode):
        n += message_records
    return n
