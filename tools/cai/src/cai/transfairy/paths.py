"""Path and tree resolution for trans-fairy.

Three locations with three contracts (DESIGN.md, Tree):
  <work_root>/trans-fairy-<job>/   churn, disposable   (download/ staged/ previous-agent/)
  <data_root>/trans-fairy/         durable             (bak/ uuid-pool.txt)
  <target_cwd>                     NAMED ONLY, never written to

The job id is a stable hash of the target cwd, so every stage of one migration
finds the same tree across separate --agent invocations without a random id to
thread through.
"""
import hashlib
import os
import re


def _first_env(*names):
    for n in names:
        v = os.environ.get(n)
        if v:
            return v
    return None


def work_root(override=None):
    """Platform temp dir; TMPDIR/TEMP/TMP, else /tmp. Override with --work-root."""
    if override:
        return os.path.abspath(override)
    return _first_env("TMPDIR", "TEMP", "TMP") or "/tmp"


def data_root(override=None):
    """$XDG_DATA_HOME/cai or ~/.local/share/cai; %LOCALAPPDATA%\\cai on Windows."""
    if override:
        return os.path.abspath(override)
    if os.name == "nt":
        base = _first_env("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "cai")
    base = _first_env("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "cai")


def job_id(target_cwd):
    return hashlib.sha256(os.path.abspath(target_cwd).encode()).hexdigest()[:8]


def slug(target_cwd):
    """Claude Code's project-dir key: the cwd with '/' and '.' replaced by '-'.

    Empirically derived (both separators observed; probed 2026-08-28). Only
    tested on plain paths -- see IMPROVEMENTS.md on the doubled-dash caveat.
    """
    return re.sub(r"[/.]", "-", os.path.abspath(target_cwd))


def projects_dir(target_cwd, claude_home=None):
    home = claude_home or os.path.expanduser("~/.claude")
    return os.path.join(home, "projects", slug(target_cwd))


class Tree:
    """Resolved paths for one job. Nothing here is created; that is init's job."""

    def __init__(self, target_cwd, work_root_override=None, data_root_override=None,
                 stage_dir_override=None, pool_override=None, claude_projects_override=None):
        self.target_cwd = os.path.abspath(target_cwd)
        self.work_root = work_root(work_root_override)
        self.data_root = data_root(data_root_override)
        self.job = job_id(self.target_cwd)
        self.job_dir = os.path.join(self.work_root, "trans-fairy-" + self.job)
        self.download = os.path.join(self.job_dir, "download")
        self.staged = stage_dir_override and os.path.abspath(stage_dir_override) \
            or os.path.join(self.job_dir, "staged")
        self.previous_agent = os.path.join(self.job_dir, "previous-agent")
        self.tf_data = os.path.join(self.data_root, "trans-fairy")
        self.bak = os.path.join(self.tf_data, "bak")
        self.pool = os.path.abspath(pool_override) if pool_override \
            else os.path.join(self.tf_data, "uuid-pool.txt")
        self.projects_dir = os.path.abspath(claude_projects_override) if claude_projects_override \
            else projects_dir(self.target_cwd)
        self.projects_override = bool(claude_projects_override)

    def write_targets(self):
        """The directories this run may write to. The injection target is not here."""
        return [self.download, self.staged, self.previous_agent, self.tf_data, self.bak]
