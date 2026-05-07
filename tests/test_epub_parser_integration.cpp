#include "book/book.h"
#include "book/book_context.h"
#include "formats/epub/epub_parser.h"
#include "formats/epub/epub_page_cache.h"
#include "shared/app_flow_utils.h"
#include "ui/text.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectTrue(const char *label, bool v) {
  if (!v) Fail(label, "expected true");
  g_pass++;
}

void ExpectFalse(const char *label, bool v) {
  if (v) Fail(label, "expected false");
  g_pass++;
}

void ExpectGt(const char *label, int actual, int threshold) {
  if (actual <= threshold) {
    char buf[128];
    snprintf(buf, sizeof(buf), "expected > %d, got %d", threshold, actual);
    Fail(label, buf);
  }
  g_pass++;
}

struct TestCtx {
  Text text;
  BookContext ctx;

  TestCtx() {
    ctx.text = &text;
    ctx.prefs = nullptr;
    ctx.status_reporter = nullptr;
    ctx.paragraph_spacing = nullptr;
    ctx.paragraph_indent = nullptr;
    ctx.orientation = nullptr;
    ctx.draw_background = nullptr;
    ctx.draw_background_user_data = nullptr;
    ctx.draw_top_background = nullptr;
    ctx.draw_top_background_user_data = nullptr;
    ctx.on_spine_progress = nullptr;
    ctx.on_spine_progress_user_data = nullptr;
  }
};

Book *MakeEpubBook(const char *folder, const char *filename) {
  TestCtx *tc = new TestCtx();
  Book *book = new Book(tc->ctx);
  book->SetFolderName(folder);
  book->SetFileName(filename);
  book->format = FORMAT_EPUB;
  return book;
}

// Open helper: PrepareForOpen + epub_parser::Open directly.
// Does NOT go through book_parser.cpp, avoiding the full format dispatch table.
uint8_t EpubOpen(Book *book) {
  book->PrepareForOpen();
  std::string path = std::string(book->GetFolderName()) + "/" +
                     std::string(book->GetFileName());
  return epub_parser::Open(book, path);
}

void TestEpubOpen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.epub";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_epub_parser_integration EPUB: fixture not found: %s\n",
            fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeEpubBook(TEST_FIXTURES_DIR "/books", "basic.epub");
  uint8_t err = EpubOpen(book);
  ExpectFalse("epub open: no error", err != 0);
  ExpectGt("epub open: pages > 0", (int)book->GetPageCount(), 0);

  // TOC: fixture has 2 chapters so at least 1 chapter entry expected.
  ExpectGt("epub open: chapters >= 1", (int)book->GetChapters().size(), 0);

  book->Close();
  g_pass++;  // close survived
  delete book;
}

void TestEpubReopen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.epub";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_epub_parser_integration EPUB reopen: fixture not found\n");
    return;
  }
  fclose(fp);

  Book *book = MakeEpubBook(TEST_FIXTURES_DIR "/books", "basic.epub");
  uint8_t err1 = EpubOpen(book);
  u16 pages1 = book->GetPageCount();
  book->Close();

  uint8_t err2 = EpubOpen(book);
  u16 pages2 = book->GetPageCount();
  book->Close();
  delete book;

  ExpectFalse("epub reopen: first open no error", err1 != 0);
  ExpectFalse("epub reopen: second open no error", err2 != 0);
  ExpectGt("epub reopen: pages > 0", (int)pages1, 0);
  ExpectTrue("epub reopen: same page count", pages1 == pages2);
}

void TestEpubInvalidFile() {
  Book *book = MakeEpubBook("/tmp", "nonexistent_3dslibris_epub.epub");
  uint8_t err = EpubOpen(book);
  ExpectTrue("epub invalid: returns error", err != 0);
  book->Close();
  delete book;
}

// Index helper: PrepareForOpen + epub_parser::Index (metadata-only path).
uint8_t EpubIndex(Book *book) {
  book->PrepareForOpen();
  std::string path = std::string(book->GetFolderName()) + "/" +
                     std::string(book->GetFileName());
  return epub_parser::Index(book, path);
}

void TestEpubIndexMetadata() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.epub";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP TestEpubIndexMetadata: fixture not found: %s\n", fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeEpubBook(TEST_FIXTURES_DIR "/books", "basic.epub");
  uint8_t err = EpubIndex(book);
  ExpectFalse("epub index: no error", err != 0);

  const char *title = book->GetTitle();
  ExpectTrue("epub index: title set", title != nullptr && title[0] != '\0');
  ExpectTrue("epub index: title matches",
             title != nullptr && std::string(title) == "Basic EPUB Fixture");

  const std::string &author = book->GetAuthor();
  ExpectTrue("epub index: author set", !author.empty());
  ExpectTrue("epub index: author matches", author == "3dslibris Test");

  book->Close();
  g_pass++;  // close survived
  delete book;
}

void TestEpubIndexMissingFile() {
  Book *book = MakeEpubBook("/tmp", "nonexistent_3dslibris_epub.epub");
  uint8_t err = EpubIndex(book);
  ExpectTrue("epub index missing: returns error", err != 0);
  book->Close();
  delete book;
}

void TestEpubIndexThenOpen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.epub";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP TestEpubIndexThenOpen: fixture not found: %s\n", fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeEpubBook(TEST_FIXTURES_DIR "/books", "basic.epub");

  uint8_t err_idx = EpubIndex(book);
  ExpectFalse("epub index-then-open: index no error", err_idx != 0);

  const char *title_after_index = book->GetTitle();
  ExpectTrue("epub index-then-open: title set after index",
             title_after_index != nullptr && title_after_index[0] != '\0');

  book->Close();

  uint8_t err_open = EpubOpen(book);
  ExpectFalse("epub index-then-open: open no error", err_open != 0);
  ExpectGt("epub index-then-open: pages > 0", (int)book->GetPageCount(), 0);

  book->Close();
  g_pass++;  // close survived
  delete book;
}

// ---------------------------------------------------------------------------
// epub_page_cache guard tests
// ---------------------------------------------------------------------------

// Common layout params used by all cache tests — matches the Text stub metrics.
static void CallTryLoad(Book *book, const char *path, bool *result) {
  *result = epub_page_cache::TryLoad(book, path,
                                     14, 2, 0, 0, 0,
                                     12, 12, 10, 36,
                                     nullptr, false);
}

void TestEpubPageCacheTryLoadNullBook() {
  bool result = true;
  CallTryLoad(nullptr, "/tmp/3dslibris_cache_test.epub", &result);
  ExpectFalse("page cache TryLoad null book returns false", result);
}

void TestEpubPageCacheTryLoadNullPath() {
  Book *book = MakeEpubBook("/tmp", "x.epub");
  bool result = true;
  CallTryLoad(book, nullptr, &result);
  ExpectFalse("page cache TryLoad null path returns false", result);
  book->Close();
  delete book;
}

void TestEpubPageCacheTryLoadMissingFile() {
  // Cache file lives at sdmc:/...; it won't exist on host → returns false safely.
  Book *book = MakeEpubBook("/tmp", "nonexistent_3dslibris_cache_test.epub");
  bool result = true;
  CallTryLoad(book, "/tmp/nonexistent_3dslibris_cache_test.epub", &result);
  ExpectFalse("page cache TryLoad missing file returns false", result);
  book->Close();
  delete book;
}

void TestEpubPageCacheSaveNullBook() {
  // Save with null book must not crash (bails immediately).
  epub_page_cache::Save(nullptr, "/tmp/x.epub",
                        14, 2, 0, 0, 0, 12, 12, 10, 36,
                        nullptr, false, false);
  g_pass++;  // no crash
}

void TestEpubPageCacheSaveZeroPages() {
  // Save with a real book that has 0 pages must not crash (bails early).
  Book *book = MakeEpubBook("/tmp", "x.epub");
  epub_page_cache::Save(book, "/tmp/x.epub",
                        14, 2, 0, 0, 0, 12, 12, 10, 36,
                        nullptr, false, false);
  g_pass++;  // no crash
  book->Close();
  delete book;
}

void TestEpubStreamWriterNullBook() {
  epub_page_cache::StreamWriter sw;
  bool ok = sw.Begin(nullptr, "/tmp/x.epub",
                     14, 2, 0, 0, 0, 12, 12, 10, 36, nullptr, false);
  ExpectFalse("StreamWriter::Begin null book returns false", ok);
  ExpectFalse("StreamWriter not open after failed Begin", sw.IsOpen());
}

void TestEpubStreamWriterNullPath() {
  Book *book = MakeEpubBook("/tmp", "x.epub");
  epub_page_cache::StreamWriter sw;
  bool ok = sw.Begin(book, nullptr,
                     14, 2, 0, 0, 0, 12, 12, 10, 36, nullptr, false);
  ExpectFalse("StreamWriter::Begin null path returns false", ok);
  ExpectFalse("StreamWriter not open after null path", sw.IsOpen());
  book->Close();
  delete book;
}

void TestEpubStreamWriterFinalizeWithoutBegin() {
  Book *book = MakeEpubBook("/tmp", "x.epub");
  epub_page_cache::StreamWriter sw;
  bool ok = sw.Finalize(book);
  ExpectFalse("StreamWriter::Finalize without Begin returns false", ok);
  book->Close();
  delete book;
}

void TestEpubStreamWriterAbortIdempotent() {
  epub_page_cache::StreamWriter sw;
  sw.Abort();
  sw.Abort();  // second abort must not crash
  g_pass++;    // no crash
}

} // namespace

int main() {
  TestEpubOpen();
  TestEpubReopen();
  TestEpubInvalidFile();
  TestEpubIndexMetadata();
  TestEpubIndexMissingFile();
  TestEpubIndexThenOpen();
  TestEpubPageCacheTryLoadNullBook();
  TestEpubPageCacheTryLoadNullPath();
  TestEpubPageCacheTryLoadMissingFile();
  TestEpubPageCacheSaveNullBook();
  TestEpubPageCacheSaveZeroPages();
  TestEpubStreamWriterNullBook();
  TestEpubStreamWriterNullPath();
  TestEpubStreamWriterFinalizeWithoutBegin();
  TestEpubStreamWriterAbortIdempotent();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
