#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
extract() {
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/$1" "$2" >> "$TASK_TMP/$3.inc"
}
for signature in 'static BrowserViewMode CurrentBrowserViewMode(' 'static int CurrentBrowserPageSize(' 'static bool ShouldCurrentBrowserLoadCovers(' 'static bool SupportsBrowserCoverWarmup(' 'static void ApplyEmbeddedCoverResult(' 'static size_t CountQueuedHeavyJobs(' 'static std::string BuildBookPath('; do
  extract source/library/app_browser_jobs.cpp "$signature" cover_jobs
done
for signature in 'bool LibraryController::HasQueuedJob(' 'void LibraryController::PrioritizeSelectedBookJobs(' 'void LibraryController::EnqueueJob(' 'void LibraryController::QueueBookWarmup(' 'void LibraryController::TickBrowserWarmup(' 'void LibraryController::ProcessJobs(' 'size_t LibraryController::PauseBrowserJobs('; do
  extract source/library/app_browser_jobs.cpp "$signature" cover_jobs
done
build_test test_browser_cover_results -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" \
  -I"$ROOT/include" \
  "$ROOT/tests/test_browser_cover_results.cpp" \
  "$ROOT/source/library/browser_view_utils.cpp" \
  "$ROOT/source/shared/app_flow_utils.cpp"
