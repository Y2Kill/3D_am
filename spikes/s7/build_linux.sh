#!/usr/bin/env bash
# Isolated Release spike. Do not change the user's shell/global configuration.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
export TMPDIR="$root/.cache/tmp-linux"
export VCPKG_ROOT="$root/.cache/vcpkg-linux"
export VCPKG_DISABLE_METRICS=1
export VCPKG_DOWNLOADS="$VCPKG_ROOT/downloads"
export VCPKG_REGISTRIES_CACHE="$root/.cache/vcpkg-registries-linux"
export VCPKG_DEFAULT_BINARY_CACHE="$root/.cache/vcpkg-bin-linux"
export VCPKG_BINARY_SOURCES="clear;files,$root/.cache/vcpkg-bin-linux,readwrite"
export X_VCPKG_ASSET_SOURCES="clear;x-azurl,file://$root/.cache/vcpkg-assets,,readwrite"
mkdir -p "$TMPDIR" "$VCPKG_DOWNLOADS" "$VCPKG_REGISTRIES_CACHE" "$VCPKG_DEFAULT_BINARY_CACHE" "$root/.cache/vcpkg-assets"
if [[ "${1:-}" == --offline ]]; then
  export VCPKG_BINARY_SOURCES="clear;files,$root/.cache/vcpkg-bin-linux,read"
  export X_VCPKG_ASSET_SOURCES="clear;x-azurl,file://$root/.cache/vcpkg-assets,,read;x-block-origin"
  shift
fi
printf '%s\n' "VCPKG_BINARY_SOURCES=$VCPKG_BINARY_SOURCES" "X_VCPKG_ASSET_SOURCES=$X_VCPKG_ASSET_SOURCES"
cmake -S spikes/s7 -B build/s7-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  "-DVCPKG_INSTALLED_DIR=$root/.cache/vcpkg-installed-s7-linux" -DVCPKG_TARGET_TRIPLET=x64-linux
cmake --build build/s7-linux --parallel 2
ctest --test-dir build/s7-linux --output-on-failure "$@"
