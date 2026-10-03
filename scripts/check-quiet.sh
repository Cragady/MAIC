#!/usr/bin/env bash
# Runs scripts/check.sh with its output in a log and prints only a verdict: "gate: pass", or the failing step,
# the failing tests and their first error lines. For agents, whose context every line of a full gate log costs.
# Usage: scripts/check-quiet.sh [LOG]   (default /tmp/maid-gate/check-<pid>.log); exits with check.sh's status.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
log="${1:-/tmp/maid-gate/check-$$.log}"
mkdir -p "$(dirname "$log")"
env -u FORCE_COLOR "$root/scripts/check.sh" > "$log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "gate: pass  (log $log)"
    exit 0
fi
echo "gate: FAIL ($rc)  (log $log)"
grep -m1 '^check: FAILED at:' "$log"
sed -n '/The following tests FAILED:/,/^Errors while running/p' "$log" | grep -E '^\s+[0-9]+ - ' | head -10
grep -E '^(FAIL|ERROR):|^\s+FAIL\s|TimeoutError|AssertionError|error:' "$log" | head -12
exit "$rc"
