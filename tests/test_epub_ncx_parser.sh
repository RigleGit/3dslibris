set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_epub_ncx_parser \
  "$TEST_ROOT/tests/test_epub_ncx_parser.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_ncx_parser.cpp" \
  "$TEST_ROOT/source/formats/common/xml_parse_utils.cpp" \
  --stubs minizip_unzip_stubs.cpp \
  --expat
