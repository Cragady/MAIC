"""Grants: SOPIA defines them, cai evaluates them.

**A grant expiry is a dynamic fact and cannot live in a document read once at
session start.** `CLAUDE.md` is loaded at startup, so an expiry written there
reads as *active* for the life of the session regardless of the clock -- which is
why that heading went stale for twenty-nine hours while work proceeded against
it, and why leaving a grant out of that file was the correct application of the
static-versus-dynamic rule rather than a lapse.

**So the grant is asked for, not read.** The document keeps the pointer, because
a pointer is static. This computes the answer, because the answer is not.

**Keyed on the session name AND its actual `sessionId`.** The name is what an
operator reads and what carries lineage; the id is what makes the grant
unforgeable. Both are needed because they drift apart on purpose -- a slice
inside a name records where a conversation came from, while the `sessionId`
records which file this is.

**Which closes the re-root gap without a rule about re-roots.** A composed
session inherits its predecessor's context -- including the belief that a grant
is live, the dangerous half -- but is minted with a new `sessionId` and simply
does not match. The inheritance problem disappears into the key.
"""
import datetime
import glob
import json
import os
import contextlib
import re
import tempfile
import time

from cai import safewrite
from cai import source
from cai import tmp
from cai.grammar import maid as fmt

ENV_VAR = "CAI_GRANTS"
# **Grant instances are not tracked.** A grant is a fact about right now -- this
# session, this window -- and committing it accumulates every expired permission
# in git history forever. The SHAPE of a grant is documentation and lives in
# SOPIA; the instances live in cai's short-lived store with a TTL.
#
# **NO REPOSITORY PATH IS A CANDIDATE. Owner's ruling, 2026-09-23.**
#
# The tracked file was kept "during the move" and the move never finished, so a
# git-tracked `docs/grants.json` was still being SERVED nineteen days after its
# last grant lapsed -- and still accumulating every expired permission in history
# forever, which is the outcome the comment above says this design exists to
# avoid. The half-measure was doing the harm the full measure was written to
# prevent.
#
# `CAI_GRANTS` remains, because a caller that sets it is pointing at a store
# deliberately and may point it anywhere -- including, if someone insists, into a
# repository. What is gone is a repository path being found by DEFAULT.
#
# **AND THE STORE IS RUNTIME STATE, WIPED ON RESTART. Owner's ruling, 2026-09-23.**
#
# A grant is a fact about the next hour. Anything that outlives the machine's
# uptime is the wrong lifetime for it, and a TTL enforced in software is a
# promise this code has already failed to keep once -- the store's mtime TTL was
# defeated by its own writer.
#
# `XDG_RUNTIME_DIR` is preferred over the system temp directory, and the reason
# is ownership rather than taste. Both are tmpfs on this machine, but `/tmp` is
# `drwxrwxrwt root root` -- a shared namespace any local user can create a
# directory in. **For a PERMISSION store that is a forging surface**: pre-create
# the path and you choose what `cai` reads. `XDG_RUNTIME_DIR` is mode 700 and
# uid-scoped, which is the same wiped-on-restart property with that problem
# already solved. The temp fallback is uid-scoped and its ownership and mode are
# CHECKED rather than assumed.
#
# **On Windows the wiped-on-restart property does not hold** -- the per-user temp
# directory survives a reboot. So the TTL stays as a backstop rather than being
# retired, and the docstrings say which guarantee is doing the work where.
TMP_NAME = "grants.json"
SOURCE_CANDIDATES = ()
SESSIONS_GLOB = os.path.expanduser("~/.claude/sessions/*.json")
ROOT_VAR = "CAI_GRANT_ROOT"
TTL_HOURS = tmp.DEFAULT_TTL_HOURS


def project_name(path):
    """A filesystem-safe name for a path, in `.claude/projects` form.

    `/home/cragady/dev2/Cascade/SOPIA` -> `-home-cragady-dev2-Cascade-SOPIA`, and
    a dot becomes a dash the same way, so `~/.claude/jobs` gives `--claude-jobs`.
    Matching the client's convention is the point: an operator who can read one
    directory listing can read the other.
    """
    p = os.path.abspath(os.path.expanduser(path))
    return re.sub(r"[/\\.:]", "-", p)


def runtime_root():
    """The directory holding grant stores. **Gone when the machine restarts.**"""
    env = os.environ.get(ROOT_VAR)
    if env:
        return os.path.abspath(os.path.expanduser(env))
    xdg = os.environ.get("XDG_RUNTIME_DIR")
    if xdg and os.path.isdir(xdg):
        return os.path.join(xdg, "cai-tools", "grants")
    uid = os.geteuid() if hasattr(os, "geteuid") else os.environ.get("USERNAME", "user")
    return os.path.join(tempfile.gettempdir(), "cai-tools-%s" % uid, "grants")


def root_problem(d):
    """Why this directory must not be trusted, or None.

    **Checked, not assumed.** The temp fallback lives in a world-writable
    namespace, so a directory being there says nothing about who made it. A
    store somebody else can write is a store that can grant.
    """
    if not os.path.isdir(d):
        return None
    st = os.stat(d)
    if hasattr(os, "geteuid") and st.st_uid != os.geteuid():
        return "%s is owned by uid %d, not by you" % (d, st.st_uid)
    if os.name != "nt" and st.st_mode & 0o022:
        return "%s is writable by group or other (mode %o)" % (d, st.st_mode & 0o777)
    return None


def _make_private(d):
    """Create `d` and its parents so that nobody but the owner can write them.

    **`os.makedirs(mode=...)` applies the mode to the LAST component only** --
    every parent it creates gets the default masked by the umask instead. Under
    the temp fallback that leaves an intermediate directory group-writable,
    which is the one property this store cannot have.
    """
    root = runtime_root()
    for target in (os.path.dirname(root), root, d):
        if not target or target == os.path.dirname(target):
            continue
        if not os.path.isdir(target):
            os.makedirs(target, mode=0o700, exist_ok=True)
        if os.name == "nt":
            continue
        st = os.stat(target)
        if st.st_uid == os.geteuid() and st.st_mode & 0o022:
            os.chmod(target, 0o700)


def store_for(repo):
    """The store holding grants ABOUT one repository.

    **Partitioned by the repository, not by the session**, because `check --repo
    PATH` always knows the path and does not always know who is asking --
    resolving identity is the fragile step, and a lookup that depends on it
    inherits that fragility.
    """
    return os.path.join(runtime_root(), project_name(repo) + ".json")


def _read_store(path):
    """One store file, or no grants. **Never raises**, and stale reads as empty."""
    try:
        if tmp.is_stale(path, TTL_HOURS):
            return {"grants": []}
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError):
        return {"grants": []}
    return doc if isinstance(doc.get("grants"), list) else {"grants": []}


class BadTimestamp(ValueError):
    """An instant that cannot be read. **Not the same as no instant at all.**

    The distinction is the whole point. `None` means *no expiry recorded*, which
    a check reads as *does not expire*; a malformed `expires` collapsing to
    `None` would turn a typo into a permanent grant. So it raises, and `check`
    turns it into a denial naming the field.
    """


def _parse(ts):
    """Read a UTC instant, or refuse to.

    **The old parser was `strptime` with one exact format, and it RAISED** --
    out of `check` and into the caller, so a hand-written `+00:00` or a
    minute-precision stamp was an exception rather than a denial. That breaks the
    single property the rest of this module rests on: a peer session found the
    shape by reading the code and could not test it, and it reproduced first try.

    Now the ordinary ISO forms all read, and **anything left over fails closed**.
    """
    if not ts:
        return None
    try:
        d = datetime.datetime.fromisoformat(str(ts).strip().replace("Z", "+00:00"))
    except (ValueError, TypeError):
        raise BadTimestamp("cannot read %r as a UTC instant" % (ts,))
    return d if d.tzinfo else d.replace(tzinfo=datetime.timezone.utc)


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def source_path():
    return source.resolve(ENV_VAR, SOURCE_CANDIDATES, None)


def load(path=None, repo=None):
    """Fetch the grant record. **A missing source is NOT an error -- it is no grant.**

    Failing closed matters more here than anywhere else: an unreadable file must
    never read as permission -- and an unsafe DIRECTORY must not either, so an
    ownership problem returns no grants with a note rather than raising.

    With `repo`, only that repository's store is read. Without one, every store
    is merged, which is what a listing wants.
    """
    if path:
        doc = source.load("__none__", (path,), None, empty={"grants": []})
    else:
        env = os.environ.get(ENV_VAR)
        if env and os.path.exists(env):
            doc = source.load(ENV_VAR, (), None, empty={"grants": []})
        else:
            root = runtime_root()
            bad = root_problem(root)
            if bad:
                doc = {"grants": [], "_path": root,
                       "_note": "refusing to read grants: %s" % bad}
            else:
                paths = ([store_for(repo)] if repo
                         else sorted(glob.glob(os.path.join(root, "*.json"))))
                merged, origins = [], []
                for sp in paths:
                    d = _read_store(sp)
                    if d.get("grants"):
                        origins.append(sp)
                    for g in d["grants"]:
                        merged.append(dict(g, _origin=sp))
                doc = {"grants": merged, "_path": ", ".join(origins) or root}
    doc.setdefault("grants", [])
    if doc.get("_note"):
        doc["_note"] = doc["_note"] + "; treating as no grant"
    return doc


def current_session(sessions_glob=None, cwd=None, session_id=None):
    # Resolved at CALL time, not bound at definition. A default argument is
    # evaluated once when the function is defined, so a caller or a test that
    # rebinds the module attribute would have no effect on it -- which is a
    # defect in testability, and the kind that makes a check unverifiable rather
    # than merely awkward.
    """The session this process is running under, if it can be determined.

    **An explicit id wins, because cwd is a bad proxy for identity.** A session
    is not where the shell happens to be: asking about repository A while sitting
    in repository B failed to identify the session at all and DENIED a grant that
    was in force. It failed closed, which is the safe direction -- but a wrong
    refusal is the thing that spends adoption fastest, and a permission system
    that answers differently depending on the working directory teaches people to
    route around it.

    So `CAI_SESSION_ID` (or an explicit argument) is consulted first, and cwd
    matching remains the fallback for callers that cannot say. **A stated id is
    still only a claim** -- it is matched against the session records rather than
    trusted, and an id naming no record resolves to nothing rather than to
    something invented.

    **But it can name a DIFFERENT real session, and that is worth stating.**
    Anything able to set an environment variable can present as any session on
    this machine and pick up its grants. That is not a hole this layer can
    close: a local grant file is written by the same party it authorises, so it
    was always organisational rather than attested, and the signing authority
    that would fix it has to be server-side. **The env var widens who can claim,
    not what a claim is worth.** It also silently overrides what a test seeds,
    which is how it was found -- a suite that passed alone and failed in a loop
    that exported it.
    """
    # A MAID session has no registry entry; a running one is known by the pid its last
    # `start` record names. Consulted beside the client's records, for the default glob only.
    maid_rows = fmt.live_sessions() if sessions_glob is None else []
    sessions_glob = sessions_glob or SESSIONS_GLOB

    stated = session_id or os.environ.get("CAI_SESSION_ID")
    if stated:
        # **A full id IS an id; only a prefix needs looking up.** The session
        # records are the CLIENT's bookkeeping, not cai's -- this session's own
        # vanished from ~/.claude/sessions while it was still running, and
        # requiring a lookup made a correctly-stated id unusable.
        #
        # This lived in `cai commit`'s CLI first, so `cai commit` and
        # `cai grant check` gave OPPOSITE answers about the same session and the
        # same grant. **An invariant belongs with the function both callers
        # share**, not with one of them -- the third instance of that today.
        if len(stated) == 36 and stated.count("-") == 4:
            return {"sessionId": stated, "name": None,
                    "note": "stated as a full id; no session record was consulted"}
        for p in glob.glob(sessions_glob):
            try:
                with open(p, encoding="utf-8") as fh:
                    d = json.load(fh)
            except (OSError, ValueError):
                continue
            if d.get("sessionId", "").startswith(stated):
                return d
        for d in maid_rows:
            if d["sessionId"].startswith(stated):
                return d
        return None

    cwd = cwd or os.getcwd()
    best = None
    records = []
    for p in glob.glob(sessions_glob):
        try:
            with open(p, encoding="utf-8") as fh:
                records.append(json.load(fh))
        except (OSError, ValueError):
            continue
    for d in records + maid_rows:
        if d.get("cwd") and cwd.startswith(d["cwd"]):
            if best is None or len(d["cwd"]) > len(best.get("cwd", "")):
                best = d
    return best


USE_STATE = os.path.expanduser("~/.claude/cai-grant-uses.json")


def _uses(path=None):
    try:
        with open(path or USE_STATE, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


class UseNotRecorded(RuntimeError):
    """A spend that could not be written down. **This must deny.**"""


LOCK_TIMEOUT = 5.0
LOCK_STALE = 15.0


@contextlib.contextmanager
def _use_lock(p, timeout=None, stale=None):
    """Exclusive hold on the use-state file, or raise.

    **Bounds are resolved at CALL time, not bound at definition.** Writing
    `timeout=LOCK_TIMEOUT` in the signature evaluates it once when the module
    loads, so rebinding the module attribute -- which is the only way to test
    the stale path without waiting fifteen seconds -- has no effect. `current_session`
    in this same file carries a comment warning about precisely this, and it was
    written again three hundred lines below it. **A knob a test cannot turn is a
    branch nothing verifies.**

    **Read-modify-write on a file shared by every session on the machine is a
    lost update waiting to happen**, and two sessions checking the same one-time
    grant both saw zero spends and were both granted. Two or three agents now
    work these repositories at once, so this stopped being theoretical.

    A stale lock is broken rather than waited on forever: a crashed holder must
    not wedge the gate, because **a gate that hangs is a gate somebody turns
    off.** Breaking one reopens the race for an instant, which is strictly
    better than never granting again.
    """
    timeout = LOCK_TIMEOUT if timeout is None else timeout
    stale = LOCK_STALE if stale is None else stale
    lock = p + ".lock"
    deadline = time.time() + timeout
    fd = None
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            break
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lock) > stale:
                    os.unlink(lock)
                    continue
            except OSError:
                pass
            if time.time() >= deadline:
                raise UseNotRecorded("could not lock %s within %.1fs" % (lock, timeout))
            time.sleep(0.02)
        except OSError as e:
            raise UseNotRecorded("could not lock %s (%s)" % (lock, e))
    try:
        yield
    finally:
        os.close(fd)
        try:
            os.unlink(lock)
        except OSError:
            pass


def _record_use(gid, path=None):
    """Consumption is ENFORCEMENT state and lives on the cai side, not in the grant.

    SOPIA defines what was granted; how many times it has been spent is a fact
    about enforcement, not about policy. Keeping it out of the grant file also
    keeps that file a pure statement of intent -- readable, diffable, and not
    rewritten by the act of asking.

    **A failure to record RAISES, and the caller denies.** It used to be
    `except OSError: pass`, so an unwritable use-state meant the grant was
    granted and the spend was not written down -- **a one-time grant silently
    becoming unlimited**, which is the one direction this module must never
    fail in. Reproduced by making the file read-only: `granted: True,
    recorded: {}`.

    The write is atomic, because truncating in place loses every spend on
    this machine if it is interrupted -- and every consuming grant then reads
    as unspent.
    """
    p = path or USE_STATE
    try:
        os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    except OSError as e:
        raise UseNotRecorded("cannot reach %s (%s)" % (p, e))
    with _use_lock(p):
        d = _uses(p)
        d[gid] = d.get(gid, 0) + 1
        tmp_path = p + ".cai-new"
        try:
            fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(d, fh, indent=2)
            os.replace(tmp_path, p)
        except OSError as e:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
            raise UseNotRecorded("could not record the spend in %s (%s)" % (p, e))
    return d[gid]


def grant_id(g):
    """The key consumption is recorded under. **It must distinguish every grant.**

    It was `repo|sessionId|granted`, which does NOT include the action -- so two
    grants issued in the same second, for the same repository and session, shared
    one key. Found in practice, on the first one-time grant ever issued: a
    commit-and-push pair where checking the commit half marked the push half
    spent, and the push was refused as `spent -- checked 1 time(s)` having never
    been checked at all.

    **It failed closed, which is the safe direction and is why it was survivable**
    -- but a spend is enforcement state, and a key that collides silently
    revokes a permission somebody was granted.
    """
    return "|".join([str(g.get("repo")), str(g.get("sessionId")), str(g.get("granted")),
                     str(g.get("action") or ""), str(g.get("effect") or "allow"),
                     ",".join(g.get("paths") or [])])


def legacy_grant_id(g):
    """The key spends were recorded under before the action was part of it.

    **Changing a key is a migration of enforcement state**, and this one was not
    treated as one: grants already spent under the old key read as UNSPENT under
    the new, so a used one-time grant became available again. Observed, not
    theorised -- the commit half of a one-time pair sat readable as in force
    after it had been used.

    The old key cannot say WHICH sibling was spent; that ambiguity is the bug it
    caused. So a legacy record counts against every grant that would have
    produced it -- the original over-broad behaviour, preserved only for records
    written under the original key. **That is the fail-closed direction**: it can
    deny something that was never used, and it can never permit something twice.
    """
    return "%s|%s|%s" % (g.get("repo"), g.get("sessionId"), g.get("granted"))


def spent_count(g, use_state=None):
    """How many times this grant has been checked, legacy records included."""
    u = _uses(use_state)
    return max(u.get(grant_id(g), 0), u.get(legacy_grant_id(g), 0))


def path_covered(declared, touched):
    """Is every touched path inside the declared scope? Returns `(ok, offenders)`.

    A declared entry covers itself and anything beneath it, so `docs/` scopes a
    directory and `docs/x.md` scopes one file. **Comparison is on path SEGMENTS,
    not on string prefixes** -- `docs/grant-shape.md` must not be covered by a
    grant naming `docs/grant`, which a `startswith` would wave through. That is
    the same substring-for-identity mistake this module has now made four times
    in other places.
    """
    dec = [d.strip("/").split("/") for d in declared if d.strip("/")]
    bad = []
    for t in touched:
        parts = t.strip("/").split("/")
        if not any(len(d) <= len(parts) and parts[:len(d)] == d for d in dec):
            bad.append(t)
    return (not bad), bad


def _applies(g, repo, session_id, at, paths, use_state):
    """Does this grant apply, on its own terms? Returns `(ok, why_not, extra)`.

    **Pure: it decides nothing about other grants and it spends nothing.**
    Consumption is the one irreversible thing a check does, so it happens once,
    to the grant that actually answers -- not to every grant that was looked at.
    """
    try:
        exp = _parse(g.get("expires"))
        started = _parse(g.get("granted"))
    except BadTimestamp as e:
        return False, ("UNREADABLE -- %s. This grant cannot be evaluated and does not "
                       "apply; fix the instant or reissue it." % e), {"malformed": True}
    if session_id is None or g.get("sessionId") != session_id:
        # The id is what makes it unforgeable. A name alone would be inherited by
        # anything wearing that name -- which is what a re-root does. Truncating
        # BOTH sides hid a prefix-vs-full mismatch, printing two different values
        # as though they were one.
        got, want = (session_id or "unknown"), (g.get("sessionId") or "?")
        if got[:8] == want[:8] and got != want:
            detail = ("names session %s and this is %s -- the same prefix, different "
                      "values. A prefix is not an id." % (want, got))
        else:
            detail = "names session %s; this is %s" % (want[:8], got[:8])
        return False, "a grant exists for %s but %s" % (repo, detail), {}
    if g.get("paused"):
        # PAUSED is not expired. Expiring a grant by moving its window into the
        # past would destroy the record of what was granted; a pause suspends it
        # and leaves the window, the scope and the reason intact.
        return False, ("PAUSED%s -- the grant stands but is suspended; its window and "
                       "scope are unchanged"
                       % (" (%s)" % g["pausedNote"] if g.get("pausedNote") else "")), \
               {"paused": True}
    if g.get("paths") and paths is not None:
        ok_p, bad_p = path_covered(g["paths"], paths)
        if not ok_p:
            return False, ("this grant covers only %s, and the change touches %s"
                           % (", ".join(g["paths"]), ", ".join(bad_p[:4]))), {"outside": bad_p}
    if started and at < started:
        return False, "grant does not begin until %s" % g["granted"], {}
    if exp and at > exp:
        mins = int((at - exp).total_seconds() // 60)
        return False, "EXPIRED %s -- %d minutes ago" % (g["expires"], mins), {}
    uses = g.get("expiresAfterChecks")
    if uses is not None:
        spent = spent_count(g, use_state)
        if spent >= uses:
            return False, ("spent -- this grant expired after %d check(s) and has been "
                           "checked %d time(s)" % (uses, spent)), {"spent": spent}
    return True, None, {}


def check(doc, repo, session_id=None, session_name=None, at=None, use_state=None,
          action=None, paths=None, consume=True):
    """Is a grant in force for this repository, this session, right now?

    Returns a verdict dict. **Every path that is not an affirmative match returns
    denial**, including every error path -- there is no branch where uncertainty
    becomes permission.
    """
    at = at or now()
    verdict = {"repo": repo, "session_id": session_id, "session_name": session_name,
               "at": at.strftime("%Y-%m-%dT%H:%M:%SZ"), "granted": False,
               "source": doc.get("_path"), "reason": None, "grant": None}
    if not doc.get("grants"):
        verdict["reason"] = doc.get("_note") or "no grants recorded"
        return verdict

    # **"I cannot tell who you are" is not "this grant is for someone else."**
    #
    # With no id, every grant failed the id comparison and the verdict read
    # `names session b5d42ad9; this is unknown` -- which a reader acts on by
    # concluding the grant belongs to another session and giving up. The truth
    # was that the question could not be answered at all, and the fix is one
    # flag away.
    #
    # It is not a rare path. `current_session` falls back to matching cwd, and
    # the ORDINARY shape of this work -- a session rooted in repository A,
    # asked about repository B -- matches nothing. A peer session hit it on its
    # first attempt, against a workflow this repository's own CLAUDE.md
    # prescribes.
    if session_id is None:
        verdict["identity"] = "unresolved"
        verdict["reason"] = (
            "IDENTITY UNRESOLVED -- cai could not determine which session is asking, so "
            "no grant can match. This is not a mismatch and not a denial of anything in "
            "particular: the question was unanswerable. Pass --session-id ID, or set "
            "CAI_SESSION_ID. cwd matching fails whenever a session is rooted in one "
            "repository and asking about another.")
        verdict["grants_for_repo"] = sum(1 for g in doc["grants"] if g.get("repo") == repo)
        return verdict

    candidates = [g for g in doc["grants"] if g.get("repo") == repo]
    # An action-scoped grant answers only about the action it names. An UNSCOPED
    # check asks about the repository generally, so a prohibition on one action
    # must not deny it -- otherwise a narrow deny silently blocks every unrelated
    # question, which is the mirror of the bug where a broad allow overrode a
    # narrow deny.
    #
    # But it is not silent either: the verdict names any action-scoped grants in
    # play, so a caller that asked generally knows to ask specifically.
    # **A PATH-SCOPED grant answers only a question that names paths**, the same
    # way an action-scoped one answers only a question that names an action. A
    # caller that did not say what it is touching has not asked a question a
    # narrow grant can answer, and letting it match would turn "only this file"
    # into "anything, if you do not mention it".
    if paths is None:
        pscoped = [g for g in candidates if g.get("paths")]
        candidates = [g for g in candidates if not g.get("paths")]
        if pscoped:
            verdict["path_scoped_grants"] = sorted(
                {"%s:%s" % (g.get("effect", "allow"), "|".join(g["paths"])) for g in pscoped})
    scoped = [g for g in candidates if g.get("action")]
    if action is not None:
        candidates = [g for g in candidates if g.get("action") in (None, action)]
    else:
        candidates = [g for g in candidates if not g.get("action")]
        if scoped:
            verdict["action_scoped_grants"] = sorted(
                {"%s:%s" % (g.get("effect", "allow"), g["action"]) for g in scoped})

    # EXPLICIT DENY WINS, and it is evaluated first.
    #
    # Found by the owner's own test: a repository-wide allow matched before the
    # action-scoped deny was ever reached, so a broad permission silently
    # overrode a specific prohibition. In a permission system that is the worst
    # available ordering -- the narrower, more deliberate statement lost to the
    # broader one, and nothing reported the conflict.
    #
    # Denies first, and within each effect the action-scoped before the
    # repository-wide, so the most specific statement is the one that answers.
    #
    # And within one specificity band, the MOST RECENT window answers. Two
    # repository-wide grants -- an original and an extension appended beside it
    # -- were otherwise separated by file order alone, so a check after both had
    # lapsed reported the OLDER expiry: `EXPIRED 07:23 -- 66 minutes ago` when
    # the live window had ended at 08:23, six minutes earlier. Both statements
    # true, describing different grants; the same shape as the bug where the
    # numbers came from one grant and the reason from another.
    def _recency(g):
        # **A grant with no expiry is UNBOUNDED, not recent.** Sorting it as
        # `"9999"` made it the newest thing in the file forever, so a dead
        # open-ended grant from three weeks ago outranked a live one issued
        # minutes earlier and answered in its place -- the verdict naming a
        # different grant from the one that mattered, which is the failure this
        # sort was added to fix. An entry with no window is ranked by when it
        # was granted, which is the only date it actually has.
        return (g.get("expires") or g.get("granted") or "", g.get("granted") or "")

    # Two passes, and the order matters: recency first, then specificity. Python's
    # sort is stable, so the specificity pass preserves recency WITHIN each band
    # rather than scrambling it.
    candidates.sort(key=_recency, reverse=True)
    candidates.sort(key=lambda g: (0 if g.get("effect") == "deny" else 1,
                                   0 if g.get("action") else 1))
    if not candidates:
        verdict["reason"] = "no grant recorded for %s" % repo
        return verdict

    considered = []
    applying = []

    def _fail(reason, g, **extra):
        """Record why one grant did not answer, without letting it overwrite the rest.

        Candidates are already ordered most-specific first, so the FIRST failure
        is the one a caller most likely means. Reporting the last examined -- the
        earlier behaviour -- named whichever grant happened to sort last, which
        described a different grant from the one the numbers came from.
        """
        entry = {"reason": reason, "grant": g}
        entry.update(extra)
        considered.append(entry)

    # **EVERY candidate is evaluated, and a grant's authority is its own terms.**
    #
    # This loop used to return on the FIRST grant that matched, which made the
    # sort order part of the answer -- and every ordering bug in this module came
    # from that: a broad allow returning before a narrow deny was reached, a
    # verdict whose numbers came from one grant and reason from another, a dead
    # open-ended grant sorting as newest forever and answering in a live grant's
    # place. Each was fixed by adjusting the sort, which is fixing the symptom:
    # **a permission system whose answer depends on file order has no stable
    # answer at all.**
    #
    # So: ask every candidate whether it applies, on its own terms and with no
    # regard for where it sits. Then combine. Nothing is skipped because
    # something else answered first, and nothing is blocked by a neighbour.
    for g in candidates:
        ok, why, extra = _applies(g, repo, session_id, at, paths, use_state)
        if ok:
            applying.append(g)
        else:
            _fail(why, g, **extra)

    if applying:
        denies = [g for g in applying if g.get("effect") == "deny"]
        allows = [g for g in applying if g.get("effect") != "deny"]

        # **A DENY still wins over an ALLOW**, and now it wins because it is a
        # deny rather than because it sorted first. An explicit prohibition is a
        # statement; the absence of one is silence.
        chosen_set = denies or allows

        # **Do not spend a one-time grant when a standing grant already covers
        # it.** Consumption is the only irreversible thing a check does, so the
        # grant that answers is the one that costs nothing where there is a
        # choice. This is a preference between grants that ALL apply, so it
        # changes what is charged, never whether permission is given.
        def _cost(g):
            """Cheapest first: standing before consuming, then most specific."""
            return (1 if g.get("expiresAfterChecks") is not None else 0,
                    -len(g.get("paths") or []),
                    0 if g.get("action") else 1)

        chosen = min(chosen_set, key=_cost)

        uses = chosen.get("expiresAfterChecks")
        if uses is not None:
            if not consume:
                verdict["would_spend"] = True
            else:
                # **If the spend cannot be written down, the grant does not
                # apply.** An unrecorded use is an unlimited grant.
                try:
                    verdict["spent"] = _record_use(grant_id(chosen), use_state)
                except UseNotRecorded as e:
                    verdict["granted"] = False
                    verdict["grant"] = chosen
                    verdict["reason"] = ("REFUSED -- this grant is spent by being checked, "
                                         "and the spend could not be recorded (%s). An "
                                         "unrecorded use is an unlimited grant, so it "
                                         "fails closed." % e)
                    return verdict
                verdict["consumed_by_this_check"] = True

        verdict["grant"] = chosen
        if len(applying) > 1:
            verdict["also_applies"] = [
                {"action": g.get("action"), "effect": g.get("effect", "allow"),
                 "paths": g.get("paths"), "expires": g.get("expires")}
                for g in applying if g is not chosen]
        if denies:
            verdict["granted"] = False
            verdict["effect"] = "deny"
            verdict["reason"] = ("DENIED explicitly%s%s"
                                 % (" for action %r" % chosen.get("action")
                                    if chosen.get("action") else "",
                                    " -- %s" % chosen["note"] if chosen.get("note") else ""))
            return verdict
        verdict["granted"] = True
        verdict["reason"] = "in force until %s" % chosen.get("expires",
                                                             "(no expiry recorded)")
        if session_name is not None and chosen.get("sessionName") != session_name:
            verdict["note"] = ("id matches but the recorded name %r differs from %r -- the "
                               "name may have been changed deliberately; the id is what "
                               "authorises" % (chosen.get("sessionName"), session_name))
        return verdict

    if considered:
        first = considered[0]
        verdict["reason"] = first["reason"]
        verdict["grant"] = first["grant"]
        for k in ("paused", "spent"):
            if k in first:
                verdict[k] = first[k]
        if len(considered) > 1:
            verdict["also_considered"] = [
                {"reason": c["reason"],
                 "action": c["grant"].get("action"),
                 "effect": c["grant"].get("effect", "allow")} for c in considered[1:]]
    return verdict


# ---------------------------------------------------------------------------
# ISSUING
#
# **The half that was never built.** `check` has existed since 2026-09-01 and
# answers well; there has never been a way to WRITE a grant, so every grant this
# system has ever evaluated was hand-authored JSON. That is not a convenience
# gap. The party holding the file open is the grantee, so the only route to
# being unblocked ran through the blocked party -- found by a peer session that
# was told to commit, was correctly refused, and discovered that the documented
# way to obtain permission did not exist.
#
# One of the three hand-written grants already carries the cost in its own note:
# a start instant typed a minute into the future, so the grant silently did not
# apply and the wrong one answered.
# ---------------------------------------------------------------------------

REF_SHAPE = re.compile(r"^[0-9a-f]{6}$")
MIN_PREFIX = 8


def _sessions(sessions_glob=None):
    out = []
    for p in glob.glob(sessions_glob or SESSIONS_GLOB):
        try:
            with open(p, encoding="utf-8") as fh:
                d = json.load(fh)
        except (OSError, ValueError):
            continue
        if d.get("sessionId"):
            out.append(d)
    if sessions_glob is None:
        # Running MAID sessions, keyed by their id: a grantee and a caller like any other.
        out.extend(fmt.live_sessions())
    return out


def resolve_target(spec, sessions_glob=None):
    """Which session is being granted to? Returns `(record, error)`.

    **The grantee is not the caller**, which is what makes this different from
    `current_session`. Three identifier namespaces coexist on this machine and
    only one of them authorises:

      sessionId   the full uuid in ~/.claude/sessions  -- this is the key
      name        what an operator reads               -- not unique
      ListAgents ref   a short hex tag                 -- NOT an id prefix

    **The ref is the trap.** A peer reported its ref as `f6b1b6` against a
    sessionId beginning `0b1e9641` -- unrelated strings. An operator pasting a
    ref from a `ListAgents` row would write an authorising key for no session,
    or for an unrelated one whose id happens to start those six characters. So a
    bare six-hex argument is REFUSED by shape, and the refusal says why.

    **It refuses on ambiguity and on no match rather than inventing either.**
    """
    spec = (spec or "").strip()
    if not spec:
        return None, "no session named"
    if REF_SHAPE.match(spec):
        return None, (
            "%r is six hex characters, which is the shape of a ListAgents ref -- and a ref "
            "is NOT a prefix of a sessionId. Writing it would key the grant to nothing, or "
            "to an unrelated session. Pass the session NAME, or at least %d characters of "
            "its actual sessionId from ~/.claude/sessions/." % (spec, MIN_PREFIX))
    records = _sessions(sessions_glob)
    if len(spec) == 36 and spec.count("-") == 4:
        for d in records:
            if d.get("sessionId") == spec:
                return d, None
        # A stated full id IS an id. The session records are the client's
        # bookkeeping and a live session's record has vanished from them before.
        return {"sessionId": spec, "name": None,
                "note": "stated as a full id; no session record matched it"}, None
    named = [d for d in records if d.get("name") == spec]
    if not named:
        named = [d for d in records if (d.get("name") or "").lower() == spec.lower()]
    if len(named) == 1:
        return named[0], None
    if len(named) > 1:
        return None, ("%r names %d live sessions: %s. A name is not unique -- pass the "
                      "sessionId." % (spec, len(named),
                                      ", ".join(d["sessionId"][:8] for d in named)))
    if len(spec) >= MIN_PREFIX:
        pre = [d for d in records if d["sessionId"].startswith(spec)]
        if len(pre) == 1:
            return pre[0], None
        if len(pre) > 1:
            return None, "%r is a prefix of %d sessions; be more specific" % (spec, len(pre))
    return None, ("no session matches %r. Names and ids come from "
                  "~/.claude/sessions/*.json -- `cai grant sessions` lists them." % spec)


def issue_store(target=None, repo=None):
    """Where a newly issued grant lands: **runtime state, per repository.**

    Wiped when the machine restarts, which is the guarantee that replaces
    trusting a TTL nothing enforced.
    """
    if target and target not in ("tmp", "repo"):
        return os.path.abspath(os.path.expanduser(target))
    env = os.environ.get(ENV_VAR)
    if env and target is None:
        return env
    if target == "repo":
        raise ValueError(
            "`--store repo` is gone. Grant INSTANCES are not tracked in any repository: "
            "committing one accumulates every expired permission in git history forever, "
            "and a tracked store was still being served nineteen days after it lapsed. The "
            "SHAPE of a grant is documentation and belongs in SOPIA; the instances belong "
            "in the ephemeral store. Point CAI_GRANTS at a path if you need another one.")
    if repo:
        return store_for(repo)
    return os.path.join(runtime_root(), TMP_NAME)


def append(grant, path, force=False):
    """Add one grant to a store. **Append, never replace.**

    Returns `(path, kept_count, lapsed_count)`. A store that cannot be parsed is NOT
    overwritten -- it is refused, because replacing it would silently drop every
    grant it holds, including ones currently in force.
    """
    doc = {"schema": 1, "grants": []}
    token = safewrite.stat_token(path)
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as fh:
                doc = json.load(fh)
        except ValueError as e:
            raise ValueError(
                "%s exists but is not readable JSON (%s). Refusing to write, because "
                "replacing it would drop every grant it holds." % (path, e))
        if not isinstance(doc.get("grants"), list):
            raise ValueError("%s has no `grants` list; refusing to write over it" % path)
    # **PRUNE WHAT HAS ALREADY LAPSED, or the write resurrects it.**
    #
    # The ephemeral store's TTL is enforced on the FILE's mtime, so a store older
    # than the TTL is ignored wholesale and fails closed. Appending rewrites the
    # file -- which refreshes that mtime and brings every stale grant inside it
    # back into force. Found on this function's first real use: one issued grant
    # revived ten from nineteen days earlier, and the next verdict cited one of
    # them as its reason.
    #
    # An entry with no `expires` (a consuming grant) has no clock to judge, and a
    # malformed one is KEPT rather than dropped -- `check` denies it and says so,
    # which is visible, where a silent deletion is not.
    at = now()
    before, lapsed = [], 0
    for g in doc["grants"]:
        try:
            exp = _parse(g.get("expires"))
        except BadTimestamp:
            before.append(g)
            continue
        if exp and at > exp:
            lapsed += 1
            continue
        before.append(g)
    doc["grants"] = before + [grant]
    # **Absent is a state, not a failure to read one.** `stat_token` returns
    # None for a file that is not there, and feeding that to `unchanged_since`
    # reads as *could not stat* -- so the first grant ever issued was refused by
    # the race check protecting the file it was creating. The two cases need
    # different questions: a file that existed must be unchanged, a file that did
    # not must still not exist.
    if token is None:
        if os.path.exists(path):
            raise ValueError("%s was created while this grant was being prepared. Refusing; "
                             "re-run and its contents will be preserved." % path)
    else:
        ok, why = safewrite.unchanged_since(path, token)
        if not ok:
            raise ValueError("%s changed while this grant was being prepared (%s). Refusing; "
                             "re-run and the new contents will be preserved." % (path, why))
    d = os.path.dirname(path) or "."
    bad = root_problem(d)
    if bad:
        raise ValueError("refusing to write a grant: %s. A store somebody else can write "
                         "is a store that can grant." % bad)
    _make_private(d)
    tmp_path = path + ".cai-new"
    # **0600, created that way rather than fixed afterwards.** A chmod after the
    # write leaves a window in which the file exists readable, and for a store
    # that decides permissions the window is the whole problem.
    fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.replace(tmp_path, path)
    return path, len(before), lapsed


def prune(path=None, all_=False):
    """Drop grants that have already lapsed. Returns `(path, kept, dropped)`.

    **The store needed a way to be cleaned that is not a text editor.** Lapsed
    entries are inert -- `check` denies them on the clock -- but they are served,
    listed, and cited as the reason for a denial, and one of them answering
    instead of the grant a caller meant is how a verdict comes to describe a
    different grant from the one it names.
    """
    if path is None:
        total_k = total_d = 0
        for sp in sorted(glob.glob(os.path.join(runtime_root(), "*.json"))):
            _p, k, dr = prune(sp, all_=all_)
            total_k += k
            total_d += dr
        return runtime_root(), total_k, total_d
    if not os.path.exists(path):
        return path, 0, 0
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    token = safewrite.stat_token(path)
    at, kept, dropped = now(), [], 0
    if all_:
        # **`--all` drops everything, in force or not.** Revoking early has no
        # other expression: a grant can be issued and it can lapse, and nothing
        # retracts one. This is the crude version of that -- it takes back every
        # grant in a store rather than one, which is blunt but is honest about
        # being blunt. A named revoke belongs here later.
        dropped = len(doc.get("grants", []))
        doc["grants"] = []
    else:
        for g in doc.get("grants", []):
            try:
                exp = _parse(g.get("expires"))
            except BadTimestamp:
                kept.append(g)
                continue
            if exp and at > exp:
                dropped += 1
                continue
            # **A SPENT grant is as dead as a lapsed one**, and a consuming
            # grant with no clock is the only kind nothing could ever remove --
            # it sat in the store polluting verdicts with nothing able to drop
            # it.
            uses = g.get("expiresAfterChecks")
            if uses is not None and spent_count(g) >= uses:
                dropped += 1
                continue
            kept.append(g)
        doc["grants"] = kept
    # **Read-verify-write, because pruning REMOVES grants.** A grant issued by
    # another session between the read and the write would be dropped without
    # trace -- a permission somebody holds, deleted by a tidy-up. `append` has
    # had this guard since the store was built; `prune` was written later and
    # did not.
    ok, why = safewrite.unchanged_since(path, token)
    if not ok:
        raise ValueError("%s changed while it was being pruned (%s). Refusing; nothing "
                         "was removed. Re-run and the new contents are preserved."
                         % (path, why))
    tmp_path = path + ".cai-new"
    with open(tmp_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.replace(tmp_path, path)
    return path, len(kept), dropped
