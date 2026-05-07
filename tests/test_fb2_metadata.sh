set -eu
source "$(dirname "$0")/test_build.sh"
FIXTURES_DIR="$TEST_ROOT/tests/fixtures"
build_test test_fb2_metadata \
  "$TEST_ROOT/tests/test_fb2_metadata.cpp" \
  "$TEST_ROOT/source/formats/common/xml_parse_utils.cpp" \
  --stubs minizip_unzip_stubs.cpp \
  --expat \
  "-DTEST_FIXTURES_DIR=\"$FIXTURES_DIR\""
