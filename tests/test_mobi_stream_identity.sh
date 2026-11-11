#!/bin/bash
# Safe vs Stream: every MOBI fixture must paginate identically, with the wrap
# fix off and on. Perf fixtures are included when .private/perf-reflow has them.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
STREAM_TMP="$(mktemp -d)"
trap 'rm -rf "$STREAM_TMP"' EXIT
python3 "$ROOT/tests/fixtures/generate_mobi_stream.py" "$STREAM_TMP/fixtures"
export TEST_MOBI_STREAM_DIR="$STREAM_TMP/fixtures"
if [ -n "${TEST_MOBI_PERF_DIR:-}" ] && [ -d "$TEST_MOBI_PERF_DIR" ]; then
  export TEST_MOBI_EXTRA_DIR="$TEST_MOBI_PERF_DIR"
elif [ -d "$ROOT/.private/perf-reflow/fixtures" ]; then
  mkdir -p "$STREAM_TMP/perf"
  for f in small big; do
    [ -f "$ROOT/.private/perf-reflow/fixtures/$f.mobi" ] && cp "$ROOT/.private/perf-reflow/fixtures/$f.mobi" "$STREAM_TMP/perf/"
  done
  export TEST_MOBI_EXTRA_DIR="$STREAM_TMP/perf"
fi
exec bash "$ROOT/tests/test_book_parser_integration.sh" mobi-stream
