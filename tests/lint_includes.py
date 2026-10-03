#!/usr/bin/env python3
"""Fails when a source file includes cpp-httplib directly instead of through maid/http.hpp (see that header)."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parent.parent
bad = []
for d in ("core", "cli", "server"):
    for p in (root / d).rglob("*"):
        if p.suffix in (".cpp", ".hpp", ".h") and p.name != "http.hpp":
            if "#include <httplib.h>" in p.read_text(errors="replace"):
                bad.append(str(p.relative_to(root)))
if bad:
    print("direct #include <httplib.h> (use \"maid/http.hpp\"): " + ", ".join(bad))
    sys.exit(1)
print("includes ok")
