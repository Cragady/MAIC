"""Known-answer tests for cai edit, with a positive control on every refusal.

Each refusal reproduces a failure from 2026-09-01. Run: python3 tests/test_edit.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from cai.edit import guard  # noqa: E402
from cai.edit import cli  # noqa: E402

fails = []
_total = [0]


def check(name, ok, detail=""):
    _total[0] += 1
    print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
    if not ok:
        fails.append(name)


d = tempfile.mkdtemp()
PY_SRC = '''import os


def alpha():
    return 1


def beta():
    return 2


def gamma():
    return 3
'''


# **The fixtures are TRACKED**, because real edits happen in repositories and the
# guard now requires a file to be recoverable before rewriting it in place. An
# untracked fixture would test the guard rather than the thing under test.
subprocess.run(["git", "init", "-q", d], check=True)
subprocess.run(["git", "-C", d, "config", "user.email", "a@b"], check=True)
subprocess.run(["git", "-C", d, "config", "user.name", "t"], check=True)


def write(name=("m.py"), body=PY_SRC):
    p = os.path.join(d, name)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(body)
    subprocess.run(["git", "-C", d, "add", name], check=True)
    return p


# --- an anchor that matches nothing ---
p = write()
r = guard.apply(p, "def delta():", "def delta2():")
check("REFUSES an anchor that appears zero times", r["ok"] is False)
check("and names it as what a silently failed edit looks like",
      "silently failed" in r["findings"][0]["detail"])
check("POSITIVE CONTROL -- the file is untouched", open(p, encoding="utf-8").read() == PY_SRC)

# --- an anchor that matches more than once ---
r = guard.apply(p, "    return", "    return")
check("REFUSES an anchor appearing more than once", r["ok"] is False)
check("and says it would hit more than it names",
      "more than it names" in r["findings"][0]["detail"])

# --- the real defect: a replacement that removes a function nobody mentioned ---
r = guard.apply(p, "def beta():\n    return 2\n\n\n", "")
check("REFUSES a replacement that removes an undeclared definition", r["ok"] is False)
check("and reports the delta it would have caused, which is what it guards",
      r["structure"]["delta"] == -1 and r["structure"]["kind"] == "definitions")
check("POSITIVE CONTROL -- the file is still untouched",
      open(p, encoding="utf-8").read() == PY_SRC)
check("POSITIVE CONTROL -- declaring the delta allows the same edit",
      guard.apply(p, "def beta():\n    return 2\n\n\n", "", expect_delta=-1)["ok"] is True)

# --- an ordinary edit that changes nothing structural ---
p = write()
r = guard.apply(p, "return 2", "return 22")
check("allows an edit that changes no structure", r["ok"] is True)
check("and the change actually landed", "return 22" in open(p, encoding="utf-8").read())

# --- markdown counts headings instead ---
mp = write("d.md", "# One\n\ntext\n\n## Two\n\nmore\n")
r = guard.apply(mp, "## Two\n\nmore\n", "")
check("markdown is guarded by heading count, not definitions",
      r["ok"] is False and r["structure"]["kind"] == "headings")

# --- dry run ---
p = write()
r = guard.apply(p, "return 1", "return 111", dry_run=True)
check("a dry run reports without writing",
      r["ok"] and "return 111" not in open(p, encoding="utf-8").read())

# --- the CLI ---
check("CLI exits 1 on refusal",
      cli.main([write(), "--old", "nope", "--new", "x"]) == 1)
check("POSITIVE CONTROL -- CLI exits 0 when it applies",
      cli.main([write(), "--old", "return 1", "--new", "return 9"]) == 0)
check("bare invocation prints the reference", cli.main([]) == 0)
check("the help says what finally held was a guard, not more care",
      "not more care" in cli.MAN_HELP)

shutil.rmtree(d, ignore_errors=True)
# --- the last gate: the result must still be what the file claims to be --------
_d = tempfile.mkdtemp()
_p = os.path.join(_d, "m.py")
_SRC = ("def a():\n    try:\n        x = 1\n    except OSError as e:\n        raise\n\n\n"
        "def b():\n    return 2\n")
open(_p, "w", encoding="utf-8").write(_SRC)
subprocess.run(["git", "-C", _d, "init", "-q"], check=False)
subprocess.run(["git", "-C", _d, "add", "-A"], check=False)

# The corruption that actually happened: a heredoc terminator where a block body
# belongs. The DEF COUNT is unchanged, so the delta gate accepts it.
_r = guard.apply(_p, "    except OSError as e:\n        raise\n",
                 "    except OSError as e:\nOLDEOF\n", expect_delta=0)
check("a corrupted replacement with an unchanged def count is REFUSED",
      _r["ok"] is False and _r["structure"]["delta"] == 0)
check("the refusal names the parse failure, not the counts",
      "would not parse" in _r["findings"][0]["detail"])
check("and the file is untouched", "OLDEOF" not in open(_p, encoding="utf-8").read())

_r2 = guard.apply(_p, "    return 2\n", "    return 3\n", expect_delta=0)
check("POSITIVE CONTROL -- a good edit still applies", _r2["ok"] is True)
check("and reports that it parses", _r2["parses"] is True)

open(_p, "w", encoding="utf-8").write(_SRC)
subprocess.run(["git", "-C", _d, "init", "-q"], check=False)
subprocess.run(["git", "-C", _d, "add", "-A"], check=False)
_r3 = guard.apply(_p, "    return 2\n", "    return 2\nOLDEOF\n", expect_delta=0)
check("STATED LIMIT -- corruption that is still valid syntax passes", _r3["ok"] is True)

_q = os.path.join(_d, "n.md")
open(_q, "w", encoding="utf-8").write("# h\n\ntext\n")
subprocess.run(["git", "-C", _d, "add", "-A"], check=False)
_r4 = guard.apply(_q, "text\n", "other\n", expect_delta=0)
check("a format with no parser here is n/a, not a finding",
      _r4["parses"] == "n/a" and _r4["ok"] is True)

_j = os.path.join(_d, "d.json")
open(_j, "w", encoding="utf-8").write('{"a": 1}\n')
subprocess.run(["git", "-C", _d, "add", "-A"], check=False)
_r5 = guard.apply(_j, '{"a": 1}', '{"a": 1,}', expect_delta=0)
check("POSITIVE CONTROL -- broken JSON is refused too", _r5["ok"] is False)

# --- the in-place writer is the one where content can be LOST -----------------
# The anchor, structure and parse gates prove the NEW content is well formed. None
# of them keeps a copy of the old. Git is the backup -- fine until the file is not
# tracked, and nothing was checking.
_ud = tempfile.mkdtemp()
_up = os.path.join(_ud, "u.py")
open(_up, "w", encoding="utf-8").write("def a():\n    return 1\n")
_ur = guard.apply(_up, "return 1", "return 2", expect_delta=0)
check("an UNTRACKED file is refused -- a rewrite there is unrecoverable",
      _ur["ok"] is False and _ur["recoverable"] is False)
check("and it is untouched", "return 1" in open(_up, encoding="utf-8").read())
check("the refusal says what would fix it",
      "Take a backup, or pass --force" in _ur["findings"][0]["detail"])
check("POSITIVE CONTROL -- --force is the operator's way through",
      guard.apply(_up, "return 1", "return 2", expect_delta=0, force=True)["ok"] is True)
subprocess.run(["git", "init", "-q", _ud], check=True)
subprocess.run(["git", "-C", _ud, "add", "u.py"], check=True)
check("POSITIVE CONTROL -- a TRACKED file needs no force; git IS the backup",
      guard.apply(_up, "return 2", "return 3", expect_delta=0)["ok"] is True)

print("\n%d checks, %d failed" % (_total[0], len(fails)))
sys.exit(1 if fails else 0)
