#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book.cpp" 'bool ReaderController::AdvanceCachePreparation(' > "$TASK_TMP/preparation_error_owner.inc"
build_test test_reader_preparation_error -- -I"$TASK_TMP" "$ROOT/tests/test_reader_preparation_error.cpp"
