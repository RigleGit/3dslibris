#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/epub/epub_parser.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/common/page_text_extract_utils.h"
#include "shared/text_token_constants.h"
#include "shared/app_flow_utils.h"
#include "ui/text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <dirent.h>
#include <vector>

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
  static TestCtx tc;
  Book *book = new Book(tc.ctx);
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
  if (!fp)
    Fail("required EPUB fixture", "cannot open repository fixture");
  fclose(fp);

  Book *book = MakeEpubBook(TEST_FIXTURES_DIR "/books", "basic.epub");
  uint8_t err = EpubOpen(book);
  ExpectFalse("epub open: no error", err != 0);
  ExpectGt("epub open: pages > 0", (int)book->GetPageCount(), 0);

  // TOC: fixture has 2 chapters so at least 1 chapter entry expected.
  ExpectGt("epub open: chapters >= 1", (int)book->GetChapters().size(), 0);

  book->Close();
  ExpectTrue("close releases parsed pages", book->GetPageCount() == 0);
  delete book;
}

void TestEmbeddedStylesFromReportedEpub() {
  TestCtx tc;
  Book book(tc.ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("embedded-styles.epub");
  book.format = FORMAT_EPUB;
  ExpectFalse("embedded styles EPUB opens", EpubOpen(&book) != 0);
  bool centered = false, right = false;
  for (int i = 0; i < book.GetPageCount(); ++i) {
    Page *page = book.GetPage(i);
    for (int j = 0; j < page->GetLength(); ++j) {
      centered |= page->GetBuffer()[j] == TEXT_PARAGRAPH_CENTER;
      right |= page->GetBuffer()[j] == TEXT_PARAGRAPH_RIGHT;
    }
  }
  ExpectTrue("real EPUB embedded center reaches page tokens", centered);
  ExpectTrue("real EPUB embedded right reaches page tokens", right);
  book.Close();
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
  if (!fp)
    Fail("required EPUB fixture", "cannot open repository fixture");
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
  if (!fp)
    Fail("required EPUB fixture", "cannot open repository fixture");
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
  ExpectTrue("close releases parsed pages", book->GetPageCount() == 0);
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
  if (!fp)
    Fail("required EPUB fixture", "cannot open repository fixture");
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
  ExpectTrue("close releases parsed pages", book->GetPageCount() == 0);
  delete book;
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

// Discover the file the serializer actually wrote. Recomputing a hash here
// previously let obsolete cache keys make every corruption case miss the file.
static std::string CacheFilePath(const char *cache_dir) {
  DIR *dir = opendir(cache_dir);
  ExpectTrue("cache output directory exists", dir != nullptr);
  std::string path;
  while (dirent *entry = readdir(dir)) {
    const std::string name(entry->d_name);
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".epc") {
      ExpectTrue("only one cache artifact", path.empty());
      path = std::string(cache_dir) + "/" + name;
    }
  }
  closedir(dir);
  ExpectTrue("serializer created cache artifact", !path.empty());
  return path;
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
  ExpectTrue("cache roundtrip directory", mkdtemp(cache_dir) != nullptr);
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
  ExpectTrue("cache directory removed", access(cache_dir, F_OK) != 0);
}

// ---------------------------------------------------------------------------
// Header validation tests (using the same temp dir trick)
// ---------------------------------------------------------------------------

static std::vector<unsigned char> ReadBytes(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  ExpectTrue("read cache artifact", fp != nullptr);
  std::vector<unsigned char> bytes;
  unsigned char buffer[256];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    bytes.insert(bytes.end(), buffer, buffer + n);
  ExpectTrue("cache artifact read completes", !ferror(fp));
  fclose(fp);
  return bytes;
}

static void WriteBytes(const std::string &path,
                       const std::vector<unsigned char> &bytes) {
  FILE *fp = fopen(path.c_str(), "wb");
  ExpectTrue("write cache artifact", fp != nullptr);
  ExpectTrue("complete cache write", fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size());
  ExpectTrue("cache write closes", fclose(fp) == 0);
}

void TestEpubPageCacheHeaderValidation() {
  char cache_dir[] = "/tmp/3dslibris-cache-hv-XXXXXX";
  ExpectTrue("cache validation directory", mkdtemp(cache_dir) != nullptr);
  epub_page_cache::SetCacheDirForTest(cache_dir);
  Book *source = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  source->SetTitle("Cache validation");
  const uint32_t text[] = {'A', 'B', 'C'};
  source->AppendPage()->SetBuffer(text, 3);
  source->AddChapter(0, "Persisted chapter", 0);
  source->SetChapterAnchorPage("OEBPS/ch1.xhtml#target", 0);
  source->RegisterInlineLinkHref("OEBPS/ch1.xhtml#target");
  InvokeSave(source, kCacheBookPath);
  const std::string path = CacheFilePath(cache_dir);
  const std::vector<unsigned char> valid = ReadBytes(path);
  ExpectGt("cache includes body after current header", (int)valid.size(), 32);
  Book *loaded = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  ExpectTrue("uncorrupted Save output loads before mutations", InvokeTryLoad(loaded, kCacheBookPath));
  ExpectTrue("positive control has real text", loaded->GetPage(0)->GetLength() == 3 &&
             loaded->GetPage(0)->GetBuffer()[1] == 'B');
  loaded->Close();

  // Byte offsets are the on-disk field contract. Every mutation preserves
  // the valid version and body except for the single field under test.
  struct Mutation { size_t offset; size_t size; uint32_t value; };
  const Mutation mutations[] = {
      {0, 4, 0xDEADBEEF}, {4, 2, 0xFFFF}, {6, 2, 1001},
      {8, 4, 0}, {8, 4, 50001}, {12, 4, 4001}, {16, 4, 4001},
      {20, 4, 8193}, {24, 4, 65536}, {28, 4, 65536}};
  for (const Mutation &mutation : mutations) {
    std::vector<unsigned char> corrupt = valid;
    for (size_t i = 0; i < mutation.size; ++i)
      corrupt[mutation.offset + i] = (unsigned char)(mutation.value >> (8 * i));
    WriteBytes(path, corrupt);
    ExpectFalse("reject mutated cache header", InvokeTryLoad(loaded, kCacheBookPath));
    ExpectTrue("invalid cache is evicted", access(path.c_str(), F_OK) != 0);
    ExpectTrue("invalid header cannot expose partial pages", loaded->GetPageCount() == 0);
    loaded->Close();
  }
  const size_t lengths[] = {4, 32, valid.size() - 1};
  for (size_t length : lengths) {
    WriteBytes(path, std::vector<unsigned char>(valid.begin(), valid.begin() + length));
    ExpectFalse("reject truncated actual cache", InvokeTryLoad(loaded, kCacheBookPath));
    ExpectTrue("truncated cache is evicted", access(path.c_str(), F_OK) != 0);
    ExpectTrue("truncated body clears partially decoded pages", loaded->GetPageCount() == 0);
    ExpectTrue("truncated body clears partially decoded chapters", loaded->GetChapters().empty());
    ExpectTrue("truncated body clears link registry", loaded->GetInlineLinkHrefCount() == 0);
    loaded->Close();
  }
  WriteBytes(path, valid);
  ExpectTrue("restored original cache loads after negative controls", InvokeTryLoad(loaded, kCacheBookPath));
  ExpectTrue("restored title survives", std::string(loaded->GetTitle()) == "Cache validation");
  source->Close();
  loaded->Close();
  delete source;
  delete loaded;
  epub_page_cache::SetCacheDirForTest(nullptr);
  ExpectTrue("remove cache artifact", remove(path.c_str()) == 0);
  ExpectTrue("remove cache directory", rmdir(cache_dir) == 0);
}

void TestEpubStreamCommitAndAbort() {
  char cache_dir[] = "/tmp/3dslibris-cache-stream-XXXXXX";
  ExpectTrue("stream cache directory", mkdtemp(cache_dir) != nullptr);
  epub_page_cache::SetCacheDirForTest(cache_dir);
  Book *source = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  const uint32_t first[] = {'F', 'i', 'r', 's', 't'};
  const uint32_t second[] = {'L', 'a', 's', 't'};
  source->AppendPage()->SetBuffer(first, 5);
  epub_page_cache::StreamWriter writer;
  ExpectTrue("stream begin", writer.Begin(source, kCacheBookPath,
      kCachePx, kCacheLS, kCachePS, kCachePI, kCacheOri,
      kCacheMl, kCacheMr, kCacheMt, kCacheMb, nullptr));
  const std::string path = CacheFilePath(cache_dir);
  ExpectTrue("stream first batch", writer.FlushPages(source, 0));
  writer.Abort();
  writer.Abort();
  ExpectFalse("aborted stream closed", writer.IsOpen());
  ExpectTrue("aborted stream removed partial artifact", access(path.c_str(), F_OK) != 0);
  Book *loaded = MakeEpubBook("/tmp", "3dslibris_cache_roundtrip_book.epub");
  ExpectFalse("aborted stream cannot be loaded", InvokeTryLoad(loaded, kCacheBookPath));
  ExpectTrue("restart stream", writer.Begin(source, kCacheBookPath,
      kCachePx, kCacheLS, kCachePS, kCachePI, kCacheOri,
      kCacheMl, kCacheMr, kCacheMt, kCacheMb, nullptr));
  ExpectTrue("stream first page", writer.FlushPages(source, 0));
  source->AppendPage()->SetBuffer(second, 4);
  source->AddChapter(1, "Second streamed page", 1);
  ExpectTrue("stream incremental second page", writer.FlushPages(source, 1));
  ExpectTrue("commit stream", writer.Finalize(source));
  ExpectFalse("committed stream closed", writer.IsOpen());
  ExpectTrue("committed stream loads", InvokeTryLoad(loaded, kCacheBookPath));
  ExpectTrue("stream pages not duplicated", loaded->GetPageCount() == 2);
  ExpectTrue("first stream page content", loaded->GetPage(0)->GetLength() == 5 && loaded->GetPage(0)->GetBuffer()[0] == 'F');
  ExpectTrue("second stream page content", loaded->GetPage(1)->GetLength() == 4 && loaded->GetPage(1)->GetBuffer()[0] == 'L');
  ExpectTrue("stream chapter target", loaded->GetChapters().size() == 1 && loaded->GetChapters()[0].page == 1);
  source->Close(); loaded->Close();
  delete source; delete loaded;
  epub_page_cache::SetCacheDirForTest(nullptr);
  ExpectTrue("remove committed stream", remove(path.c_str()) == 0);
  ExpectTrue("remove stream directory", rmdir(cache_dir) == 0);
}

void TestNavSpineOrderAndAnchors() {
  const char *fixture = getenv("TEST_EPUB_NAV_PATH");
  ExpectTrue("NAV fixture supplied", fixture != nullptr);
  const std::string path(fixture);
  const size_t slash = path.find_last_of('/');
  Book *book = MakeEpubBook(path.substr(0, slash).c_str(), path.substr(slash + 1).c_str());
  ExpectFalse("packaged NAV EPUB opens", EpubOpen(book) != 0);
  const char *labels[] = {"First navigation label", "Second navigation label", "Final navigation label"};
  const char *hrefs[] = {"OEBPS/chapter1.xhtml#start", "OEBPS/chapter1.xhtml#later", "OEBPS/chapter2.xhtml#final"};
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  ExpectTrue("all metadata NAV entries retained", chapters.size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    ExpectTrue("NAV metadata labels replace document titles", chapters[i].title == labels[i]);
    u16 anchor_page = 65535;
    ExpectTrue("NAV target resolves actual parsed anchor", book->FindChapterAnchorPage(hrefs[i], &anchor_page));
    ExpectTrue("chapter points to its own fragment", chapters[i].page == anchor_page);
    if (i > 0)
      ExpectTrue("NAV follows spine and distinct within-document anchors", chapters[i].page > chapters[i-1].page);
  }
  book->Close();
  delete book;
}

} // namespace

int main() {
  TestEmbeddedStylesFromReportedEpub();
  TestNavSpineOrderAndAnchors();
  TestEpubOpen();
  TestRealEpubOpenFromEnv();
  TestEpubReopen();
  TestEpubInvalidFile();
  TestEpubIndexMetadata();
  TestEpubIndexMissingFile();
  TestEpubIndexThenOpen();
  TestEpubPageCacheRoundtrip();
  TestEpubPageCacheHeaderValidation();
  TestEpubStreamCommitAndAbort();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
