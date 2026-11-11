#!/bin/bash
# Debug build: a failed cache save after the Stream tier released pages must not
# complete, while Safe still completes unsaved. The debug flag seams are read
# relative to the working directory, so run from a scratch directory.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
python3 "$ROOT/tests/fixtures/generate_mobi_stream.py" "$WORK/fixtures"
export TEST_MOBI_STREAM_DIR="$WORK/fixtures"
cd "$WORK"
exec bash "$ROOT/tests/test_book_parser_debug.sh" stream-persistence
