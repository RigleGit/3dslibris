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
#include "formats/common/page_cache_field_limits.h"
#include "formats/common/page_cache_utils.h"
#include "shared/path_constants.h"
#include "shared/open_cancel_poll.h"
#include <3ds.h>
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
static const u16 kEpubPageCacheVersion = 10;
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

struct EpubCacheLayoutParams {
  long long file_size;
  long long file_mtime;
  int pixel_size;
  int line_spacing;
  int paragraph_spacing;
  int paragraph_indent;
  int orientation;
  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;
  std::string regular_font;
};

static EpubCacheLayoutParams
BuildLayoutParams(const char *book_path, int pixel_size, int line_spacing,
                  int paragraph_spacing, int paragraph_indent, int orientation,
                  int margin_left, int margin_right, int margin_top,
                  int margin_bottom, const char *regular_font) {
  EpubCacheLayoutParams params = {};
  if (!book_path)
    return params;

  struct stat st;
  if (stat(book_path, &st) == 0) {
    params.file_size = (long long)st.st_size;
    params.file_mtime = (long long)st.st_mtime;
  }

  params.pixel_size = pixel_size;
  params.line_spacing = line_spacing;
  params.paragraph_spacing = paragraph_spacing;
  params.paragraph_indent = paragraph_indent;
  params.orientation = orientation;
  params.margin_left = margin_left;
  params.margin_right = margin_right;
  params.margin_top = margin_top;
  params.margin_bottom = margin_bottom;
  params.regular_font = regular_font ? regular_font : "";
  return params;
}

static std::string BuildCachePath(Book *book, const char *book_path,
                                  const EpubCacheLayoutParams &params) {
  if (!book_path)
    return std::string();
  page_cache_utils::PageCacheLayoutParams layout_params;
  layout_params.file_size = params.file_size;
  layout_params.file_mtime = params.file_mtime;
  layout_params.pixel_size = params.pixel_size;
  layout_params.line_spacing = params.line_spacing;
  layout_params.paragraph_spacing = params.paragraph_spacing;
  layout_params.paragraph_indent = params.paragraph_indent;
  layout_params.orientation = params.orientation;
  layout_params.margin_left = params.margin_left;
  layout_params.margin_right = params.margin_right;
  layout_params.margin_top = params.margin_top;
  layout_params.margin_bottom = params.margin_bottom;
  layout_params.regular_font = params.regular_font;
  layout_params.variant_token = "pub3:";
  layout_params.variant_token +=
      book && book->GetPublisherTextIndentEnabled() ? "i1" : "i0";
  layout_params.variant_token +=
      book && book->GetPublisherBlockMarginsEnabled() ? ":m1" : ":m0";
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

static std::vector<page_cache_utils::CachedChapter>
CollectChapters(const std::vector<ChapterEntry> &chapters) {
  std::vector<page_cache_utils::CachedChapter> cached;
  cached.reserve(chapters.size());
  for (size_t i = 0; i < chapters.size(); i++) {
    const ChapterEntry &chapter = chapters[i];
    cached.push_back(page_cache_utils::CachedChapter(chapter.page, chapter.level,
                                                     chapter.title));
  }
  return cached;
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

  EpubCacheLayoutParams params = BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom,
      regular_font);

  std::string cache_path = BuildCachePath(book, book_path, params);
  if (cache_path.empty())
    return false;

#ifdef DSLIBRIS_DEBUG
  // Per-step timing: large books show ~700us per cached page; this breakdown
  // identifies which section dominates (page deserialize vs anchor map vs
  // inline-image registration) so optimization targets the right hot loop.
  const u64 t_load_begin = osGetTime();
  u64 t_after_hdr = t_load_begin;
  u64 t_after_pages = t_load_begin;
  u64 t_after_chapters = t_load_begin;
  u64 t_after_doc_starts = t_load_begin;
  u64 t_after_anchors = t_load_begin;
  u64 t_after_images = t_load_begin;
  u64 t_after_hrefs = t_load_begin;
#endif

  // Bulk-read whole file into memory; on a 7706-page book the per-record
  // fread() path was costing ~10s here (1.3ms per page) because each page
  // = 2 stdio reads (length + body) plus locking. One fread() replaces ~15k.
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
  rewind(fp);
  std::vector<uint8_t> file_buf((size_t)file_size_long);
  if (fread(file_buf.data(), 1, file_buf.size(), fp) != file_buf.size()) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
  fclose(fp);
  fp = NULL;

  page_cache_utils::BufReader reader(file_buf.data(), file_buf.size());

  EpubPageCacheHeader hdr;
  if (!reader.ReadRaw(&hdr, sizeof(hdr))) {
    remove(cache_path.c_str());
    return false;
  }
  if (hdr.magic != kEpubPageCacheMagic ||
      hdr.version != kEpubPageCacheVersion || hdr.page_count == 0 ||
      hdr.page_count > 50000 || hdr.chapter_count > 4000 ||
      hdr.title_len > 1000 || hdr.doc_start_count > 4000 ||
      hdr.anchor_count > 8192 ||
      hdr.image_count > 65535 || hdr.link_href_count > 65535) {
    remove(cache_path.c_str());
    return false;
  }

  std::string title;
  if (!page_cache_utils::ReadRawString(&reader, hdr.title_len, &title)) {
    remove(cache_path.c_str());
    return false;
  }
#ifdef DSLIBRIS_DEBUG
  t_after_hdr = osGetTime();
#endif

  bool ok = true;
  std::vector<page_cache_utils::CachedPage> pages;
  ok = page_cache_utils::ReadPages(&reader, hdr.page_count,
                                   page_cache_limits::kPageMaxBytes, &pages);
  if (ok)
    AppendPages(book, pages);
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
      if (!reader.ReadRaw(&doc_page, sizeof(doc_page))) {
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
      if (!reader.ReadRaw(&anchor_page, sizeof(anchor_page))) {
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
  IStatusReporter *r = book ? book->GetStatusReporter() : nullptr;
  if (!book || !book_path || book->GetPageCount() == 0)
    return;

  DBG_LOGF(r, "EPUB cache save: begin pages=%d closing=%d", (int)book->GetPageCount(), (int)closing);
#ifdef DSLIBRIS_DEBUG
  u64 t0_save = osGetTime();
#endif
  EnsureCacheDirs();

  EpubCacheLayoutParams params = BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom,
      regular_font);

  std::string cache_path = BuildCachePath(book, book_path, params);
  if (cache_path.empty())
    return;

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  title = page_cache_utils::ClampString(title, page_cache_limits::kTitleMaxBytes);
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  const std::unordered_map<std::string, u16> &doc_starts =
      book->GetChapterDocStartPages();
  const std::unordered_map<std::string, u16> &anchors =
      book->GetChapterAnchorPages();
  const std::vector<page_cache_utils::CachedChapter> cached_chapters =
      CollectChapters(chapters);
  const u16 page_count = book->GetPageCount();

  EpubPageCacheHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kEpubPageCacheMagic;
  hdr.version = kEpubPageCacheVersion;
  hdr.title_len = (u16)title.size();
  hdr.page_count = (u32)page_count;
  hdr.chapter_count = (u32)cached_chapters.size();
  hdr.doc_start_count = (u32)doc_starts.size();
  hdr.anchor_count = (u32)anchors.size();
  hdr.image_count = book->GetInlineImageCount();
  hdr.link_href_count = book->GetInlineLinkHrefCount();

  DBG_LOGF(r, "EPUB cache save: fopen path=%s", cache_path.c_str());
  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp) {
    DBG_LOGF(r, "EPUB cache save: fopen failed");
    return;
  }
  setvbuf(fp, NULL, _IOFBF, 262144);

  bool ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  DBG_LOGF(r, "EPUB cache save: write-hdr ok=%d", (int)ok);
  if (ok)
    ok = page_cache_utils::WriteRawString(fp, title);

  DBG_LOGF(r, "EPUB cache save: write-pages begin count=%d", (int)page_count);
  for (u16 i = 0; ok && i < page_count; i++) {
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    const u16 len16 = (u16)(length > 0 ? length : 0);
    if (len16 > page_cache_limits::kPageMaxBytes) { ok = false; break; }
    if (fwrite(&len16, 1, sizeof(len16), fp) != sizeof(len16)) { ok = false; break; }
    if (len16 > 0) {
      const u32 *buffer = page->GetBuffer();
      if (!buffer) { ok = false; break; }
      if (fwrite(buffer, 1, (size_t)len16 * sizeof(u32), fp) != (size_t)len16 * sizeof(u32)) { ok = false; break; }
    }
  }
  DBG_LOGF(r, "EPUB cache save: write-pages done ok=%d", (int)ok);

  if (ok)
    ok = page_cache_utils::WriteChapters(fp, cached_chapters,
                                         page_cache_limits::kChapterTitleMaxBytes);
  DBG_LOGF(r, "EPUB cache save: write-chapters done ok=%d", (int)ok);

  if (ok) {
    for (auto &kv : doc_starts) {
      if (open_cancel_poll::Poll(closing ? nullptr : book, book->GetStatusReporter(),
                                  "epub-cache-docstarts")) {
        ok = false;
        break;
      }
      u16 doc_page = kv.second;
      if (fwrite(&doc_page, 1, sizeof(doc_page), fp) != sizeof(doc_page)) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp, kv.first, page_cache_limits::kPathMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    for (auto &kv : anchors) {
      if (open_cancel_poll::Poll(closing ? nullptr : book, book->GetStatusReporter(),
                                  "epub-cache-anchors")) {
        ok = false;
        break;
      }
      u16 anchor_page = kv.second;
      if (fwrite(&anchor_page, 1, sizeof(anchor_page), fp) !=
          sizeof(anchor_page)) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp, kv.first, kPageCacheHrefMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    u32 img_count = book->GetInlineImageCount();
    for (u32 i = 0; i < img_count; i++) {
      if (open_cancel_poll::Poll(closing ? nullptr : book, book->GetStatusReporter(),
                                  "epub-cache-images")) {
        ok = false;
        break;
      }
      const std::string *imgpath = book->GetInlineImagePath((u16)i);
      if (!imgpath || imgpath->empty()) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp, *imgpath, page_cache_limits::kPathMaxBytes, false)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    u32 link_count = book->GetInlineLinkHrefCount();
    for (u32 i = 1; i <= link_count; i++) {
      if (open_cancel_poll::Poll(closing ? nullptr : book, book->GetStatusReporter(),
                                  "epub-cache-links")) {
        ok = false;
        break;
      }
      const std::string *href = book->GetInlineLinkHref((u16)i);
      if (!href || href->empty()) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp, *href, kPageCacheHrefMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  fclose(fp);
  if (!ok)
    remove(cache_path.c_str());
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(r, "EPUB cache save: done ok=%d ms=%u pages=%d",
           (int)ok, (unsigned)(osGetTime() - t0_save), (int)book->GetPageCount());
#endif
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

StreamWriter::StreamWriter() : fp_(NULL), cache_path_(), pages_written_(0) {}

StreamWriter::~StreamWriter() { Abort(); }

bool StreamWriter::Begin(Book *book, const char *book_path, int pixel_size,
                         int line_spacing, int paragraph_spacing,
                         int paragraph_indent, int orientation, int margin_left,
                         int margin_right, int margin_top, int margin_bottom,
                         const char *regular_font) {
  if (fp_)
    Abort();
  if (!book || !book_path)
    return false;

  EnsureCacheDirs();

  EpubCacheLayoutParams params = BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom,
      regular_font);

  cache_path_ = BuildCachePath(book, book_path, params);
  if (cache_path_.empty())
    return false;

  fp_ = fopen(cache_path_.c_str(), "wb");
  if (!fp_)
    return false;
  // Use a large write buffer to amortise per-page fwrite() calls into
  // block-sized I/O on the SD card. Without this, each ctrulib FSFILE_Write
  // IPC call adds ~1s of overhead per 32 KB flush, making large books
  // (e.g. 9000+ pages) take many minutes to save.
  setvbuf(fp_, NULL, _IOFBF, 262144);

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  title = page_cache_utils::ClampString(title, page_cache_limits::kTitleMaxBytes);

  EpubPageCacheHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kEpubPageCacheMagic;
  hdr.version = kEpubPageCacheVersion;
  hdr.title_len = (u16)title.size();

  bool ok = fwrite(&hdr, 1, sizeof(hdr), fp_) == sizeof(hdr);
  if (ok)
    ok = page_cache_utils::WriteRawString(fp_, title);

  if (!ok) {
    fclose(fp_);
    fp_ = NULL;
    remove(cache_path_.c_str());
    return false;
  }

  pages_written_ = 0;
  return true;
}

bool StreamWriter::FlushPages(Book *book, u16 from_page) {
  if (!fp_ || !book)
    return false;

  const u16 total = book->GetPageCount();
  for (u16 i = from_page; i < total; i++) {
    if (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                               "epub-stream-flush"))
      return false;
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    const u16 len16 = (u16)(length > 0 ? length : 0);
    if (len16 > page_cache_limits::kPageMaxBytes)
      return false;
    if (fwrite(&len16, 1, sizeof(len16), fp_) != sizeof(len16))
      return false;
    if (len16 > 0) {
      const u32 *buffer = page->GetBuffer();
      if (!buffer)
        return false;
      const size_t byte_count = (size_t)len16 * sizeof(u32);
      if (fwrite(buffer, 1, byte_count, fp_) != byte_count)
        return false;
    }
    pages_written_++;
  }
  return true;
}

bool StreamWriter::Finalize(Book *book) {
  if (!fp_ || !book || pages_written_ == 0)
    return false;

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  const std::vector<page_cache_utils::CachedChapter> cached_chapters =
      CollectChapters(chapters);
  const std::unordered_map<std::string, u16> &doc_starts =
      book->GetChapterDocStartPages();
  const std::unordered_map<std::string, u16> &anchors =
      book->GetChapterAnchorPages();

  bool ok = page_cache_utils::WriteChapters(fp_, cached_chapters,
                                            page_cache_limits::kChapterTitleMaxBytes);

  if (ok) {
    for (auto &kv : doc_starts) {
      if (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                 "epub-stream-docstarts")) {
        ok = false;
        break;
      }
      u16 doc_page = kv.second;
      if (fwrite(&doc_page, 1, sizeof(doc_page), fp_) != sizeof(doc_page)) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp_, kv.first, page_cache_limits::kPathMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    for (auto &kv : anchors) {
      if (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                 "epub-stream-anchors")) {
        ok = false;
        break;
      }
      u16 anchor_page = kv.second;
      if (fwrite(&anchor_page, 1, sizeof(anchor_page), fp_) !=
          sizeof(anchor_page)) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp_, kv.first, kPageCacheHrefMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    u32 img_count = book->GetInlineImageCount();
    for (u32 i = 0; i < img_count; i++) {
      if (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                 "epub-stream-images")) {
        ok = false;
        break;
      }
      const std::string *imgpath = book->GetInlineImagePath((u16)i);
      if (!imgpath || imgpath->empty()) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp_, *imgpath, page_cache_limits::kPathMaxBytes, false)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    u32 link_count = book->GetInlineLinkHrefCount();
    for (u32 i = 1; i <= link_count; i++) {
      if (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                 "epub-stream-links")) {
        ok = false;
        break;
      }
      const std::string *href = book->GetInlineLinkHref((u16)i);
      if (!href || href->empty()) {
        ok = false;
        break;
      }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              fp_, *href, kPageCacheHrefMaxBytes, true)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    const char *title_c = book->GetTitle();
    std::string title = title_c ? title_c : "";
    title = page_cache_utils::ClampString(title, page_cache_limits::kTitleMaxBytes);

    EpubPageCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kEpubPageCacheMagic;
    hdr.version = kEpubPageCacheVersion;
    hdr.title_len = (u16)title.size();
    hdr.page_count = pages_written_;
    hdr.chapter_count = (u32)cached_chapters.size();
    hdr.doc_start_count = (u32)doc_starts.size();
    hdr.anchor_count = (u32)anchors.size();
    hdr.image_count = book->GetInlineImageCount();
    hdr.link_href_count = book->GetInlineLinkHrefCount();

    if (fseek(fp_, 0, SEEK_SET) != 0)
      ok = false;
    else
      ok = fwrite(&hdr, 1, sizeof(hdr), fp_) == sizeof(hdr);
  }

  fclose(fp_);
  fp_ = NULL;
  if (!ok) {
    remove(cache_path_.c_str());
    return false;
  }
  cache_path_.clear();
  return true;
}

void StreamWriter::Abort() {
  if (fp_) {
    fclose(fp_);
    fp_ = NULL;
    if (!cache_path_.empty()) {
      remove(cache_path_.c_str());
      cache_path_.clear();
    }
  }
  pages_written_ = 0;
}

#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *dir) {
  g_host_test_cache_dir = dir ? dir : "";
}
#endif

} // namespace epub_page_cache
