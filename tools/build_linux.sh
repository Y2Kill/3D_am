#!/usr/bin/env bash
# Shared local/CI build; all downloads and temporary files stay in the project.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
case "${1:-}" in
  min|cli) preset="linux-$1"; shift ;;
  *) echo 'Usage: tools/build_linux.sh min|cli [--offline] [--ci] [ctest-args...]' >&2; exit 2 ;;
esac
offline=false
ci=false
ctest_args=()
while (($#)); do
  case "$1" in
    --offline) offline=true ;;
    --ci) ci=true ;;
    *) ctest_args+=("$1") ;;
  esac
  shift
done
export TMPDIR="$root/.cache/tmp-linux"
mkdir -p "$TMPDIR"
configure_args=()
test_args=()
if $ci; then
  configure_args+=(-DAM_REQUIRE_PYTHON_TESTS=ON)
  test_args+=(--output-on-failure --output-junit "$root/build/$preset/junit.xml")
else
  configure_args+=(-DAM_REQUIRE_PYTHON_TESTS=OFF)
fi
if [[ -n "${AM_PYTHON_EXTRA_PATH:-}" ]]; then
  configure_args+=("-DAM_PYTHON_EXTRA_PATH=$AM_PYTHON_EXTRA_PATH")
fi
if [[ "$preset" == linux-cli ]]; then
  export VCPKG_ROOT="$root/.cache/vcpkg-linux"
  export VCPKG_DISABLE_METRICS=1
  export VCPKG_DOWNLOADS="$VCPKG_ROOT/downloads"
  export VCPKG_REGISTRIES_CACHE="$root/.cache/vcpkg-registries-linux"
  export VCPKG_DEFAULT_BINARY_CACHE="$root/.cache/vcpkg-bin-linux"
  export VCPKG_BINARY_SOURCES="clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite"
  export X_VCPKG_ASSET_SOURCES="clear;x-azurl,file://$root/.cache/vcpkg-assets,,readwrite"
  if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    echo 'ERROR: Run tools/ci/bootstrap_vcpkg.sh first.' >&2
    exit 1
  fi
  mkdir -p "$VCPKG_DOWNLOADS" "$VCPKG_REGISTRIES_CACHE" "$VCPKG_DEFAULT_BINARY_CACHE" "$root/.cache/vcpkg-assets"
  if $offline; then
    export VCPKG_BINARY_SOURCES="clear;files,$VCPKG_DEFAULT_BINARY_CACHE,read"
    export X_VCPKG_ASSET_SOURCES="clear;x-azurl,file://$root/.cache/vcpkg-assets,,read;x-block-origin"
  fi
  printf '%s\n' "VCPKG_BINARY_SOURCES=$VCPKG_BINARY_SOURCES" "X_VCPKG_ASSET_SOURCES=$X_VCPKG_ASSET_SOURCES"
else
  # FetchContent includes build/subbuild caches, which cannot be shared with MSVC in WSL.
  configure_args+=("-DFETCHCONTENT_BASE_DIR=$root/.cache/fetchcontent/linux")
  if $offline; then
    configure_args+=(-DFETCHCONTENT_FULLY_DISCONNECTED=ON)
  else
    configure_args+=(-DFETCHCONTENT_FULLY_DISCONNECTED=OFF)
  fi
fi
cmake --preset "$preset" "${configure_args[@]}"
cmake --build --preset "$preset"
ctest_result=0
ctest --preset "$preset" "${test_args[@]}" "${ctest_args[@]}" || ctest_result=$?
report_result=0
if $ci; then
  python3 -I tools/coverage/req_report.py --build-dir "$root/build/$preset" \
    --matrix "$root/docs/plan/acceptance_matrix.md" || report_result=$?
fi
if ((ctest_result)); then exit "$ctest_result"; fi
exit "$report_result"
