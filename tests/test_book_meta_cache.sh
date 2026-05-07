set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_book_meta_cache \
  "$TEST_ROOT/tests/test_book_meta_cache.cpp" \
  "$TEST_ROOT/source/formats/common/book_meta_cache.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp"
