#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
# Keep production headers ahead of stubs: Text's ownership contract must be
# checked on the real type. FreeType declarations are already vendored.
build_test test_resource_owner_copy "$TEST_ROOT/tests/test_resource_owner_copy.cpp" \
  -I"$TEST_ROOT/tests/stubs/resource_owner" \
  -I"$TEST_ROOT/third_party/mupdf/thirdparty/freetype/include" \
  -- -fno-exceptions -fno-rtti
