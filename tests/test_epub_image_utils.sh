#!/bin/sh
set -eu
source "$(dirname "$0")/test_build.sh"

build_test test_epub_image_utils \
  "$TEST_ROOT/tests/test_epub_image_utils.cpp" \
  "$TEST_ROOT/source/formats/common/epub_image_utils.cpp" \
  --stubs minizip_unzip_stubs.cpp \
  -I"$TEST_ROOT/third_party/mupdf/thirdparty/zlib/contrib"
