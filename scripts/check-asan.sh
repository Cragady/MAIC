#!/usr/bin/env bash
# Everything under AddressSanitizer and UBSan: the asan preset (build-asan/), the whole ctest suite there, the
# headless smoke run of the real binary, and a longer fuzz run. Usage: scripts/check-asan.sh [FUZZ_SECONDS]
# A sanitizer report fails the run; leak checking is off (LuaJIT and FTXUI keep allocations for the process's life).
set -uo pipefail
cd "$(dirname "$0")/.."
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
scripts/check.sh --asan || exit 1
echo "== fuzz for ${1:-20} s per target under asan"
FUZZ_SECONDS="${1:-20}" build-asan/core/fuzz_parsers || { echo "check-asan: the fuzzer found something" >&2; exit 1; }
echo "check-asan: all green"
