#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 - "$TEST_ROOT/source/reader/app_book_internal.cpp" > "$TASK_TMP/relayout_state_owner.inc" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
start = source.index('namespace reader_internal {') + len('namespace reader_internal {')
end = source.index('const char *SafeBookName(', start)
print(source[start:end])
PY
for signature in 'std::list<int> CopyBookmarksAsInts(' 'OpenBookRelayoutState CaptureRelayoutState('; do
  python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/reader/app_book_internal.cpp" "$signature" >> "$TASK_TMP/relayout_state_owner.inc"
done
build_test test_reader_relayout_state -- -I"$TASK_TMP" -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" "$TEST_ROOT/tests/test_reader_relayout_state.cpp"
