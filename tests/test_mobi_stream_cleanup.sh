set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_mobi_stream_cleanup \
  "$TEST_ROOT/tests/test_mobi_stream_cleanup.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_text_cleanup.cpp"
