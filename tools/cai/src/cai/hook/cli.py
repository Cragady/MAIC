"""cai hook -- what an installed git hook actually does.

**The installed hook is a shim and this is the behaviour.** Everything that could
change -- which action is asked about, what a refusal says, what it exits -- lives
here rather than in the file on disk, so improving it reaches every enrolled
repository at once. A hook carrying its own logic would be a second home for
behaviour, and one that only updates when somebody remembers to reinstall it.
"""
import argparse
import os
import sys

from cai import source

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_VAR = "CAI_HOOKS"
SOURCE_CANDIDATES = (os.path.join(source.SOPIA, "docs", "hooks.json"),)
FALLBACK = os.path.join(HERE, "hooks.json")


def load():
    """The hook definitions. SOPIA defines them; this fetches."""
    return source.load(ENV_VAR, SOURCE_CANDIDATES, FALLBACK,
                       empty={"hooks": {}, "shim": ""}, package="hook",
                       remote_path="docs/hooks.json")


MAN_HELP = """cai hook -- full reference

  cai hook pre-commit --repo PATH
  cai hook pre-push   --repo PATH

  Installed git hooks call this. They are shims by design: the behaviour lives
  here, so it improves without anyone reinstalling a hook file.

WHAT IT CHECKS
  Whether a grant is in force for this repository, this session, and the action
  the hook stands for. Nothing else -- a pre-commit hook runs before the message
  exists, so it cannot check a subject or run a verification. Those belong to
  `cai commit`, used deliberately.

ON REFUSAL
  It prints the verdict and exits non-zero, which aborts the git operation.

  AN AGENT MUST REPORT THE REFUSAL AND STOP. It does not bypass, and it does not
  propose bypassing as the remedy. --no-verify exists and is the OPERATOR'S: the
  whole value of a gate is that the thing being gated cannot open it.

EXIT   0 allowed   1 refused   2 usage
"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai hook", add_help=False)
    doc = load()
    ap.add_argument("name")
    ap.add_argument("--repo", default=".")
    args = ap.parse_args(argv)

    spec = (doc.get("hooks") or {}).get(args.name)
    if not spec:
        sys.stderr.write("cai: no hook definition for %r (source: %s)\n"
                         % (args.name, doc.get("_path")))
        return 2
    repo = os.path.abspath(args.repo)
    try:
        from cai.grant import store as gstore
        doc = gstore.load()
        sess = gstore.current_session(cwd=repo)
        sid = (sess or {}).get("sessionId") or (sess or {}).get("id")
        v = gstore.check(doc, repo, session_id=sid,
                         session_name=(sess or {}).get("name"), action=spec["action"])
    except Exception as e:                                       # noqa: BLE001
        sys.stderr.write("cai: %s refused -- the grant could not be evaluated (%s); "
                         "failing closed\n" % (spec["what"], e))
        return 1

    if v["granted"]:
        return 0
    sys.stderr.write("cai: %s refused for %s\n" % (spec["what"], repo))
    sys.stderr.write("     %s\n" % v["reason"])
    for other in v.get("also_considered", []):
        sys.stderr.write("     also: %s -- %s\n"
                         % (other.get("action") or "repo-wide", other["reason"]))
    if spec.get("note"):
        sys.stderr.write("     %s\n" % spec["note"])
    sys.stderr.write("     An agent must report this and stop. The bypass is the "
                     "operator's.\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
