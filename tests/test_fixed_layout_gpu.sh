#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/app/app.cpp" 'bool App::PresentIfDirty(' > "$TASK_TMP/gpu_present_under_test.inc"
build_test test_fixed_layout_gpu -- -D__3DS__ -I"$TASK_TMP" -I"$ROOT/tests/gpu_stubs" -I"$ROOT/include" \
  "$ROOT/tests/test_fixed_layout_gpu.cpp" \
  "$ROOT/source/formats/common/fixed_layout_gpu_renderer.cpp" \
  "$ROOT/source/formats/common/fixed_layout_tile_grid_utils.cpp"
for scenario in flush upload rgb_upload low_memory fragmented frame_failure failure resize; do
  "$TEST_OUTDIR/test_fixed_layout_gpu" "$scenario"
done
