#include "book/book.h"
#include "book/book_context.h"
#include "book/book_parser.h"
#include "shared/app_flow_utils.h"
#include "ui/text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

Book *MakeBook(const char *folder, const char *filename, format_t fmt) {
  TestCtx *tc = new TestCtx();
  Book *book = new Book(tc->ctx);
  book->SetFolderName(folder);
  book->SetFileName(filename);
  book->format = fmt;
  return book;
}

void TestTxtOpen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.txt";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_book_parser_integration TXT: fixture not found: %s\n",
            fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeBook(TEST_FIXTURES_DIR "/books", "basic.txt", FORMAT_UNDEF);
  uint8_t err = book_parser::Open(book);
  ExpectFalse("txt open: no error", err != 0);
  ExpectGt("txt open: pages > 0", (int)book->GetPageCount(), 0);
  book->Close();
  g_pass++;  // close survived
  delete book;
}

void TestTxtReopen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.txt";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_book_parser_integration TXT reopen: fixture not found\n");
    return;
  }
  fclose(fp);

  Book *book = MakeBook(TEST_FIXTURES_DIR "/books", "basic.txt", FORMAT_UNDEF);
  uint8_t err1 = book_parser::Open(book);
  u16 pages1 = book->GetPageCount();
  book->Close();

  uint8_t err2 = book_parser::Open(book);
  u16 pages2 = book->GetPageCount();
  book->Close();
  delete book;

  ExpectFalse("txt reopen: first open no error", err1 != 0);
  ExpectFalse("txt reopen: second open no error", err2 != 0);
  ExpectGt("txt reopen: pages consistent", (int)pages1, 0);
  ExpectTrue("txt reopen: same page count", pages1 == pages2);
}

void TestFb2Open() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.fb2";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_book_parser_integration FB2: fixture not found: %s\n",
            fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeBook(TEST_FIXTURES_DIR "/books", "basic.fb2", FORMAT_UNDEF);
  uint8_t err = book_parser::Open(book);
  ExpectFalse("fb2 open: no error", err != 0);
  ExpectGt("fb2 open: pages > 0", (int)book->GetPageCount(), 0);
  book->Close();
  g_pass++;  // close survived
  delete book;
}

void TestRtfOpen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.rtf";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_book_parser_integration RTF: fixture not found: %s\n",
            fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeBook(TEST_FIXTURES_DIR "/books", "basic.rtf", FORMAT_UNDEF);
  uint8_t err = book_parser::Open(book);
  ExpectFalse("rtf open: no error", err != 0);
  ExpectGt("rtf open: pages > 0", (int)book->GetPageCount(), 0);
  book->Close();
  g_pass++;
  delete book;
}

void TestMarkdownOpen() {
  const char *fixture = TEST_FIXTURES_DIR "/books/basic.md";
  FILE *fp = fopen(fixture, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_book_parser_integration Markdown: fixture not found: %s\n",
            fixture);
    return;
  }
  fclose(fp);

  Book *book = MakeBook(TEST_FIXTURES_DIR "/books", "basic.md", FORMAT_UNDEF);
  uint8_t err = book_parser::Open(book);
  ExpectFalse("markdown open: no error", err != 0);
  ExpectGt("markdown open: pages > 0", (int)book->GetPageCount(), 0);
  book->Close();
  g_pass++;
  delete book;
}

void TestCorruptFile() {
  Book *book = MakeBook("/tmp", "nonexistent_3dslibris_fixture.txt", FORMAT_UNDEF);
  uint8_t err = book_parser::Open(book);
  ExpectTrue("corrupt/missing: returns error", err != 0);
  book->Close();
  delete book;
}

} // namespace

int main() {
  TestTxtOpen();
  TestTxtReopen();
  TestFb2Open();
  TestRtfOpen();
  TestMarkdownOpen();
  TestCorruptFile();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
