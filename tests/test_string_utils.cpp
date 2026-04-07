#include "../include/shared/string_utils.h"
#include "test_assert.h"

#include <string>

static void TestSanitizeBasic() {
  std::string r = SanitizeFat32Name("Hello World");
  test::ExpectStrEq("basic", r.c_str(), "Hello_World");
}

static void TestSanitizeSpecialChars() {
  std::string r = SanitizeFat32Name("a\\b/c:d*e?f\"g<h>i|j");
  test::ExpectStrEq("special chars", r.c_str(), "a_b_c_d_e_f_g_h_i_j");
}

static void TestSanitizeNonAscii() {
  // UTF-8 bytes for Japanese characters are >= 0x80, replaced with _
  std::string r = SanitizeFat32Name("book\xc3\xa9name");
  test::ExpectStrEq("non-ascii", r.c_str(), "book_name");
}

static void TestSanitizeCollapseUnderscores() {
  std::string r = SanitizeFat32Name("a___b   c _ d");
  test::ExpectStrEq("collapse underscores", r.c_str(), "a_b_c_d");
}

static void TestSanitizeTrimEdges() {
  std::string r = SanitizeFat32Name("...___hello___...");
  test::ExpectStrEq("trim edges", r.c_str(), "hello");
}

static void TestSanitizeEmpty() {
  std::string r = SanitizeFat32Name("");
  test::ExpectStrEq("empty fallback", r.c_str(), "book");
}

static void TestSanitizeAllSpecial() {
  std::string r = SanitizeFat32Name(":::***???");
  test::ExpectStrEq("all-special fallback", r.c_str(), "book");
}

static void TestSanitizeTruncate() {
  std::string long_name(200, 'x');
  std::string r = SanitizeFat32Name(long_name, 80);
  test::ExpectEq("truncated length", (int)r.size(), 80);
}

static void TestSanitizeTruncateNoTrailingUnderscore() {
  // 79 'a' then underscore then more chars — truncation at 80 should trim
  // the trailing underscore.
  std::string input(79, 'a');
  input += "___extra";
  std::string r = SanitizeFat32Name(input, 80);
  test::ExpectTrue("no trailing _", r.back() != '_');
}

static void TestSanitizeLeadingDots() {
  std::string r = SanitizeFat32Name("..hidden");
  test::ExpectStrEq("leading dots", r.c_str(), "hidden");
}

static void TestSanitizeControlChars() {
  std::string input = "a\x01" "\x1f" "b";
  std::string r = SanitizeFat32Name(input);
  test::ExpectStrEq("control chars", r.c_str(), "a_b");
}

int main() {
  TestSanitizeBasic();
  TestSanitizeSpecialChars();
  TestSanitizeNonAscii();
  TestSanitizeCollapseUnderscores();
  TestSanitizeTrimEdges();
  TestSanitizeEmpty();
  TestSanitizeAllSpecial();
  TestSanitizeTruncate();
  TestSanitizeTruncateNoTrailingUnderscore();
  TestSanitizeLeadingDots();
  TestSanitizeControlChars();
  printf("All SanitizeFat32Name tests passed.\n");
  return 0;
}
