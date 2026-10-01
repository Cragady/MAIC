"""cai enroll -- what this repository is held to, and how strongly.

**Bare invocation prints what an agent should ASK**, not what it should decide.
Enrollment is an operator's choice about their own repository, and a tool that
picked a default would be making that choice by omission -- which is how a
repository ends up enrolled without anyone having agreed to it, and how the
record of the decision is lost.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

# The shim's text and the set of hooks are DATA, fetched the same way as every
# other thing cai reads. Hardcoding them here would make cai the definer as well
# as the enforcer, which is the split this architecture exists to keep.
def hook_definitions():
    from cai.hook import cli as hookcli
    return hookcli.load()


def hook_body(name, doc=None):
    doc = doc or hook_definitions()
    shim = doc.get("shim") or ""
    return shim.replace("{name}", name)


SECOND_ASK = """
AND ONE MORE, BECAUSE THIS REPOSITORY HAS NO `CLAUDE.md`

  `/init` has not been run here. Enrolling puts cai's gates in force in this
  repository -- so its rules would apply with no local document stating them,
  which leaves them enforced but not findable by anyone reading the repo.

  Ask this alongside the level, and wait:

    "This repository has no CLAUDE.md. Do you want to run `/init` now, so the
     rules being enforced here are also written down here?"

  **cai cannot run `/init`** -- it is a client command. This notices and asks;
  you relay; the operator decides and runs it. Enrolment does not depend on the
  answer and is not blocked by it.
"""

MAN_HELP = """cai enroll -- full reference

  cai enroll                      print this, and ask the operator
  cai enroll status  --repo PATH
  cai enroll --weak  --repo PATH
  cai enroll --strong --repo PATH
  cai enroll --off   --repo PATH
  cai enroll         --repo PATH   re-apply the decision already on record

RE-APPLYING
  With no level, `cai enroll --repo PATH` restores whatever was last decided.
  Hooks live in `.git/` and do not survive a clone; the marker is committed and
  does, so re-running reconciles reality to the decision rather than asking
  again for an answer that already exists.

  With no level AND no decision on record, it refuses and tells you to ask.
  There is still no default.

WHAT TO ASK, IF YOU ARE AN AGENT READING THIS
  Do not choose. Ask, in these terms, and wait:

    "Should <repo> be enrolled with cai, and how strongly?

       strong -- git hooks call the gate, so every commit and push passes it
                 whether or not anyone chose to. Bypass needs --no-verify.
       weak   -- the tools are available and nothing is installed. Using them
                 is a habit rather than a rule.
       off    -- no enrollment."

  A repository that does not want enforcement should SAY SO, rather than being
  enrolled by default and having hooks removed later -- which loses the record
  of the decision and leaves nobody able to tell refusal from neglect.

STRONG IS PRE-FLIGHTED, BECAUSE IT COULD OTHERWISE BRICK THE REPOSITORY
  Strong enrollment installs hooks that block a commit when they fail. If the
  hooks cannot succeed -- no `cai` on PATH, no `grant` sub-command, no grant
  record for the repository -- then every commit is refused, including the one
  that would fix it.

  That is an impossible ask rather than a strict one, and `cai enroll` refuses
  it. --force exists and is the OPERATOR'S; an agent reports the refusal and
  stops.

STRONG AND WEAK
  Strong installs `pre-commit` and `pre-push` hooks that call `cai commit` and
  `cai grant`. Every commit passes the gate whether or not anyone chose to.

  Weak installs nothing. It is NOT the absence of enforcement -- it is
  enforcement by convergence: the more cai is used, the more it is the only tool
  used, and the weak form approaches the strong one without ever having forced
  anything. That makes the tool's own quality the mechanism, which is a
  constraint rather than a compliment: a tool people route around is not weakly
  enrolled, it is abandoned.

THE ESCAPE HATCH IS THE OPERATOR'S
  --no-verify survives the strong form on purpose. A hook that cannot be
  bypassed becomes a hostage situation the first time it is wrong.

  AN AGENT MAY NEVER REACH FOR IT. An agent that meets a refusal reports it and
  stops. It does not bypass, and it does not propose bypassing as the remedy.
  The whole value of a gate is that the thing being gated cannot open it.

EXIT   0 ok   1 not enrolled / check failed   2 usage
"""


def levels():
    """The enrolment levels, from notation. Falls back to the built-in set.

    A vocabulary with two homes drifts, and this one is written in a dictionary
    an operator reads. Fetching it means the tool cannot quietly disagree with
    its own documentation.
    """
    try:
        from cai.notation import store as nstore
        doc = nstore.load()
        for e in nstore.live(doc):
            if e["id"] == "enrollment-level":
                return tuple(e["symbols"])
    except Exception:                                            # noqa: BLE001
        pass
    return ("strong", "weak", "off")


def _root(repo):
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=repo,
                       capture_output=True, text=True)
    return p.stdout.strip() or os.path.abspath(repo)


def has_claude_md(repo):
    """Has `/init` been run here? **cai cannot run it; it can only notice.**

    `/init` is a client slash command, so this reports and the operator acts --
    the same shape as `cai enroll` itself, which prints the question and refuses
    to answer it.
    """
    return os.path.exists(os.path.join(_root(repo) or repo, "CLAUDE.md"))


def gates_in_force(repo):
    """**Step one of two: are cai's gates in force in this repository?**

    **Owner, 2026-09-03:** *"part of `cai enroll` should bring the gates in
    regardless of the strength of enrollment. It should be a two step process."*

    Enrollment had one axis doing two jobs. It now has two:

        step 1  ARE the gates in force here          this function
        step 2  HOW are they enforced                strong: hooks, so a plain
                                                     `git commit` cannot miss them
                                                     weak: tools available, and
                                                     convergence does the rest

    **Strength was never the right question for a gate to ask.** A weakly
    enrolled repository has still said yes to the gates; what it declined was
    having them installed where forgetting is impossible. Reading strength to
    decide whether a grant applies conflated a decision about ENFORCEMENT with a
    decision about SCOPE.

        strong | weak   the gates apply
        no marker       the gates apply -- see below
        off             DECLINED, and only this turns them off

    **Everything except an explicit `off` is gated, and that is the owner's
    ruling rather than an oversight.** I had built the opposite -- a repository
    that declared nothing going ungated -- on the reasoning that cai should
    enforce only what is declared. She corrected it mid-build:

        *"Those more open repos get our protections for the price of us
        following it."*

    **Which reframes what the gate is.** It is not a rule imposed on a repository
    that never asked for one; it is a discipline this toolset brings wherever it
    works, and the cost is borne by us rather than by the repository. A more open
    repository is not one that opted out of care.

    **`off` is the single exception, and it earns that by being a decision.** The
    marker exists precisely so that declining is recorded rather than inferred
    from a missing hook -- so ignoring it would make the one deliberate act here
    the only one with no effect.
    """
    st = status(repo)
    return (st.get("declared") or st.get("enrollment")) != "off"


def status(repo):
    root = _root(repo)
    hooks = os.path.join(root, ".git", "hooks")
    installed = [h for h in ("pre-commit", "pre-push")
                 if os.path.exists(os.path.join(hooks, h))
                 and "cai " in open(os.path.join(hooks, h), encoding="utf-8",
                                    errors="replace").read()]
    marker = os.path.join(root, ".cai-enrollment")
    declared = None
    if os.path.exists(marker):
        try:
            declared = json.load(open(marker, encoding="utf-8")).get("enrollment")
        except (OSError, ValueError):
            declared = "unreadable"
    return {"claude_md": has_claude_md(repo),"repo": root, "declared": declared, "hooks_installed": installed,
            "effective": "strong" if installed else (declared or "not enrolled")}


def strong_preflight(repo):
    """What must already be true for strong enrollment not to brick the repository.

    **Owner's ruling, 2026-09-01: strong enrollment should never brick a repo, so
    that is an impossible ask, and an impossible ask should be refused.** A hook
    that cannot succeed blocks every commit -- so the conditions for it to
    succeed are checked before it is installed, not discovered afterwards by a
    developer who can no longer commit the fix.

    This was not hypothetical. The hook first shipped calling a flag the CLI did
    not accept, and the installed `cai` predated the subcommands the hook needs.
    Either would have refused every commit in a repository that had just been
    told it was protected.
    """
    problems = []
    exe = shutil.which("cai")
    if not exe:
        problems.append("`cai` is not on PATH, so the hooks would fail on every commit")
    else:
        p = subprocess.run(["cai", "grant", "--help"], capture_output=True, text=True)
        if p.returncode != 0 or "no such tool" in (p.stdout + p.stderr).lower():
            problems.append("the installed `cai` has no `grant` sub-command -- the hooks "
                            "call it, so every commit would be refused")
    try:
        from cai.grant import store as gstore
        doc = gstore.load()
        root = _root(repo)
        if not any(g.get("repo") == root for g in doc.get("grants", [])):
            problems.append("no grant record exists for %s, so every commit would be "
                            "refused until one is seeded" % root)
    except Exception as e:                                       # noqa: BLE001
        problems.append("the grant store could not be read (%s), so the hooks could not "
                        "succeed" % e)
    return problems


def enroll(repo, level, dry_run=False, force=False):
    root = _root(repo)
    hooks = os.path.join(root, ".git", "hooks")
    written, removed = [], []
    if level == "strong" and not force:
        problems = strong_preflight(repo)
        if problems:
            return {"repo": root, "enrollment": None, "refused": True,
                    "problems": problems,
                    "note": "strong enrollment would refuse every commit here, which is an "
                            "impossible ask rather than a strict one. Fix the conditions "
                            "above and re-run. --force exists and is the OPERATOR'S, never "
                            "an agent's."}
    if level == "strong":
        defs = hook_definitions()
        for name in sorted(defs.get("hooks") or {}):
            body = hook_body(name, defs)
            path = os.path.join(hooks, name)
            if not dry_run:
                os.makedirs(hooks, exist_ok=True)
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(body)
                os.chmod(path, 0o755)
            written.append(path)
    else:
        for name in sorted(hook_definitions().get("hooks") or {}):
            path = os.path.join(hooks, name)
            if os.path.exists(path) and "cai " in open(path, encoding="utf-8",
                                                       errors="replace").read():
                if not dry_run:
                    os.remove(path)
                removed.append(path)
    marker = os.path.join(root, ".cai-enrollment")
    if not dry_run:
        with open(marker, "w", encoding="utf-8") as fh:
            json.dump({"enrollment": level,
                       "note": "Declared by the operator. `off` is a recorded decision, "
                               "not the absence of one -- which is why it is written down "
                               "rather than inferred from a missing hook."}, fh, indent=2)
    return {"repo": root, "enrollment": level, "hooks_written": written,
            "hooks_removed": removed, "marker": marker, "dry_run": dry_run}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        # **One question or two, decided by looking.** Owner, 2026-09-03: if enroll
        # can detect that `/init` has not been run here, it should ask about that
        # too. Enrolling a repository puts cai's gates in force in it; a repo with
        # no CLAUDE.md then has rules applying to it and no local document saying
        # so, which is the gap between a rule being enforced and a rule being
        # findable. **It is asked, never done** -- `/init` is the client's and the
        # answer is the operator's, same contract as the level itself.
        if not has_claude_md("."):
            print(SECOND_ASK)
        return 0
    ap = argparse.ArgumentParser(prog="cai enroll", add_help=False)
    ap.add_argument("cmd", nargs="?", default=None)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--strong", action="store_true")
    ap.add_argument("--weak", action="store_true")
    ap.add_argument("--off", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="install strong hooks despite a failing pre-flight. "
                         "The operator's alone; an agent reports and stops.")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "status":
        st = status(args.repo)
        print(json.dumps(st, ensure_ascii=False, indent=2))
        return 0 if st["effective"] != "not enrolled" else 1

    # The levels are a NOTATION, not a literal here. `push` in a grant and `push`
    # in a hook are the same word by construction rather than by coincidence, and
    # the same must hold for these -- otherwise a level means one thing in the
    # tool and another in the dictionary that documents it.
    known = levels()
    picked = [n for n, v in (("strong", args.strong), ("weak", args.weak),
                             ("off", args.off)) if v and n in known]
    if len(picked) > 1:
        sys.stderr.write("error: choose at most one of --strong, --weak, --off.\n")
        return 2
    if not picked:
        # **No level given re-applies the recorded decision.** Owner's ruling,
        # 2026-09-01. Hooks live in `.git/` and do not survive a clone; the marker
        # is committed and does. Re-running therefore reconciles reality to the
        # decision already made, rather than asking again for an answer that
        # exists.
        prior = status(args.repo).get("declared")
        if prior in ("strong", "weak", "off"):
            res = enroll(args.repo, prior, dry_run=args.dry_run, force=args.force)
            res["reapplied"] = prior
            res["note_reapplied"] = ("no level was given, so the recorded decision was "
                                     "re-applied. Hooks do not survive a clone; the marker "
                                     "does, which is what makes this reconcilable.")
            print(json.dumps(res, ensure_ascii=False, indent=2))
            return 1 if res.get("refused") else 0
        sys.stderr.write("error: no level given and no decision on record for this "
                         "repository. There is no default -- a repository that is not to "
                         "be enrolled should SAY SO rather than be enrolled by omission. "
                         "Run `cai enroll` with no arguments to see what to ask.\n")
        return 2
    res = enroll(args.repo, picked[0], dry_run=args.dry_run, force=args.force)
    print(json.dumps(res, ensure_ascii=False, indent=2))
    return 1 if res.get("refused") else 0


if __name__ == "__main__":
    sys.exit(main())
