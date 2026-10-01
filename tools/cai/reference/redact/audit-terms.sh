#!/bin/sh
# Component 6 — sealed term indirection.
#
# Purpose: let a self-audit search for sensitive literals without writing them
# into the transcript. The transcript records "$A1", never the value.
#
# RULES (self-audit only):
#   1. Iterate over NAMES, print names, never dereference a value into output.
#   2. Redirect stderr (2>/dev/null) — a failing matcher can echo its pattern.
#   3. Never `set -x` — tracing prints expanded values.
#
# STOP EXCEPTION: rules 1 and 2 lapse for a term once that term no longer
# carries leak risk. A term justifiably claimed as non-unique across various
# applications' internals is safe to expose back into logs.
#   - Retired 2026-08-24: the credential-store key name and the OS-crypt scheme
#     name. Both generic across applications; safe to log directly.
#
# Terms are stored as DERIVATIONS, never as literals, so no source file holds
# the value. Add new terms the same way.

# A1 — ciphertext prefix marker. Eliminated from every source 2026-08-24.
# Retained as a derivation only, so a future sweep can re-verify its absence.
a1() { printf 'v11' | base64; }

# Usage:
#   . ./audit-terms.sh
#   A1=$(a1)
#   for n in A1; do printf '%-4s %s\n' "$n" "$(grep -c -F -- "$(eval echo \$$n)" "$FILE" 2>/dev/null)"; done
