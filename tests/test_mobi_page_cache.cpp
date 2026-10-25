#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/common/page_cache_utils.h"
#include "formats/mobi/mobi_page_cache.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

namespace epub_page_cache {
void SavePending(Book *, bool) {}
} // namespace epub_page_cache

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

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectEqString(const char *label, const std::string &actual,
                    const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected '" + expected + "', got '" +
         actual + "'");
  }
}

static const char *kCacheBookPath =
    "/tmp/3dslibris_mobi_cache_roundtrip_book.mobi";
static const int kCachePx = 14;
static const int kCacheLS = 2;
static const int kCachePS = 0;
static const int kCachePI = 0;
static const int kCacheOri = 0;
static const int kCacheMl = 12;
static const int kCacheMr = 12;
static const int kCacheMt = 10;
static const int kCacheMb = 36;
static const bool kLineWrapFix = true;

Book *MakeMobiBook() {
  BookContext ctx;
  Book *book = new Book(ctx);
  book->SetFolderName("/tmp");
  book->SetFileName("3dslibris_mobi_cache_roundtrip_book.mobi");
  book->format = FORMAT_XHTML;
  book->SetMobiLineWrapFix(kLineWrapFix);
  return book;
}

std::string CacheFilePath(const char *cache_dir) {
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
  lp.variant_token = kLineWrapFix ? "1" : "0";
  return page_cache_utils::BuildPageCachePath(cache_dir, ".mpc",
                                              kCacheBookPath, lp);
}

void AddPages(Book *book) {
  Page *page0 = book->AppendPage();
  const uint32_t first[] = {'M', 'O', 'B', 'I'};
  page0->SetBuffer(first, 4);
  page_search::Events events;
  page_search::Annotate(events, 1, page_search::Skip);
  page0->SetSearchEvents(events);

  Page *page1 = book->AppendPage();
  const uint32_t second[] = {'c', 'a', 'c', 'h', 'e'};
  page1->SetBuffer(second, 5);
  page_search::Events rtl;
  page_search::Annotate(rtl, 1, page_search::RtlStart);
  page_search::Annotate(rtl, 3, page_search::RtlEnd);
  page1->SetSearchEvents(rtl);
}

void InvokeSave(Book *book) {
  mobi_page_cache::Save(book, kCacheBookPath, kCachePx, kCacheLS, kCachePS,
                        kCachePI, kCacheOri, kCacheMl, kCacheMr, kCacheMt,
                        kCacheMb, nullptr, kLineWrapFix);
}

bool InvokeTryLoad(Book *book) {
  return mobi_page_cache::TryLoad(book, kCacheBookPath, kCachePx, kCacheLS,
                                  kCachePS, kCachePI, kCacheOri, kCacheMl,
                                  kCacheMr, kCacheMt, kCacheMb, nullptr,
                                  kLineWrapFix);
}

void AssertLoadedBook(Book *book) {
  ExpectEq("loaded page count", book->GetPageCount(), 2);
  ExpectEq("search continuity survives cache",
           page_search::FlagsAt(book->GetPage(0)->GetSearchEvents(), 1), page_search::Skip);
  ExpectTrue("cached RTL range remains balanced",
             page_search::Valid(book->GetPage(1)->GetSearchEvents(), 5));
  ExpectEq("cached RTL range begins at original offset",
           page_search::FlagsAt(book->GetPage(1)->GetSearchEvents(), 1), page_search::RtlStart);
  ExpectEq("cached RTL range ends at original offset",
           page_search::FlagsAt(book->GetPage(1)->GetSearchEvents(), 3), page_search::RtlEnd);
  ExpectEqString("loaded title", book->GetTitle(), "MOBI Cache Title");
  ExpectEq("loaded chapter count", (int)book->GetChapters().size(), 1);
  ExpectEqString("loaded chapter title", book->GetChapters()[0].title,
                 "Cached Chapter");
  ExpectEq("loaded toc quality", (int)book->GetTocQuality(),
           (int)TOC_QUALITY_MIXED);
  ExpectEq("loaded toc direct count", book->GetTocDirectCount(), 2);
  ExpectEq("loaded toc heuristic count", book->GetTocHeuristicCount(), 1);
  ExpectEq("loaded toc unresolved count", book->GetTocUnresolvedCount(), 0);
  ExpectFalse("loaded render settings match line wrap",
              book->NeedsMobiRenderRefresh());

  Page *page0 = book->GetPage(0);
  Page *page1 = book->GetPage(1);
  ExpectEq("loaded page 0 length", page0 ? page0->GetLength() : -1, 4);
  ExpectEq("loaded page 1 length", page1 ? page1->GetLength() : -1, 5);
  ExpectEq("loaded page 0 first char", page0->GetBuffer()[0], 'M');
  ExpectEq("loaded page 1 last char", page1->GetBuffer()[4], 'e');
}

void RunRoundtrip(bool force_stream_load) {
  char cache_dir[] = "/tmp/3dslibris-mobi-cache-XXXXXX";
  if (!mkdtemp(cache_dir))
    Fail("mkdtemp failed");

  mobi_page_cache::SetCacheDirForTest(cache_dir);
  mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(force_stream_load ? 1 : -1);

  Book *book = MakeMobiBook();
  book->SetTitle("MOBI Cache Title");
  AddPages(book);
  book->AddChapter(1, "Cached Chapter", 0);
  book->SetTocConfidence(TOC_QUALITY_MIXED, 2, 1, 0);
  InvokeSave(book);

  std::string cache_file = CacheFilePath(cache_dir);
  FILE *fp = fopen(cache_file.c_str(), "rb");
  ExpectTrue("cache file exists", fp != nullptr);
  if (fp)
    fclose(fp);

  Book *loaded = MakeMobiBook();
  const bool ok = InvokeTryLoad(loaded);
  ExpectTrue(force_stream_load ? "stream TryLoad returns true"
                               : "bulk TryLoad returns true",
             ok);
  AssertLoadedBook(loaded);

  book->Close();
  loaded->Close();
  delete book;
  delete loaded;

  mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(-1);
  mobi_page_cache::SetCacheDirForTest(nullptr);
  remove(cache_file.c_str());
  rmdir(cache_dir);
}

// Exercise real files on both sides of the automatic 16 MiB cutoff.
void RunLargeRoundtrip(int page_count) {
  char cache_dir[] = "/tmp/3dslibris-mobi-large-XXXXXX";
  ExpectTrue("large cache directory", mkdtemp(cache_dir) != nullptr);
  mobi_page_cache::SetCacheDirForTest(cache_dir);
  mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(-1);
  Book *book = MakeMobiBook();
  book->SetTitle("Large MOBI");
  std::vector<u32> data(1024);
  for (int page = 0; page < page_count; page++) {
    for (int i = 0; i < 1024; i++) data[i] = 33 + (page + i) % 90;
    book->AppendPage()->SetBuffer(data.data(), (int)data.size());
  }
  book->AddChapter(17, "Large chapter", 0);
  InvokeSave(book);
  delete book;

  const std::string cache_file = CacheFilePath(cache_dir);
  FILE *fp = fopen(cache_file.c_str(), "rb");
  ExpectTrue("large cache saved", fp != nullptr);
  fseek(fp, 0, SEEK_END);
  const long bytes = ftell(fp);
  fclose(fp);
  ExpectTrue("fixture straddles automatic bulk/stream cutoff",
             page_count == 4090 ? bytes < 16L * 1024 * 1024
                                : bytes > 16L * 1024 * 1024);
  Book *loaded = MakeMobiBook();
  ExpectTrue("large cache loads", InvokeTryLoad(loaded));
  ExpectEq("large page count", loaded->GetPageCount(), page_count);
  ExpectEqString("large title", loaded->GetTitle(), "Large MOBI");
  ExpectEq("large chapter count", (int)loaded->GetChapters().size(), 1);
  ExpectEq("large chapter page", loaded->GetChapters()[0].page, 17);
  for (int page = 0; page < page_count; page++) {
    Page *p = loaded->GetPage(page);
    ExpectEq("large page length", p->GetLength(), 1024);
    for (int i = 0; i < 1024; i++)
      ExpectEq("large cache preserves every codepoint", p->GetBuffer()[i],
               33 + (page + i) % 90);
  }
  delete loaded;
  mobi_page_cache::SetCacheDirForTest(nullptr);
  remove(cache_file.c_str());
  rmdir(cache_dir);
}

void TestTruncatedCaches() {
  char cache_dir[] = "/tmp/3dslibris-mobi-truncated-XXXXXX";
  ExpectTrue("truncation cache directory", mkdtemp(cache_dir) != nullptr);
  mobi_page_cache::SetCacheDirForTest(cache_dir);
  Book *book = MakeMobiBook();
  book->SetTitle("MOBI Cache Title");
  AddPages(book);
  book->AddChapter(1, "Cached Chapter", 0);
  book->SetTocConfidence(TOC_QUALITY_MIXED, 2, 1, 0);
  InvokeSave(book);
  delete book;
  const std::string cache_file = CacheFilePath(cache_dir);
  FILE *fp = fopen(cache_file.c_str(), "rb");
  ExpectTrue("fixture cache saved", fp != nullptr);
  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  rewind(fp);
  std::vector<unsigned char> bytes((size_t)size);
  ExpectTrue("read fixture", fread(bytes.data(), 1, bytes.size(), fp) == bytes.size());
  fclose(fp);

  // The v22 wire header stores image_count at offset 16. Add a trailing image
  // record to exercise EOF in the path and follow-lines fields as well.
  bytes[16] = 1;
  const std::string image_path = "mobi:recindex:1";
  bytes.push_back((unsigned char)image_path.size());
  bytes.push_back(0);
  bytes.insert(bytes.end(), image_path.begin(), image_path.end());
  bytes.push_back(3);
  int cases = 0;
  for (bool stream : {false, true}) {
    mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(stream ? 1 : -1);
    for (size_t cut = 0; cut <= bytes.size(); cut++) {
      fp = fopen(cache_file.c_str(), "wb");
      ExpectTrue("write truncation fixture", fp != nullptr);
      ExpectTrue("write truncated bytes", fwrite(bytes.data(), 1, cut, fp) == cut);
      fclose(fp);
      Book *loaded = MakeMobiBook();
      const bool ok = InvokeTryLoad(loaded);
      if (cut == bytes.size()) {
        ExpectTrue("complete cache remains readable", ok);
        AssertLoadedBook(loaded);
      } else {
        ExpectFalse("truncated cache rejected", ok);
        ExpectEq("partial pages discarded", loaded->GetPageCount(), 0);
        ExpectTrue("partial chapters discarded", loaded->GetChapters().empty());
        ExpectTrue("broken cache removed for rebuild",
                   access(cache_file.c_str(), F_OK) != 0);
      }
      delete loaded;
      cases++;
    }
  }
  // Independent v22 wire layout: 32-byte header, raw title, then two
  // length-prefixed u32 page buffers before the search event section.
  const size_t event = 32 + std::string("MOBI Cache Title").size() + 2 + 4*4 + 2 + 5*4 + 2;
  ExpectEq("fixture event offset matches wire schema", bytes[event], 33);
  for (bool stream : {false, true}) {
    mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(stream ? 1 : -1);
    for (int fault = 0; fault < 5; ++fault) {
      std::vector<unsigned char> corrupt = bytes;
      if (fault == 0) corrupt[event] = (5 << 5) | 1; // Beyond four-token page.
      if (fault == 1) corrupt[event] = 32; // Offset one without any event flag.
      if (fault == 3) corrupt[event] = 32 | 8; // Unclosed RTL range.
      if (fault == 4) corrupt[event] = 32 | 16; // RTL end without a start.
      if (fault == 2) corrupt[4] = 25; // Previous image pagination must be rebuilt.
      fp = fopen(cache_file.c_str(), "wb");
      ExpectTrue("open corrupt metadata fixture", fp != nullptr);
      ExpectTrue("write corrupt metadata fixture", fwrite(corrupt.data(), 1, corrupt.size(), fp) == corrupt.size());
      fclose(fp);
      Book *loaded = MakeMobiBook();
      ExpectFalse("invalid metadata or old cache rejected", InvokeTryLoad(loaded));
      ExpectEq("invalid metadata exposes no partial pages", loaded->GetPageCount(), 0);
      ExpectTrue("invalid cache removed for normal regeneration", access(cache_file.c_str(), F_OK) != 0);
      delete loaded;
    }
  }
  mobi_page_cache::SetMaxBulkCacheLoadBytesForTest(-1);
  mobi_page_cache::SetCacheDirForTest(nullptr);
  remove(cache_file.c_str());
  rmdir(cache_dir);
  printf("MOBI cache: %d truncation/complete cases passed\n", cases);
}

} // namespace

int main() {
  RunRoundtrip(false);
  RunRoundtrip(true);
  RunLargeRoundtrip(4090);
  RunLargeRoundtrip(4096);
  TestTruncatedCaches();
  return 0;
}
