"""cai name -- session names: what they mean, and why they are shaped that way.

**The implementation over two things it does not own.** The shape of a name is a
document; the symbols in it are notations. This renders, parses and explains
them, and **defines nothing** -- a marker's meaning is `cai notation`'s, and the
field order is `cai document`'s.

**Bare invocation prints the reference, and the reference says WHY.** The
questions a reader actually has -- why the cwd leads, what a glyph means, why a
slice may not match the current `sessionId` -- had their answers in two
repositories' prose. A tool whose bare invocation explains its own reasoning
puts them where the question arrives.
"""
import argparse
import json
import re
import sys

from cai.document import store as dstore
from cai.notation import store as nstore

SLICE = re.compile(r"\s-\s([0-9a-f]{8})\s*$", re.I)

MAN_HELP = """cai name -- full reference

  cai name                     this reference
  cai name explain             the shape and every marker, from the sources
  cai name parse "NAME"        break a name into its parts
  cai name render --cwd X [--fork] [--open-forks] [--role pivot|anchor] --slice ABCD1234
  cai name markers [--show-gated]

WHAT THIS TOOL OWNS
  Nothing. The shape is a document, the symbols are notation, and this arranges
  them. It may not define a marker: that is `cai notation`, and a definition in
  two places drifts.

WHY THE CWD LEADS
  A listing truncates the tail, so the marker has to lead -- it is what an
  operator scans for. And sibling repositories are the pattern implemented more
  often than forking, so the ordering serves the recurring case rather than the
  glyphs.

WHY A SLICE MAY NOT MATCH THE sessionId
  Because it is documentation, not an address. ListAgents matches the whole name
  string; nothing parses the hash. A slice that disagrees with the current id
  recorded where the conversation CAME FROM, which is a different fact from
  which file this is -- and arguably the more useful one.

  Never rewrite a slice this tool did not write. A mismatch may be deliberate.

GATED MARKERS
  Some markers are withheld by default. `not-eligible` is the case: knowledge of
  contamination is itself contamination, so an agent reading the marker learns
  something a clean agent would not have. --show-gated reveals them, and is the
  OPERATOR'S switch.

EXIT   0 ok   1 nothing to report   2 usage
"""

GATED = {"not-eligible"}


def parse(name):
    """Split a name into its parts, reporting what it cannot determine."""
    out = {"raw": name, "slice": None, "cwd": None, "markers": [], "unrecognised": []}
    m = SLICE.search(name)
    head = name
    if m:
        out["slice"] = m.group(1)
        head = name[:m.start()]
    doc = nstore.load()
    index = nstore.symbols(doc)
    tokens = head.split()
    for tok in tokens:
        hits = index.get(tok)
        if hits:
            out["markers"].append({"symbol": tok, "id": hits[0]["id"],
                                   "name": hits[0]["name"]})
        elif out["cwd"] is None:
            out["cwd"] = tok
        else:
            out["unrecognised"].append(tok)
    return out


def render(cwd, slice_, fork=False, open_forks=False, role=None):
    doc = nstore.load()

    def sym(defid):
        for e in nstore.live(doc):
            if e["id"] == defid:
                return e["symbols"][0]
        return None

    parts = [cwd]
    if fork:
        parts.append(sym("fork"))
    if open_forks:
        parts.append(sym("has-open-forks"))
    if role in ("pivot", "anchor"):
        parts.append(sym("role-%s" % role))
    return "%s - %s" % (" ".join(p for p in parts if p), slice_)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "--man-help"):
        print(MAN_HELP)
        return 0
    ap = argparse.ArgumentParser(prog="cai name", add_help=False)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("explain")
    pa = sub.add_parser("parse")
    pa.add_argument("name")
    re_ = sub.add_parser("render")
    re_.add_argument("--cwd", required=True)
    re_.add_argument("--slice", dest="slice_", required=True)
    re_.add_argument("--fork", action="store_true")
    re_.add_argument("--open-forks", action="store_true")
    re_.add_argument("--role", choices=["pivot", "anchor"])
    mk = sub.add_parser("markers")
    mk.add_argument("--show-gated", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "explain":
        shape = dstore.get(dstore.load(), "session-name")
        ndoc = nstore.load()
        out = {"shape": shape, "markers": []}
        for part in (shape or {}).get("parts", []):
            for defid in part.get("notation", []):
                for e in nstore.live(ndoc):
                    if e["id"] == defid:
                        out["markers"].append({"part": part["key"], "id": defid,
                                               "symbols": e["symbols"],
                                               "definition": e["definition"]})
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0 if shape else 1

    if args.cmd == "parse":
        print(json.dumps(parse(args.name), ensure_ascii=False, indent=2))
        return 0
    if args.cmd == "render":
        print(render(args.cwd, args.slice_, fork=args.fork,
                     open_forks=args.open_forks, role=args.role))
        return 0
    if args.cmd == "markers":
        doc = nstore.load()
        out = []
        for e in nstore.live(doc):
            if e.get("scope") != "session name":
                continue
            if e["id"] in GATED and not args.show_gated:
                out.append({"id": e["id"], "withheld": True,
                            "why": ("gated: knowledge of contamination is itself "
                                    "contamination, so an agent reading this learns "
                                    "something a clean agent would not have. "
                                    "--show-gated is the operator's switch.")})
                continue
            out.append({"id": e["id"], "symbols": e["symbols"], "name": e["name"],
                        "status": nstore.status_of(e), "definition": e["definition"]})
        print(json.dumps({"markers": out}, ensure_ascii=False, indent=2))
        return 0
    print(MAN_HELP)
    return 2


if __name__ == "__main__":
    sys.exit(main())
