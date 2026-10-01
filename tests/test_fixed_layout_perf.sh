#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
build_test test_fixed_layout_perf -- \
  -pthread -DDSLIBRIS_DEBUG -DFIXED_PERF_HOST_TEST \
  -I"$ROOT/tests/stubs/perf" -I"$ROOT/include" \
  "$ROOT/tests/test_fixed_layout_perf.cpp" \
  "$ROOT/source/shared/fixed_layout_perf.cpp"
