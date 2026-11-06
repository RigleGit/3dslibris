#include "../include/shared/string_utils.h"
#include "test_assert.h"

#include <string>
#include "allocation_counter.h"

static void TestSanitizeBasic() {
  std::string r = SanitizeFat32Name("Hello World");
  test::ExpectStrEq("basic", r.c_str(), "Hello_World");
}

static void TestSanitizeSpecialChars() {
  std::string r = SanitizeFat32Name("a\\b/c:d*e?f\"g<h>i|j");
  test::ExpectStrEq("special chars", r.c_str(), "a_b_c_d_e_f_g_h_i_j");
}

static void TestSanitizeNonAscii() {
  // UTF-8 e-acute (\xc3\xa9) is a valid 2-byte sequence — preserved.
  std::string r = SanitizeFat32Name("book\xc3\xa9name");
  test::ExpectStrEq("non-ascii utf8", r.c_str(), "book\xc3\xa9name");
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

static void TestSanitizeCjk() {
  // U+65E5 U+672C U+8A9E = "日本語"
  std::string r = SanitizeFat32Name("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
  test::ExpectStrEq("CJK preserved", r.c_str(),
                     "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
}

static void TestSanitizeArabic() {
  // U+0643 U+062A U+0627 U+0628 = "كتاب"
  std::string r = SanitizeFat32Name("\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8");
  test::ExpectStrEq("Arabic preserved", r.c_str(),
                     "\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8");
}

static void TestSanitizeHebrew() {
  // U+05E1 U+05E4 U+05E8 = "ספר"
  std::string r = SanitizeFat32Name("\xd7\xa1\xd7\xa4\xd7\xa8");
  test::ExpectStrEq("Hebrew preserved", r.c_str(),
                     "\xd7\xa1\xd7\xa4\xd7\xa8");
}

static void TestSanitizeMixedAsciiCjk() {
  std::string r = SanitizeFat32Name(
      "My_Book_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
  test::ExpectStrEq("mixed ascii+CJK", r.c_str(),
                     "My_Book_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
}

static void TestSanitizeTruncateUtf8Boundary() {
  // 3 CJK chars = 9 bytes. Truncating at max_len=8 must not split a char.
  std::string input = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
  std::string r = SanitizeFat32Name(input, 8);
  // Should yield 2 complete CJK chars = 6 bytes.
  test::ExpectEq("truncated CJK length", (int)r.size(), 6);
  test::ExpectStrEq("truncated CJK value", r.c_str(),
                     "\xe6\x97\xa5\xe6\x9c\xac");
}

static void TestSanitizeInvalidUtf8() {
  // 0xFE is never a valid UTF-8 lead byte.
  std::string r = SanitizeFat32Name("a\xfe" "b");
  test::ExpectStrEq("invalid utf8 byte", r.c_str(), "a_b");
}

static void TestSanitizeBrokenContinuation() {
  // Lead byte for 3-byte seq (0xE6) but only 1 continuation byte.
  std::string r = SanitizeFat32Name("a\xe6\x97" "b");
  test::ExpectStrEq("broken continuation", r.c_str(), "a_b");
}

static void TestContainsWithoutCopies() {
  const std::string values[] = {"", "AbCd", std::string("a\0Bc", 4),
      std::string(1, (char)0x80) + "Z", std::string(100, 'A')};
  for (const auto &h : values) for (const auto &n : values) {
    const bool expected = ToLowerAscii(h).find(ToLowerAscii(n)) != std::string::npos;
    test::ExpectEq("string search preserves byte and empty semantics", ContainsNoCase(h, n), expected);
    const std::string cneedle(n.c_str());
    const bool cexpected = !cneedle.empty() &&
        ToLowerAscii(h).find(ToLowerAscii(cneedle)) != std::string::npos;
    test::ExpectEq("C search preserves NUL and empty semantics", ContainsNoCase(h, n.c_str()), cexpected);
  }
  test::ExpectFalse("null needle", ContainsNoCase("abc", (const char *)nullptr));
  const std::string haystack(256, 'A'), needle(32, 'a');
  allocation_count = 0; count_allocations = true;
  const bool strings = ContainsNoCase(haystack, needle);
  const bool cstrings = ContainsNoCase(haystack, needle.c_str());
  count_allocations = false;
  test::ExpectTrue("both searches match", strings && cstrings);
  test::ExpectEq("case insensitive search never copies input", (int)allocation_count, 0);
}

int main() {
  TestContainsWithoutCopies();
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
  TestSanitizeCjk();
  TestSanitizeArabic();
  TestSanitizeHebrew();
  TestSanitizeMixedAsciiCjk();
  TestSanitizeTruncateUtf8Boundary();
  TestSanitizeInvalidUtf8();
  TestSanitizeBrokenContinuation();
  printf("All SanitizeFat32Name tests passed.\n");
  return 0;
}
