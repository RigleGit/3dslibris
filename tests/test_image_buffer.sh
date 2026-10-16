#!/usr/bin/env bash
set -eu
source "$(dirname "$0")/test_build.sh"
if [ "$(uname -s)" = Linux ]; then
  build_test test_image_buffer "$TEST_ROOT/tests/test_image_buffer.cpp" -DIMAGE_TEST_WRAP_MALLOC -Wl,--wrap=malloc
else
  build_test test_image_buffer "$TEST_ROOT/tests/test_image_buffer.cpp"
fi
