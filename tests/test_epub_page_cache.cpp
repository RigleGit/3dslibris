#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/common/reflow_block_cache.h"
#include "file_write_failure_utils.h"
#include "shared/status_reporter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <new>

static bool refuse_large_array=false, refuse_all_arrays=false;
static unsigned refused_arrays=0;
void *operator new[](size_t n,const std::nothrow_t &) noexcept {
  if(refuse_all_arrays || (refuse_large_array && n==65536)){++refused_arrays;return NULL;}
  return ::operator new[](n);
}
#undef write
extern "C" ssize_t write(int,const void *,size_t);
static bool observe_block_writes=false;
static size_t largest_block_write=0;
extern "C" ssize_t ObserveBlockWrite(int fd,const void *p,size_t n) {
  if(observe_block_writes) largest_block_write=std::max(largest_block_write,n);
  return write(fd,p,n);
}

#undef fflush
extern "C" int fflush(FILE *);
static unsigned cache_flushes = 0;
extern "C" int ObserveCacheFlush(FILE *fp) {
  ++cache_flushes;
  return fflush(fp);
}

#undef fwrite
extern "C" size_t fwrite(const void *, size_t, size_t, FILE *);
static bool observe_cache_writes=false;
static size_t cache_step_bytes=0,cache_largest_write=0;
extern "C" size_t ObserveCacheWrite(const void *data,size_t size,size_t count,FILE *fp) {
  if(observe_cache_writes) {
    cache_step_bytes+=size*count;
    if(size*count>cache_largest_write) cache_largest_write=size*count;
  }
  return fwrite(data,size,count,fp);
}

#undef read
extern "C" ssize_t read(int, void *, size_t);
static bool count_block_reads = false;
static unsigned block_reads = 0;
extern "C" ssize_t ObserveBlockRead(int fd, void *data, size_t size) {
  if (count_block_reads)
    ++block_reads;
  return read(fd, data, size);
}

#include <zlib.h>
#undef uncompress
extern "C" int uncompress(Bytef *, uLongf *, const Bytef *, uLong);
static bool fail_inflate_memory = false;
static unsigned forced_inflate_calls = 0;
static unsigned inflate_failure_limit = ~0u;
extern "C" int ObserveUncompress(Bytef *out, uLongf *size, const Bytef *in,
                                 uLong n) {
  if (fail_inflate_memory && forced_inflate_calls < inflate_failure_limit) {
    ++forced_inflate_calls;
    return Z_MEM_ERROR;
  }
  return uncompress(out, size, in, n);
}

#ifdef DSLIBRIS_TEST_CLOCK
long long dslibris_test_clock = 0;
long long dslibris_test_clock_step = 1;
#endif

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
    source.Close();
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
  loaded.SetOpenSessionId(317);
  Check(!fixture.Load(&loaded), label);
  Check(loaded.GetOpenSessionId() == 317,
        "failed cache load preserves opening session");
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
  Check(fixture.CacheFile() == fixture.directory + "/eabacb38b0cce68f.epc",
        "existing EPUB layout keeps its persistent cache identity");
  for (u16 version : {21,22,23}) {
    fixture.Save();
    fixture.PatchPageIndex(sizeof(u32), version);
    CheckRejected(fixture, "layouts measured before the font fix are rebuilt");
  }
}

void TestBufferedWriteFailureRemovesCache() {
  Fixture fixture;
  fixture.AddPages(2);
  fixture.Save();
  Require(remove(fixture.CacheFile().c_str()) == 0, "remove initial cache");
  Require(file_write_failure::Run([&]() {
    epub_page_cache::Save(&fixture.source, kBookPath, 14, 2, 0, 0, 0,
                          12, 12, 10, 36, NULL);
  }), "run save with a real file-size limit");
  Check(fixture.CacheFile().empty(),
        "failed final buffered flush removes incomplete EPUB cache");
  fixture.Save();
  Book loaded(fixture.ctx);
  Check(fixture.Load(&loaded) && loaded.GetPageCount() == 2,
        "EPUB cache can be saved and reopened after a write failure");
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

void TestInterruptedReplacementPreservesCache() {
  Fixture fixture;
  fixture.AddPages(2);
  fixture.Save();
  const u32 replacement[] = {'b', 'b'};
  fixture.source.GetPage(0)->SetBuffer(replacement, 2);
  epub_page_cache::StreamWriter writer;
  Require(fixture.Begin(&writer), "begin replacement writer");
  Require(writer.FlushPages(&fixture.source, 0), "write replacement pages");
  writer.Abort();
  Book loaded(fixture.ctx);
  const bool ok = fixture.Load(&loaded);
  Check(ok && loaded.GetPageCount() == 2 &&
            loaded.GetPage(0)->GetBuffer()[0] == 'a',
        "aborted replacement keeps the previous complete EPUB cache");
}

void TestSaveStepsBudgetMetadataAndPreserveContent() {
  Fixture fixture;
  fixture.AddPages(2);
  for (int i = 0; i < 1000; ++i)
    fixture.source.SetChapterAnchorPage("chapter.xhtml#" + std::to_string(i), 1);
  fixture.source.SetChapterDocStartPage("chapter.xhtml", 0);
  fixture.source.AddChapter(1, "Second page", 2);
  const u16 link = fixture.source.RegisterInlineLinkHref("chapter.xhtml#7");
  Require(link != 0, "register cache link");
  page_search::Events events;
  page_search::Annotate(events, 1, page_search::Skip);
  fixture.source.GetPage(1)->SetSearchEvents(events);
  epub_page_cache::StreamWriter writer;
  Require(fixture.Begin(&writer), "begin stepped cache save");
  unsigned steps = 0;
  cache_flushes = 0;
  epub_page_cache::StreamWriter::StepResult result;
  do {
    result = writer.Step(&fixture.source, 1, 0);
    ++steps;
    if (result == epub_page_cache::StreamWriter::Pending)
      Check(cache_flushes == 0, "pending slices do not force stdio writes to SD");
    Require(steps < 1100, "stepped writer finishes without an unbounded retry");
  } while (result == epub_page_cache::StreamWriter::Pending);
  Check(cache_flushes == 1, "cache is flushed before atomic publication");
  Check(result == epub_page_cache::StreamWriter::Complete && steps >= 1000,
        "byte budget covers anchors as well as page buffers");
  Book loaded(fixture.ctx);
  const bool ok = fixture.Load(&loaded);
  Check(ok && loaded.GetPageCount() == 2, "stepped cache loads through the real reader");
  if (ok) {
    u16 page = 0;
    Check(loaded.FindChapterAnchorPage("chapter.xhtml#999", &page) && page == 1,
          "last anchor survives incremental serialization");
    Check(loaded.GetChapters().size() == 1 && loaded.GetChapters()[0].page == 1 &&
              loaded.GetChapters()[0].title == "Second page",
          "stepped save preserves chapters");
    Check(page_search::FlagsAt(loaded.GetPage(1)->GetSearchEvents(), 1) == page_search::Skip,
          "stepped save preserves search annotations");
    Check(loaded.GetInlineLinkHrefCount() == 1 &&
              *loaded.GetInlineLinkHref(1) == "chapter.xhtml#7",
          "stepped save preserves link resources");
  }
}

void TestLargeRecordsYieldWithinWriteBudget() {
  Fixture fixture;
  fixture.AddPages(1,4096);
  page_search::Events events;
  for(unsigned i=0;i<=4096;++i) page_search::Annotate(events,i,page_search::Space);
  fixture.source.GetPage(0)->SetSearchEvents(events);
  epub_page_cache::StreamWriter writer;
  Require(fixture.Begin(&writer),"begin large page/search records");
  unsigned steps=0;
  epub_page_cache::StreamWriter::StepResult result;
  cache_largest_write=0;
  cache_step_bytes=0;observe_cache_writes=true;
  result=writer.Step(&fixture.source,4,0);observe_cache_writes=false;
  Check(result==epub_page_cache::StreamWriter::Pending && cache_step_bytes<=4,
        "a record prefix cannot force its entire payload into the same slice");
  do {
    cache_step_bytes=0;observe_cache_writes=true;
    result=writer.Step(&fixture.source,4096,0);
    observe_cache_writes=false;
    Check(cache_step_bytes<=4096,"large page/search data yields within the slice byte budget");
    Require(++steps<40,"partial records finish without stalling");
  } while(result==epub_page_cache::StreamWriter::Pending);
  Check(result==epub_page_cache::StreamWriter::Complete && steps>=8,
        "large page and search arrays span multiple resumable slices");
  Check(cache_largest_write<=4096,"individual writes remain bounded");
  Book loaded(fixture.ctx);
  const bool ok=fixture.Load(&loaded);
  Check(ok && loaded.GetPageCount()==1 && loaded.GetPage(0)->GetLength()==4096 &&
        loaded.GetPage(0)->GetSearchEvents()==events,
        "partial records round-trip without changing the cache format");
}

void TestBookOwnsPendingWriter() {
  Fixture fixture;
  fixture.AddPages(20, 1024);
  fixture.source.SetFolderName("/tmp");
  fixture.source.SetFileName("3dslibris-epub-cache-owner.epub");
  auto mark_pending = [&]() {
    fixture.source.SetPendingEpubPageCacheSaveWithParams(14, 2, 0, 0, 0,
                                                       12, 12, 10, 36, NULL);
  };
  mark_pending();
  fixture.source.PumpPendingEpubPageCacheSave();
  Check(fixture.source.HasPendingEpubPageCacheSave(),
        "one idle step does not serialize an entire large book");
  fixture.source.CancelPendingEpubPageCacheSave();
  Check(!fixture.source.HasPendingEpubPageCacheSave() && fixture.CacheFile().empty(),
        "shutdown cancellation releases the writer and removes its temporary");
  mark_pending();
  fixture.source.PumpPendingEpubPageCacheSave();
  while (fixture.source.HasPendingEpubPageCacheSave())
    fixture.source.PumpPendingEpubPageCacheSave();
  Check(!fixture.source.HasPendingEpubPageCacheSave(),
        "explicit flush completes the pending writer");
  Book loaded(fixture.ctx);
  Check(fixture.Load(&loaded) && loaded.GetPageCount() == 20 &&
            loaded.GetPage(19)->GetLength() == 1024,
        "completion after idle work saves every page exactly once");
}

void TestBlockCacheRoundtrip() {
  Fixture fixture;
  fixture.AddPages(80, 1024);
  fixture.source.SetTitle("Block cache");
  fixture.source.AddChapter(31, "Middle", 1);
  fixture.source.SetChapterDocStartPage("one.xhtml", 0);
  fixture.source.SetChapterDocStartPage("two.xhtml", 40);
  fixture.source.SetChapterAnchorPage("two.xhtml#anchor", 47);
  fixture.source.RegisterInlineLinkHref("two.xhtml#anchor");
  std::vector<u32> linked;
  for (unsigned i = 0; i < 256; ++i) {
    linked.push_back(TEXT_LINK_START);
    linked.push_back(1);
    linked.push_back('x');
    linked.push_back(TEXT_LINK_END);
  }
  fixture.source.GetPage(47)->SetBuffer(linked.data(), linked.size());
  page_search::Events events;
  page_search::Annotate(events, 2, page_search::Space);
  fixture.source.GetPage(47)->SetSearchEvents(events);
  const std::string path = fixture.directory + "/blocks.epc";
  reflow_block_cache::Writer writer;
  Require(writer.Begin(&fixture.source, path), "begin block writer");
  Require(writer.AppendDocument(0, 40), "first document");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Require(writer.AppendDocument(40, 80), "second document");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Require(writer.Finish(), "start publishing index");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Check(writer.GetResult() == reflow_block_cache::Complete,
        "publish complete v23 cache");
  // A replacement must never damage the already published cache.
  Require(writer.Begin(&fixture.source, path), "begin replacement to cancel");
  Require(writer.AppendDocument(0, 40), "append cancelled replacement");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  writer.Abort();
  Check(access((path + ".tmp").c_str(), F_OK) != 0,
        "v23 cancellation removes only temporary output");
  const std::string blocked_backup = path + ".bak";
  Require(mkdir(blocked_backup.c_str(), 0700) == 0, "block backup publication");
  FILE *blocker = fopen((blocked_backup + "/keep").c_str(), "wb");
  Require(blocker != NULL, "create deterministic publication refusal");
  fclose(blocker);
  Require(writer.Begin(&fixture.source, path), "begin failing replacement");
  Require(writer.AppendDocument(0, 80), "append failing replacement");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Require(writer.Finish(), "finalize failing replacement");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Check(writer.GetResult() == reflow_block_cache::IoError,
        "v23 publication failure is explicit");
  writer.Abort();
  Check(access((path + ".tmp").c_str(), F_OK) != 0,
        "failed replacement cleans temporary output");
  remove((blocked_backup + "/keep").c_str());
  rmdir(blocked_backup.c_str());
  // The following independent read checks that the previous content survived.
  struct stat info;
  stat(path.c_str(), &info);
  Book memory_limited(fixture.ctx);
  reflow_block_cache::Reader failed_reader;
  Require(failed_reader.Begin(&memory_limited, path, 0) ==
              reflow_block_cache::Pending,
          "start failing inflater");
  fail_inflate_memory = true;
  forced_inflate_calls = 0;
  while (failed_reader.Step() == reflow_block_cache::Pending) {
  }
  fail_inflate_memory = false;
  Check(forced_inflate_calls > 0 &&
            failed_reader.GetResult() == reflow_block_cache::NoMemory,
        "inflate allocation failure is not cache corruption");
  Check(access(path.c_str(), R_OK) == 0, "inflate OOM keeps the cache");
  failed_reader.Close();
  memory_limited.Close();
  Book loaded(fixture.ctx);
  reflow_block_cache::Reader reader;
  Require(reader.Begin(&loaded, path, 0) == reflow_block_cache::Pending,
          "open complete block reader");
  block_reads = 0;
  count_block_reads = true;
  unsigned steps = 0;
  while (reader.Step() == reflow_block_cache::Pending)
    ++steps;
  count_block_reads = false;
  Check(block_reads <= 4,
        "complete reader groups adjacent small blocks into bounded reads");
  Check(reader.GetResult() == reflow_block_cache::Complete && steps > 1,
        "v23 loading is cooperative");
  Check(steps < 40, "complete reader batches decoded pages rather than waiting "
                    "one frame per page");
  Check(loaded.GetPageCount() == 80 && loaded.GetPage(79)->GetLength() == 1024,
        "all pages restored");
  Check(loaded.GetPage(47)->GetSearchEvents() == events,
        "search events restored");
  Check(loaded.GetChapters().size() == 1 && loaded.GetChapters()[0].page == 31,
        "chapters restored");
  u16 target = 0;
  Check(loaded.FindChapterAnchorPage("two.xhtml#anchor", &target) &&
            target == 47,
        "anchors restored");
  stat(path.c_str(), &info);
  Check(info.st_size < 80 * 1024, "block format reduces repetitive content");
  reader.Close();
  loaded.Close();
  loaded.SetPosition(47);
  reader.Begin(&loaded, path, 128 * 1024);
  unsigned initial_steps = 0;
  while (reader.Step() == reflow_block_cache::Pending && ++initial_steps < 200) {
  }
  Check(reader.GetResult() == reflow_block_cache::Complete &&
            reader.IsReady(47) && !reader.IsReady(0),
        "partial reader starts at saved position");
  Check(info.st_size <= 65536 || reader.BytesRead() < (size_t)info.st_size,
        "partial reopen avoids unrelated blocks");
  const size_t bytes_before_neighbor = reader.BytesRead();
  Require(reader.Request(48), "request decoded neighbor");
  Check(reader.IsReady(48) && reader.BytesRead() == bytes_before_neighbor,
        "decoded neighbors are available immediately without I/O or another "
        "frame");
  reader.Request(47);
  reader.Pin(47);
  const u32 *pinned = loaded.GetPage(47)->GetBuffer();
  Text text;
  std::vector<u16> left(400 * 240), right(400 * 240);
  text.screenleft = left.data();
  text.screenright = right.data();
  text.screen = right.data();
  text.capture_rendered_text = true;
  loaded.GetPage(47)->Draw(&text);
  const size_t selections =
      loaded.GetPage(47)->GetRenderedSelectables().capacity() *
      sizeof(Page::SelectableRenderEntry);
  reader.FrameEnd();
  Check(selections > 0 && reader.ResidentBytes() >= selections,
        "residency includes rendered selectable storage");
  Check(reader.ResidentBytes() <= 128 * 1024,
        "frame boundary trims auxiliary page payload");
  for (unsigned page = 0; page < 80; ++page) {
    reader.Request(page);
    unsigned turns = 0;
    while (reader.Step() == reflow_block_cache::Pending && ++turns < 40) {
    }
    Check(reader.GetResult() == reflow_block_cache::Complete &&
              reader.IsReady(page),
          "residency makes progress when eviction is needed");
    Check(reader.ResidentBytes() <= 128 * 1024, "payload stays within budget");
    Check(loaded.GetPage(47)->GetBuffer() == pinned,
          "visible page remains pinned");
  }
  fail_inflate_memory = true;
  forced_inflate_calls = 0;
  inflate_failure_limit = 1;
  reader.Request(0);
  unsigned retries = 0;
  while (reader.Step() == reflow_block_cache::Pending && ++retries < 20) {
  }
  fail_inflate_memory = false;
  Check(forced_inflate_calls == 1 &&
            reader.GetResult() == reflow_block_cache::Complete &&
            reader.IsReady(0),
        "resident inflate OOM retries after releasing unrelated payload");
  Check(loaded.GetPage(47)->GetBuffer() == pinned &&
            reader.ResidentBytes() <= 128 * 1024,
        "OOM retry preserves pinned page and residency budget");
  fail_inflate_memory = true;
  forced_inflate_calls = 0;
  inflate_failure_limit = ~0u;
  reader.Request(79);
  retries = 0;
  while (reader.Step() == reflow_block_cache::Pending && ++retries < 20) {
  }
  fail_inflate_memory = false;
  Check(reader.GetResult() == reflow_block_cache::NoMemory &&
            forced_inflate_calls == 2 && retries < 20,
        "persistent resident OOM retries once and stops");
  Check(loaded.GetPage(47)->GetBuffer() == pinned &&
            access(path.c_str(), R_OK) == 0,
        "persistent OOM keeps visible page and cache");
  reader.Close();
  loaded.Close();
  loaded.SetPosition(47);
  Require(loaded.StartReflowPageCache(path, 128 * 1024),
          "attach private page backend");
  while (loaded.AdvanceReflowPageCache() > 0) {
  }
  Check(loaded.IsReflowPageReady(47), "Book restores saved page");
  loaded.SetPosition(79);
  Check(loaded.GetPosition() == 47,
        "navigation waits before committing position");
  while (loaded.AdvanceReflowPageCache() > 0) {
  }
  Check(loaded.GetPosition() == 79 && loaded.IsReflowPageReady(79),
        "loaded navigation commits target");
  loaded.SetPosition(78);
  Check(loaded.GetPosition() == 78 && !loaded.HasPendingReflowNavigation() &&
            loaded.IsReflowPageReady(78),
        "cached neighbor navigation commits synchronously");
  loaded.Close();
  loaded.SetPosition(150);
  Require(loaded.StartReflowPageCache(path, 128 * 1024),
          "open cache with stale saved position");
  while (loaded.AdvanceReflowPageCache() > 0) {}
  Check(loaded.GetPosition() == 79 && loaded.IsReflowPageReady(79),
        "cache open clamps stale position to resident last page");
  loaded.SetPosition(0);
  Check(loaded.GetPosition() >= 0 && loaded.GetPosition() < 80,
        "pending navigation never exposes stale out-of-range position");
  while (loaded.AdvanceReflowPageCache() > 0) {}
  Check(loaded.GetPosition() == 0 && loaded.GetPage()->GetLength() > 0,
        "navigation from stale saved position reaches first page");
  loaded.Close();
  loaded.SetPosition(150);
  Require(loaded.StartReflowPageCache(path, 128 * 1024),
          "start stale cache before first page is resident");
  while (!loaded.GetPageCount() && loaded.AdvanceReflowPageCache() > 0) {}
  loaded.SetPosition(0);
  Check(loaded.GetPosition() >= 0 && loaded.GetPosition() < 80,
        "navigation during initial cache load clamps previous position");
  while (loaded.AdvanceReflowPageCache() > 0) {}
  Check(loaded.GetPosition() == 0 && loaded.IsReflowPageReady(0),
        "initial pending navigation commits requested page");
  loaded.Close();
  epub_page_cache::LoadSession facade;
  facade.Begin(&loaded, path);
  while (facade.Step() == epub_page_cache::LoadSession::Pending) {
  }
  Check(facade.GetResult() == epub_page_cache::LoadSession::Complete &&
            loaded.GetPageCount() == 80,
        "v23 reopens through the shared complete reader");
  loaded.Close();
  const u32 *prepared = fixture.source.GetPage(0)->GetBuffer();
  fixture.source.SetOpenSessionId(901);
  reflow_block_cache::Reader *validated = new reflow_block_cache::Reader;
  Require(validated->Begin(&fixture.source, path, 128 * 1024, true) ==
              reflow_block_cache::Pending,
          "validate index while prepared pages remain resident");
  while (validated->Step() == reflow_block_cache::Pending) {
  }
  Check(validated->GetResult() == reflow_block_cache::Complete &&
            fixture.source.GetPage(0)->GetBuffer() == prepared,
        "index validation preserves prepared page buffers");
  const size_t checked_bytes = validated->BytesRead();
  Require(validated->ActivatePreparedPages() &&
              fixture.source.AdoptPreparedReflowPageCache(validated),
          "adopt checked cache without closing the book");
  while (fixture.source.AdvanceReflowPageCache() > 0) {
  }
  Check(fixture.source.GetOpenSessionId() == 901 &&
            fixture.source.GetPage(0)->GetBuffer() == prepared,
        "resident activation preserves session and visible payload");
  Check(validated->BytesRead() == checked_bytes,
        "activation does not reread the saved page from SD");
  FILE *fp = fopen(path.c_str(), "r+b");
  fseek(fp, 35, SEEK_SET);
  int byte = fgetc(fp);
  fseek(fp, 35, SEEK_SET);
  fputc(byte ^ 64, fp);
  fclose(fp);
  reader.Begin(&loaded, path, 0);
  while (reader.Step() == reflow_block_cache::Pending) {
  }
  Check(reader.GetResult() == reflow_block_cache::Corrupt,
        "block CRC detects changed data");
  reader.Close();
  loaded.Close();
  remove(path.c_str());
}

void TestLongBlockIndexStrings() {
  Fixture fixture;
  fixture.AddPages(1);
  const std::string title =
      std::string(999, 'a') + "\xc3\xa9" + std::string(200, 'b');
  const std::string href(4096, 'h');
  fixture.source.SetTitle(title.c_str());
  fixture.source.AddChapter(0, std::string(3000, 'c'));
  fixture.source.RegisterInlineLinkHref(href);
  fixture.source.SetChapterDocStartPage(href, 0);
  const std::string path = fixture.directory + "/long.epc";
  reflow_block_cache::Writer writer;
  Require(writer.Begin(&fixture.source, path) && writer.AppendDocument(0, 1),
          "begin long metadata cache");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Check(writer.Finish(), "long metadata can finish cache");
  while (writer.Step() == reflow_block_cache::Pending) {
  }
  Check(writer.GetResult() == reflow_block_cache::Complete,
        "display labels cannot disable caching");
  Book loaded(fixture.ctx);
  reflow_block_cache::Reader reader;
  reader.Begin(&loaded, path, 0);
  while (reader.Step() == reflow_block_cache::Pending) {
  }
  Check(reader.GetResult() == reflow_block_cache::Complete,
        "long metadata reopens");
  if (reader.GetResult() == reflow_block_cache::Complete) {
    Check(std::string(loaded.GetTitle()) == std::string(999, 'a'),
          "display title truncates at a UTF8 boundary");
    Check(loaded.GetChapters()[0].title == std::string(2048, 'c'),
          "chapter display label is bounded");
    const std::string *restored = loaded.GetInlineLinkHref(1);
    u16 page = 99;
    Check(restored && *restored == href &&
              loaded.FindChapterDocStartPage(href, &page) && page == 0,
          "resource identities are never truncated");
  }
  reader.Close();
  loaded.Close();
  writer.Abort();
  remove(path.c_str());
}

void TestCooperativeLegacyLoad() {
  Fixture fixture;
  fixture.AddPages(60, 1024);
  fixture.Save();
  Book loaded(fixture.ctx);
  epub_page_cache::LoadSession reader;
  Check(reader.Begin(&loaded, fixture.CacheFile()) ==
            epub_page_cache::LoadSession::Pending,
        "legacy load opens without decoding all pages");
  Check(loaded.GetPageCount() == 0,
        "begin never exposes partial page collection");
  unsigned steps = 0;
  while (reader.Step() == epub_page_cache::LoadSession::Pending)
    ++steps;
  Check(reader.GetResult() == epub_page_cache::LoadSession::Complete &&
            steps > 1 && loaded.GetPageCount() == 60,
        "bounded buffered load restores complete book");
  loaded.Close();
  reader.Begin(&loaded, fixture.CacheFile());
  reader.Step();
  reader.Cancel();
  Check(reader.GetResult() == epub_page_cache::LoadSession::Cancelled &&
            loaded.GetPageCount() == 0,
        "cancelling clears partial pages");
  Check(access(fixture.CacheFile().c_str(), R_OK) == 0,
        "cancellation preserves valid input file");
}

void TestCloseNeverStartsDeferredEpubSave() {
  Fixture fixture;
  fixture.AddPages(2);
  fixture.source.SetFolderName("/tmp");
  fixture.source.SetFileName("3dslibris-epub-cache-owner.epub");
  fixture.source.SetPendingEpubPageCacheSaveWithParams(14, 2, 0, 0, 0, 12, 12,
                                                       10, 36, NULL);
  observe_cache_writes = true;
  cache_step_bytes = 0;
  fixture.source.Close();
  observe_cache_writes = false;
  Check(cache_step_bytes == 0, "closing never writes a deferred EPUB cache");
  Check(fixture.CacheFile().empty(),
        "closing does not publish an optional cache");
}

#if defined(DSLIBRIS_DEBUG) && defined(DSLIBRIS_TEST_CLOCK)
struct MetricsReporter : IStatusReporter {
  std::string load, save;
  unsigned slow=0;
  void PrintStatus(const char *message) override {
    if (strstr(message, "EPUB cache load: hdr=")) load = message;
    if (strstr(message, "EPUB cache save: done")) save = message;
    if (strstr(message, "EPUB cache save: slow slice")) ++slow;
  }
  void PrintStatus(std::string message) override { PrintStatus(message.c_str()); }
};

unsigned long long Metric(const std::string &message, const char *key) {
  const size_t start = message.find(key);
  Check(start != std::string::npos, key);
  return start == std::string::npos ? 0 :
      strtoull(message.c_str() + start + strlen(key), NULL, 10);
}

void TestCacheLoadMetricsStayWithinTotal() {
  // Exercise both real reader branches with a monotonic platform clock.
  for (bool large : {false, true}) {
    Fixture fixture;
    fixture.AddPages(large ? 4200 : 2, large ? 1024 : 2);
    fixture.source.AddChapter(1, "Second page", 1);
    fixture.source.SetChapterDocStartPage("chapter.xhtml", 0);
    fixture.source.SetChapterAnchorPage("chapter.xhtml#start", 1);
    fixture.Save();
    MetricsReporter reporter;
    BookContext ctx = fixture.ctx;
    ctx.status_reporter = &reporter;
    Book loaded(ctx);
    if (large)
      dslibris_test_clock_step = 100;
    Require(fixture.Load(&loaded), "load real cache for metrics");
    dslibris_test_clock_step = 1;
    const unsigned long long total = Metric(reporter.load, " total=");
    Check(total > 0, "load metrics use elapsed platform time");
    for (const char *key : {" hdr=", " pages=", " chapters=", " doc_starts=",
                            " anchors=", " images=", " hrefs="})
      Check(Metric(reporter.load, key) <= total,
            "each cache load phase fits within total time");
  }
}

void TestCacheSaveMetricsSeparateIdleAndClosing() {
  Fixture fixture;
  MetricsReporter reporter;
  BookContext ctx = fixture.ctx;
  ctx.status_reporter = &reporter;
  Book book(ctx);
  const u32 text[] = {'a', 'b'};
  book.AppendPage()->SetBuffer(text, 2);
  book.AppendPage()->SetBuffer(text, 2);
  epub_page_cache::StreamWriter writer;
  Require(writer.Begin(&book, kBookPath, 14, 2, 0, 0, 0,
                       12, 12, 10, 36, NULL), "begin metrics save");
  dslibris_test_clock +=
      10000; // Time outside Step must not count as active I/O.
  Require(writer.Step(&book, 10, 0) == epub_page_cache::StreamWriter::Pending,
          "save starts with a bounded idle step");
  // Closing is deliberately slower; it must not contaminate the idle maximum.
  dslibris_test_clock_step = 100;
  Require(writer.FlushPages(&book, 1, true) && writer.Finalize(&book, true),
          "finish real cache with explicit flush");
  dslibris_test_clock_step = 1;
  const unsigned long long idle = Metric(reporter.save, " idle_max_slice_ms=");
  const unsigned long long closing = Metric(reporter.save, " closing_max_slice_ms=");
  Check(idle > 0 && idle < 100, "idle maximum excludes slow closing work");
  Check(closing >= 100, "closing work has its own maximum");
  Check(Metric(reporter.save, " max_slice_ms=") == closing,
        "overall maximum includes closing work");
  Check(Metric(reporter.save, " closing=") == 1,
        "completion still identifies the final explicit flush");
  Book loaded(fixture.ctx);
  Check(fixture.Load(&loaded) && loaded.GetPageCount() == 2,
        "metrics do not alter the saved cache");
  // A cache interrupted before completion must still expose slow I/O without
  // producing a log on every idle frame.
  epub_page_cache::StreamWriter slow_writer;
  Require(slow_writer.Begin(&book,kBookPath,14,2,0,0,0,12,12,10,36,NULL),"begin slow diagnostics");
  reporter.slow=0;dslibris_test_clock_step=100;
  Require(slow_writer.Step(&book,1,0)==epub_page_cache::StreamWriter::Pending,"first slow slice");
  Require(slow_writer.Step(&book,1,0)==epub_page_cache::StreamWriter::Pending,"second slow slice");
  Check(reporter.slow==1,"unfinished slow slices are logged with bounded frequency");
  dslibris_test_clock_step=1;slow_writer.Abort();
}
#endif

} // namespace

static void TestPhysicalWriterFallback() {
  BookContext ctx;Book book(ctx);
  for(unsigned p=0;p<8;++p) {
    u32 tokens[4096];for(unsigned i=0;i<4096;++i) tokens[i]=((i+p*4096)*2654435761u)|0x10000u;
    book.AppendPage()->SetBuffer(tokens,4096);
  }
  const std::string path="/tmp/3dslibris-writer-fallback.epc";
  reflow_block_cache::Writer writer;
  refuse_large_array=true;refused_arrays=0;
  Require(writer.Begin(&book,path),"writer falls back to small physical staging");
  refuse_large_array=false;
  Check(refused_arrays==1,"writer really attempted the fallible large staging");
  observe_block_writes=true;largest_block_write=0;
  Require(writer.AppendDocument(0,book.GetPageCount()),"append with 16K staging");
  while(writer.Step()==reflow_block_cache::Pending) {}
  Require(writer.Finish(),"finish with 16K staging");
  while(writer.Step()==reflow_block_cache::Pending) {}
  observe_block_writes=false;
  Check(writer.GetResult()==reflow_block_cache::Complete && largest_block_write==16384,"physical fallback groups writes at 16K");
  Book warm(ctx);reflow_block_cache::Reader reader;
  reader.Begin(&warm,path,0);while(reader.Step()==reflow_block_cache::Pending) {}
  Check(reader.GetResult()==reflow_block_cache::Complete && warm.GetPageCount()==8 && warm.GetPage(7)->GetBuffer()[4095]==(((4095+7*4096)*2654435761u)|0x10000u),"fallback keeps complete payload");
  writer.Abort();refuse_all_arrays=true;
  Check(!writer.Begin(&book,path) && writer.GetResult()==reflow_block_cache::NoMemory,"physical staging OOM is explicit");
  refuse_all_arrays=false;writer.Abort();
  reader.Close();remove(path.c_str());
}

static void TestMobiBlockIdentity() {
  BookContext ctx;
  Book book(ctx);
  const u32 tokens[] = {'M', 0xffffffffU, 0x10000U};
  book.AppendPage()->SetBuffer(tokens, 3);
  const std::string path = "/tmp/3dslibris-mpc27-test.mpc";
  reflow_block_cache::Writer writer;
  Require(writer.Begin(&book, path, reflow_block_cache::Mobi), "begin MPC27");
  Require(writer.AppendDocument(0, 1), "append MPC27");
  while (writer.Step() == reflow_block_cache::Pending) {}
  Require(writer.Finish(), "finish MPC27");
  while (writer.Step() == reflow_block_cache::Pending) {}
  Check(writer.GetResult() == reflow_block_cache::Complete, "MPC27 publishes");
  Book warm(ctx), wrong(ctx);
  reflow_block_cache::Reader reader, epub;
  reader.Begin(&warm, path, 128*1024, false, reflow_block_cache::Mobi);
  while (reader.Step() == reflow_block_cache::Pending) {}
  Check(reader.GetResult() == reflow_block_cache::Complete && warm.GetPageCount()==1 && warm.GetPage(0)->GetBuffer()[1]==0xffffffffU, "MPC27 preserves render tokens");
  epub.Begin(&wrong, path, 128*1024);
  while (epub.Step() == reflow_block_cache::Pending) {}
  Check(epub.GetResult()==reflow_block_cache::Corrupt, "EPC rejects MPC header");
  remove(path.c_str());
}

// Otherwise-valid old payloads must be rejected solely for their layout
// version, before either the complete or resident reader exposes any pages.
static void TestOldBlockLayoutsAreRejected() {
  const struct { reflow_block_cache::Format format; u16 version; } cases[] = {
    {reflow_block_cache::Epub,23},
    {reflow_block_cache::Mobi,27},
    {reflow_block_cache::Fb2,1}
  };
  for (const auto &test : cases) {
    Fixture fixture;
    fixture.AddPages(1);
    const std::string path=fixture.directory+"/old-layout.cache";
    reflow_block_cache::Writer writer;
    Require(writer.Begin(&fixture.source,path,test.format),"begin layout fixture");
    Require(writer.AppendDocument(0,1),"append layout fixture");
    while (writer.Step()==reflow_block_cache::Pending) {}
    Require(writer.Finish(),"finish layout fixture");
    while (writer.Step()==reflow_block_cache::Pending) {}
    Require(writer.GetResult()==reflow_block_cache::Complete,"publish layout fixture");
    FILE *fp=fopen(path.c_str(),"r+b");
    Require(fp!=NULL,"open old layout fixture");
    Require(!fseek(fp,4,SEEK_SET),"seek layout version");
    Require(fwrite(&test.version,1,2,fp)==2,"patch old layout version");
    fclose(fp);
    for (size_t budget : {size_t(0),size_t(128*1024)}) {
      Book loaded(fixture.ctx);
      reflow_block_cache::Reader reader;
      reader.Begin(&loaded,path,budget,false,test.format);
      while (reader.Step()==reflow_block_cache::Pending) {}
      Check(reader.GetResult()==reflow_block_cache::Corrupt && loaded.GetPageCount()==0,
            "old block layout cannot expose pages");
    }
    if (test.format==reflow_block_cache::Epub) {
      Book loaded(fixture.ctx);
      epub_page_cache::LoadSession facade;
      facade.Begin(&loaded,path);
      while (facade.Step()==epub_page_cache::LoadSession::Pending) {}
      Check(facade.GetResult()!=epub_page_cache::LoadSession::Complete &&
                loaded.GetPageCount()==0,"EPUB facade rejects old block layout");
    }
    remove(path.c_str());
  }
}

int main(int argc, char **argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  if (argc > 2 || (selected != "all" && selected != "maps" &&
      selected != "maps-load" && selected != "maps-save" &&
      selected != "load-large-pages" && selected != "load-large-search" &&
      selected != "stream-counts" && selected != "write-failure" &&
      selected != "replacement" && selected != "steps" && selected != "owner" &&
      selected != "metrics")) {
    fprintf(stderr, "usage: %s [all|maps|maps-load|maps-save|load-large-pages|"
                    "load-large-search|stream-counts|write-failure|replacement|steps|owner|metrics]\n", argv[0]);
    return 2;
  }
  if (selected == "all") {
    TestPhysicalWriterFallback();
    TestMobiBlockIdentity();
    TestLongBlockIndexStrings();
    TestBlockCacheRoundtrip();
    TestCooperativeLegacyLoad();
    TestCloseNeverStartsDeferredEpubSave();
    TestPreviousLayoutCacheIsRejected();
    TestOldBlockLayoutsAreRejected();
  }
  if (selected == "all" || selected == "write-failure")
    TestBufferedWriteFailureRemovesCache();
  if (selected == "all" || selected == "replacement")
    TestInterruptedReplacementPreservesCache();
  if (selected == "all" || selected == "steps")
    TestSaveStepsBudgetMetadataAndPreserveContent();
  if (selected == "all" || selected == "steps")
    TestLargeRecordsYieldWithinWriteBudget();
  if (selected == "all" || selected == "owner")
    TestBookOwnsPendingWriter();
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
#if defined(DSLIBRIS_DEBUG) && defined(DSLIBRIS_TEST_CLOCK)
  if (selected == "all" || selected == "metrics") {
    TestCacheLoadMetricsStayWithinTotal();
    TestCacheSaveMetricsSeparateIdleAndClosing();
  }
#endif
  printf("EPUB cache owner: %d failures\n", failures);
  return failures ? 1 : 0;
}
