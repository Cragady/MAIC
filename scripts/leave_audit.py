#!/usr/bin/env python3
"""The leave-cases audit: every way of leaving a session (tests/leave_cases.json) against what MAID defines today.

Prints how many combinations are defined and how many are UNDEFINED, with the UNDEFINED ones grouped by reason.
A reminder, never a gate: it always exits 0, whatever the file says or however it is broken (docs/testing.md).
Usage: scripts/leave_audit.py [--all]   (--all also prints every combination with what it maps to)
"""
import itertools
import json
import os
import sys

CASES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "leave_cases.json")


def fits(when, combo):
    for dim, want in when.items():
        if combo.get(dim) not in (want if isinstance(want, list) else [want]):
            return False
    return True


def resolve(rules, combo, depth=0):
    """(status, text, rule) for one combination: status is "defined", "n/a" or "UNDEFINED", and rule the index of
    the first rule that fits it (-1 for none)."""
    for i, rule in enumerate(rules):
        if not fits(rule.get("when", {}), combo):
            continue
        if "same_as" in rule:
            if depth > 8:
                return "UNDEFINED", "same_as loops in the file", i
            status, text, _ = resolve(rules, dict(combo, **rule["same_as"]), depth + 1)
            return status, (text if status == "UNDEFINED" else rule.get("prefix", "") + text), i
        if rule.get("is") == "UNDEFINED":
            return "UNDEFINED", rule.get("why", "no reason given"), i
        text = rule.get("is", "")
        return ("n/a" if text.startswith("n/a") else "defined"), text, i
    return "UNDEFINED", "no rule in the file covers it", -1


def group(combos, dims):
    """One rule's combinations, compactly: dim=a|b for each dimension they do not cover in full."""
    parts = []
    for dim, values in dims.items():
        seen = [v for v in values if any(c[dim] == v for c in combos)]
        if len(seen) < len(values):
            parts.append(dim + "=" + "|".join(seen))
    return " ".join(parts) or "every combination"


def main():
    with open(CASES) as f:
        data = json.load(f)
    dims, rules = data["dimensions"], data["rules"]
    for rule in rules:
        for dim, want in rule.get("when", {}).items():
            for v in want if isinstance(want, list) else [want]:
                if v not in dims.get(dim, []):
                    print("leave audit: a rule names %s=%s, which is not a dimension's value" % (dim, v))
    combos = [dict(zip(dims, values)) for values in itertools.product(*dims.values())]
    results = [(c,) + resolve(rules, c) for c in combos]
    undefined = {}  # why -> rule -> combinations, so each line below is one rule's share
    for c, status, text, rule in results:
        if status == "UNDEFINED":
            undefined.setdefault(text, {}).setdefault(rule, []).append(c)
    defined = sum(status != "UNDEFINED" for _, status, _, _ in results)
    na = sum(status == "n/a" for _, status, _, _ in results)
    print("leave audit: %d combinations, %d defined (%d of them cannot happen), %d UNDEFINED" % (len(results), defined, na, len(results) - defined))
    for why, by_rule in sorted(undefined.items(), key=lambda kv: -sum(map(len, kv[1].values()))):
        print("  UNDEFINED %4d  %s" % (sum(map(len, by_rule.values())), why))
        for cs in by_rule.values():
            print("                 " + group(cs, dims))
    if "--all" in sys.argv[1:]:
        for c, status, text, _ in results:
            print("%-9s %s: %s" % (status, " ".join(c.values()), text))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a reminder must never fail the gate
        print("leave audit: could not read %s: %s" % (CASES, e))
    sys.exit(0)
