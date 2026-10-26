#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/settings/prefs.cpp" 'bool Prefs::FlushPendingWrite(' > "$TASK_TMP/shutdown_owner.inc"
for signature in 'void ReaderController::TryPersistProgress(' 'void ReaderController::ResetProgressAutosave(' 'void ReaderController::CloseBook('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book.cpp" "$signature" >> "$TASK_TMP/shutdown_owner.inc"
done
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/app/app_lifecycle.cpp" 'void App::PrepareForShutdown(' >> "$TASK_TMP/shutdown_owner.inc"
build_test test_shutdown_persistence -- -I"$TASK_TMP" "$ROOT/tests/test_shutdown_persistence.cpp"
