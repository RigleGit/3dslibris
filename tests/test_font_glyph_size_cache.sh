#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
if ! command -v pkg-config >/dev/null || ! pkg-config --exists freetype2; then
  echo "SKIP: native FreeType development files unavailable"
  exit 77
fi
export TEST_ADVANCE_FONT_DIR="$TEST_ROOT/sdmc/3ds/3dslibris/font"
export TEST_ADVANCE_FALLBACK="$TEST_ROOT/third_party/mupdf/resources/fonts/noto/NotoSerifArmenian-Regular.otf"
FT_FLAGS=()
while IFS= read -r flag; do
  [ -n "$flag" ] && FT_FLAGS+=("$flag")
done < <(split_flags "$(pkg-config --cflags --libs freetype2)")
build_test test_font_glyph_size_cache -- -fno-exceptions -fno-rtti \
  -I"$TEST_ROOT/tests/stubs/resource_owner" \
  "$TEST_ROOT/tests/test_font_glyph_size_cache.cpp" \
  "$TEST_ROOT/source/ui/font_manager.cpp" \
  "$TEST_ROOT/source/shared/debug_log.cpp" "${FT_FLAGS[@]}"
