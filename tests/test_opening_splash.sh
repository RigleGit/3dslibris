#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/ui/text.cpp" 'int Text::GetStringAdvance(' > "$TASK_TMP/opening_text_measure.inc"
# Private measurement helper is kept private in production.
if rg -q '^static int MeasureOpeningText\(' "$ROOT/source/reader/app_book_internal.cpp"; then
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book_internal.cpp" 'static int MeasureOpeningText(' >> "$TASK_TMP/opening_splash_under_test.inc"
fi
for signature in 'std::string EllipsizeToWidth(' 'std::vector<std::string> BuildOpeningTitleLines(' 'void DrawOpeningSplashImpl('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book_internal.cpp" "$signature" >> "$TASK_TMP/opening_splash_under_test.inc"
done
for signature in 'void App::SetOrientation(' 'void App::SetHandedness('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/app/app.cpp" "$signature" >> "$TASK_TMP/opening_orientation_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/app/app_views.cpp" 'void App::ShowCurrentBookView(' >> "$TASK_TMP/opening_orientation_under_test.inc"
build_test test_opening_splash -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" -I"$ROOT/third_party/utf8proc" \
 "$ROOT/tests/test_opening_splash.cpp"
