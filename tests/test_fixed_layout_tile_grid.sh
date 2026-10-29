#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
build_test test_fixed_layout_tile_grid -- -I"$ROOT/include" \
  "$ROOT/tests/test_fixed_layout_tile_grid.cpp" \
  "$ROOT/source/formats/common/fixed_layout_tile_grid_utils.cpp"
