/*
    3dslibris - epub_page_cache.cpp
    EPUB persistent page cache serialization.
    Extracted from epub.cpp by Rigle.
*/

#include "book/book_parse_deps.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/common/reflow_block_cache.h"
#include "formats/common/cache_memory_utils.h"

#include "book/book.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "shared/debug_log.h"
#include "formats/common/book_search_cache.h"
#include "formats/common/page_cache_field_limits.h"
#include "formats/common/page_cache_utils.h"
#include "shared/path_constants.h"
#include "shared/open_cancel_poll.h"
#include <3ds.h>
#include <algorithm>
#include <cerrno>
#include <new>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace {

static const std::string &kEpubCacheBaseDir = paths::GetCacheBaseDir();
static const std::string &kEpubCacheDir = paths::GetEpubCacheDir();

#ifdef DSLIBRIS_HOST_TEST
static std::string g_host_test_cache_dir;
#endif

static const std::string &GetEffectiveCacheDir() {
#ifdef DSLIBRIS_HOST_TEST
  if (!g_host_test_cache_dir.empty())
    return g_host_test_cache_dir;
#endif
  return kEpubCacheDir;
}
static const u32 kEpubPageCacheMagic = 0x45504347U;
// Skip the old indexed v23 as well: both layouts used incorrect font metrics.
static const u16 kEpubPageCacheVersion = 24;
static const u16 kPageCacheHrefMaxBytes = 2048;

struct EpubPageCacheHeader {
  u32 magic;
  u16 version;
  u16 title_len;
  u32 page_count;
  u32 chapter_count;
  u32 doc_start_count;
  u32 anchor_count;
  u32 image_count;
  u32 link_href_count;
};


static bool IsValidHeader(const EpubPageCacheHeader &hdr) {
  return hdr.magic == kEpubPageCacheMagic &&
         hdr.version == kEpubPageCacheVersion && hdr.page_count > 0 &&
         hdr.page_count <= 50000 && hdr.chapter_count <= 4000 &&
         hdr.title_len <= 1000 && hdr.doc_start_count <= 4000 &&
         hdr.anchor_count <= 8192 && hdr.image_count <= 65535 &&
         hdr.link_href_count <= 65535;
}


static std::string BuildCachePath(Book *book, const char *book_path,
                                  const page_cache_utils::PageCacheLayoutParams &params) {
  if (!book_path)
    return std::string();
  page_cache_utils::PageCacheLayoutParams layout_params = params;
  layout_params.variant_token = "pub4:";
  layout_params.variant_token +=
      book && book->GetPublisherTextIndentEnabled() ? "i1" : "i0";
  layout_params.variant_token +=
      book && book->GetPublisherBlockMarginsEnabled() ? ":m1" : ":m0";
  layout_params.variant_token +=
      book && book->GetPublisherHorizontalMarginsEnabled() ? ":h1" : ":h0";
  layout_params.variant_token +=
      std::string(":a") + (char)('0' + (book ? book->GetTextAlignMode() : 0));
  if (book && !book->GetEbookImagesEnabled())
    layout_params.variant_token += ":no-images";
  layout_params.variant_token += CapturePageCacheFontIdentity(book ? book->GetText() : NULL);
  return page_cache_utils::BuildPageCachePath(
      GetEffectiveCacheDir(), ".epc", book_path, layout_params);
}

static void EnsureCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kEpubCacheBaseDir.c_str(), 0777);
  mkdir(kEpubCacheDir.c_str(), 0777);
  initialized = true;
}

} // namespace

namespace epub_page_cache {

std::string CachePath(Book *book, const char *source,
                      const page_cache_utils::PageCacheLayoutParams &params) {
  return BuildCachePath(book, source, params);
}

struct LoadSession::State {
  enum Stage {
    Header,
    Title,
    Pages,
    Search,
    Chapters,
    Documents,
    Anchors,
    Images,
    Links,
    Done
  };
  Book *book;
  reflow_block_cache::Reader *block_reader;
  FILE *fp;
  std::string path;
  unsigned char *buffer;
  size_t capacity, begin, end, bytes, calls, file_size;
  bool need_fill, eof;
  Stage stage;
  u32 index;
  Result result;
  EpubPageCacheHeader header;
  u64 started, marks[9];
  State()
      : book(NULL), block_reader(NULL), fp(NULL), buffer(NULL), capacity(65536),
        begin(0), end(0), bytes(0), calls(0), file_size(0), need_fill(true),
        eof(false), stage(Header), index(0), result(Missing), started(0) {
    memset(&header, 0, sizeof(header));
    memset(marks, 0, sizeof(marks));
  }
  ~State() {
    if (fp)
      fclose(fp);
    delete block_reader;
    delete[] buffer;
  }
  void Advance() {
    marks[(unsigned)stage] = osGetTime();
    stage = (Stage)(stage + 1);
    index = 0;
  }
  bool One(page_cache_utils::BufReader &r) {
    using namespace page_cache_utils;
    if (stage == Header) {
      EpubPageCacheHeader h;
      if (!r.ReadRaw(&h, sizeof(h)))
        return false;
      if (!IsValidHeader(h)) {
        result = Corrupt;
        return false;
      }
      header = h;
      book->ReservePageCapacity(h.page_count);
      Advance();
      return true;
    }
    if (stage == Title) {
      std::string title;
      if (!ReadRawString(&r, header.title_len, &title))
        return false;
      if (!title.empty())
        book->SetTitle(title.c_str());
      Advance();
      return true;
    }
    const u32 counts[] = {0,
                          0,
                          header.page_count,
                          header.page_count,
                          header.chapter_count,
                          header.doc_start_count,
                          header.anchor_count,
                          header.image_count,
                          header.link_href_count};
    if (index >= counts[(unsigned)stage]) {
      Advance();
      return true;
    }
    if (stage == Pages || stage == Search) {
      u16 n = 0;
      if (!r.ReadRaw(&n, 2))
        return false;
      const unsigned limit = stage == Pages
                                 ? page_cache_limits::kPageMaxBytes
                                 : book->GetPage(index)->GetLength() + 1;
      if (n > limit) {
        result = Corrupt;
        return false;
      }
      if (!r.Remaining((size_t)n * 4))
        return false;
      std::vector<u32> words(n);
      if (n)
        r.ReadRaw(words.data(), (size_t)n * 4);
      if (stage == Pages) {
        Page *page = book->AppendPage();
        page_buffer_utils::OwnedPageBuffer owned =
            page_buffer_utils::AdoptPageBuffer(&words);
        page->AdoptBuffer(&owned);
      } else {
        Page *page = book->GetPage(index);
        if (!page_search::Valid(words, page->GetLength())) {
          result = Corrupt;
          return false;
        }
        page->SetSearchEvents(words);
      }
    } else {
      u16 page = 0;
      u8 level = 0;
      const bool ref =
          stage == Chapters || stage == Documents || stage == Anchors;
      if (ref && !r.ReadRaw(&page, 2))
        return false;
      if (stage == Chapters && !r.ReadRaw(&level, 1))
        return false;
      u16 n = 0;
      if (!r.ReadRaw(&n, 2))
        return false;
      const unsigned maximum =
          stage == Chapters ? page_cache_limits::kChapterTitleMaxBytes : 2048;
      if (n > maximum || ((stage == Images || stage == Links) && !n) ||
          (ref && stage != Chapters && page >= header.page_count)) {
        result = Corrupt;
        return false;
      }
      if (!r.Remaining(n))
        return false;
      std::string text((const char *)r.cur, n);
      r.cur += n;
      if (stage == Chapters) {
        if (page < header.page_count)
          book->AddChapter(page, text, level);
      } else if (stage == Documents)
        book->SetChapterDocStartPage(text, page);
      else if (stage == Anchors)
        book->SetChapterAnchorPage(text, page);
      else if (stage == Images)
        book->RegisterInlineImage(text);
      else if (!book->RegisterInlineLinkHref(text)) {
        result = Corrupt;
        return false;
      }
    }
    ++index;
    return true;
  }
  void Log() {
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(
        book->GetStatusReporter(),
        "EPUB cache load: hdr=%llums pages=%llums(%u) chapters=%llums(%u) "
        "doc_starts=%llums(%u) anchors=%llums(%u) images=%llums(%u) "
        "hrefs=%llums(%u) total=%llums",
        (unsigned long long)(marks[Title] - started),
        (unsigned long long)(marks[Search] - marks[Title]), header.page_count,
        (unsigned long long)(marks[Chapters] - marks[Search]),
        header.chapter_count,
        (unsigned long long)(marks[Documents] - marks[Chapters]),
        header.doc_start_count,
        (unsigned long long)(marks[Anchors] - marks[Documents]),
        header.anchor_count,
        (unsigned long long)(marks[Images] - marks[Anchors]),
        header.image_count, (unsigned long long)(marks[Links] - marks[Images]),
        header.link_href_count, (unsigned long long)(osGetTime() - started));
    DBG_LOGF(book->GetStatusReporter(),
             "CACHE IO load bytes=%u calls=%u buffer=%u ms=%llu",
             (unsigned)bytes, (unsigned)calls, (unsigned)capacity,
             (unsigned long long)(osGetTime() - started));
#endif
  }
};
LoadSession::LoadSession() : state_(NULL) {}
LoadSession::~LoadSession() {
  Cancel();
  delete state_;
}
LoadSession::Result LoadSession::Begin(Book *book, const std::string &path) {
  Cancel();
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_)
    return NoMemory;
  State &s = *state_;
  s.book = book;
  s.path = path;
  s.started = osGetTime();
  if (!book || book->GetPageCount())
    return s.result = IoError;
  s.fp = fopen(path.c_str(), "rb");
  if (!s.fp)
    return s.result = errno == ENOENT ? Missing : IoError;
  setvbuf(s.fp, NULL, _IONBF, 0);
  struct stat st;
  if (fstat(fileno(s.fp), &st) == 0)
    s.file_size = (size_t)st.st_size;
  uint32_t magic = 0;
  uint16_t version = 0;
  if (fread(&magic, 1, 4, s.fp) == 4 && fread(&version, 1, 2, s.fp) == 2 &&
      magic == kEpubPageCacheMagic && version == 25) {
    fclose(s.fp);
    s.fp = NULL;
    s.block_reader = new (std::nothrow) reflow_block_cache::Reader;
    if (!s.block_reader)
      return s.result = NoMemory;
    const reflow_block_cache::Result r = s.block_reader->Begin(book, path, 0);
    return s.result = r == reflow_block_cache::Pending    ? Pending
                      : r == reflow_block_cache::NoMemory ? NoMemory
                                                        : IoError;
  }
  rewind(s.fp);
  s.buffer = new (std::nothrow) unsigned char[s.capacity + 16390];
  if (!s.buffer) {
    s.capacity = 16384;
    s.buffer = new (std::nothrow) unsigned char[s.capacity + 16390];
  }
  if (!s.buffer)
    return s.result = NoMemory;
  return s.result = Pending;
}
LoadSession::Result LoadSession::GetResult() const {
  return state_ ? state_->result : NoMemory;
}
unsigned LoadSession::Progress() const {
  return state_
             ? (state_->block_reader
                    ? (unsigned)state_->block_reader->BytesRead()
                    : (unsigned)(state_->bytes - state_->end + state_->begin))
             : 0;
}
unsigned LoadSession::Total() const {
  return state_ ? (unsigned)state_->file_size : 0;
}

void LoadSession::Cancel() {
  if (state_ && state_->result == Pending) {
    state_->result = Cancelled;
    if (state_->fp) {
      fclose(state_->fp);
      state_->fp = NULL;
    }
    if (state_->block_reader)
      state_->block_reader->Close();
    if (state_->book)
      state_->book->Close();
  }
}
LoadSession::Result LoadSession::Step() {
  if (!state_ || state_->result != Pending)
    return GetResult();
  State &s = *state_;
  if (open_cancel_poll::Poll(s.book, s.book->GetStatusReporter(),
                             "epub-cache-load")) {
    Cancel();
    return Cancelled;
  }
  if (s.block_reader) {
    const reflow_block_cache::Result result = s.block_reader->Step();
    if (result == reflow_block_cache::Pending)
      return Pending;
    s.result = result == reflow_block_cache::Complete   ? Complete
               : result == reflow_block_cache::Corrupt  ? Corrupt
               : result == reflow_block_cache::NoMemory ? NoMemory
                                                      : IoError;
    if (s.result != Complete) {
      const unsigned session = s.book->GetOpenSessionId();
      s.book->Close();
      s.book->SetOpenSessionId(session);
    }
    return s.result;
  }
  const u64 start = osGetTime();
  // Do not refill after a CPU yield: prefetched records still own the buffer.
  if (s.need_fill) {
    if (!cache_memory::HasHeadroom(s.capacity * 4, 512 * 1024))
      s.result = NoMemory;
    if (s.begin) {
      memmove(s.buffer, s.buffer + s.begin, s.end - s.begin);
      s.end -= s.begin;
      s.begin = 0;
    }
    if (s.end > 16390)
      s.result = Corrupt;
    else if (s.result == Pending) {
      const size_t got = fread(s.buffer + s.end, 1, s.capacity, s.fp);
      ++s.calls;
      s.bytes += got;
      s.end += got;
      s.eof = !got;
      s.need_fill = false;
      if (ferror(s.fp))
        s.result = IoError;
    }
  }
  while (s.result == Pending && s.stage != State::Done) {
    page_cache_utils::BufReader r(s.buffer + s.begin, s.end - s.begin);
    if (!s.One(r)) {
      s.need_fill = true;
      if (s.eof && s.result == Pending)
        s.result = Corrupt;
      break;
    }
    s.begin = (size_t)(r.cur - s.buffer);
    if (osGetTime() - start >= 50)
      break;
  }
  if (s.stage == State::Done) {
    s.result = Complete;
    s.Log();
  }
  if (s.result != Pending) {
    fclose(s.fp);
    s.fp = NULL;
    if (s.result != Complete) {
      const unsigned session = s.book->GetOpenSessionId();
      s.book->Close();
      s.book->SetOpenSessionId(session);
    }
  }
  return s.result;
}

bool TryLoad(Book *book, const char *book_path, int pixel_size,
             int line_spacing, int paragraph_spacing, int paragraph_indent,
             int orientation, int margin_left, int margin_right, int margin_top,
             int margin_bottom, const char *regular_font) {
  if (!book || !book_path)
    return false;
  EnsureCacheDirs();
  const page_cache_utils::PageCacheLayoutParams params =
      page_cache_utils::BuildLayoutParams(
          book_path, pixel_size, line_spacing, paragraph_spacing,
          paragraph_indent, orientation, margin_left, margin_right, margin_top,
          margin_bottom, regular_font);
  const std::string path = BuildCachePath(book, book_path, params);
  LoadSession reader;
  LoadSession::Result result = reader.Begin(book, path);
  while (result == LoadSession::Pending)
    result = reader.Step();
  if (result == LoadSession::Corrupt)
    remove(path.c_str());
  return result == LoadSession::Complete;
}

void Save(Book *book, const char *book_path, int pixel_size,
          int line_spacing, int paragraph_spacing, int paragraph_indent,
          int orientation, int margin_left, int margin_right, int margin_top,
          int margin_bottom, const char *regular_font, bool closing) {
  if (!book || !book_path || book->GetPageCount() == 0)
    return;
  StreamWriter writer;
  if (!writer.Begin(book, book_path, pixel_size, line_spacing,
                    paragraph_spacing, paragraph_indent, orientation,
                    margin_left, margin_right, margin_top, margin_bottom,
                    regular_font))
    return;
  if (!writer.FlushPages(book, 0, closing) || !writer.Finalize(book, closing))
    writer.Abort();
}

void SavePending(Book *book, bool closing) {
  if (!book || !book->HasPendingEpubPageCacheSave())
    return;

  const char *folder = book->GetFolderName();
  const char *file = book->GetFileName();
  if (!folder || !*folder || !file || !*file) {
    book->SetPendingEpubPageCacheSave(false);
    return;
  }

  std::string path(folder);
  path.push_back('/');
  path += file;

  const Book::EpubCacheSaveParams &p = book->GetEpubCacheSaveParams();
  Save(book, path.c_str(),
       p.pixel_size, p.line_spacing, p.paragraph_spacing, p.paragraph_indent,
       p.orientation, p.margin_left, p.margin_right, p.margin_top,
       p.margin_bottom, p.regular_font.empty() ? NULL : p.regular_font.c_str(),
       closing);

  book->SetPendingEpubPageCacheSave(false);
}

struct StreamWriter::State {
  enum Stage { Pages, Search, Chapters, Documents, Anchors, Images, Links, Commit, Done };
  typedef std::unordered_map<std::string, u16> References;
  FILE *fp;
  Book *owner;
  std::string path, temporary;
  EpubPageCacheHeader header;
  Stage stage;
  u32 pages_written;
  size_t index, step_bytes;
  References::const_iterator reference;
  bool reference_ready, frozen, record_started;
  size_t record_offset;
  unsigned session, revision;
  size_t documents, anchors;
#ifdef DSLIBRIS_DEBUG
  u64 started_ms, active_ms, max_slice_ms;
  u64 idle_max_slice_ms, closing_max_slice_ms;
  unsigned slices;
  u64 last_slow_log_ms;
  unsigned slow_slices;
#endif

  State() : fp(NULL), owner(NULL), stage(Pages), pages_written(0), index(0),
            step_bytes(0), reference_ready(false), frozen(false), record_started(false),
            record_offset(0), session(0),
            revision(0), documents(0), anchors(0)
#ifdef DSLIBRIS_DEBUG
            , started_ms(osGetTime()), active_ms(0), max_slice_ms(0),
            idle_max_slice_ms(0), closing_max_slice_ms(0), slices(0),
            last_slow_log_ms(0), slow_slices(0)
#endif
  { memset(&header, 0, sizeof(header)); }

  bool Write(const void *data, size_t bytes) {
    if (bytes && fwrite(data, 1, bytes, fp) != bytes)
      return false;
    step_bytes += bytes;
    return true;
  }

  bool String(const std::string &value, size_t maximum, bool allow_empty) {
    const u16 size = (u16)std::min(value.size(), maximum);
    return (allow_empty || size != 0) && Write(&size, sizeof(size)) &&
           Write(value.data(), size);
  }

  // Leave the record prefix in place while its words span multiple idle
  // slices. Pages/search arrays stay borrowed from the immutable layout.
  bool Words(const u32 *data, size_t count, size_t max_bytes) {
    if (record_offset == count)
      return true;
    if (!data)
      return false;
    if (max_bytes && step_bytes >= max_bytes)
      return true;
    const size_t available = max_bytes ? max_bytes - step_bytes : 4096;
    if (available < sizeof(u32) && step_bytes)
      return true;
    size_t words = std::min(count - record_offset, (size_t)1024);
    // A budget below one word still makes progress, as with primitive fields.
    words = std::min(words, std::max((size_t)1, available / sizeof(u32)));
    if (!Write(data + record_offset, words * sizeof(u32)))
      return false;
    record_offset += words;
    return true;
  }

  bool PageRecord(Book *book, size_t max_bytes = 0) {
    Page *page = book->GetPage((int)pages_written);
    const int length = page ? page->GetLength() : 0;
    if (length < 0 || length > page_cache_limits::kPageMaxBytes)
      return false;
    if (!record_started) {
      const u16 size = (u16)length;
      if (!Write(&size, sizeof(size)))
        return false;
      record_started = true;
    }
    if (!Words(page ? page->GetBuffer() : NULL, length, max_bytes))
      return false;
    if (record_offset == (size_t)length) {
      ++pages_written;
      record_offset = 0;
      record_started = false;
    }
    return true;
  }

  void Freeze(Book *book) {
    header.page_count = book->GetPageCount();
    header.chapter_count = (u32)book->GetChapters().size();
    header.image_count = book->GetInlineImageCount();
    header.link_href_count = book->GetInlineLinkHrefCount();
    documents = book->GetChapterDocStartPages().size();
    anchors = book->GetChapterAnchorPages().size();
    session = book->GetOpenSessionId();
    revision = book->GetLayoutRevision();
    frozen = true;
  }

  bool Matches(Book *book) const {
    return owner == book && header.page_count == book->GetPageCount() &&
           header.chapter_count == book->GetChapters().size() &&
           documents == book->GetChapterDocStartPages().size() &&
           anchors == book->GetChapterAnchorPages().size() &&
           header.image_count == book->GetInlineImageCount() &&
           header.link_href_count == book->GetInlineLinkHrefCount() &&
           session == book->GetOpenSessionId() && revision == book->GetLayoutRevision();
  }

  void Advance() {
    stage = (Stage)((int)stage + 1);
    index = 0;
    reference_ready = false;
  }

  bool Publish() {
    struct stat info;
    const bool previous = stat(path.c_str(), &info) == 0;
    if ((!previous && errno != ENOENT) || (previous && !S_ISREG(info.st_mode)))
      return false;
    const std::string backup = path + ".bak";
    // Some SD backends cannot rename over an existing destination.
    if (previous) {
      if ((remove(backup.c_str()) != 0 && errno != ENOENT) ||
          rename(path.c_str(), backup.c_str()) != 0)
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
      if (previous)
        rename(backup.c_str(), path.c_str());
      return false;
    }
    temporary.clear();
    if (previous)
      remove(backup.c_str());
    return true;
  }

  bool Next(Book *book, size_t max_bytes) {
    switch (stage) {
    case Pages:
      if (pages_written < header.page_count)
        return PageRecord(book, max_bytes);
      Advance();
      return true;
    case Search: {
      if (index >= header.page_count) {
        Advance();
        return true;
      }
      Page *page = book->GetPage((int)index);
      const page_search::Events &events = page->GetSearchEvents();
      if (!record_started) {
        if (events.size() > 65535 ||
            !page_search::Valid(events, page->GetLength()))
          return false;
        const u16 count = (u16)events.size();
        if (!Write(&count, sizeof(count)))
          return false;
        record_started = true;
      }
      if (!Words(events.data(), events.size(), max_bytes))
        return false;
      if (record_offset == events.size()) {
        ++index;
        record_offset = 0;
        record_started = false;
      }
      return true;
    }
    case Chapters: {
      if (index >= header.chapter_count) {
        Advance();
        return true;
      }
      const ChapterEntry &chapter = book->GetChapters()[index++];
      return Write(&chapter.page, sizeof(chapter.page)) &&
             Write(&chapter.level, sizeof(chapter.level)) &&
             String(chapter.title, page_cache_limits::kChapterTitleMaxBytes,
                    true);
    }
    case Documents:
    case Anchors: {
      const References &map = stage == Documents
                                  ? book->GetChapterDocStartPages()
                                  : book->GetChapterAnchorPages();
      if (!reference_ready) {
        reference = map.begin();
        reference_ready = true;
      }
      if (reference == map.end()) {
        Advance();
        return true;
      }
      const References::value_type &entry = *reference++;
      if (entry.second >= header.page_count)
        return true;
      const bool ok =
          Write(&entry.second, sizeof(entry.second)) &&
          String(entry.first,
                 stage == Documents ? page_cache_limits::kPathMaxBytes
                                    : kPageCacheHrefMaxBytes,
                 true);
      if (ok) {
        if (stage == Documents)
          ++header.doc_start_count;
        else
          ++header.anchor_count;
      }
      return ok;
    }
    case Images:
    case Links: {
      const bool image = stage == Images;
      const u32 count = image ? header.image_count : header.link_href_count;
      if (index >= count) {
        Advance();
        return true;
      }
      const std::string *value =
          image ? book->GetInlineImagePath((u16)index)
                : book->GetInlineLinkHref((u16)(index + 1));
      ++index;
      return value && String(*value,
                             image ? page_cache_limits::kPathMaxBytes
                                   : kPageCacheHrefMaxBytes,
                             false);
    }
    case Commit: {
      bool ok = fseek(fp, 0, SEEK_SET) == 0 && Write(&header, sizeof(header));
      if (fflush(fp) != 0)
        ok = false;
      if (fclose(fp) != 0)
        ok = false;
      fp = NULL;
      if (!ok || !Publish())
        return false;
      stage = Done;
      return true;
    }
    case Done:
      return true;
    }
    return false;
  }

  void Record(Book *book, u64 begin, bool closing, StepResult result) {
#ifdef DSLIBRIS_DEBUG
    const u64 now = osGetTime();
    const u64 elapsed = now - begin;
    active_ms += elapsed;
    max_slice_ms = std::max(max_slice_ms, elapsed);
    if (closing)
      closing_max_slice_ms = std::max(closing_max_slice_ms, elapsed);
    else
      idle_max_slice_ms = std::max(idle_max_slice_ms, elapsed);
    ++slices;
    if (!closing && elapsed >= 100) {
      ++slow_slices;
      if (slow_slices == 1 || now - last_slow_log_ms >= 5000) {
        last_slow_log_ms = now;
        DBG_LOGF(book->GetStatusReporter(),
                 "EPUB cache save: slow slice ms=%llu next=%d page=%u/%u "
                 "bytes=%u part=%u",
                 (unsigned long long)elapsed, (int)stage,
                 (unsigned)pages_written, (unsigned)header.page_count,
                 (unsigned)step_bytes, (unsigned)record_offset);
      }
    }
    if (result != Pending) {
      DBG_LOGF(book->GetStatusReporter(),
               "EPUB cache save: done ok=%d ms=%llu pages=%u active_ms=%llu "
               "max_slice_ms=%llu slices=%u closing=%d idle_max_slice_ms=%llu "
               "closing_max_slice_ms=%llu",
               result == Complete ? 1 : 0,
               (unsigned long long)(now - started_ms),
               (unsigned)header.page_count, (unsigned long long)active_ms,
               (unsigned long long)max_slice_ms, slices, closing ? 1 : 0,
               (unsigned long long)idle_max_slice_ms,
               (unsigned long long)closing_max_slice_ms);
    }
#else
    (void)book;
    (void)begin;
    (void)closing;
    (void)result;
#endif
  }
};

StreamWriter::StreamWriter() : state_(NULL) {}
StreamWriter::~StreamWriter() { Abort(); }
bool StreamWriter::IsOpen() const { return state_ && state_->fp; }

bool StreamWriter::Begin(Book *book, const char *book_path, int pixel_size,
                         int line_spacing, int paragraph_spacing,
                         int paragraph_indent, int orientation, int margin_left,
                         int margin_right, int margin_top, int margin_bottom,
                         const char *regular_font) {
  Abort();
  if (!book || !book_path)
    return false;
#ifdef DSLIBRIS_DEBUG
  const u64 setup_begin = osGetTime();
#endif
  EnsureCacheDirs();
  state_ = new (std::nothrow) State;
  if (!state_)
    return false;
  State &s = *state_;
  s.owner = book;
  const page_cache_utils::PageCacheLayoutParams params =
      page_cache_utils::BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom, regular_font);
  s.path = BuildCachePath(book, book_path, params);
  s.temporary = s.path + ".tmp";
  if (s.path.empty() || !(s.fp = fopen(s.temporary.c_str(), "wb")) ||
      setvbuf(s.fp, NULL, _IOFBF, 65536) != 0) {
    Abort();
    return false;
  }
  const char *title_c = book->GetTitle();
  u16 title_size = 0;
  while (title_c && title_size < page_cache_limits::kTitleMaxBytes && title_c[title_size])
    ++title_size;
  s.header.magic = kEpubPageCacheMagic;
  s.header.version = kEpubPageCacheVersion;
  s.header.title_len = title_size;
  // Keep page_count zero until every section has been written successfully.
  if (!s.Write(&s.header, sizeof(s.header)) ||
      !s.Write(title_c, title_size)) {
    Abort();
    return false;
  }
  DBG_LOGF(book->GetStatusReporter(),
           "EPUB cache save: begin pages=%u buffer_bytes=65536 setup_ms=%llu",
           (unsigned)book->GetPageCount(),
           (unsigned long long)(osGetTime() - setup_begin));
  return true;
}

bool StreamWriter::FlushPages(Book *book, u16 from_page, bool closing) {
  if (!IsOpen() || !book || state_->owner != book ||
      state_->stage != State::Pages || from_page != state_->pages_written)
    return false;
  const u64 started = osGetTime();
  while (state_->pages_written < book->GetPageCount()) {
    if (open_cancel_poll::Poll(closing ? NULL : book, book->GetStatusReporter(),
                               "epub-stream-pages") || !state_->PageRecord(book))
      return false;
  }
  state_->Record(book, started, closing, Pending);
  return true;
}

StreamWriter::StepResult StreamWriter::Step(Book *book, size_t max_bytes,
                                            u32 max_ms, bool closing) {
  if (!state_ || !book || state_->owner != book) {
    Abort();
    return Failed;
  }
  if (state_->stage == State::Done)
    return Complete;
  if (!state_->fp)
    return Failed;
  if (book->GetPageCount() == 0) {
    Abort();
    return Failed;
  }
  const u64 begin = osGetTime();
  if (!state_->frozen)
    state_->Freeze(book);
  if (!state_->Matches(book)) {
    Abort();
    return Failed;
  }
  state_->step_bytes = 0;
  bool ok = true;
  unsigned records = 0;
  while (state_->stage != State::Done) {
    if (records && ((max_bytes && state_->step_bytes >= max_bytes) ||
                    (max_ms && osGetTime() - begin >= max_ms)))
      break;
    // Metadata records are at most 2053 bytes; do not begin one near the
    // end of a bounded slice. Primitive test budgets still advance one record.
    if (records && max_bytes &&
        max_bytes - state_->step_bytes <
            (state_->stage > State::Search ? (size_t)2053 : sizeof(u32)))
      break;
    if (open_cancel_poll::Poll(closing ? NULL : book, book->GetStatusReporter(),
                               "epub-cache-step") ||
        !state_->Next(book, max_bytes)) {
      ok = false;
      break;
    }
    ++records;
  }
  // stdio already has a bounded 16 KiB buffer. Let full buffers drain during
  // Write; Commit flushes/checks the final tail before publishing the cache.
  const StepResult result = !ok                            ? Failed
                            : state_->stage == State::Done ? Complete
                                                           : Pending;
  state_->Record(book, begin, closing, result);
  if (!ok)
    Abort();
  return result;
}

bool StreamWriter::Finalize(Book *book, bool closing) {
  if (!IsOpen() || !book || state_->pages_written == 0 ||
      state_->pages_written != book->GetPageCount()) {
    Abort();
    return false;
  }
  return Step(book, 0, 0, closing) == Complete;
}

void StreamWriter::Abort() {
  if (!state_)
    return;
  if (state_->fp)
    fclose(state_->fp);
  if (!state_->temporary.empty())
    remove(state_->temporary.c_str());
  delete state_;
  state_ = NULL;
}

#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *dir) {
  g_host_test_cache_dir = dir ? dir : "";
}
#endif

} // namespace epub_page_cache
