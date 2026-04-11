#include "../include/shared/string_utils.h"
#include "test_assert.h"

static void TestEmptyTitle() {
  const char *r = BrowserDisplayNameSource("", "foo.epub");
  test::ExpectStrEq("empty title", r, "foo.epub");
}

static void TestNullTitle() {
  const char *r = BrowserDisplayNameSource(nullptr, "foo.epub");
  test::ExpectStrEq("null title", r, "foo.epub");
}

static void TestTitleEqualsStem() {
  const char *r = BrowserDisplayNameSource("foo", "foo.epub");
  test::ExpectStrEq("title equals stem", r, "foo.epub");
}

static void TestTitleDifferentFromStem() {
  const char *r = BrowserDisplayNameSource("My Book", "foo.epub");
  test::ExpectStrEq("title differs from stem", r, "My Book");
}

static void TestTitleWithSpecialChars() {
  const char *r = BrowserDisplayNameSource("Cien a\xc3\xb1os de soledad", "cien_anios.epub");
  test::ExpectStrEq("title with special chars", r, "Cien a\xc3\xb1os de soledad");
}

static void TestNullFilename() {
  const char *r = BrowserDisplayNameSource("My Book", nullptr);
  test::ExpectStrEq("null filename", r, "My Book");
}

int main() {
  TestEmptyTitle();
  TestNullTitle();
  TestTitleEqualsStem();
  TestTitleDifferentFromStem();
  TestTitleWithSpecialChars();
  TestNullFilename();
  printf("All BrowserDisplayNameSource tests passed.\n");
  return 0;
}
