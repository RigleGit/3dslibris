#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
mkdir -p "$TASK_TMP/3ds"
cat > "$TASK_TMP/3ds/archive.h" <<'HEADER'
#pragma once
typedef unsigned long long u64;
extern "C" int archive_getmtime(const char *, unsigned long long *);
HEADER
build_test test_console_file_identity -- -D__3DS__ -I"$TASK_TMP" -I"$TEST_ROOT/include" "$TEST_ROOT/tests/test_console_file_identity.cpp" "$TEST_ROOT/source/formats/common/page_cache_utils.cpp" "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" -lz
