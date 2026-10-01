"""The dictionary: definitions, their symbols, and when each was decided.

**One definition may carry several symbols.** `⚑` and `(╯°Д°)╯︵ ┻━┻` are the
same marker written two ways, so lookup is many-to-one and a rename is an added
symbol rather than a new entry.

**Retirement is a lifetime, not a version.** Every definition carries the UTC
instant of the decision behind it; a retired one also carries a fallout period
after which it stops being served. That makes the question continuous -- *what
was live at this instant* -- with no era boundaries to place a document inside
and no version headers to declare.
"""
import datetime
import json
import os
import subprocess

from cai import source

HERE = os.path.dirname(os.path.abspath(__file__))

# SOPIA defines; cai enforces and fetches. The dictionary is a definition, so it
# lives there -- and this resolution order is the same one a hosted store will
# slot into, which is what makes that migration a no-op for callers: they were
# already asking cai rather than reading a file.
ENV_VAR = "CAI_NOTATION_DICTIONARY"
SOURCE_CANDIDATES = (
    os.path.expanduser("~/dev2/Cascade/SOPIA/docs/guides/notation-dictionary.json"),
)
FALLBACK_PATH = os.path.join(HERE, "dictionary.json")
DEFAULT_PATH = FALLBACK_PATH  # kept for callers that pass nothing and want the embedded set


def source_path():
    """Where the dictionary is being read from, and whether that is the source.

    Returns (path, is_source). The embedded copy is a fallback, not the source --
    it answers only when SOPIA cannot be reached, which is the case an outside
    adopter runs in permanently.
    """
    return source.resolve(ENV_VAR, SOURCE_CANDIDATES, FALLBACK_PATH)
DEFAULT_LIFETIME_DAYS = 30


def _parse(ts):
    if not ts:
        return None
    return datetime.datetime.strptime(ts.replace("Z", ""), "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=datetime.timezone.utc)


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def load(path=None):
    """Fetch the dictionary, preferring the source over the embedded fallback."""
    if path:
        return source.load("__none__", (path,), None, empty={"definitions": []})
    return source.load(ENV_VAR, SOURCE_CANDIDATES, FALLBACK_PATH,
                       empty={"definitions": []}, package="notation",
                       remote_path="docs/guides/notation-dictionary.json")


def expired(entry, at=None):
    """Whether a retired entry has aged past its fallout period.

    Time is a heuristic for disuse, never proof of it: an archived repository can
    outlive any fallout period. That is a stated trade rather than a hidden one.
    """
    retired = _parse(entry.get("retired"))
    if not retired:
        return False
    days = entry.get("lifetime_days")
    if days is None:
        days = DEFAULT_LIFETIME_DAYS
    return (at or now()) > retired + datetime.timedelta(days=days)


def live(doc, at=None):
    """Definitions in force at an instant.

    **Filtered on read, never deleted by the act of asking.** Cleaning during a
    lookup means a read that mutates, and two reads then disagree about the
    store. Physical removal is a separate, deliberate step.
    """
    at = at or now()
    out = []
    for e in doc["definitions"]:
        decided = _parse(e.get("decided"))
        if decided and decided > at:
            continue
        if expired(e, at):
            continue
        out.append(e)
    return out


STATUSES = ("in force", "candidate", "declined", "retired")


def status_of(entry):
    """What kind of thing this entry is, which is not the same as whether it resolves.

    **`declined` is the one worth having.** A marker that was considered and
    deliberately rejected still needs to resolve -- someone meeting it in old
    material should learn it was declined rather than find nothing -- but it must
    not read as available. `LOC` is the seeded case: live state that a written
    name cannot keep true, answered by request instead. Recording the refusal is
    what stops it being re-proposed.
    """
    if entry.get("retired"):
        return "retired"
    return entry.get("status", "in force")


def resolve(doc, symbol, at=None):
    """Every definition that claims this symbol at an instant.

    Returns a list, not one entry: a symbol may legitimately appear on a live
    definition and on a retired-but-unexpired one, and **collapsing that to a
    single answer is where a repurposed symbol reads as wrong rather than as
    ambiguous.**
    """
    return [e for e in live(doc, at) if symbol in e["symbols"]]


def symbols(doc, at=None, scannable_only=True):
    """Symbol -> definitions, for scanning material.

    **Not every definition names a text marker.** The confidence classes are the
    digits 1-5, meaningful only inside a map's grade field -- searching prose for
    them matched 321 times on the first run. A definition that names a value
    rather than a marker carries `scannable: false` and is left out of a scan,
    which is the scope question answered per entry instead of per caller.
    """
    index = {}
    for e in live(doc, at):
        if scannable_only and not e.get("scannable", True):
            continue
        for s in e["symbols"]:
            index.setdefault(s, []).append(e)
    return index


def is_stale(doc):
    """Whether this copy is the source, and if not, how far behind it is."""
    return source.stale(doc)


# Named by the owner 2026-09-01. It carries both halves: `frozen` is the contract
# -- immutable, one commit -- and `defaults` is what the contents are for, the
# values used when a source cannot be reached. A directory whose name states its
# own contract needs no separate rule saying what may go in it.
# Two frozen contexts, and they are not the same thing. `defaults-frozen` holds
# material that never had a source -- a test fixture, a reference artifact.
# `sync-frozen` holds dated snapshots OF a source, taken by `cai sync`. Both keep
# the one-commit contract; only the second can say what it is a copy of.
FROZEN_DIRS = ("defaults-frozen", "sync-frozen")
FROZEN_DIR = FROZEN_DIRS[0]

# **The two contexts do not need the same gates**, owner 2026-09-02.
#
# `defaults-frozen` holds material with NO source: a fixture, a reference
# artifact. Nothing can be compared against anything, so the only available
# evidence that it is what it was is that nobody touched it -- which is what the
# single-commit rule is. It is a proxy for immutability, used because there is
# no better witness.
#
# `sync-frozen` has a better witness. A snapshot states WHAT it is a copy of and
# WHEN it was taken, so its identity comes from its content rather than from its
# history. A second commit -- a move, a reformat, a directory rename -- does not
# change what the file claims to be, and failing it for that would be enforcing
# a proxy in the presence of the thing it was standing in for.
#
# So the single-commit requirement applies only where there is nothing else, and
# a snapshot is instead required to carry its provenance.
SYNC_PROVENANCE = ("syncedFrom", "synced", "name")


def _git(args, cwd):
    try:
        out = subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                             text=True, timeout=15)
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def _sidecar_status(path, result, frozen_dir):
    if True:
        # A file that cannot carry an internal stamp gets a SIDECAR: `<name>.frozen`,
        # itself JSON, itself frozen, and naming the file it vouches for. Without one
        # the rename bypass stays open for exactly this class -- a positive test
        # against the real fixture renamed it and the history gate passed it clean.
        result["claims_frozen"] = True
        side = path + ".frozen"
        result["sidecar"] = side if os.path.exists(side) else None
        result["stamp_gate"] = ("via sidecar" if result["sidecar"]
                                else "unavailable -- no internal stamp and no sidecar")
        if result["sidecar"]:
            try:
                with open(side, encoding="utf-8") as fh:
                    sd = json.load(fh)
                result["stamp"] = sd.get("frozen")
                result["stamped_name"] = sd.get("vouches_for")
                result["actual_name"] = os.path.basename(path)
                result["name_ok"] = result["stamped_name"] == result["actual_name"]
            except (OSError, ValueError):
                result["name_ok"] = None
        else:
            result["name_ok"] = None
        parts = os.path.abspath(path).split(os.sep)
        result["in_frozen_dir"] = any(f in parts[:-1] for f in FROZEN_DIRS)
        d = os.path.dirname(os.path.abspath(path))
        log = _git(["log", "--oneline", "--", os.path.basename(path)], d)
        if not log:
            result["note"] = ("no commit records this file, so nothing can check it at all "
                              "-- reported rather than passed")
            return result
        result["validated"] = True
        result["commits"] = len(log.splitlines())
        result["single_commit"] = result["commits"] == 1
        # A missing sidecar FAILS rather than downgrading. Downgrading on absence
        # was itself the bypass: renaming the file orphans its sidecar, the lookup
        # follows the new name, finds nothing, and falls back to the weaker check --
        # so the dodge worked exactly as before. Absence of evidence was being read
        # as absence of a requirement.
        # A file needing a sidecar cannot carry provenance internally, so the
        # history gate stays -- there is nothing else to witness it.
        result["context"] = ("sync-frozen" if "sync-frozen" in parts[:-1]
                             else "defaults-frozen")
        result["single_commit_required"] = True
        name_ok = result["name_ok"] is True
        result["kept"] = bool(result["in_frozen_dir"] and result["single_commit"] and name_ok)
        if result["kept"]:
            result["note"] = ("kept: one commit, inside the frozen directory, and a sidecar "
                              "vouches for this exact name")
        else:
            why = []
            if not result["in_frozen_dir"]:
                why.append("not under a %r directory" % frozen_dir)
            if not result["single_commit"]:
                why.append("%d commits touch this path" % result["commits"])
            if result["name_ok"] is False:
                why.append("its sidecar vouches for %r but the file is now %r -- a rename "
                           "orphans the sidecar, which is the bypass this requirement closes"
                           % (result["stamped_name"], result["actual_name"]))
            elif result["name_ok"] is None:
                why.append("no sidecar vouches for it. A file here that cannot carry its own "
                           "stamp REQUIRES one, because falling back to history alone is what "
                           "a rename exploits")
            result["note"] = "BROKEN: " + "; ".join(why)
        return result



def freeze_status(path, tolerance_seconds=900, frozen_dir=FROZEN_DIR):
    """Whether a frozen file kept its contract. Two independent gates.

    **Gate one, the stamp.** A freeze writes its instant into the file; git
    records the commit separately. Two records of one fact from different
    systems, neither able to forge the other, so a file edited after freezing
    shows a commit later than the stamp it carries.

    **Gate two, the history — and it is the stronger one.** A frozen file should
    have **exactly one commit touching its path**: the one that moved it into the
    frozen directory. A second commit is an edit, whatever the timestamps say.
    **This gate needs no clock**, which matters, because the stamp gate found its
    first real defect in a stamp written as local time labelled `Z` -- a clock
    error inside the clock check.

    **Two loose gates make one strong gate.** Neither is sufficient alone: a
    stamp can be wrong, and a single commit could still carry edited content if
    the freeze commit itself was wrong. Together they are hard to pass by
    accident.

    **`synced` is not `frozen`.** A copy that tracks a source is updated on
    purpose and has many commits; it reports staleness, not a freeze. Only a
    `frozen` field claims this contract.

    Reports; never repairs, and returns the deltas so a person can judge a
    borderline case rather than being handed a verdict.
    """
    result = {"path": path, "claims_frozen": False, "in_frozen_dir": False,
              "commits": None, "single_commit": None, "stamp": None,
              "committed": None, "delta_seconds": None, "validated": False}
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except OSError:
        result["note"] = "unreadable"
        return result
    except ValueError:
        # Not a JSON object, so it cannot carry an internal stamp. Gate one is
        # unavailable; the sidecar carries it instead.
        return _sidecar_status(path, result, frozen_dir)

    if doc.get("synced") and not doc.get("frozen"):
        result["note"] = ("carries a `synced` stamp, not `frozen` -- a copy that tracks a "
                          "source is updated on purpose and makes no freeze claim")
        return result
    result["stamp"] = doc.get("frozen")
    if not result["stamp"]:
        # It parsed as JSON but claims nothing. A sidecar may still vouch for it --
        # a one-record JSONL file is valid JSON, so the format alone cannot decide
        # which path applies. Fall through to the sidecar rather than concluding
        # from the parse.
        if os.path.exists(path + ".frozen"):
            return _sidecar_status(path, result, frozen_dir)
        else:
            result["note"] = "carries no freeze stamp, so it does not claim to be frozen"
            return result
    result["claims_frozen"] = True

    # GATE THREE: the file records the name it was frozen under.
    #
    # Without it the history gate has a bypass: RENAME a tampered file and its new
    # path has exactly one commit, which passes. Recording the name closes that --
    # the stamp names the old file and the path names a different one.
    #
    # It is also what makes re-freezing legitimate rather than a violation. A
    # frozen file can never take a second commit, so an update is a move plus a
    # name change: the new path carries one commit and holds the contract, while
    # the old path keeps its own. Append rather than mutate, the same shape used
    # everywhere else here.
    result["stamped_name"] = doc.get("name")
    actual = os.path.basename(path)
    result["actual_name"] = actual
    if result["stamped_name"] is None:
        result["name_gate"] = "unavailable -- the stamp records no name"
        result["name_ok"] = None
    else:
        result["name_ok"] = result["stamped_name"] == actual

    parts = os.path.abspath(path).split(os.sep)
    result["in_frozen_dir"] = any(f in parts[:-1] for f in FROZEN_DIRS)

    d = os.path.dirname(os.path.abspath(path))
    log = _git(["log", "--oneline", "--", os.path.basename(path)], d)
    if not log:
        result["note"] = ("no commit records this file, so its stamp is a claim nothing "
                          "can check -- reported rather than passed")
        return result
    result["validated"] = True
    result["commits"] = len(log.splitlines())
    result["single_commit"] = result["commits"] == 1

    committed = _git(["log", "-1", "--format=%cI", "--", os.path.basename(path)], d)
    if committed:
        result["committed"] = committed
        delta = (datetime.datetime.fromisoformat(committed) - _parse(result["stamp"])).total_seconds()
        result["delta_seconds"] = int(delta)

    stamp_ok = result["delta_seconds"] is not None and result["delta_seconds"] <= tolerance_seconds
    name_ok = result["name_ok"] is not False
    parts_dir = os.path.abspath(path).split(os.sep)[:-1]
    is_sync = "sync-frozen" in parts_dir
    result["context"] = "sync-frozen" if is_sync else "defaults-frozen"
    if is_sync:
        # Identity comes from the content, so the history gate is not applied and
        # the provenance fields are required instead.
        missing = [k for k in SYNC_PROVENANCE if not doc.get(k)]
        result["provenance_ok"] = not missing
        result["missing_provenance"] = missing
        result["single_commit_required"] = False
        result["kept"] = bool(result["in_frozen_dir"] and stamp_ok and name_ok
                              and not missing)
    else:
        result["single_commit_required"] = True
        result["kept"] = bool(result["in_frozen_dir"] and result["single_commit"]
                              and stamp_ok and name_ok)
    if result["kept"] and is_sync:
        result["note"] = ("kept: inside sync-frozen, stamp within tolerance, name matches, "
                          "and it says what it copied and when. The commit count is not "
                          "checked here -- identity comes from the content, not the history")
    elif result["kept"]:
        result["note"] = ("kept: one commit, inside the frozen directory, stamp within "
                          "tolerance, and the name it was frozen under matches")
    else:
        why = []
        if not result["in_frozen_dir"]:
            why.append("not under a %r directory, so it cannot claim the contract" % frozen_dir)
        if result.get("single_commit_required") and not result["single_commit"]:
            why.append("%d commits touch this path; a defaults-frozen file has one, the "
                       "move that froze it -- and nothing else can witness that it is "
                       "unchanged" % result["commits"])
        if result.get("missing_provenance"):
            why.append("a sync-frozen snapshot must say what it copied and when; missing "
                       "%s" % ", ".join(result["missing_provenance"]))
        if not stamp_ok:
            why.append("committed %s seconds after its stamp" % result["delta_seconds"])
        if result["name_ok"] is False:
            why.append("frozen as %r but is now %r -- a rename gives a tampered file a fresh "
                       "path with one commit, which is the bypass this gate closes"
                       % (result["stamped_name"], actual))
        result["note"] = "BROKEN: " + "; ".join(why)
    return result


def compact(doc, at=None):
    """Physically drop expired entries. Separate from reading, on purpose."""
    keep = [e for e in doc["definitions"] if not expired(e, at)]
    dropped = [e["id"] for e in doc["definitions"] if expired(e, at)]
    return dict(doc, definitions=keep), dropped
