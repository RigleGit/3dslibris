#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
if ! command -v pkg-config >/dev/null || ! pkg-config --exists freetype2; then
  echo 'SKIP: native FreeType development files unavailable'
  exit 77
fi
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
export TEST_STDIO_FONT_DIR="$TEST_ROOT/sdmc/3ds/3dslibris/font"
python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/app/app.cpp" \
  'void App::InitScreens(' > "$TASK_TMP/stdio_handoff.inc"
FT_FLAGS=()
while IFS= read -r flag; do
  [ -n "$flag" ] && FT_FLAGS+=("$flag")
done < <(split_flags "$(pkg-config --cflags --libs freetype2)")
# Observe stdout at the SDK device boundary, without a production test seam.
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -fno-exceptions -fno-rtti \
  -Dprintf=PlatformPrintf -I"$TEST_ROOT/tests/stubs/resource_owner" \
  -I"$TEST_ROOT/include" "${FT_FLAGS[@]}" -c \
  "$TEST_ROOT/source/ui/font_manager.cpp" -o "$TASK_TMP/fonts.o"
build_test test_stdio_framebuffer_handoff -- -fno-exceptions -fno-rtti \
  -I"$TASK_TMP" -I"$TEST_ROOT/tests/stubs/resource_owner" \
  "$TEST_ROOT/tests/test_stdio_framebuffer_handoff.cpp" "$TASK_TMP/fonts.o" \
  "$TEST_ROOT/source/shared/debug_log.cpp" "${FT_FLAGS[@]}"
