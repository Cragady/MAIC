#!/bin/sh
# json_pick: a MAIC script tool in shell, with jq doing the work. See docs/tools.md for the format.
#
# The arguments arrive as one JSON object on stdin; jq pulls the two fields out, then runs the filter over
# the file. Output is the result; a failing jq (bad file, bad filter) exits non-zero and the model sees stderr.
args=$(cat)
file=$(printf '%s' "$args" | jq -r '.file')
filter=$(printf '%s' "$args" | jq -r '.filter')
[ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
jq -r "$filter" "$file"
