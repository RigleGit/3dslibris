#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/epub/epub_page_cache.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mobi_page_cache {
void SavePending(Book *) {}
}

namespace {

int failures = 0;
const char *const kBookPath = "/tmp/3dslibris-epub-cache-owner.epub";

void Require(bool ok, const char *label) {
  if (!ok) {
    fprintf(stderr, "fixture failed: %s\n", label);
    std::exit(1);
  }
}

void Check(bool ok, const char *label) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", label);
    ++failures;
  }
}

// The file header is part of the persistent format, not a production seam.
struct CacheHeader {
  u32 magic;
  u16 version, title_len;
  u32 pages, chapters, docs, anchors, images, links;
};

struct Fixture {
  BookContext ctx;
  Book source;
  std::string directory;

  Fixture() : source(ctx) {
    char pattern[] = "/tmp/3dslibris-epub-owner-XXXXXX";
    Require(mkdtemp(pattern) != NULL, "temporary cache directory");
    directory = pattern;
    epub_page_cache::SetCacheDirForTest(directory.c_str());
  }

  ~Fixture() {
    const std::string path = CacheFile();
    if (!path.empty()) remove(path.c_str());
    rmdir(directory.c_str());
    epub_page_cache::SetCacheDirForTest(NULL);
  }

  std::string CacheFile() const {
    DIR *dir = opendir(directory.c_str());
    Require(dir != NULL, "open cache directory");
    std::string path;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] != '.') path = directory + "/" + entry->d_name;
    }
    closedir(dir);
    return path;
  }

  void AddPages(int count, int length = 2) {
    std::vector<u32> text(length, 'a');
    for (int i = 0; i < count; ++i)
      source.AppendPage()->SetBuffer(text.data(), length);
  }

  void Save() {
    epub_page_cache::Save(&source, kBookPath, 14, 2, 0, 0, 0,
                          12, 12, 10, 36, NULL);
    Require(!CacheFile().empty(), "cache was saved");
  }

  bool Load(Book *book) {
    return epub_page_cache::TryLoad(book, kBookPath, 14, 2, 0, 0, 0,
                                    12, 12, 10, 36, NULL);
  }

  bool Begin(epub_page_cache::StreamWriter *writer) {
    return writer->Begin(&source, kBookPath, 14, 2, 0, 0, 0,
                          12, 12, 10, 36, NULL);
  }

  CacheHeader ReadHeader() {
    CacheHeader header;
    FILE *fp = fopen(CacheFile().c_str(), "rb");
    Require(fp != NULL, "open saved cache");
    Require(fread(&header, 1, sizeof(header), fp) == sizeof(header), "read header");
    fclose(fp);
    return header;
  }

  void PatchPageIndex(long offset, u16 value) {
    FILE *fp = fopen(CacheFile().c_str(), "r+b");
    Require(fp != NULL, "open cache for mutation");
    Require(fseek(fp, offset, SEEK_SET) == 0, "seek mutation");
    Require(fwrite(&value, 1, sizeof(value), fp) == sizeof(value), "write mutation");
    fclose(fp);
  }
};

void CheckRejected(Fixture &fixture, const char *label) {
  Book loaded(fixture.ctx);
  Check(!fixture.Load(&loaded), label);
  Check(loaded.GetPageCount() == 0, "rejection clears partially loaded pages");
  Check(loaded.GetChapterDocStartPages().empty() &&
            loaded.GetChapterAnchorPages().empty(),
        "rejection clears partially loaded navigation");
  Check(fixture.CacheFile().empty(), "invalid cache is removed for rebuild");
}

void TestPreviousLayoutCacheIsRejected() {
  Fixture fixture;
  fixture.AddPages(1);
  fixture.Save();
  fixture.PatchPageIndex(sizeof(u32), 21);
  CheckRejected(fixture, "previous image pagination is rebuilt");
}

void TestNavigationIndicesAreValidated() {
  // Both readers own validation separately; exercise the actual size cutoff.
  for (bool large : {false, true}) {
    const int count = large ? 4200 : 1;
    const int length = large ? 1024 : 2;
    for (int anchor = 0; anchor < 2; ++anchor) {
      for (u16 invalid : {u16(count), u16(65535)}) {
        Fixture fixture;
        fixture.AddPages(count, length);
        if (anchor)
          fixture.source.SetChapterAnchorPage("chapter.xhtml#start", 0);
        else
          fixture.source.SetChapterDocStartPage("chapter.xhtml", 0);
        fixture.Save();
        const CacheHeader header = fixture.ReadHeader();
        // Page records, then one empty search-events record for every page.
        const long navigation = sizeof(header) + header.title_len +
            count * (2 * sizeof(u16) + length * sizeof(u32));
        fixture.PatchPageIndex(navigation, invalid);
        CheckRejected(fixture, anchor ? "out-of-range anchor rejects cache"
                                      : "out-of-range document start rejects cache");
      }
    }
  }
}

void TestSaveOmitsUnresolvedNavigation() {
  Fixture fixture;
  fixture.AddPages(2);
  fixture.source.SetChapterDocStartPage("valid.xhtml", 0);
  fixture.source.SetChapterAnchorPage("valid.xhtml#start", 1);
  // Empty trailing spine documents and anchors can point at the next page.
  fixture.source.SetChapterDocStartPage("empty.xhtml", 2);
  fixture.source.SetChapterAnchorPage("empty.xhtml#end", 2);
  fixture.source.SetChapterDocStartPage("bad.xhtml", 65535);
  fixture.source.SetChapterAnchorPage("bad.xhtml#end", 65535);
  fixture.Save();
  Book loaded(fixture.ctx);
  Check(fixture.Load(&loaded), "filtered navigation cache remains reusable");
  u16 page = 99;
  Check(loaded.FindChapterDocStartPage("valid.xhtml", &page) && page == 0,
        "valid document start survives save");
  Check(loaded.FindChapterAnchorPage("valid.xhtml#start", &page) && page == 1,
        "valid anchor survives save");
  Check(!loaded.FindChapterDocStartPage("empty.xhtml", &page) &&
            !loaded.FindChapterAnchorPage("empty.xhtml#end", &page) &&
            !loaded.FindChapterDocStartPage("bad.xhtml", &page) &&
            !loaded.FindChapterAnchorPage("bad.xhtml#end", &page),
        "unresolved navigation is omitted instead of invalidating every reopen");
}

void TestLargeCachePreservesEarlierReadFailure(bool search_record) {
  Fixture fixture;
  fixture.AddPages(4200, 1024);
  fixture.Save();
  struct stat st;
  Require(stat(fixture.CacheFile().c_str(), &st) == 0 &&
              st.st_size > 16 * 1024 * 1024,
          "fixture reaches production streaming cutoff");
  const CacheHeader header = fixture.ReadHeader();
  const long pages_begin = sizeof(header) + header.title_len;
  const long offset = search_record
      ? pages_begin + 4200L * (sizeof(u16) + 1024 * sizeof(u32))
      : pages_begin;
  fixture.PatchPageIndex(offset, 65535);
  CheckRejected(fixture, search_record ? "stream search failure cannot be reset by chapters"
                                       : "stream page failure cannot be reset by chapters");
}

void TestStreamWriterCountsMatchSerializedPages() {
  {
    Fixture fixture;
    fixture.AddPages(1);
    epub_page_cache::StreamWriter writer;
    Require(fixture.Begin(&writer), "begin incremental writer");
    Require(writer.FlushPages(&fixture.source, 0), "write first page");
    fixture.AddPages(1);
    page_search::Events events;
    page_search::Annotate(events, 1, page_search::Skip);
    fixture.source.GetPage(1)->SetSearchEvents(events);
    Require(writer.FlushPages(&fixture.source, 1), "write second page");
    Check(writer.Finalize(&fixture.source), "complete incremental writer finalizes");
    Book loaded(fixture.ctx);
    const bool loaded_ok = fixture.Load(&loaded);
    Check(loaded_ok && loaded.GetPageCount() == 2,
          "incremental writer produces two readable pages");
    if (loaded_ok && loaded.GetPageCount() == 2)
      Check(page_search::FlagsAt(loaded.GetPage(1)->GetSearchEvents(), 1) ==
                page_search::Skip, "incremental writer retains second-page search metadata");
  }
  {
    Fixture fixture;
    fixture.AddPages(1);
    epub_page_cache::StreamWriter writer;
    Require(fixture.Begin(&writer), "begin incomplete writer");
    Require(writer.FlushPages(&fixture.source, 0), "write incomplete batch");
    fixture.AddPages(1);
    Check(!writer.Finalize(&fixture.source), "unflushed page rejects finalization");
    Check(fixture.CacheFile().empty(), "incomplete writer removes corrupt partial cache");
  }
  {
    Fixture fixture;
    fixture.AddPages(1);
    epub_page_cache::StreamWriter writer;
    Require(fixture.Begin(&writer), "begin duplicate-batch writer");
    Require(writer.FlushPages(&fixture.source, 0), "write original batch");
    Check(!writer.FlushPages(&fixture.source, 0), "duplicate batch is rejected");
    writer.Abort();
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  if (argc > 2 || (selected != "all" && selected != "maps" &&
      selected != "maps-load" && selected != "maps-save" &&
      selected != "load-large-pages" && selected != "load-large-search" &&
      selected != "stream-counts")) {
    fprintf(stderr, "usage: %s [all|maps|maps-load|maps-save|load-large-pages|"
                    "load-large-search|stream-counts]\n", argv[0]);
    return 2;
  }
  if (selected == "all")
    TestPreviousLayoutCacheIsRejected();
  if (selected == "all" || selected == "maps" || selected == "maps-load")
    TestNavigationIndicesAreValidated();
  if (selected == "all" || selected == "maps" || selected == "maps-save")
    TestSaveOmitsUnresolvedNavigation();
  if (selected == "all" || selected == "load-large-pages")
    TestLargeCachePreservesEarlierReadFailure(false);
  if (selected == "all" || selected == "load-large-search")
    TestLargeCachePreservesEarlierReadFailure(true);
  if (selected == "all" || selected == "stream-counts")
    TestStreamWriterCountsMatchSerializedPages();
  printf("EPUB cache owner: %d failures\n", failures);
  return failures ? 1 : 0;
}
