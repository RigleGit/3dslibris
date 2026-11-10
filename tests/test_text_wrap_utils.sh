#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
build_test test_text_wrap_utils -- -I"$ROOT/include" -I"$ROOT/tests" \
  "$ROOT/tests/test_text_wrap_utils.cpp" "$ROOT/source/shared/text_wrap_utils.cpp"
