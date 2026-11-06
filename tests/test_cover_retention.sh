#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
mkdir -p "$TASK_TMP/library" "$TASK_TMP/failure" "$TASK_TMP/absent"
python3 - "$TEST_ROOT" "$TASK_TMP" <<'PY'
from pathlib import Path
import sys
root,tmp=map(Path,sys.argv[1:])
(tmp/'3ds.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n')
s=(root/'source/library/cover_cache.cpp').read_text()
s='\n'.join(l for l in s.splitlines() if not l.startswith('#include'))
(tmp/'cache.cpp').write_text('#include "test_folder_cover_support.h"\n'+s)
PY
export FOLDER_COVER_TEST_ROOT="$TASK_TMP/library" RETENTION_TEST=write
build_test test_cover_retention -- -I"$TASK_TMP" -I"$TEST_ROOT/tests" -I"$TEST_ROOT/third_party/stb" \
 "$TEST_ROOT/tests/test_cover_retention.cpp" "$TASK_TMP/cache.cpp" \
 "$TEST_ROOT/source/formats/common/file_read_utils.cpp" "$TEST_ROOT/source/library/cover_override_utils.cpp"
RETENTION_TEST=restart "$TEST_OUTDIR/test_cover_retention"
FOLDER_COVER_TEST_ROOT="$TASK_TMP/failure" RETENTION_TEST=failure "$TEST_OUTDIR/test_cover_retention"
FOLDER_COVER_TEST_ROOT="$TASK_TMP/absent" RETENTION_TEST=absent "$TEST_OUTDIR/test_cover_retention"
