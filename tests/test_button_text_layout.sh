#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
build_test test_button_text_layout -- -I"$ROOT/include" -I"$ROOT/tests" \
  "$ROOT/tests/test_button_text_layout.cpp"
