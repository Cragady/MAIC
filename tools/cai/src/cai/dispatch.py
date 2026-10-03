"""`cai` -- the single entry point.

Subcommands are DISCOVERED, not hardcoded. A tool registers itself under the
`cai.tools` entry-point group from its own distribution, which means it ships
with its own dependencies and installs independently. `cai` stays one command on
PATH; a heavy tool never drags its dependency chain into a light one.

A package registers itself like this:

    [project.entry-points."cai.tools"]
    diction = "diction.cli:main"

Built-ins are registered the same way, in this package's own pyproject.
"""
import importlib.util
import os
import re
import sys
from importlib.metadata import entry_points

GROUP = "cai.tools"

# Tools this package knows about but does not depend on. Named so `cai` can say
# what exists rather than only what is installed.
KNOWN = {
    "diction": "speech to a maintained numbered list  (pip install diction)",
}


# src/cai/dispatch.py -> ../../pyproject.toml
PYPROJECT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "pyproject.toml")


class _Entry:
    """An entry point read from pyproject.toml rather than from an installed distribution."""

    def __init__(self, name, target):
        self.name, self.value = name, target

    def load(self):
        mod, _, attr = self.value.partition(":")
        obj = __import__(mod, fromlist=["*"])
        return getattr(obj, attr) if attr else obj


def pyproject_entry_points(group, path=PYPROJECT):
    """The entry points `pyproject.toml` beside this package registers under `group`.

    **The registry has one home, and it is that file.** Inside MAID the package runs
    from its directory without ever being installed, so `importlib.metadata` has no
    distribution to ask; the same table is read from where it is written instead
    of being copied into code, which is the two-homes drift this suite closes
    everywhere else.
    """
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return {}
    m = re.search(r'\[project\.entry-points\."%s"\]\n((?:[^\[]*\n)*)' % re.escape(group), text)
    if not m:
        return {}
    return {n: _Entry(n, t) for n, t in re.findall(r'^([\w\-]+)\s*=\s*"([^"]+)"', m.group(1), re.M)}


def discover():
    """name -> (loader, distribution name). Installed tools, else the registry in pyproject.toml."""
    found = {}
    for ep in entry_points(group=GROUP):
        found[ep.name] = ep
    if not found:
        found = pyproject_entry_points(GROUP)
        # MAID's own diction (its top-level diction/, which the `cai` wrapper puts on
        # sys.path) registers the way the docstring above shows; until it is there,
        # `cai diction` answers from KNOWN as before.
        if importlib.util.find_spec("diction") and importlib.util.find_spec("diction.cli"):
            found["diction"] = _Entry("diction", "diction.cli:main")
    return found


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    found = discover()

    if not argv or argv[0] in ("-h", "--help"):
        print("cai <tool> [args]\n")
        if found:
            for name in sorted(found):
                print(f"  {name}")
        else:
            print("  (no tools installed)")
        missing = {k: v for k, v in KNOWN.items() if k not in found}
        if missing:
            print("\nnot installed:")
            for name, how in sorted(missing.items()):
                print(f"  {name:<12} {how}")
        return 0

    name = argv[0]
    if name in found:
        return found[name].load()(argv[1:])

    if name in KNOWN:
        print(f"cai: {name} is not installed. {KNOWN[name]}", file=sys.stderr)
        return 4

    print(f"cai: no such tool: {name}", file=sys.stderr)
    if found:
        print(f"     installed: {', '.join(sorted(found))}", file=sys.stderr)
    return 2
