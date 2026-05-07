set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_epub_toc_diag_utils \
  "$TEST_ROOT/tests/test_epub_toc_diag_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_toc_diag_utils.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp" \
  -I"$TEST_ROOT/tests/stubs"
