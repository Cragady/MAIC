#!/usr/bin/env bash
# Moves this machine's maic directories to their maid names, once, after the rename (scripts/rename-to-maid.py).
#
#   scripts/migrate-to-maid.sh            dry run: print every step, change nothing (the default)
#   scripts/migrate-to-maid.sh --apply    move the directories; everything else stays a printed step
#
# --apply moves directories and nothing else: it never opens, edits, renames or deletes a file of yours. The
# one file it reads is maic's own trust store, for the project directories it names. Everything else (files
# inside the moved directories, links, units, root-owned paths, the checkout) is printed for you to do. maid
# reads no maic path, so run this before the first maid start.
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

for p in maic maic-server maic-relay; do
    if pgrep -x "$p" >/dev/null; then
        echo "$p is running; quit it first (sessions park on quit; maic daemon stop for the daemon)" >&2
        [ "$apply" = 1 ] && exit 1
    fi
done

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
    while IFS= read -r d; do [ -n "$d" ] && trusted+=("$d"); done < <(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1])).get("dirs", {})))' "$store")
    break
done

echo "== directories ($([ "$apply" = 1 ] && echo moving || echo dry run))"
move "$config/maic" "$config/maid"
inside=""
for sub in artifacts reviews sessions vendor; do
    [ -d "$state/maic/$sub" ] && [ ! -e "$state/maid" ] && inside+=" $sub/"
done
move "$state/maic" "$state/maid"
[ -n "$inside" ] && echo "        with$inside"
if [ -n "$runtime" ]; then move "$runtime/maic" "$runtime/maid"; else move "/tmp/maic-$(id -u)" "/tmp/maid-$(id -u)"; fi
for d in "${trusted[@]}"; do
    move "$d/.maic" "$d/.maid"
done
true

# Paths inside the state directory are printed as they are after --apply; a dry run looks under the old one.
new_state="$state/maid"
seen_state="$state/maic"
[ -d "$seen_state" ] || seen_state="$new_state"

echo
echo "== files to rename yourself, after the move (maid reads only the maid names)"
for d in "${trusted[@]}"; do
    [ -f "$d/MAIC.md" ] && echo "  mv '$d/MAIC.md' '$d/MAID.md'"
done
for art in "$seen_state"/artifacts/*/; do
    [ -d "$art" ] || continue
    id=$(basename "$art")
    for f in artifact notify-protocol; do
        [ -f "$art.maic-$f.json" ] && echo "  mv '$new_state/artifacts/$id/.maic-$f.json' '$new_state/artifacts/$id/.maid-$f.json'"
    done
done
echo "  (until then an artifact reads as sandboxed, with ALLOW_INSECURE off and no approved notify protocol)"

echo
echo "== files that may still say maic (this script does not read them; check them yourself)"
echo "  grep -rIn -i maic '$config/maid' '$config/nvim' '$new_state/artifacts'"
for d in "${trusted[@]}"; do
    { [ -d "$d/.maic" ] || [ -d "$d/.maid" ]; } && echo "  grep -rIn -i maic '$d/.maid'"
    [ -f "$d/.mcp.json" ] && echo "  $d/.mcp.json: an MCP server maic (maic channel) becomes \"maid\": {\"command\": \"maid\", \"args\": [\"channel\"]}"
done
echo "  ~/.claude.json, ~/.claude/settings.json: an MCP server maic and rules naming mcp__maic__ become maid and mcp__maid__"
[ -e "$seen_state/vendor/comfyui/extra_model_paths.yaml" ] &&
    echo "  $new_state/vendor/comfyui/extra_model_paths.yaml: maid vendor wire comfyui writes a maid: block; delete the old maic: block"
vars=$(env | grep -o '^MAIC_[A-Z0-9_]*' | paste -sd' ' || true)
[ -n "$vars" ] && echo "  set in this shell, read by maid only as MAID_*: $vars"
true

echo
echo "== links that point at the old names (relink after the folder move below)"
for dir in "$state/maid/vendor" "$state/maic/vendor"; do
    [ -d "$dir" ] || continue
    { find "$dir" -maxdepth 2 -type l -printf '  %p -> %l\n' 2>/dev/null || true; } | { grep -i maic || true; }
done
# ComfyUI's custom node and workflow links in the checkout; wiring skips a link that exists, even a dangling one.
for dir in "$HOME/dev2/MAIC/vendor" "$HOME/dev2/MAID/vendor"; do
    [ -d "$dir" ] || continue
    find "$dir" -maxdepth 4 -type l \( -lname '*/dev2/MAIC/*' -o -lname '*/maic/*' \) -printf '  %p -> %l    (ln -sfn the maid path)\n' 2>/dev/null || true
done
for link in "$HOME"/bin/maic*; do
    [ -L "$link" ] && echo "  rm '$link'    # maid's release links ~/bin/maid*"
done
[ -d "$HOME/program-files/maic" ] && echo "  $HOME/program-files/maic: the maic releases; scripts/release.sh installs maid into ~/program-files/maid"
true

echo
echo "== systemd user units"
units=0
for unit in "$config"/systemd/user/maic-*; do
    [ -e "$unit" ] || continue
    units=1
    echo "  systemctl --user disable --now '$(basename "$unit")' && rm '$unit'"
done
[ "$units" = 1 ] && echo "  then, for the ones you had: maid audit-trail schedule install, maid daemon unit install"
if command -v docker >/dev/null && docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx maic-dk; then
    echo "  docker rm -f maic-dk    # maid starts its container as maid-dk"
fi

echo
echo "== root-owned paths (sudo; not done by this script)"
[ -e /var/lib/maic/tripwire ] && echo "  the maic tripwire is TRIPPED: maic unlock first, or trip maid's once it is installed"
if [ -e /usr/local/sbin/maic-lock ] || [ -e /etc/sudoers.d/maic ] || [ -e /var/lib/maic ]; then
    echo "  sudo ~/dev2/MAID/harness/install-tripwire.sh    # maid checks /var/lib/maid/tripwire and has none until this runs"
    echo "  sudo rm /etc/sudoers.d/maic /usr/local/sbin/maic-lock && sudo rm -rf /var/lib/maic"
fi
[ -d /etc/maic ] && echo "  sudo mv /etc/maic /etc/maid    # and its MAIC.md to MAID.md"
[ -d /etc/maic-relay ] && echo "  sudo mv /etc/maic-relay /etc/maid-relay    # the relay's certificates"
[ -d /usr/local/share/maic ] && echo "  sudo rm -rf /usr/local/bin/maic* /usr/local/share/maic    # an old install; maid installs as maid"
true

echo
echo "== the checkout (not done by this script)"
echo "  git -C ~/dev2/MAIC worktree list    # remove other worktrees first, or repair each after the move"
echo "  mv ~/dev2/MAIC ~/dev2/MAID && git -C ~/dev2/MAID worktree repair"
echo "  rm -rf ~/dev2/MAID/build            # its CMake cache holds the old path"
echo "  maid vendor wire comfyui            # or relink the links listed above"
echo "  the nvim spec: dir = \"~/dev2/MAIC/maic.nvim\" -> \"~/dev2/MAID/maid.nvim\", require(\"maic\") -> require(\"maid\")"

echo
echo "== in the browser"
echo "  /a/ asks for a new login (the cookie is maid_artifacts now); reload the phone page (the tunnel salt is maid-tunnel-v1)"
echo "  review drafts kept in the browser's IndexedDB maic-review-local are not carried over"
[ "$apply" = 1 ] || { echo; echo "dry run: nothing changed. Run with --apply to move the directories."; }
