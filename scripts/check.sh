#!/usr/bin/env bash
# The build gate: configure, build, test, in that order, stopping at the first failure.
# Usage: scripts/check.sh [--quick] [--asan]
#   --quick   configure, build and ctest only (what the pre-push hook runs)
#   --asan    the asan preset (build-asan/) instead of the default one
# The build's own exit code gates everything: ctest alone passes on stale binaries when a test target fails to
# compile, which is how a broken test once went unnoticed. After ctest the python suites run again on their own,
# verbosely, so a failing case is named and a skipped TUI suite is visible rather than silent.
set -uo pipefail
cd "$(dirname "$0")/.."

quick=0
preset=default
for arg in "$@"; do
    case "$arg" in
        --quick) quick=1 ;;
        --asan) preset=asan ;;
        *) echo "usage: $0 [--quick] [--asan]" >&2; exit 2 ;;
    esac
done
if [ -z "${VCPKG_ROOT:-}" ]; then
    echo "check: VCPKG_ROOT is not set" >&2
    exit 2
fi

step() {
    echo "== $*"
    "$@" || { echo "check: FAILED at: $*" >&2; exit 1; }
}

step cmake --preset "$preset"
step cmake --build --preset "$preset" -j
step ctest --preset "$preset"
[ "$quick" = 1 ] && exit 0

build=build
[ "$preset" = asan ] && build=build-asan
step python3 tests/cli_smoke.py "$build/cli/maic"
echo "== python3 tests/test_tui.py $build/cli/maic -v"
python3 tests/test_tui.py "$build/cli/maic" -v
rc=$?
if [ "$rc" = 77 ]; then
    echo "check: the TUI suite was skipped (no pyte, and no uv with pyte cached; MAIC_NETWORK_TESTS=1 lets uv fetch it)"
elif [ "$rc" != 0 ]; then
    echo "check: FAILED at: tests/test_tui.py" >&2
    exit 1
fi
echo "check: all green ($preset)"
