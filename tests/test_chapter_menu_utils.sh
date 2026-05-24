set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_chapter_menu_utils \
  "$TEST_ROOT/tests/test_chapter_menu_utils.cpp" \
  "$TEST_ROOT/source/menus/chapter_menu_utils.cpp"
