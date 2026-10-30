#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
if [ -z "${MUPDF_HOST_LIB_DIR:-}" ]; then
 echo 'SKIP: native MuPDF libraries required (MUPDF_HOST_LIB_DIR)'; exit 77
fi
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
cd "$TASK_TMP"
if [ "$(uname -s)" = Darwin ]; then STRIP=-Wl,-dead_strip; else STRIP=-Wl,--gc-sections; fi
export LDFLAGS="${LDFLAGS:-} $STRIP"
build_test test_library_metadata_mupdf -DLIBRARY_METADATA_NATIVE_MUPDF \
 "$TEST_ROOT/tests/test_library_metadata_mupdf.cpp" \
 "$TEST_ROOT/source/formats/common/library_metadata_mupdf.cpp" \
 "$TEST_ROOT/source/shared/utf8_utils.cpp" \
 "$MUPDF_HOST_LIB_DIR/libmupdf.a" "$MUPDF_HOST_LIB_DIR/libmupdf-third.a" \
 -I"$TEST_ROOT/third_party/mupdf/include" -lm
