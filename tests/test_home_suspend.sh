#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/app/app_lifecycle.cpp" "void App::HandleAppletHook(" >> "$TASK_TMP/home_suspend_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/book/book_worker_lifecycle.cpp" "void Book::SuspendFixedLayoutWorkers(" >> "$TASK_TMP/home_suspend_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/book/book_worker_lifecycle.cpp" "void Book::ResumeFixedLayoutWorkers(" >> "$TASK_TMP/home_suspend_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book.cpp" "void ReaderController::OnAppletSuspendRequested(" >> "$TASK_TMP/home_suspend_under_test.inc"
build_test test_home_suspend -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_home_suspend.cpp"
