#!/usr/bin/env bash
# REFERENCE EXEMPLAR — DO NOT RUN. NOT AN IMPLEMENTATION.
#
# The hand-driven stage 01 (`split`) as it was actually executed on 2026-08-24,
# recovered 2026-08-28 from the building session's transcript:
#
#   ~/.claude/projects/-home-cragady-dev2-ping-pong-SOP-tmp/
#     69ce886c-f27a-4a98-b929-b08b651feb40.jsonl   (lines 31, 35, 38)
#
# DESIGN.md recorded this stage as "exists only as an unsaved heredoc." Its only
# copy lived in a transcript — outside every repository, under no version
# control, in a project directory that can be deleted with no git underneath.
# That is why it was recovered before anything else in trans-fairy was touched.
#
# It is kept for the same reason `previous-agent/` is kept: it is the model the
# builder was written against, and proof the shape works on a real export. It is
# NOT the specification. `split` gets written against DESIGN.md.
#
# ---------------------------------------------------------------------------
# TWO INVARIANTS WERE BORN HERE. Both are visible below.
#
#   Invariant 2 — "Backups never use `cp -n`. It fails open."
#       Block B opens with `cp -n`. If the .bak already exists, cp declines
#       silently, returns success, and `&&` carries straight on into the
#       destructive write. The backup that did not happen is indistinguishable
#       from the backup that did.
#
#   Invariant 10 — "A backup is any operation that makes a copy, never one that
#       consumes the original."
#       Block B rewrites conversations.json in place, removing the extracted
#       conversation from the source. The source is consumed by the split.
#
# Read them as the reason those invariants exist, not as an example to follow.
# ---------------------------------------------------------------------------


# --- A. probe the export's shape ------------------------------------------
python3 -c "
import json
d=json.load(open('conversations.json'))
print(type(d), len(d))
print(list(d[0].keys()) if isinstance(d,list) else list(d.keys()))
"


# --- B. the split itself --------------------------------------------------
# BOTH ANTI-PATTERNS ARE IN THIS BLOCK. See header.
cp -n conversations.json conversations.json.bak && python3 - <<'EOF'
import json

tgt = json.load(open('conversation-target.json'))
uuid = tgt['uuid']

convs = json.load(open('conversations.json'))
match = [c for c in convs if c.get('uuid') == uuid]
rest  = [c for c in convs if c.get('uuid') != uuid]

assert len(match) == 1, f"expected 1 match, got {len(match)}"
c = match[0]
print(f"matched: {c['name']!r}  created={c['created_at']}  messages={len(c['chat_messages'])}")

with open('/home/cragady/dev2/ping-pong-SOP/tmp/ping-pong-SOP.json', 'w') as f:
    json.dump(c, f, indent=2, ensure_ascii=False)

with open('conversations.json', 'w') as f:
    json.dump(rest, f, indent=2, ensure_ascii=False)

print(f"remaining conversations: {len(rest)}")
EOF


# --- C. verification, and this part is worth keeping ----------------------
# Asserts the extracted conversation is gone from the remainder. A positive
# check on the outcome rather than on the absence of errors, which is the
# discipline APPARATUS.md Component 4 argues for.
python3 -c "
import json
c=json.load(open('/home/cragady/dev2/ping-pong-SOP/tmp/ping-pong-SOP.json'))
r=json.load(open('conversations.json'))
print('extracted uuid:', c['uuid'])
print('present in remaining:', any(x['uuid']==c['uuid'] for x in r))
print('remaining names:', [x['name'] for x in r])
"
