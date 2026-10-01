"""Known-answer tests for `cai notation flush` -- two gates before a record may be deleted.

Run: python3 tests/test_flush.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.notation import flush  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


d = tempfile.mkdtemp()
BOARD = os.path.join(d, "board.md")
open(BOARD, "w", encoding="utf-8").write(
    "# The board\n\n### The emulated-origin marker\n\nDesigned, unbuilt.\n")

# --- what counts as an item ---------------------------------------------------
check("a list entry is an item", len(flush.items("- a thing\n")) == 1)
check("REGRESSION -- a heading is NOT an item by default",
      [e["text"] for e in flush.items("# A transient record\n\n- a thing\n")] == ["a thing"])
check("because the file's own title would otherwise block deletion forever",
      len(flush.items("# Title\n")) == 0)
check("records that list under headings can opt in",
      len(flush.items("## An item\n", headings=True)) == 1)
check("but the H1 title stays out even then",
      len(flush.items("# Title\n", headings=True)) == 0)
check("prose is never an item", len(flush.items("just a sentence.\n")) == 0)

# --- gate one: every item marked ----------------------------------------------
r = flush.check("- marked ▣ board.md :: Designed, unbuilt\n- not marked\n", root=d)
check("one unmarked item blocks deletion", r["safe_to_delete"] is False)
check("and it is named", r["unmarked"][0]["text"] == "not marked")
check("the reason says it fails toward keeping the file", "fails toward keeping" in r["why"])

# --- gate two: every mark verifies against the board --------------------------
ok = flush.check("- x ▣ board.md :: Designed, unbuilt\n", root=d)
check("POSITIVE CONTROL -- a verified claim clears both gates",
      ok["safe_to_delete"] is True)
check("and says so plainly", "safe to delete" in ok["verdict"])

bad = flush.check("- x ▣ board.md :: a phrase that is not there\n", root=d)
check("a mark whose phrase is not on the board FAILS", bad["safe_to_delete"] is False)
check("and says the stated phrase is missing",
      "stated phrase is not on that board" in bad["failed_claims"][0]["why"])

gone = flush.check("- x ▣ nosuch.md :: anything\n", root=d)
check("a mark naming a board that does not exist fails",
      gone["safe_to_delete"] is False)
check("with the dangling-pointer framing",
      "not there" in gone["failed_claims"][0]["why"])

nob = flush.check("- x ▣\n", root=d)
check("a mark that names no board and has no default does NOT pass",
      nob["safe_to_delete"] is False)
check("because an unverifiable claim is not a passing one",
      "cannot be checked" in nob["failed_claims"][0]["why"])
withdef = flush.check("- x ▣\n", root=d, default_board="board.md")
check("POSITIVE CONTROL -- a default board makes the same mark checkable",
      withdef["safe_to_delete"] is False or withdef["marked"] == 1)

# --- the wording problem the `::` syntax exists for ---------------------------
reworded = flush.check("- the emulated origin thing ▣ board.md\n", root=d)
check("matching the temp file's OWN wording fails when the board reworded it",
      reworded["safe_to_delete"] is False)
check("and the message says to state the landing phrase",
      "state the landing phrase" in reworded["failed_claims"][0]["why"])
stated = flush.check("- the emulated origin thing ▣ board.md :: Designed, unbuilt\n", root=d)
check("POSITIVE CONTROL -- stating it passes, which is why the syntax exists",
      stated["safe_to_delete"] is True)
check("and the report says which it matched on",
      "stated phrase" in str(stated))

# --- an empty record is not a flushed one -------------------------------------
empty = flush.check("# nothing\n\nprose only.\n", root=d)
check("a record with no items is NOT safe to delete", empty["safe_to_delete"] is False)
check("and the verdict distinguishes empty from flushed",
      "not the same as being flushed" in empty["verdict"])

# --- a landing phrase may be quoted, so the line can pass a form check --------
q = flush.check("- x ▣ board.md :: `Designed, unbuilt`\n", root=d)
check("REGRESSION -- a backtick-quoted landing phrase still matches",
      q["safe_to_delete"] is True)
check("POSITIVE CONTROL -- quoting does not make a wrong phrase match",
      flush.check("- x ▣ board.md :: `not there at all`\n", root=d)["safe_to_delete"] is False)
check("straight quotes are stripped too",
      flush.check('- x \u25a3 board.md :: "Designed, unbuilt"\n', root=d)["safe_to_delete"] is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
