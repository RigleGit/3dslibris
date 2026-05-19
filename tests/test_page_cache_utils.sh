set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_page_cache_utils \
  "$TEST_ROOT/tests/test_page_cache_utils.cpp" \
  "$TEST_ROOT/source/formats/common/page_cache_utils.cpp" \
  "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" \
  -lz
