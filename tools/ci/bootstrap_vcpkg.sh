#!/usr/bin/env bash
# Project-local clone, pinned by vcpkg.json. Optional argument: cache directory.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
export TMPDIR="$root/.cache/tmp-linux"
mkdir -p "$TMPDIR"
python3 "$root/tools/ci/bootstrap_vcpkg.py" "$@"
