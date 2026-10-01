#!/usr/bin/env python3
"""Runs cai-tools' own test suite from MAIC's copy (ctest `cai_tools`). Usage: run_tests.py [NAME...]

Each tests/test_*.py is a plain script that exits 0 on pass; none use pytest or unittest. They are
run as written, from tools/cai, with the package at tools/cai/src where their own `../src` line looks
for it. The entry-point registry they probe through importlib.metadata is served from a dist-info
generated out of pyproject.toml, which is what an installed copy would have. MAIC's state and
runtime directories and cai's temp and grant stores are temp dirs; CAI_NO_REMOTE is set as a default,
and test_remote lifts it for its own remote checks as it always has.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def dist_info(into):
    """A dist-info for cai-tools, from pyproject.toml, so importlib.metadata sees the registry."""
    with open(os.path.join(HERE, "pyproject.toml"), encoding="utf-8") as fh:
        text = fh.read()
    version = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M).group(1)
    d = os.path.join(into, "cai_tools-%s.dist-info" % version)
    os.makedirs(d)
    with open(os.path.join(d, "METADATA"), "w", encoding="utf-8") as fh:
        fh.write("Metadata-Version: 2.1\nName: cai-tools\nVersion: %s\n" % version)
    lines = []
    for group in re.finditer(r'\[project\.entry-points\."([^"]+)"\]\n((?:[^\[]*\n)*)', text):
        lines.append("[%s]" % group.group(1))
        for m in re.finditer(r'^([\w\-]+)\s*=\s*"([^"]+)"', group.group(2), re.M):
            lines.append("%s = %s" % (m.group(1), m.group(2)))
        lines.append("")
    with open(os.path.join(d, "entry_points.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def main(names):
    tmp = tempfile.mkdtemp(prefix="cai-tests-")
    try:
        dist_info(tmp)
        env = dict(os.environ)
        env["PYTHONPATH"] = os.pathsep.join([tmp, os.path.join(HERE, "src")] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
        for var in ("XDG_RUNTIME_DIR", "XDG_STATE_HOME", "CAI_TMP", "CAI_GRANT_ROOT"):
            env[var] = os.path.join(tmp, var.lower())
            os.makedirs(env[var], mode=0o700)
        env["CAI_NO_REMOTE"] = "1"
        env.pop("CAI_SESSION_ID", None)
        tests = sorted(f for f in os.listdir(os.path.join(HERE, "tests")) if re.match(r"test_.*\.py$", f))
        if names:
            tests = [t for t in tests if t[:-3] in names or t in names]
        failed = []
        for t in tests:
            r = subprocess.run([sys.executable, os.path.join(HERE, "tests", t)], cwd=HERE, env=env,
                               capture_output=True, text=True, timeout=600)
            ok = r.returncode == 0
            print(("ok    " if ok else "FAIL  ") + t, flush=True)
            if not ok:
                failed.append(t)
                print(r.stdout[-3000:], r.stderr[-3000:], sep="\n", flush=True)
        print("%d suites, %d failed" % (len(tests), len(failed)))
        return 1 if failed else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
