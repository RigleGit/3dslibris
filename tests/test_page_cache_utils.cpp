#include "formats/common/page_cache_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected '" + expected + "', got '" + actual +
         "'");
  }
}

void ExpectEq(const char *label, unsigned actual, unsigned expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectSize(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected size " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

FILE *OpenTempFile(char *path_template) {
  int fd = mkstemp(path_template);
  if (fd < 0)
    Fail("mkstemp failed");
  FILE *fp = fdopen(fd, "w+b");
  if (!fp) {
    close(fd);
    Fail("fdopen failed");
  }
  return fp;
}

page_cache_utils::PageCacheLayoutParams SampleParams() {
  page_cache_utils::PageCacheLayoutParams params;
  params.file_size = 1111;
  params.file_mtime = 2222;
  params.pixel_size = 15;
  params.line_spacing = 4;
  params.paragraph_spacing = 2;
  params.paragraph_indent = 1;
  params.orientation = 0;
  params.margin_left = 8;
  params.margin_right = 9;
  params.margin_top = 10;
  params.margin_bottom = 11;
  params.regular_font = "fonts/regular.bcfnt";
  return params;
}

void TestBuildPathStable() {
  page_cache_utils::PageCacheLayoutParams params = SampleParams();
  const std::string path1 = page_cache_utils::BuildPageCachePath(
      "sdmc:/3ds/3dslibris/cache/epub", ".epc", "/books/sample.epub", params);
  const std::string path2 = page_cache_utils::BuildPageCachePath(
      "sdmc:/3ds/3dslibris/cache/epub", ".epc", "/books/sample.epub", params);
  ExpectFalse("path should not be empty", path1.empty());
  ExpectEq("path should be stable", path1, path2);

  params.margin_left++;
  const std::string path3 = page_cache_utils::BuildPageCachePath(
      "sdmc:/3ds/3dslibris/cache/epub", ".epc", "/books/sample.epub", params);
  ExpectFalse("path should change when params change", path1 == path3);
}

void TestBuildPathVariantToken() {
  page_cache_utils::PageCacheLayoutParams params = SampleParams();
  params.variant_token = "0";
  const std::string path1 = page_cache_utils::BuildPageCachePath(
      "sdmc:/3ds/3dslibris/cache/mobi", ".mpc", "/books/sample.mobi", params);
  params.variant_token = "1";
  const std::string path2 = page_cache_utils::BuildPageCachePath(
      "sdmc:/3ds/3dslibris/cache/mobi", ".mpc", "/books/sample.mobi", params);
  ExpectFalse("variant token should affect path", path1 == path2);
}

void TestBoundedStringRoundTrip() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-string-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  ExpectTrue("write bounded string",
             page_cache_utils::WriteLengthPrefixedString16(fp, "chapter title",
                                                           2048, true));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::string value;
  ExpectTrue("read bounded string",
             page_cache_utils::ReadLengthPrefixedString16(fp, 2048, true,
                                                          &value));
  ExpectEq("roundtrip bounded string", value, "chapter title");

  fclose(fp);
  std::remove(tmp_name);
}

void TestPagesRoundTrip() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-pages-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  std::vector<page_cache_utils::CachedPage> pages;
  pages.push_back(page_cache_utils::CachedPage());
  pages.push_back(page_cache_utils::CachedPage(3, 0));
  pages[1][0] = 1;
  pages[1][1] = 2;
  pages[1][2] = 3;
  pages.push_back(page_cache_utils::CachedPage(2, 0));
  pages[2][0] = 9;
  pages[2][1] = 8;

  ExpectTrue("write pages", page_cache_utils::WritePages(fp, pages, 4096));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::vector<page_cache_utils::CachedPage> loaded;
  ExpectTrue("read pages",
             page_cache_utils::ReadPages(fp, (uint32_t)pages.size(), 4096,
                                         &loaded));
  ExpectSize("page count", loaded.size(), pages.size());
  ExpectSize("page 0 size", loaded[0].size(), 0);
  ExpectSize("page 1 size", loaded[1].size(), 3);
  ExpectEq("page 1 byte 0", loaded[1][0], 1);
  ExpectEq("page 1 byte 1", loaded[1][1], 2);
  ExpectEq("page 1 byte 2", loaded[1][2], 3);
  ExpectSize("page 2 size", loaded[2].size(), 2);
  ExpectEq("page 2 byte 0", loaded[2][0], 9);
  ExpectEq("page 2 byte 1", loaded[2][1], 8);

  fclose(fp);
  std::remove(tmp_name);
}

void TestChaptersRoundTrip() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-chapters-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  std::vector<page_cache_utils::CachedChapter> chapters;
  chapters.push_back(page_cache_utils::CachedChapter(4, 0, "Intro"));
  chapters.push_back(page_cache_utils::CachedChapter(19, 2, "Appendix"));

  ExpectTrue("write chapters",
             page_cache_utils::WriteChapters(fp, chapters, 2048));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::vector<page_cache_utils::CachedChapter> loaded;
  ExpectTrue("read chapters",
             page_cache_utils::ReadChapters(fp, (uint32_t)chapters.size(), 2048,
                                            &loaded));
  ExpectSize("chapter count", loaded.size(), chapters.size());
  ExpectEq("chapter 0 page", loaded[0].page, 4);
  ExpectEq("chapter 0 level", loaded[0].level, 0);
  ExpectEq("chapter 0 title", loaded[0].title, "Intro");
  ExpectEq("chapter 1 page", loaded[1].page, 19);
  ExpectEq("chapter 1 level", loaded[1].level, 2);
  ExpectEq("chapter 1 title", loaded[1].title, "Appendix");

  fclose(fp);
  std::remove(tmp_name);
}

void TestRawStringRoundTrip() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-rawstr-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  std::string utf8 = "Caf\xc3\xa9 \xe2\x80\x94 \xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9";
  ExpectTrue("write raw UTF-8 string", page_cache_utils::WriteRawString(fp, utf8));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::string got;
  ExpectTrue("read raw UTF-8 string",
             page_cache_utils::ReadRawString(fp, utf8.size(), &got));
  ExpectEq("raw string roundtrip matches", got, utf8);

  fclose(fp);
  std::remove(tmp_name);
}

void TestClampString() {
  ExpectEq("clamp: below limit unchanged",
           page_cache_utils::ClampString("hello", 10), std::string("hello"));
  ExpectEq("clamp: exactly at limit",
           page_cache_utils::ClampString("abcde", 5), std::string("abcde"));
  ExpectEq("clamp: over limit truncated",
           page_cache_utils::ClampString("abcdefgh", 4), std::string("abcd"));
  ExpectEq("clamp: empty string",
           page_cache_utils::ClampString("", 10), std::string(""));
}

void TestTruncatedPagesRead() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-trunc-pages-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  std::vector<page_cache_utils::CachedPage> pages;
  pages.push_back(page_cache_utils::CachedPage(2, 0));
  pages[0][0] = 10; pages[0][1] = 20;
  ExpectTrue("write 1 page", page_cache_utils::WritePages(fp, pages, 4096));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::vector<page_cache_utils::CachedPage> loaded;
  ExpectFalse("read more pages than written fails",
              page_cache_utils::ReadPages(fp, 5, 4096, &loaded));

  fclose(fp);
  std::remove(tmp_name);
}

void TestTruncatedChaptersRead() {
  char tmp_name[] = "/tmp/3dslibris-page-cache-trunc-chap-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  // Write only 2 bytes (incomplete chapter header: page field is u16 but level+title missing)
  uint16_t partial = 7;
  ExpectTrue("write partial data",
             fwrite(&partial, 1, sizeof(partial), fp) == sizeof(partial));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::vector<page_cache_utils::CachedChapter> loaded;
  ExpectFalse("read truncated chapter fails",
              page_cache_utils::ReadChapters(fp, 1, 2048, &loaded));

  fclose(fp);
  std::remove(tmp_name);
}

void TestEmptyPagesRoundTrip() {
  std::vector<page_cache_utils::CachedPage> empty;
  // WritePages with empty vector writes nothing — ReadPages(count=0) should succeed
  char tmp_name[] = "/tmp/3dslibris-page-cache-empty-XXXXXX";
  FILE *fp = OpenTempFile(tmp_name);

  ExpectTrue("write 0 pages", page_cache_utils::WritePages(fp, empty, 4096));
  ExpectTrue("seek to start", fseek(fp, 0, SEEK_SET) == 0);

  std::vector<page_cache_utils::CachedPage> loaded;
  ExpectTrue("read 0 pages ok",
             page_cache_utils::ReadPages(fp, 0, 4096, &loaded));
  ExpectSize("0 pages loaded", loaded.size(), 0);

  fclose(fp);
  std::remove(tmp_name);
}

void TestBuildPathDifferentBookPaths() {
  page_cache_utils::PageCacheLayoutParams params = SampleParams();
  const std::string p1 = page_cache_utils::BuildPageCachePath(
      "/cache/epub", ".epc", "/books/a.epub", params);
  const std::string p2 = page_cache_utils::BuildPageCachePath(
      "/cache/epub", ".epc", "/books/b.epub", params);
  ExpectFalse("different book paths produce different cache paths", p1 == p2);
}

} // namespace

int main() {
  TestBuildPathStable();
  TestBuildPathVariantToken();
  TestBoundedStringRoundTrip();
  TestPagesRoundTrip();
  TestChaptersRoundTrip();
  TestRawStringRoundTrip();
  TestClampString();
  TestTruncatedPagesRead();
  TestTruncatedChaptersRead();
  TestEmptyPagesRoundTrip();
  TestBuildPathDifferentBookPaths();
  return 0;
}
