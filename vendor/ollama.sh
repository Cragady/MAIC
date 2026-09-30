#!/usr/bin/env bash
# Installs a pinned official Ollama Linux release into $MAIC_VENDOR/ollama/<version>/ after verifying it
# against the checksum file published with that release, and points $MAIC_VENDOR/ollama/current at it.
# Privacy is enforced at run time by services/ollama.json (OLLAMA_NO_CLOUD, no remotes, loopback only).
#
#   vendor/ollama.sh install   MAIC_VENDOR=<state>/vendor  OLLAMA_VERSION=v0.35.0  OLLAMA_URL=... OLLAMA_CHECKSUMS=...
set -euo pipefail
cmd="${1:-install}"
: "${MAIC_VENDOR:?set by maic}"
: "${OLLAMA_VERSION:?set by maic from vendor/manifest.json}"
dir="$MAIC_VENDOR/ollama"
mkdir -p "$dir"

case "$cmd" in
wire)
    [ -L "$dir/cli-history" ] || ln -s "$HOME/.ollama/history" "$dir/cli-history"
    "$dir/current/bin/ollama" --version
    ;;
install)
    if [ -x "$dir/$OLLAMA_VERSION/bin/ollama" ]; then
        echo "ollama $OLLAMA_VERSION already installed"
    elif [ -x "$dir/current/bin/ollama" ] && [ ! -e "$dir/$OLLAMA_VERSION" ]; then
        echo "using the adopted install at $(readlink -f "$dir/current"); maic vendor unlink ollama first to download $OLLAMA_VERSION"
    else
        for tool in curl zstd tar sha256sum; do command -v $tool >/dev/null || { echo "$tool is required" >&2; exit 1; }; done
        tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
        echo "downloading ollama $OLLAMA_VERSION ..."
        curl -fL --progress-bar -o "$tmp/ollama.tar.zst" "$OLLAMA_URL"
        curl -fsSL -o "$tmp/sha256sum.txt" "$OLLAMA_CHECKSUMS"
        want="$(grep 'ollama-linux-amd64.tar.zst$' "$tmp/sha256sum.txt" | awk '{print $1}')"
        have="$(sha256sum "$tmp/ollama.tar.zst" | awk '{print $1}')"
        [ -n "$want" ] || { echo "no checksum for the linux amd64 archive in the release's sha256sum.txt" >&2; exit 1; }
        [ "$want" = "$have" ] || { echo "CHECKSUM MISMATCH: expected $want, got $have; not installing" >&2; exit 1; }
        echo "checksum ok"
        mkdir -p "$dir/$OLLAMA_VERSION"
        zstd -dc "$tmp/ollama.tar.zst" | tar -x -C "$dir/$OLLAMA_VERSION"
    fi
    ln -sfn "$dir/$OLLAMA_VERSION" "$dir/current"
    # The `ollama run` readline history is an artifact worth seeing from maic.
    [ -L "$dir/cli-history" ] || ln -s "$HOME/.ollama/history" "$dir/cli-history"
    "$dir/current/bin/ollama" --version
    echo "ollama at $dir/current (bin/ollama)"
    ;;
check)
    [ -x "$dir/current/bin/ollama" ] && "$dir/current/bin/ollama" --version || { echo "not installed"; exit 1; }
    ;;
*)
    echo "usage: $0 install|wire|check" >&2
    exit 2
    ;;
esac
