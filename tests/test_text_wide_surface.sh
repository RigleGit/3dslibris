#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
if ! pkg-config --exists freetype2; then
  echo 'SKIP: wide surface integration needs host FreeType headers'
  exit 77
fi
# Keep actual Text, surface and blitter owners. Extract only the existing
# renderer lifecycle/accessors to avoid pulling glyph/image decoder dependencies.
for signature in 'TextRenderer::TextRenderer(' 'TextRenderer::~TextRenderer(' 'void TextRenderer::SetScreen(' 'u16 *TextRenderer::GetScreen(' 'void TextRenderer::SetOrientation(' 'u8 TextRenderer::GetOrientation(' 'void TextRenderer::SetColorMode(' 'int TextRenderer::GetColorMode('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/ui/text_renderer.cpp" "$signature" >> "$TASK_TMP/renderer_accessors.inc"
done
if [ "$(uname -s)" = Darwin ]; then
  DEAD_STRIP=-Wl,-dead_strip
else
  DEAD_STRIP=-Wl,--gc-sections
fi
build_test test_text_wide_surface -- -ffunction-sections -fdata-sections "$DEAD_STRIP" \
  $(pkg-config --cflags freetype2) -I"$TASK_TMP" \
  -I"$ROOT/tests/stubs/wide_surface" -I"$ROOT/include" \
  "$ROOT/tests/test_text_wide_surface.cpp" \
  "$ROOT/source/ui/text.cpp" "$ROOT/source/ui/text_screen_surface.cpp" \
  "$ROOT/source/ui/text_wide_surface.cpp" \
  "$ROOT/source/formats/common/fixed_layout_blit_utils.cpp"
