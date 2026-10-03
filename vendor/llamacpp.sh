#!/usr/bin/env bash
# Builds the vendored llama.cpp OUT OF TREE into $MAID_VENDOR/llama.cpp-build/ (Release; CUDA when nvcc is
# around; no TLS, so the binaries cannot fetch models from Hugging Face or any other https host) and links
# its bin/ in as $MAID_VENDOR/llamacpp/bin. The model llama-server loads is the symlink
# $MAID_VENDOR/llamacpp/current-model.gguf, which `maid vendor use llamacpp PATH` points at a GGUF;
# services/llamacpp.json runs the server on 127.0.0.1:8081 against it. Idempotent: run again to rebuild.
#
#   vendor/llamacpp.sh install|update|wire|check   MAID_VENDOR=<state>/vendor  MAID_STATE=<state>
#   install/update configure and build (no network: the submodule is already fetched); wire only links bin/.
#
# The checkout is $MAID_VENDOR/llama.cpp (a symlink to the submodule, or to an adopted checkout).
set -euo pipefail
cmd="${1:-install}"
: "${MAID_VENDOR:?set by maid}"
: "${MAID_STATE:?set by maid}"
src="$MAID_VENDOR/llama.cpp"
build="$MAID_VENDOR/llama.cpp-build"
home="$MAID_VENDOR/llamacpp"
[ -f "$src/CMakeLists.txt" ] || { echo "no llama.cpp at $src (maid vendor add llamacpp, or maid vendor adopt llamacpp PATH)" >&2; exit 1; }

wire() {
    mkdir -p "$home"
    if [ ! -x "$build/bin/llama-server" ]; then
        echo "not built yet: maid vendor add llamacpp"
        return 0
    fi
    ln -sfn "$build/bin" "$home/bin"
    echo "llama.cpp at $home/bin (llama-server, llama-cli, llama-quantize, llama-gguf-split)"
    if [ -e "$home/current-model.gguf" ]; then
        echo "model: $(readlink -f "$home/current-model.gguf")"
    else
        echo "no model yet: maid vendor use llamacpp /path/to/model.gguf"
    fi
}

case "$cmd" in
wire)
    wire
    ;;
install|update)
    command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
    # GGML_CUDA needs nvcc; the toolkit under /usr/local/cuda is not always on PATH. A CPU build still runs
    # llama-server, only slower, and ignores -ngl with a warning.
    cuda=OFF
    if command -v nvcc >/dev/null; then
        cuda=ON
    elif [ -x /usr/local/cuda/bin/nvcc ]; then
        cuda=ON
        export CUDACXX=/usr/local/cuda/bin/nvcc
    fi
    echo "configuring llama.cpp (GGML_CUDA=$cuda) in $build ..."
    # LLAMA_OPENSSL=OFF: no TLS, so `-hf` and any https model URL fail instead of downloading; the server
    # runs on loopback and needs none. (LLAMA_CURL is a deprecated no-op at this tag.)
    cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=$cuda -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF
    cmake --build "$build" --config Release -j"$(nproc)" --target llama-server llama-cli llama-quantize llama-gguf-split
    wire
    "$home/bin/llama-server" --version
    ;;
check)
    [ -x "$home/bin/llama-server" ] && "$home/bin/llama-server" --version || { echo "not installed"; exit 1; }
    ;;
*)
    echo "usage: $0 install|update|wire|check" >&2
    exit 2
    ;;
esac
