set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_txt_loader \
  "$TEST_ROOT/tests/test_txt_loader.cpp" \
  "$TEST_ROOT/source/formats/txt/txt_loader.cpp" \
  "$TEST_ROOT/source/formats/common/text_helpers.cpp" \
  "$TEST_ROOT/source/formats/common/file_read_utils.cpp" \
  "$TEST_ROOT/source/shared/utf8_utils.cpp"
