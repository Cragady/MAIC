#!/usr/bin/env bash
# Builds the vendored whisper.cpp OUT OF TREE into $MAIC_VENDOR/whisper.cpp-build/ (Release; CUDA when nvcc is
# around; WHISPER_CURL off, so nothing in it can fetch a model) and links its bin/ in as $MAIC_VENDOR/whisper/bin.
# The model whisper-server loads is the symlink ${MAIC_MODELS}/whisper/current.bin, which
# `maic vendor use whisper FILE` points at a ggml .bin; services/whisper.json runs the server on 127.0.0.1:8083
# against it. diction is its only client (docs/diction.md). Idempotent: run again to rebuild.
#
#   vendor/whispercpp.sh install|update|wire|check   MAIC_VENDOR=<state>/vendor  MAIC_STATE=<state>
#   install/update configure and build (no network: the submodule is already fetched); wire only links bin/.
#
# The checkout is $MAIC_VENDOR/whisper.cpp (a symlink to the submodule, or to an adopted checkout).
set -euo pipefail
cmd="${1:-install}"
: "${MAIC_VENDOR:?set by maic}"
: "${MAIC_STATE:?set by maic}"
src="$MAIC_VENDOR/whisper.cpp"
build="$MAIC_VENDOR/whisper.cpp-build"
home="$MAIC_VENDOR/whisper"
models="${MAIC_MODELS_DIR:-$MAIC_STATE/models}/whisper"
[ -f "$src/CMakeLists.txt" ] || { echo "no whisper.cpp at $src (maic vendor add whisper, or maic vendor adopt whisper PATH)" >&2; exit 1; }

wire() {
    mkdir -p "$home"
    if [ ! -x "$build/bin/whisper-server" ]; then
        echo "not built yet: maic vendor add whisper"
        return 0
    fi
    ln -sfn "$build/bin" "$home/bin"
    echo "whisper.cpp at $home/bin (whisper-server, whisper-cli)"
    if [ -e "$models/current.bin" ]; then
        echo "model: $(readlink -f "$models/current.bin")"
    else
        echo "no model yet: maic vendor use whisper /path/to/ggml-model.bin (docs/diction.md names one)"
    fi
}

case "$cmd" in
wire)
    wire
    ;;
install|update)
    command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
    cuda=OFF
    if command -v nvcc >/dev/null; then
        cuda=ON
    elif [ -x /usr/local/cuda/bin/nvcc ]; then
        cuda=ON
        export CUDACXX=/usr/local/cuda/bin/nvcc
    fi
    echo "configuring whisper.cpp (GGML_CUDA=$cuda) in $build ..."
    # WHISPER_CURL=OFF: no downloader in the binaries. FFMPEG and SDL2 off: diction sends 16 kHz mono WAV itself
    # and captures the mic with parecord, so neither the server's --convert nor the stream examples are needed.
    cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=$cuda -DWHISPER_CURL=OFF -DWHISPER_SDL2=OFF \
          -DWHISPER_COMMON_FFMPEG=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DWHISPER_BUILD_SERVER=ON
    cmake --build "$build" --config Release -j"$(nproc)" --target whisper-server whisper-cli
    wire
    "$home/bin/whisper-cli" --help 2>&1 | head -1
    ;;
check)
    [ -x "$home/bin/whisper-server" ] || { echo "not installed"; exit 1; }
    ;;
*)
    echo "usage: $0 install|update|wire|check" >&2
    exit 2
    ;;
esac
