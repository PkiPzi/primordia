#!/usr/bin/env bash
# Launch Primordia (builds it first if needed).
#   ./run.sh               integrated GPU (default)
#   ./run.sh --nvidia      render on the NVIDIA GPU via PRIME offload
# Any other arguments go to the app, e.g. ./run.sh --seed 42
set -e
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BIN="$DIR/build/primordia"
if [ ! -x "$BIN" ]; then
  cmake -S "$DIR" -B "$DIR/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DIR/build" -j"$(nproc)"
fi
if [ "${1:-}" = "--nvidia" ]; then
  shift
  export __NV_PRIME_RENDER_OFFLOAD=1
  export __GLX_VENDOR_LIBRARY_NAME=nvidia
  export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
  export __VK_LAYER_NV_optimus=NVIDIA_only
fi
exec "$BIN" "$@"
