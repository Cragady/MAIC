#!/usr/bin/env bash
# Moves this machine's maic directories to their maid names, once, after the rename (scripts/rename-to-maid.py).
#
#   scripts/migrate-to-maid.sh            dry run: print every step, change nothing (the default)
#   scripts/migrate-to-maid.sh --apply    move the directories; everything else stays a printed step
#
# --apply moves directories and nothing else: it never opens, edits or deletes a file. Files that still say
# maic (settings, project settings, the nvim spec, instruction files) are listed by path and line number
# only, for you to edit. maid reads no maic path, so run this before the first maid start.
set -euo pipefail

apply=0
case "${1:-}" in
    --apply) apply=1 ;;
    ""|--dry-run) ;;
    *) echo "usage: $0 [--dry-run|--apply]" >&2; exit 2 ;;
esac

config="${XDG_CONFIG_HOME:-$HOME/.config}"
state="${XDG_STATE_HOME:-$HOME/.local/state}"
runtime="${XDG_RUNTIME_DIR:-}"

if pgrep -x maic >/dev/null || pgrep -x maic-server >/dev/null || pgrep -x maic-relay >/dev/null; then
    echo "a maic process is running; quit it first (sessions park on quit)" >&2
    [ "$apply" = 1 ] && exit 1
fi

move() {
    local from="$1" to="$2"
    [ -e "$from" ] || return 0
    if [ -e "$to" ]; then
        echo "  skip  $from  ($to already exists; merge by hand)"
        return 0
    fi
    echo "  move  $from -> $to"
    [ "$apply" = 1 ] && mv -- "$from" "$to"
    return 0
}

# The trust store names every directory maic was ever asked about; read before its directory moves.
trusted=()
for store in "$state/maic/trust.json" "$state/maid/trust.json"; do
    [ -f "$store" ] || continue
    while IFS= read -r d; do trusted+=("$d"); done < <(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1])).get("dirs", {})))' "$store")
    break
done

echo "== directories ($([ "$apply" = 1 ] && echo moving || echo dry run))"
move "$config/maic" "$config/maid"
move "$state/maic" "$state/maid"
[ -n "$runtime" ] && move "$runtime/maic" "$runtime/maid"
for d in "${trusted[@]}"; do
    move "$d/.maic" "$d/.maid"
done

echo
echo "== instruction files to rename yourself (maid reads MAID.md, not MAIC.md)"
for d in "${trusted[@]}"; do
    [ -f "$d/MAIC.md" ] && echo "  mv '$d/MAIC.md' '$d/MAID.md'"
done
true

echo
echo "== files that still say maic (path:line; edit them yourself)"
for dir in "$config/maid" "$config/maic" "$config/nvim"; do
    [ -d "$dir" ] && { grep -rIni maic -- "$dir" 2>/dev/null || true; } | cut -d: -f1,2 | sed 's/^/  /'
done
for d in "${trusted[@]}"; do
    for sub in .maid .maic; do
        [ -d "$d/$sub" ] && { grep -rIni maic -- "$d/$sub" 2>/dev/null || true; } | cut -d: -f1,2 | sed 's/^/  /'
    done
done
true

echo
echo "== links that point at the old names (relink after the folder move below)"
for dir in "$state/maid/vendor" "$state/maic/vendor"; do
    [ -d "$dir" ] || continue
    { find "$dir" -maxdepth 2 -type l -printf '  %p -> %l\n' 2>/dev/null || true; } | { grep -i maic || true; }
done
for link in "$HOME"/bin/maic*; do
    [ -L "$link" ] && echo "  rm '$link'    # maid's release links ~/bin/maid*"
done
true

echo
echo "== systemd user units"
units=0
for unit in "$config"/systemd/user/maic-*; do
    [ -e "$unit" ] || continue
    units=1
    echo "  systemctl --user disable --now '$(basename "$unit")' && rm '$unit'"
done
[ "$units" = 1 ] && echo "  then: maid audit-trail schedule install"
if command -v docker >/dev/null && docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx maic-dk; then
    echo "  docker rm -f maic-dk    # maid starts its container as maid-dk"
fi

echo
echo "== the checkout (not done by this script)"
echo "  git -C ~/dev2/MAIC worktree list    # remove other worktrees first"
echo "  mv ~/dev2/MAIC ~/dev2/MAID && git -C ~/dev2/MAID worktree repair"
echo "  rm -rf ~/dev2/MAID/build            # its CMake cache holds the old path"
echo "  maid vendor wire comfyui            # or relink the links listed above"
echo "  the nvim spec: dir = \"~/dev2/MAID/maid.nvim\", require(\"maid\")"
[ "$apply" = 1 ] || { echo; echo "dry run: nothing changed. Run with --apply to move the directories."; }
