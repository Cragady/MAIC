"""`context` -- reflow the messages back into the current working context.

**Owner, 2026-09-03, and this is the definition the member is built to:**
*"I like `cai reflow context` because it gives the impression that the abstract
of the context is being reflowed. Not that the actual transcript files are being
modified lossily... In the compact chunk, when I ask for a reflow for context,
this is what I mean: reflowing the messages back into the current working
context."*

**WHAT IS BEING REFLOWED IS THE CONTEXT. The transcript is the SOURCE, not the
target.** That distinction is the whole member, and getting it backwards is what
destroyed a live transcript on 2026-09-02: the name said context and the code
operated on the file.

**The name was never the defect, and removing it was my over-correction.** A
reflow is *a change in data that changes its overall shape* -- owner's
definition -- and that is precisely what this does to a context: the same
messages, in a shape that fits somewhere they did not fit before. The transcript
is where they are read FROM.

**So the output goes to the caller, and the source is untouchable here.** No
in-place mode exists; `IN_PLACE` is False, so the family refuses to write results
back, and `write` refuses again on its own account. Both, deliberately: the
family-level rule protects every future member, and the member-level one
survives someone changing the family.

**One implementation, two names, which is an alias rather than a second home.**
`cai.read.reader` holds the projection. `cai read` names what you do to a
transcript; `cai reflow context` names what happens to your context. Different
objects, same mechanism, and the dictionary carries both under `read-operation`.
"""
from cai.read import reader

#: The transcript is read, never written. See the module docstring for why this
#: is a property of the operation rather than a guard bolted onto it.
IN_PLACE = False


def read(path):
    """Transcript data is JSONL; hand the family the raw text."""
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def write(path, data):
    """**Refuse.** The context is the target; the transcript is the source."""
    raise ValueError(
        "cai reflow context reflows the CONTEXT, not the transcript. %s is where the "
        "messages are read from, and writing the result back would replace the record "
        "with a view of it -- readable, and never resumable again. The output is for "
        "your context; use --to PATH to keep a copy elsewhere." % path)


def apply(data, mode="conversation", since_compaction=True, max_block=None,
          before_compaction=False):
    """Reflow a transcript's messages into the shape a context can hold.

    `mode` is the projection selection, so the mode axis here is the one already
    recorded as `projection-selection` rather than a second vocabulary.
    """
    text, rep = reader.read(data, select=mode, since_compaction=since_compaction,
                            before_compaction=before_compaction,
                            max_block=max_block)
    rep = dict(rep, member="context", mode=mode,
               changed=bool(text) and text != data,
               note=rep.get("note", "") + " The reflowed shape is for a context to hold; "
                                         "the transcript it came from is unchanged.")
    return text, rep
