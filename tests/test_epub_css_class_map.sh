set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_epub_css_class_map \
  "$TEST_ROOT/tests/test_epub_css_class_map.cpp" \
  "$TEST_ROOT/source/book/epub_css_tokenizer.cpp" \
  "$TEST_ROOT/source/book/epub_css_class_map.cpp" \
  "$TEST_ROOT/source/book/book_xml_css_style_utils.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp"
