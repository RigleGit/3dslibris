#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'static BrowserViewMode CurrentBrowserViewMode(' 'static int CurrentBrowserPageSize(' 'static bool ShouldCurrentBrowserLoadCovers(' 'static std::string BuildBookPath(' 'void LibraryController::UnloadNonVisibleBrowserCoverCaches(' 'void LibraryController::LoadVisibleBrowserCoverCaches('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/library/app_browser_covers.cpp" "$signature" >> "$TASK_TMP/browser_navigation_under_test.inc"
done
for signature in 'void LibraryController::browser_nextpage(' 'void LibraryController::browser_prevpage('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/library/app_browser.cpp" "$signature" >> "$TASK_TMP/browser_navigation_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/library/browser_grid_view.cpp" 'int HitTestBookIndex(' > "$TASK_TMP/browser_grid_hit_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/library/browser_list_view.cpp" 'int HitTestBookIndex(' > "$TASK_TMP/browser_list_hit_under_test.inc"
build_test test_browser_nav -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_browser_nav.cpp" "$ROOT/source/ui/browser_nav.cpp" "$ROOT/source/library/browser_view_utils.cpp" "$ROOT/source/library/browser_presentation_hit_utils.cpp" "$ROOT/source/library/browser_folder_input_utils.cpp"
