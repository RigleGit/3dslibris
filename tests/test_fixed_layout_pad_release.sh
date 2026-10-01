#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 - "$ROOT" "$TASK_TMP" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1])/'source/reader/fixed_layout_reader_input.cpp'
s=source.read_text()
p=Path(sys.argv[2])
start=s.index('namespace {')
line=s.count('\n', 0, start)+1
p.joinpath('pad_helpers.inc').write_text(f'#line {line} "{source}"\n' + s[start:s.index('namespace fixed_layout_input {')])

PY
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/fixed_layout_reader_input.cpp" "bool HandleInBook(" > "$TASK_TMP/pad_handler.inc"
build_test test_fixed_layout_pad_release -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_fixed_layout_pad_release.cpp" "$ROOT/source/reader/fixed_layout_input_utils.cpp" "$ROOT/source/formats/common/pdf_view_utils.cpp"
