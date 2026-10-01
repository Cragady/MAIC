"""Preconditions for a commit, checked before the commit happens.

**Every failure this exists for is the same shape: the commit ran without
verifying what it depended on.** Recorded from one session, 2026-09-01 --

  three commits with failing tests, because the test run and the commit were
  separate statements in one shell line and the commit did not care;

  three commits after a file edit had raised and written nothing, for the same
  reason, leaving a message describing work the diff did not contain;

  two commits whose subject named a file absent from the change.

**None of these were caught by care and all of them were caught afterwards.**
That is the signature of a check that belongs at the point of effect rather than
in a habit. Chaining with `&&` fixes it for the person who remembers; this fixes
it for the person who does not.
"""
import os
import re
import subprocess


def _git(args, cwd, timeout=60):
    try:
        p = subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                           text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.SubprocessError) as e:
        return 1, "", str(e)


def staged_paths(repo):
    rc, out, _ = _git(["diff", "--cached", "--name-only"], repo)
    return [l for l in out.splitlines() if l.strip()] if rc == 0 else []


def subject_scope(message):
    """The `<scope>:` prefix this repository's commit subjects use, if present."""
    first = message.strip().split("\n", 1)[0]
    m = re.match(r"^([A-Za-z0-9_.\-/ ]+):", first)
    return m.group(1).strip() if m else None


def scope_matches(scope, paths):
    """Whether the subject's scope plausibly names something in the change.

    **A heuristic, and reported as a warning rather than a refusal.** It caught a
    real case -- a commit whose subject said `CLAUDE.md` while the diff held only
    `docs/grants.json` -- but a scope may legitimately name a component rather
    than a path, so a false positive must not block a correct commit.
    """
    if not scope or not paths:
        return True
    tokens = [t for t in re.split(r"[ /]+", scope.lower()) if len(t) > 2]
    blob = " ".join(paths).lower()
    return any(t in blob for t in tokens) if tokens else True


def _gates_in_force(repo):
    """Does this repository say cai's gates apply? **Fails CLOSED on doubt** --
    a repository that cannot be read is not one that said no."""
    try:
        from cai.enroll import cli as ecli
        return ecli.gates_in_force(repo)
    except Exception:                                            # noqa: BLE001
        return True


def check(repo, message, verify=None, require_grant=None, session_id=None,
          action="commit", consume=True):
    """Everything that must hold before a commit. Returns (ok, findings).

    **`require_grant` defaults to the REPOSITORY'S OWN DECLARATION, from
    2026-09-03.** `None` means *ask the repo*; `True` and `False` force it either
    way, and `--no-grant` remains the operator's bypass.

    **Owner: enrollment brings the gates in regardless of strength, in two
    steps** -- step one is whether the gates apply, step two is how they are
    enforced. **Strength was never the right question for a gate**: it decides
    ENFORCEMENT, not scope.

    **And the base is ON, everywhere except an explicit `off`.** I built the
    opposite first -- an undeclared repository going ungated, reasoning that cai
    should enforce only what is declared. The owner corrected it mid-build:
    *"those more open repos get our protections for the price of us following
    it."* **The gate is not a rule imposed on a repository that never asked for
    one; it is a discipline this toolset brings, paid for by us.** A more open
    repository is not one that opted out of care, and only a recorded `off`
    says otherwise.

    **It fails CLOSED on doubt.** An unreadable marker is treated as in force.

    **The previous default, kept because the ordering is the lesson.** It
    defaulted to FALSE,
    which meant this gate checked a grant only when asked -- and nobody asked.
    Every commit routed through it for a full session was ungated while the tool
    reported success, and the separate `cai grant check` runs beside them were
    decoration. **The gate was correct about everything it verified; the grant
    was simply not among those things.**

    That is the third default found wrong in the unsafe direction in two days,
    after `IN_PLACE` and the enrollment hook. The pattern is worth stating: **a
    default is a decision made for everyone who does not think about it**, so the
    safe answer has to be the one you get by not thinking about it. Opting IN to
    a safety check reverses that -- it asks for attention from exactly the caller
    who has none to spare.

    `--no-grant` remains, and it is the OPERATOR's. An agent meeting a refusal
    reports it and stops; it does not reach for the bypass and does not propose
    reaching for it, because a gate the gated party can open is not a gate.
    """
    findings = []
    paths = staged_paths(repo)

    if not paths:
        findings.append(("refuse", "nothing is staged -- a commit here would be empty, "
                                   "which is what an edit that silently failed looks like"))

    if verify:
        v = subprocess.run(verify, cwd=repo, shell=True, capture_output=True, text=True)
        rc = v.returncode
        if rc != 0:
            findings.append(("refuse", "verification failed: %r exited %d -- this is the "
                                       "check that three commits skipped by running it in "
                                       "the same breath as the commit" % (verify, rc)))

    if require_grant is None:
        require_grant = _gates_in_force(repo)
    if require_grant:
        try:
            from cai.grant import store as gstore
            v = gstore.check(gstore.load(repo=os.path.abspath(repo)),
                             os.path.abspath(repo),
                             session_id=session_id, action=action, paths=paths,
                             consume=consume)
            if not v["granted"]:
                findings.append(("refuse", "grant denied: %s" % v["reason"]))
        except Exception as e:                                   # noqa: BLE001
            findings.append(("refuse", "grant could not be evaluated (%s); failing closed" % e))

    scope = subject_scope(message)
    if not scope_matches(scope, paths):
        findings.append(("warn", "the subject says %r but the change touches %s -- a commit "
                                 "whose message names work it does not contain has happened "
                                 "here twice" % (scope, ", ".join(paths[:4]))))

    ok = not any(k == "refuse" for k, _ in findings)
    return ok, findings


def outgoing_paths(repo):
    """Paths touched by commits not yet upstream. Empty list if it cannot be told."""
    rc, out, _ = _git(["diff", "--name-only", "@{u}..HEAD"], repo)
    if rc != 0:
        return []
    return [l for l in out.splitlines() if l.strip()]


def push(repo, require_grant=None, session_id=None, dry_run=False, timeout=120):
    """Publishing is a SEPARATE permission from committing.

    A commit is local and reversible; a push is outward-facing and is the point
    at which work becomes someone else's problem. Granting one should not grant
    the other, so this asks for `push` rather than reusing the commit verdict.
    """
    result = {"repo": repo, "action": "push", "ok": True, "dry_run": dry_run,
              "findings": []}
    if require_grant is None:
        require_grant = _gates_in_force(repo)
    if require_grant:
        try:
            from cai.grant import store as gstore
            # **What a push carries is what is not yet upstream**, which is the
            # only honest answer to "which paths does this push touch". Without
            # it a path-scoped grant could not gate a push at all, and the
            # narrow half of a commit-and-push grant would be the half that does
            # nothing.
            v = gstore.check(gstore.load(repo=os.path.abspath(repo)),
                             os.path.abspath(repo), session_id=session_id,
                             action="push", paths=outgoing_paths(repo),
                             consume=not dry_run)
            if not v["granted"]:
                result["ok"] = False
                result["findings"].append({"level": "refuse",
                                           "detail": "grant denied for push: %s" % v["reason"]})
        except Exception as e:                                   # noqa: BLE001
            result["ok"] = False
            result["findings"].append({"level": "refuse",
                                       "detail": "push grant could not be evaluated (%s); "
                                                 "failing closed" % e})
    if not result["ok"] or dry_run:
        return result
    rc, out, err = _git(["push"], repo, timeout=timeout)
    result["pushed"] = rc == 0
    if rc != 0:
        result["ok"] = False
        result["findings"].append({"level": "refuse", "detail": (err or out).strip()[:400]})
    return result


def commit(repo, message, verify=None, require_grant=None, session_id=None,
           dry_run=False):
    ok, findings = check(repo, message, verify=verify, require_grant=require_grant,
                         session_id=session_id, consume=not dry_run)
    result = {"repo": repo, "ok": ok, "dry_run": dry_run,
              "staged": staged_paths(repo),
              "findings": [{"level": k, "detail": d} for k, d in findings]}
    if not ok or dry_run:
        return result
    p = subprocess.run(["git", "commit", "-q", "-F", "-"], cwd=repo, input=message,
                       capture_output=True, text=True)
    result["committed"] = p.returncode == 0
    if p.returncode != 0:
        result["ok"] = False
        result["findings"].append({"level": "refuse", "detail": p.stderr.strip()})
    else:
        _, sha, _ = _git(["log", "-1", "--format=%h %s"], repo)
        result["commit"] = sha.strip()
    return result
