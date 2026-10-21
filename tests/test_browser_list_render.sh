#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE_ROOT="${MENU_ORIENTATION_SOURCE_ROOT:-$ROOT}"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'static std::string TrimSpacesLocal(' 'size_t Utf8BytesForCharCount(' 'void DrawWrappedTitleInsideCover('; do
  python3 "$ROOT/tests/extract_test_function.py" \
    "$SOURCE_ROOT/source/library/browser_presentation_utils.cpp" "$signature" \
    >> "$TASK_TMP/browser_wrapped_title_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" \
  "$SOURCE_ROOT/source/library/browser_list_view.cpp" 'void DrawPage(' \
  > "$TASK_TMP/browser_list_render_under_test.inc"
build_test test_browser_list_render -- -I"$ROOT/tests/stubs" \
  -I"$SOURCE_ROOT/include" -I"$TASK_TMP" \
  "$ROOT/tests/test_browser_list_render.cpp" \
  "$ROOT/source/library/browser_view_utils.cpp"
