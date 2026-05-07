#include "book/book.h"
#include "book/book_context.h"
#include "formats/epub/epub_parser.h"
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

} // namespace

int main() {
  TestEpubOpen();
  TestEpubReopen();
  TestEpubInvalidFile();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
