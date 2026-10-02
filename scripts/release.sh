#!/usr/bin/env bash
# Builds a tagged release of maic into ~/program-files/maic/<tag>/ and points ~/bin/maic at it.
# Usage: scripts/release.sh v0.1.0-beta.2        (tags HEAD if the tag does not exist yet)
#        scripts/release.sh                      (reinstalls the tag HEAD is on, if any)
# Runs scripts/check.sh (configure, build, ctest, the python suites) first and refuses to release when it fails.
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
fi
# The gate first, on the dev build: a release is never cut from a tree whose build or tests fail.
scripts/check.sh || { echo "release: scripts/check.sh failed; nothing tagged or installed" >&2; exit 1; }
if ! git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    git tag -a "$tag" -m "maic $tag"
    echo "tagged $tag"
fi

prefix="$HOME/program-files/maic/$tag"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git archive "$tag" | tar -x -C "$work"
# git archive leaves submodules out; the build needs the pinned LuaJIT source (the others are runtime only).
mkdir -p "$work/vendor/lua-pins"
git -C vendor/lua-pins archive HEAD | tar -x -C "$work/vendor/lua-pins"
# The installed binary keeps using this repository as its root (services/, vendor/ scripts and submodules),
# so `maic vendor add` and `maic up` work from the release the same as from the dev build.
cmake -S "$work" -B "$work/build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DMAIC_GIT_VERSION="$tag" \
      -DMAIC_SOURCE_ROOT="$(pwd)" -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
cmake --build "$work/build" -j --target maic maic-server maic-relay >/dev/null
cmake --install "$work/build" >/dev/null
# Everything the install put in bin gets its ~/bin link, so a helper added to CMakeLists.txt is never left out:
# maic, maic-server, maic-relay, maic-workflow-edit, maic-storyboard, maic-danbooru-tags, maic-panel-check,
# maic-leak-audit, maic-diction, cai and maic-cai (a link to the `cai` wrapper, docs/cai.md).
for exe in "$prefix"/bin/*; do
    ln -sfn "$exe" "$HOME/bin/$(basename "$exe")"
done
echo "installed $("$prefix/bin/maic" --version) at $prefix; ~/bin/maic points to it"
echo "dev build stays at build/cli/maic (run it directly while testing)"
