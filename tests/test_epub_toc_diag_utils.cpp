#include "formats/epub/epub_toc_diag_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectStrEq(const char *label, const std::string &actual,
                 const char *expected) {
  if (actual != expected) {
    char buf[512];
    snprintf(buf, sizeof(buf), "expected '%s', got '%s'", expected,
             actual.c_str());
    Fail(label, buf);
  }
  g_pass++;
}

// ---- ClipForDiag ----

void TestClipShort() {
  ExpectStrEq("clip: short string unchanged",
              epub_toc_diag_utils::ClipForDiag("hello", 10), "hello");
}

void TestClipExact() {
  ExpectStrEq("clip: exact length unchanged",
              epub_toc_diag_utils::ClipForDiag("hello", 5), "hello");
}

void TestClipLong() {
  ExpectStrEq("clip: long string truncated",
              epub_toc_diag_utils::ClipForDiag("hello world", 5), "hello...");
}

void TestClipEmpty() {
  ExpectStrEq("clip: empty string unchanged",
              epub_toc_diag_utils::ClipForDiag("", 10), "");
}

// ---- NormalizeAsciiSearchText ----

void TestNormalizeSearchBasic() {
  ExpectStrEq("search: lowercases ASCII",
              epub_toc_diag_utils::NormalizeAsciiSearchText("Hello World"),
              "hello world");
}

void TestNormalizeSearchPunct() {
  ExpectStrEq("search: strips punctuation",
              epub_toc_diag_utils::NormalizeAsciiSearchText("Hello, World!"),
              "hello world");
}

void TestNormalizeSearchMultiSpace() {
  ExpectStrEq("search: collapses spaces",
              epub_toc_diag_utils::NormalizeAsciiSearchText("one   two"),
              "one two");
}

void TestNormalizeSearchLeadingPunct() {
  ExpectStrEq("search: trims leading punct",
              epub_toc_diag_utils::NormalizeAsciiSearchText("...hello"),
              "hello");
}

void TestNormalizeSearchEmpty() {
  ExpectStrEq("search: empty input",
              epub_toc_diag_utils::NormalizeAsciiSearchText(""), "");
}

void TestNormalizeSearchMaxOut() {
  // max_out limits output length
  std::string result =
      epub_toc_diag_utils::NormalizeAsciiSearchText("abcdefghij", 4);
  if (result.size() > 4) {
    char buf[128];
    snprintf(buf, sizeof(buf), "expected len<=4, got %u chars: '%s'",
             (unsigned)result.size(), result.c_str());
    Fail("search: max_out", buf);
  }
  g_pass++;
}

void TestNormalizeSearchDigits() {
  ExpectStrEq("search: preserves digits",
              epub_toc_diag_utils::NormalizeAsciiSearchText("Chapter 3"),
              "chapter 3");
}

// ---- NormalizeTocTitle ----

void TestTocTitleBasic() {
  ExpectStrEq("toc: basic title unchanged",
              epub_toc_diag_utils::NormalizeTocTitle("Chapter One"),
              "Chapter One");
}

void TestTocTitleCollapseWhitespace() {
  ExpectStrEq("toc: collapse internal whitespace",
              epub_toc_diag_utils::NormalizeTocTitle("Chapter  One"),
              "Chapter One");
}

void TestTocTitleTrimLeadingTrailing() {
  ExpectStrEq("toc: trim leading/trailing space",
              epub_toc_diag_utils::NormalizeTocTitle("  Chapter One  "),
              "Chapter One");
}

void TestTocTitleDottedLeader() {
  ExpectStrEq("toc: strip dotted leader",
              epub_toc_diag_utils::NormalizeTocTitle("Introduction....... 1"),
              "Introduction");
}

void TestTocTitleDottedLeaderNoNumber() {
  ExpectStrEq("toc: strip dotted leader without trailing number",
              epub_toc_diag_utils::NormalizeTocTitle("Preface ......"),
              "Preface");
}

void TestTocTitleChapterLikeNumberKept() {
  // "Chapter 3" — trailing number is the chapter label, not a page number
  std::string result = epub_toc_diag_utils::NormalizeTocTitle("Chapter 3");
  ExpectStrEq("toc: chapter-like prefix keeps trailing number", result,
              "Chapter 3");
}

void TestTocTitleTrailingPunct() {
  ExpectStrEq("toc: strip trailing punct",
              epub_toc_diag_utils::NormalizeTocTitle("Epilogue."),
              "Epilogue");
}

void TestTocTitleEmpty() {
  ExpectStrEq("toc: empty input",
              epub_toc_diag_utils::NormalizeTocTitle(""), "");
}

void TestTocTitleNewline() {
  ExpectStrEq("toc: collapse newline",
              epub_toc_diag_utils::NormalizeTocTitle("Part\nOne"),
              "Part One");
}

} // namespace

int main() {
  TestClipShort();
  TestClipExact();
  TestClipLong();
  TestClipEmpty();

  TestNormalizeSearchBasic();
  TestNormalizeSearchPunct();
  TestNormalizeSearchMultiSpace();
  TestNormalizeSearchLeadingPunct();
  TestNormalizeSearchEmpty();
  TestNormalizeSearchMaxOut();
  TestNormalizeSearchDigits();

  TestTocTitleBasic();
  TestTocTitleCollapseWhitespace();
  TestTocTitleTrimLeadingTrailing();
  TestTocTitleDottedLeader();
  TestTocTitleDottedLeaderNoNumber();
  TestTocTitleChapterLikeNumberKept();
  TestTocTitleTrailingPunct();
  TestTocTitleEmpty();
  TestTocTitleNewline();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
