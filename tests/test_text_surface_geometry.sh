#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 - "$ROOT/source/ui/text_screen_surface.cpp" > "$TASK_TMP/text_surface_under_test.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1])
text=source.read_text()
start=text.index('namespace {')
end=text.index('void TextRenderer::CopyScreen(')
print('#line %d "%s"' % (text.count('\n', 0, start)+1, source))
print(text[start:end])
PY
for signature in 'void TextRenderer::ClearScreen()' 'void TextRenderer::ClearRect(' 'void TextRenderer::MarkScreenDirty(' 'void TextRenderer::MarkScreenDirtyRect(' 'void TextRenderer::MarkCurrentScreenDirtyRect('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/ui/text_screen_surface.cpp" "$signature" >> "$TASK_TMP/text_surface_under_test.inc"
done
for signature in 'u16 *TextRenderer::GetScreen(' 'void TextRenderer::SetScreen(' 'int TextRenderer::GetColorMode(' 'void TextRenderer::SetColorMode(' 'void TextRenderer::PrintSplash('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/ui/text_renderer.cpp" "$signature" >> "$TASK_TMP/text_surface_under_test.inc"
done
build_test test_text_surface_geometry -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_text_surface_geometry.cpp"
