#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/epub/epub_parser.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/common/page_cache_utils.h"
#include "shared/text_token_constants.h"
#include "shared/app_flow_utils.h"
#include "ui/text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

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

void TestRealEpubOpenFromEnv() {
  const char *real_epub = getenv("REAL_EPUB_PATH");
  if (!real_epub || real_epub[0] == '\0') {
    fprintf(stderr, "SKIP TestRealEpubOpenFromEnv: REAL_EPUB_PATH not set\n");
    return;
  }

  FILE *fp = fopen(real_epub, "r");
  if (!fp)
    Fail("real epub open: fixture exists", "REAL_EPUB_PATH cannot be opened");
  fclose(fp);

  std::string path(real_epub);
  size_t slash = path.find_last_of('/');
  std::string folder = slash == std::string::npos ? "." : path.substr(0, slash);
  std::string filename =
      slash == std::string::npos ? path : path.substr(slash + 1);

  Book *book = MakeEpubBook(folder.c_str(), filename.c_str());
  uint8_t err = EpubOpen(book);
  ExpectFalse("real epub open: no error", err != 0);
  ExpectGt("real epub open: pages > 0", (int)book->GetPageCount(), 0);
  ExpectTrue("real epub open: stops before unsafe 3DS page volume",
             book->GetPageCount() <= 5500);

  book->Close();
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
                                     nullptr);
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
                        nullptr, false);
  g_pass++;  // no crash
}

void TestEpubPageCacheSaveZeroPages() {
  // Save with a real book that has 0 pages must not crash (bails early).
  Book *book = MakeEpubBook("/tmp", "x.epub");
  epub_page_cache::Save(book, "/tmp/x.epub",
                        14, 2, 0, 0, 0, 12, 12, 10, 36,
                        nullptr, false);
  g_pass++;  // no crash
  book->Close();
  delete book;
}

void TestEpubStreamWriterNullBook() {
  epub_page_cache::StreamWriter sw;
  bool ok = sw.Begin(nullptr, "/tmp/x.epub",
                     14, 2, 0, 0, 0, 12, 12, 10, 36, nullptr);
  ExpectFalse("StreamWriter::Begin null book returns false", ok);
  ExpectFalse("StreamWriter not open after failed Begin", sw.IsOpen());
}

void TestEpubStreamWriterNullPath() {
  Book *book = MakeEpubBook("/tmp", "x.epub");
  epub_page_cache::StreamWriter sw;
  bool ok = sw.Begin(book, nullptr,
                     14, 2, 0, 0, 0, 12, 12, 10, 36, nullptr);
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

// ---------------------------------------------------------------------------
// Cache layout params matching the Text stub fixed metrics.
// ---------------------------------------------------------------------------

static const char *kCacheBookPath = "/tmp/3dslibris_cache_roundtrip_book.epub";
static const int   kCachePx       = 14;
static const int   kCacheLS       = 2;
static const int   kCachePS       = 0;
static const int   kCachePI       = 0;
static const int   kCacheOri      = 0;
static const int   kCacheMl       = 12;
static const int   kCacheMr       = 12;
static const int   kCacheMt       = 10;
static const int   kCacheMb       = 36;

// Compute the cache file path that Save/TryLoad will use for kCacheBookPath
// with the metrics above (file_size=0, file_mtime=0 since file doesn't exist).
static std::string CacheFilePath(const char *cache_dir) {
  page_cache_utils::PageCacheLayoutParams lp;
  lp.file_size = 0;
  lp.file_mtime = 0;
  lp.pixel_size = kCachePx;
  lp.line_spacing = kCacheLS;
  lp.paragraph_spacing = kCachePS;
  lp.paragraph_indent = kCachePI;
  lp.orientation = kCacheOri;
  lp.margin_left = kCacheMl;
  lp.margin_right = kCacheMr;
  lp.margin_top = kCacheMt;
  lp.margin_bottom = kCacheMb;
  lp.regular_font = "";
  lp.variant_token = "pub2";
  return page_cache_utils::BuildPageCachePath(cache_dir, ".epc", kCacheBookPath, lp);
}

static void InvokeSave(Book *book, const char *path) {
  epub_page_cache::Save(book, path,
                        kCachePx, kCacheLS, kCachePS, kCachePI, kCacheOri,
                        kCacheMl, kCacheMr, kCacheMt, kCacheMb,
                        nullptr, false);
}

static bool InvokeTryLoad(Book *book, const char *path) {
  return epub_page_cache::TryLoad(book, path,
                                  kCachePx, kCacheLS, kCachePS, kCachePI, kCacheOri,
                                  kCacheMl, kCacheMr, kCacheMt, kCacheMb,
                                  nullptr);
}

// ---------------------------------------------------------------------------
// Roundtrip test
// ---------------------------------------------------------------------------

void TestEpubPageCacheRoundtrip() {
  char cache_dir[] = "/tmp/3dslibris-cache-rt-XXXXXX";
  if (!mkdtemp(cache_dir)) {
    fprintf(stderr, "SKIP TestEpubPageCacheRoundtrip: mkdtemp failed\n");
    return;
  }
  epub_page_cache::SetCacheDirForTest(cache_dir);

  // Build a Book with 2 pages, 1 chapter, a title, and an inline link href.
  Book *book = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  book->SetTitle("Cache Roundtrip Title");
  book->AppendPage();   // page 0 (empty buffer)
  Page *linked_page = book->AppendPage();
  const u16 href_id = book->RegisterInlineLinkHref("OEBPS/ch1.xhtml#section");
  const uint32_t linked_buffer[] = {TEXT_LINK_START, href_id, 'G',
                                    'o', TEXT_LINK_END};
  linked_page->SetBuffer(linked_buffer,
                         (int)(sizeof(linked_buffer) / sizeof(linked_buffer[0])));
  book->AddChapter(0, "Cache Chapter One", 0);
  book->SetChapterAnchorPage("OEBPS/ch1.xhtml#section", 1);

  InvokeSave(book, kCacheBookPath);

  // Load into a fresh Book.
  Book *book2 = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  bool loaded = InvokeTryLoad(book2, kCacheBookPath);

  ExpectTrue("cache roundtrip: TryLoad returns true", loaded);
  ExpectTrue("cache roundtrip: page count matches",
             (int)book2->GetPageCount() == (int)book->GetPageCount());
  const char *t = book2->GetTitle();
  ExpectTrue("cache roundtrip: title survives",
             t && std::string(t) == "Cache Roundtrip Title");
  ExpectTrue("cache roundtrip: chapter count matches",
             book2->GetChapters().size() == 1);
  if (!book2->GetChapters().empty())
    ExpectTrue("cache roundtrip: chapter title survives",
               book2->GetChapters()[0].title == "Cache Chapter One");
  ExpectTrue("cache roundtrip: inline href count survives",
             book2->GetInlineLinkHrefCount() == 1);
  const std::string *href = book2->GetInlineLinkHref(1);
  ExpectTrue("cache roundtrip: inline href survives",
             href && *href == "OEBPS/ch1.xhtml#section");
  Page *loaded_linked_page = book2->GetPage(1);
  ExpectTrue("cache roundtrip: link token page survives",
             loaded_linked_page && loaded_linked_page->GetInlineLinkCount() == 1);
  u16 anchor_page = 0;
  ExpectTrue("cache roundtrip: inline anchor target survives",
             book2->FindChapterAnchorPage("OEBPS/ch1.xhtml#section",
                                          &anchor_page) &&
                 anchor_page == 1);

  book->Close();
  book2->Close();
  delete book;
  delete book2;

  // Cleanup
  epub_page_cache::SetCacheDirForTest(nullptr);
  std::string cache_file = CacheFilePath(cache_dir);
  if (!cache_file.empty())
    remove(cache_file.c_str());
  rmdir(cache_dir);
  g_pass++;  // cleanup survived
}

// ---------------------------------------------------------------------------
// Header validation tests (using the same temp dir trick)
// ---------------------------------------------------------------------------

static void WriteU32LE(FILE *fp, uint32_t v) {
  fwrite(&v, 1, sizeof(v), fp);
}
static void WriteU16LE(FILE *fp, uint16_t v) {
  fwrite(&v, 1, sizeof(v), fp);
}

static FILE *OpenCacheFileForWrite(const char *cache_dir, std::string *path_out) {
  *path_out = CacheFilePath(cache_dir);
  if (path_out->empty())
    return nullptr;
  return fopen(path_out->c_str(), "wb");
}

static bool TryLoadCorrupt(const char *cache_dir) {
  Book *book = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  bool ok = InvokeTryLoad(book, kCacheBookPath);
  book->Close();
  delete book;
  return ok;
}

void TestEpubPageCacheHeaderValidation() {
  char cache_dir[] = "/tmp/3dslibris-cache-hv-XXXXXX";
  if (!mkdtemp(cache_dir)) {
    fprintf(stderr, "SKIP TestEpubPageCacheHeaderValidation: mkdtemp failed\n");
    return;
  }
  epub_page_cache::SetCacheDirForTest(cache_dir);

  std::string path;

  // Wrong magic
  {
    FILE *fp = OpenCacheFileForWrite(cache_dir, &path);
    if (fp) {
      WriteU32LE(fp, 0xDEADBEEFU); // bad magic
      WriteU16LE(fp, 6);           // version
      WriteU16LE(fp, 0);           // title_len
      WriteU32LE(fp, 1);           // page_count
      WriteU32LE(fp, 0);           // chapter_count
      WriteU32LE(fp, 0);           // doc_start_count
      WriteU32LE(fp, 0);           // image_count
      fclose(fp);
      ExpectFalse("cache hdr: wrong magic → TryLoad returns false",
                  TryLoadCorrupt(cache_dir));
    }
  }

  // Wrong version
  {
    FILE *fp = OpenCacheFileForWrite(cache_dir, &path);
    if (fp) {
      WriteU32LE(fp, 0x45504347U); // correct magic
      WriteU16LE(fp, 0xFFFFU);     // bad version
      WriteU16LE(fp, 0);
      WriteU32LE(fp, 1);
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      fclose(fp);
      ExpectFalse("cache hdr: wrong version → TryLoad returns false",
                  TryLoadCorrupt(cache_dir));
    }
  }

  // Zero page_count (invalid)
  {
    FILE *fp = OpenCacheFileForWrite(cache_dir, &path);
    if (fp) {
      WriteU32LE(fp, 0x45504347U);
      WriteU16LE(fp, 6);
      WriteU16LE(fp, 0);
      WriteU32LE(fp, 0);           // page_count == 0 → invalid
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      fclose(fp);
      ExpectFalse("cache hdr: page_count=0 → TryLoad returns false",
                  TryLoadCorrupt(cache_dir));
    }
  }

  // Excessive page_count
  {
    FILE *fp = OpenCacheFileForWrite(cache_dir, &path);
    if (fp) {
      WriteU32LE(fp, 0x45504347U);
      WriteU16LE(fp, 6);
      WriteU16LE(fp, 0);
      WriteU32LE(fp, 99999U);      // > 50000 limit
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      WriteU32LE(fp, 0);
      fclose(fp);
      ExpectFalse("cache hdr: page_count>50000 → TryLoad returns false",
                  TryLoadCorrupt(cache_dir));
    }
  }

  // Truncated header (only 4 bytes written)
  {
    FILE *fp = OpenCacheFileForWrite(cache_dir, &path);
    if (fp) {
      WriteU32LE(fp, 0x45504347U); // just the magic, header incomplete
      fclose(fp);
      ExpectFalse("cache hdr: truncated header → TryLoad returns false",
                  TryLoadCorrupt(cache_dir));
    }
  }

  epub_page_cache::SetCacheDirForTest(nullptr);
  if (!path.empty())
    remove(path.c_str());
  rmdir(cache_dir);
  g_pass++;  // cleanup survived
}

} // namespace

int main() {
  TestEpubOpen();
  TestRealEpubOpenFromEnv();
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
  TestEpubPageCacheRoundtrip();
  TestEpubPageCacheHeaderValidation();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
