#!/bin/sh
# run-all -- convenience wrapper over the trans-fairy sub-commands (not the pipeline itself).
# Runs the full non-agent flow for one export against one target cwd.
#
#   bin/run-all.sh <export.json> <target-uuid> [--target-cwd PATH]
#
# For the agent-driven, one-stage-per-invocation flow, use --agent instead;
# this wrapper is the human convenience path.
set -e
EXPORT="$1"; UUID="$2"; shift 2 || true
if [ -z "$EXPORT" ] || [ -z "$UUID" ]; then
  echo "usage: run-all.sh <export.json> <target-uuid> [--target-cwd PATH]" >&2
  exit 2
fi
cai trans-fairy "$@" init
cai trans-fairy "$@" split --export "$EXPORT" --target-uuid "$UUID"
cai trans-fairy "$@" build
cai trans-fairy "$@" install
echo "done. resume with the printed 'claude -r ... --fork-session' command."
