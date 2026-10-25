/*
    3dslibris - epub_page_cache.cpp
    EPUB persistent page cache serialization.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_page_cache.h"

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
static const u16 kEpubPageCacheVersion = 22;
static const u16 kPageCacheHrefMaxBytes = 2048;
// Bulk-loading cache files duplicates memory (file buffer + decoded pages).
// Stream larger caches to avoid retaining a second full copy in memory.
static const long kMaxBulkCacheLoadBytes = 16L * 1024L * 1024L;

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
  layout_params.variant_token = "pub3:";
  layout_params.variant_token +=
      book && book->GetPublisherTextIndentEnabled() ? "i1" : "i0";
  layout_params.variant_token +=
      book && book->GetPublisherBlockMarginsEnabled() ? ":m1" : ":m0";
  layout_params.variant_token +=
      book && book->GetPublisherHorizontalMarginsEnabled() ? ":h1" : ":h0";
  if (book && !book->GetEbookImagesEnabled())
    layout_params.variant_token += ":no-images";
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

static void AppendPages(Book *book,
                        const std::vector<page_cache_utils::CachedPage> &pages) {
  if (!book)
    return;

  book->ReservePageCapacity(pages.size());

  for (size_t i = 0; i < pages.size(); i++) {
    page_cache_utils::CachedPage &cached_page =
        const_cast<page_cache_utils::CachedPage &>(pages[i]);
    Page *page = book->AppendPage();
    page_buffer_utils::OwnedPageBuffer owned =
        page_buffer_utils::AdoptPageBuffer(&cached_page);
    page->AdoptBuffer(&owned);
  }
}

static bool AppendPagesFromFile(FILE *fp, Book *book, u32 count,
                                u16 max_page_codepoints) {
  if (!fp || !book)
    return false;

  book->ReservePageCapacity(count);
  for (u32 i = 0; i < count; i++) {
    u16 length = 0;
    if (fread(&length, 1, sizeof(length), fp) != sizeof(length) ||
        length > max_page_codepoints) {
      return false;
    }

    page_cache_utils::CachedPage cached_page(length);
    const size_t byte_count = (size_t)length * sizeof(u32);
    if (length > 0 &&
        fread(cached_page.data(), 1, byte_count, fp) != byte_count) {
      return false;
    }

    Page *page = book->AppendPage();
    page_buffer_utils::OwnedPageBuffer owned =
        page_buffer_utils::AdoptPageBuffer(&cached_page);
    page->AdoptBuffer(&owned);
  }
  return true;
}


static void AppendChapters(
    Book *book, const std::vector<page_cache_utils::CachedChapter> &chapters) {
  if (!book)
    return;

  for (size_t i = 0; i < chapters.size(); i++) {
    const page_cache_utils::CachedChapter &chapter = chapters[i];
    if (chapter.page < book->GetPageCount())
      book->AddChapter(chapter.page, chapter.title, chapter.level);
  }
}

} // namespace

namespace epub_page_cache {

bool TryLoad(Book *book, const char *book_path, int pixel_size,
             int line_spacing, int paragraph_spacing, int paragraph_indent,
             int orientation, int margin_left, int margin_right, int margin_top,
             int margin_bottom, const char *regular_font) {
  if (!book || !book_path)
    return false;

  EnsureCacheDirs();

  page_cache_utils::PageCacheLayoutParams params =
      page_cache_utils::BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom,
      regular_font);

  std::string cache_path = BuildCachePath(book, book_path, params);
  if (cache_path.empty())
    return false;

#ifdef DSLIBRIS_DEBUG
  // The pages phase includes the associated search data in both readers.
  const u64 t_load_begin = osGetTime();
  u64 t_after_hdr = t_load_begin;
  u64 t_after_pages = t_load_begin;
  u64 t_after_chapters = t_load_begin;
  u64 t_after_doc_starts = t_load_begin;
  u64 t_after_anchors = t_load_begin;
  u64 t_after_images = t_load_begin;
  u64 t_after_hrefs = t_load_begin;
#endif

  // Bulk-read smaller caches; larger ones stay on the bounded-memory path.
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  long file_size_long = ftell(fp);
  if (file_size_long <= (long)sizeof(EpubPageCacheHeader)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
  const bool use_stream_load = (file_size_long > kMaxBulkCacheLoadBytes);
  rewind(fp);

  EpubPageCacheHeader hdr;
  if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr) || !IsValidHeader(hdr)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }

  std::string title;
  if (!page_cache_utils::ReadRawString(fp, hdr.title_len, &title)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
#ifdef DSLIBRIS_DEBUG
  t_after_hdr = osGetTime();
#endif

  bool ok = true;
  if (use_stream_load) {
#ifdef DSLIBRIS_DEBUG
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(),
               "EPUB cache load: stream large cache size=%ld path=%s",
               file_size_long, cache_path.c_str());
    }
#endif
    ok = AppendPagesFromFile(fp, book, hdr.page_count,
                             page_cache_limits::kPageMaxBytes);
    if (ok) ok = book_search_cache::Read(fp, book);
#ifdef DSLIBRIS_DEBUG
    t_after_pages = osGetTime();
#endif
  } else {
    std::vector<uint8_t> file_buf((size_t)file_size_long);
    memcpy(file_buf.data(), &hdr, sizeof(hdr));
    if (hdr.title_len > 0)
      memcpy(file_buf.data() + sizeof(hdr), title.data(), hdr.title_len);
    const size_t remaining =
        file_buf.size() - (sizeof(hdr) + (size_t)hdr.title_len);
    if (remaining > 0 &&
        fread(file_buf.data() + sizeof(hdr) + hdr.title_len, 1, remaining,
              fp) != remaining) {
      fclose(fp);
      remove(cache_path.c_str());
      return false;
    }

    page_cache_utils::BufReader reader(file_buf.data(), file_buf.size());
    EpubPageCacheHeader skip_hdr;
    std::string skip_title;
    if (!reader.ReadRaw(&skip_hdr, sizeof(skip_hdr)) ||
        !page_cache_utils::ReadRawString(&reader, skip_hdr.title_len,
                                         &skip_title)) {
      fclose(fp);
      remove(cache_path.c_str());
      return false;
    }

    std::vector<page_cache_utils::CachedPage> pages;
    ok = page_cache_utils::ReadPages(&reader, hdr.page_count,
                                     page_cache_limits::kPageMaxBytes, &pages);
    if (ok)
      AppendPages(book, pages);
    if (ok) ok = book_search_cache::Read(&reader, book);
#ifdef DSLIBRIS_DEBUG
    t_after_pages = osGetTime();
#endif

    if (ok) {
      std::vector<page_cache_utils::CachedChapter> chapters;
      ok = page_cache_utils::ReadChapters(&reader, hdr.chapter_count,
                                          page_cache_limits::kChapterTitleMaxBytes,
                                          &chapters);
      if (ok)
        AppendChapters(book, chapters);
    }
#ifdef DSLIBRIS_DEBUG
    t_after_chapters = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.doc_start_count; i++) {
        u16 doc_page = 0;
        if (!reader.ReadRaw(&doc_page, sizeof(doc_page)) ||
            doc_page >= hdr.page_count) {
          ok = false;
          break;
        }
        std::string docpath;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                &reader, page_cache_limits::kPathMaxBytes, true, &docpath)) {
          ok = false;
          break;
        }
        book->SetChapterDocStartPage(docpath, doc_page);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_doc_starts = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.anchor_count; i++) {
        u16 anchor_page = 0;
        if (!reader.ReadRaw(&anchor_page, sizeof(anchor_page)) ||
            anchor_page >= hdr.page_count) {
          ok = false;
          break;
        }
        std::string href;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                &reader, kPageCacheHrefMaxBytes, true, &href)) {
          ok = false;
          break;
        }
        book->SetChapterAnchorPage(href, anchor_page);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_anchors = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.image_count; i++) {
        std::string imgpath;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                &reader, page_cache_limits::kPathMaxBytes, false, &imgpath)) {
          ok = false;
          break;
        }
        book->RegisterInlineImage(imgpath);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_images = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.link_href_count; i++) {
        std::string href;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                &reader, kPageCacheHrefMaxBytes, true, &href)) {
          ok = false;
          break;
        }
        if (book->RegisterInlineLinkHref(href) == 0) {
          ok = false;
          break;
        }
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_hrefs = osGetTime();
#endif
    fclose(fp);
    fp = NULL;
  }
  if (use_stream_load) {
    std::vector<page_cache_utils::CachedChapter> chapters;
    if (ok)
      ok = page_cache_utils::ReadChapters(fp, hdr.chapter_count,
                                          page_cache_limits::kChapterTitleMaxBytes,
                                          &chapters);
    if (ok)
      AppendChapters(book, chapters);
#ifdef DSLIBRIS_DEBUG
    t_after_chapters = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.doc_start_count; i++) {
        u16 doc_page = 0;
        if (fread(&doc_page, 1, sizeof(doc_page), fp) != sizeof(doc_page) ||
            doc_page >= hdr.page_count) {
          ok = false;
          break;
        }
        std::string docpath;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                fp, page_cache_limits::kPathMaxBytes, true, &docpath)) {
          ok = false;
          break;
        }
        book->SetChapterDocStartPage(docpath, doc_page);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_doc_starts = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.anchor_count; i++) {
        u16 anchor_page = 0;
        if (fread(&anchor_page, 1, sizeof(anchor_page), fp) !=
            sizeof(anchor_page) || anchor_page >= hdr.page_count) {
          ok = false;
          break;
        }
        std::string href;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                fp, kPageCacheHrefMaxBytes, true, &href)) {
          ok = false;
          break;
        }
        book->SetChapterAnchorPage(href, anchor_page);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_anchors = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.image_count; i++) {
        std::string imgpath;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                fp, page_cache_limits::kPathMaxBytes, false, &imgpath)) {
          ok = false;
          break;
        }
        book->RegisterInlineImage(imgpath);
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_images = osGetTime();
#endif

    if (ok) {
      for (u32 i = 0; i < hdr.link_href_count; i++) {
        std::string href;
        if (!page_cache_utils::ReadLengthPrefixedString16(
                fp, kPageCacheHrefMaxBytes, true, &href)) {
          ok = false;
          break;
        }
        if (book->RegisterInlineLinkHref(href) == 0) {
          ok = false;
          break;
        }
      }
    }
#ifdef DSLIBRIS_DEBUG
    t_after_hrefs = osGetTime();
#endif
    fclose(fp);
    fp = NULL;
  }
#ifdef DSLIBRIS_DEBUG
  if (book->GetStatusReporter()) {
    DBG_LOGF(book->GetStatusReporter(),
             "EPUB cache load: hdr=%llums pages=%llums(%u) chapters=%llums(%u) "
             "doc_starts=%llums(%u) anchors=%llums(%u) images=%llums(%u) "
             "hrefs=%llums(%u) total=%llums",
             (unsigned long long)(t_after_hdr - t_load_begin),
             (unsigned long long)(t_after_pages - t_after_hdr),
             (unsigned)hdr.page_count,
             (unsigned long long)(t_after_chapters - t_after_pages),
             (unsigned)hdr.chapter_count,
             (unsigned long long)(t_after_doc_starts - t_after_chapters),
             (unsigned)hdr.doc_start_count,
             (unsigned long long)(t_after_anchors - t_after_doc_starts),
             (unsigned)hdr.anchor_count,
             (unsigned long long)(t_after_images - t_after_anchors),
             (unsigned)hdr.image_count,
             (unsigned long long)(t_after_hrefs - t_after_images),
             (unsigned)hdr.link_href_count,
             (unsigned long long)(osGetTime() - t_load_begin));
  }
#endif
  if (!ok) {
    if (fp)
      fclose(fp);
    book->Close();
    remove(cache_path.c_str());
    return false;
  }

  if (!title.empty())
    book->SetTitle(title.c_str());

  return true;
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
  bool reference_ready, frozen;
  unsigned session, revision;
  size_t documents, anchors;
#ifdef DSLIBRIS_DEBUG
  u64 started_ms, active_ms, max_slice_ms;
  u64 idle_max_slice_ms, closing_max_slice_ms;
  unsigned slices;
#endif

  State() : fp(NULL), owner(NULL), stage(Pages), pages_written(0), index(0),
            step_bytes(0), reference_ready(false), frozen(false), session(0),
            revision(0), documents(0), anchors(0)
#ifdef DSLIBRIS_DEBUG
            , started_ms(osGetTime()), active_ms(0), max_slice_ms(0),
            idle_max_slice_ms(0), closing_max_slice_ms(0), slices(0)
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

  bool PageRecord(Book *book) {
    Page *page = book->GetPage((int)pages_written);
    const int length = page ? page->GetLength() : 0;
    if (length < 0 || length > page_cache_limits::kPageMaxBytes)
      return false;
    const u16 size = (u16)length;
    if (!Write(&size, sizeof(size)) ||
        (size && (!page->GetBuffer() ||
                  !Write(page->GetBuffer(), size * sizeof(u32)))))
      return false;
    ++pages_written;
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

  bool Next(Book *book) {
    switch (stage) {
    case Pages:
      if (pages_written < header.page_count)
        return PageRecord(book);
      Advance();
      return true;
    case Search: {
      if (index >= header.page_count) { Advance(); return true; }
      Page *page = book->GetPage((int)index++);
      const page_search::Events &events = page->GetSearchEvents();
      if (events.size() > 65535 || !page_search::Valid(events, page->GetLength()))
        return false;
      const u16 count = (u16)events.size();
      return Write(&count, sizeof(count)) && Write(events.data(), count * sizeof(u32));
    }
    case Chapters: {
      if (index >= header.chapter_count) { Advance(); return true; }
      const ChapterEntry &chapter = book->GetChapters()[index++];
      return Write(&chapter.page, sizeof(chapter.page)) &&
             Write(&chapter.level, sizeof(chapter.level)) &&
             String(chapter.title, page_cache_limits::kChapterTitleMaxBytes, true);
    }
    case Documents:
    case Anchors: {
      const References &map = stage == Documents ? book->GetChapterDocStartPages()
                                                : book->GetChapterAnchorPages();
      if (!reference_ready) { reference = map.begin(); reference_ready = true; }
      if (reference == map.end()) { Advance(); return true; }
      const References::value_type &entry = *reference++;
      if (entry.second >= header.page_count)
        return true;
      const bool ok = Write(&entry.second, sizeof(entry.second)) &&
          String(entry.first, stage == Documents ? page_cache_limits::kPathMaxBytes
                                                : kPageCacheHrefMaxBytes, true);
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
      if (index >= count) { Advance(); return true; }
      const std::string *value = image ? book->GetInlineImagePath((u16)index)
                                      : book->GetInlineLinkHref((u16)(index + 1));
      ++index;
      return value && String(*value, image ? page_cache_limits::kPathMaxBytes
                                           : kPageCacheHrefMaxBytes, false);
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
    const u64 elapsed = now - (slices ? begin : started_ms);
    active_ms += elapsed;
    max_slice_ms = std::max(max_slice_ms, elapsed);
    if (closing)
      closing_max_slice_ms = std::max(closing_max_slice_ms, elapsed);
    else
      idle_max_slice_ms = std::max(idle_max_slice_ms, elapsed);
    ++slices;
    if (result != Pending) {
      DBG_LOGF(book->GetStatusReporter(),
               "EPUB cache save: done ok=%d ms=%llu pages=%u active_ms=%llu max_slice_ms=%llu slices=%u closing=%d idle_max_slice_ms=%llu closing_max_slice_ms=%llu",
               result == Complete ? 1 : 0, (unsigned long long)(now - started_ms),
               (unsigned)header.page_count, (unsigned long long)active_ms,
               (unsigned long long)max_slice_ms, slices, closing ? 1 : 0,
               (unsigned long long)idle_max_slice_ms,
               (unsigned long long)closing_max_slice_ms);
    }
#else
    (void)book; (void)begin; (void)closing; (void)result;
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
      setvbuf(s.fp, NULL, _IOFBF, 16384) != 0) {
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
  DBG_LOGF(book->GetStatusReporter(), "EPUB cache save: begin pages=%u buffer_bytes=16384",
           (unsigned)book->GetPageCount());
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
    if (open_cancel_poll::Poll(closing ? NULL : book, book->GetStatusReporter(),
                               "epub-cache-step") || !state_->Next(book)) {
      ok = false;
      break;
    }
    ++records;
  }
  // Flush this bounded batch here; do not defer a large hidden write to Close.
  if (ok && state_->fp && fflush(state_->fp) != 0)
    ok = false;
  const StepResult result = !ok ? Failed : state_->stage == State::Done ? Complete : Pending;
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
