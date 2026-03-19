#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT_DIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUT_DIR"

clang++ -std=c++17 -Wall -Wextra -Werror -I"$ROOT_DIR/include" \
  -I"$ROOT_DIR/tests/stubs" \
  "$ROOT_DIR/tests/test_parse_page_buffer.cpp" \
  "$ROOT_DIR/source/core/parse.cpp" \
  -o "$OUT_DIR/test_parse_page_buffer"

"$OUT_DIR/test_parse_page_buffer"
