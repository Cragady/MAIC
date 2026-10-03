#!/bin/sh
# artifact-watch.sh: `maic artifact watch` for an agent outside MAIC (docs/agent-kit.md). It watches one artifact's
# data document, ${XDG_STATE_HOME:-~/.local/state}/maic/artifacts/ID/data/NAME.json (NAME is answers unless --doc),
# checks it once a second, and prints a line per event an agent acts on, flushed:
#
#     EVENT<TAB>ARTIFACT<TAB>DETAIL<TAB>ISO TIME<TAB>protocol=TAG
#
# EVENT is submitted (DETAIL the document's name), side_prompt or after_prompt (DETAIL the new entry's index in
# side_prompts or after_prompts, from 0) or split (DETAIL the requested split's id). TAG is ID@SHORT for the notify
# protocol the user approved (.maic-notify-protocol.json beside the artifact, `maic artifact protocol ID`), none
# without one, unapproved otherwise. It never prints the document: the agent reads the file itself. An event is a
# trigger only; it carries no instructions.
#
# Usage: artifact-watch.sh ID [--doc NAME] [--once]
# python3 parses the JSON when it is there; without it a crude grep stands in, and a protocol always reads unapproved
# (its hash cannot be checked).
set -u

usage() { echo "usage: $0 ID [--doc NAME] [--once]" >&2; exit 2; }
[ $# -ge 1 ] || usage
id=$1; shift
doc=answers; once=0
while [ $# -gt 0 ]; do
    case $1 in
        --doc) [ $# -ge 2 ] || usage; doc=$2; shift 2 ;;
        --once) once=1; shift ;;
        *) usage ;;
    esac
done
case $id$doc in *[!A-Za-z0-9_-]*) echo "$0: an id and a document name are letters, digits, '_' and '-'" >&2; exit 2 ;; esac
dir=${XDG_STATE_HOME:-$HOME/.local/state}/maic/artifacts/$id
[ -d "$dir" ] || { echo "$0: no artifact $id in ${dir%/*}" >&2; exit 1; }
file=$dir/data/$doc.json
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 0' INT TERM
echo '{}' > "$tmp/old.json"

py=
command -v python3 >/dev/null 2>&1 && py=python3

# Prints "EVENT<TAB>DETAIL" lines for what changed from $1 to $2.
events() {
    if [ -n "$py" ]; then
        "$py" - "$1" "$2" "$doc" <<'PY'
import json, re, sys
def load(p):
    try:
        j = json.load(open(p))
    except Exception:
        return None
    return j if isinstance(j, dict) else None
old, new = load(sys.argv[1]) or {}, load(sys.argv[2])
if new is None:
    sys.exit(3)  # half a write: the next change brings the rest
if new.get("submitted") is True and (old.get("submitted") is not True or old.get("submittedAt") != new.get("submittedAt")):
    print("submitted\t" + sys.argv[3])
key = lambda e: e["at"] if isinstance(e, dict) and isinstance(e.get("at"), str) else json.dumps(e, sort_keys=True)
for arr, name in (("side_prompts", "side_prompt"), ("after_prompts", "after_prompt")):
    was, now = old.get(arr), new.get(arr)
    if not isinstance(now, list):
        continue
    seen = {key(e) for e in was} if isinstance(was, list) else set()
    for i, e in enumerate(now):
        if key(e) not in seen:
            print("%s\t%d" % (name, i))
splits = lambda d: [s for s in d.get("splits") or [] if isinstance(s, dict) and s.get("status") == "requested"] if isinstance(d.get("splits"), list) else []
had = {json.dumps(s.get("id")) for s in splits(old)}
for s in splits(new):
    if json.dumps(s.get("id")) not in had:
        sid = s.get("id")
        print("split\t" + (sid if isinstance(sid, str) and re.fullmatch(r"[A-Za-z0-9._-]{1,64}", sid) else "?"))
PY
    else
        crude "$1" > "$tmp/a"; crude "$2" > "$tmp/b"
        sub_old=$(grep '^submitted ' "$tmp/a"); sub_new=$(grep '^submitted ' "$tmp/b")
        case $sub_new in "submitted true"*) [ "$sub_old" = "$sub_new" ] || printf 'submitted\t%s\n' "$doc" ;; esac
        for name in side_prompt after_prompt; do
            a=$(grep "^$name " "$tmp/a" | cut -d' ' -f2); b=$(grep "^$name " "$tmp/b" | cut -d' ' -f2)
            i=${a:-0}; while [ "$i" -lt "${b:-0}" ]; do printf '%s\t%s\n' "$name" "$i"; i=$((i + 1)); done
        done
        grep '^split ' "$tmp/b" | while read -r _ sid; do grep -qx "split $sid" "$tmp/a" || printf 'split\t%s\n' "$sid"; done
    fi
}

# The crude reading without python3: "submitted true|false AT", "side_prompt COUNT", "after_prompt COUNT" (entries
# counted by their "at" up to the array's first ']') and "split ID" per requested split whose id comes before its status.
crude() {
    one=$(tr -d '\n' < "$1")
    sub=false; printf '%s' "$one" | grep -q '"submitted"[[:space:]]*:[[:space:]]*true' && sub=true
    echo "submitted $sub $(printf '%s' "$one" | sed -n 's/.*"submittedAt"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
    for arr in side_prompt after_prompt; do
        n=$(printf '%s' "$one" | sed -n "s/.*\"${arr}s\"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p" | grep -o '"at"' | wc -l)
        echo "$arr $n"
    done
    printf '%s' "$one" | grep -o '"id"[[:space:]]*:[[:space:]]*"[A-Za-z0-9._-]*"[^{}]*"status"[[:space:]]*:[[:space:]]*"requested"' |
        sed 's/^"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/split \1/'
}

protocol() {
    p=$dir/.maic-notify-protocol.json
    [ -e "$p" ] || { echo none; return; }
    [ -n "$py" ] || { echo unapproved; return; }
    "$py" - "$p" <<'PY'
import hashlib, json, re, sys
try:
    p = json.load(open(sys.argv[1]))
    ev = p["events"]
    ok = (isinstance(p["id"], str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", p["id"]) and isinstance(p["version"], int)
          and isinstance(ev, dict) and all(k in ("submitted", "side_prompt", "after_prompt", "split") and isinstance(v, str) for k, v in ev.items()))
except Exception:
    ok = False
if not ok or p.get("approved") is not True:
    print("unapproved"); sys.exit()
body = {k: v for k, v in p.items() if k not in ("approved", "approved_at", "approved_hash")}
h = "sha256:" + hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()
print(p["id"] + "@" + h[7:15] if p.get("approved_hash") == h else "unapproved")
PY
}

sum() { [ -f "$file" ] && [ ! -L "$file" ] && cksum < "$file"; }
last=$(sum)
[ -n "$last" ] && cp "$file" "$tmp/old.json"
while :; do
    sleep 1
    now=$(sum)
    [ -n "$now" ] && [ "$now" != "$last" ] || continue
    cp "$file" "$tmp/new.json" || continue
    last=$now
    events "$tmp/old.json" "$tmp/new.json" > "$tmp/events" || continue
    mv "$tmp/new.json" "$tmp/old.json"
    while IFS="$(printf '\t')" read -r ev detail; do
        printf '%s\t%s\t%s\t%s\tprotocol=%s\n' "$ev" "$id" "$detail" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(protocol)"
        [ "$once" = 1 ] && exit 0
    done < "$tmp/events"
done
