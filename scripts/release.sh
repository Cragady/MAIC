#!/usr/bin/env bash
# Builds a tagged release of maic into ~/program-files/maic/<tag>/ and points ~/bin/maic at it.
# Usage: scripts/release.sh v0.1.0-beta.2        (tags HEAD if the tag does not exist yet)
#        scripts/release.sh                      (reinstalls the tag HEAD is on, if any)
# The dev build in build/ is untouched, so the installed copy stays stable while development continues.
set -euo pipefail
cd "$(dirname "$0")/.."

tag="${1:-$(git describe --tags --exact-match 2>/dev/null || true)}"
if [ -z "$tag" ]; then
    echo "usage: $0 vX.Y.Z[-beta.N]   (HEAD is not on a tag)" >&2
    exit 1
fi
if ! git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    if [ -n "$(git status --porcelain)" ]; then
        echo "commit or stash your changes before tagging $tag" >&2
        exit 1
    fi
    git tag -a "$tag" -m "maic $tag"
    echo "tagged $tag"
fi

prefix="$HOME/program-files/maic/$tag"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git archive "$tag" | tar -x -C "$work"
# The release build uses the same vcpkg toolchain as the dev preset.
cmake -S "$work" -B "$work/build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DMAIC_GIT_VERSION="$tag" \
      -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
cmake --build "$work/build" -j --target maic >/dev/null
cmake --install "$work/build" >/dev/null
ln -sfn "$prefix/bin/maic" "$HOME/bin/maic"
echo "installed $("$prefix/bin/maic" --version) at $prefix; ~/bin/maic points to it"
echo "dev build stays at build/cli/maic (run it directly while testing)"
