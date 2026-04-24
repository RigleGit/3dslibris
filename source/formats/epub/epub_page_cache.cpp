/*
    3dslibris - epub_page_cache.cpp
    EPUB persistent page cache serialization.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_page_cache.h"

#include "book/book.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "debug_log.h"
#include "formats/common/page_cache_utils.h"
#include "path_utils.h"
#include "shared/open_cancel_poll.h"
#include <3ds.h>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace {

static const char *kEpubCacheBaseDir = paths::kCacheBaseDir;
static const char *kEpubCacheDir = paths::kEpubCacheDir;
static const u32 kEpubPageCacheMagic = 0x45504347U;
static const u16 kEpubPageCacheVersion = 3;
static const u16 kPageCacheTitleMaxBytes = 1000;
static const u16 kPageCachePageMaxBytes = 4096;
static const u16 kPageCacheChapterTitleMaxBytes = 2048;
static const u16 kPageCachePathMaxBytes = 2048;

struct EpubPageCacheHeader {
  u32 magic;
  u16 version;
  u16 title_len;
  u32 page_count;
  u32 chapter_count;
  u32 doc_start_count;
  u32 image_count;
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
  EpubCacheLayoutParams params;
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

static std::string BuildCachePath(const char *book_path,
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
  return page_cache_utils::BuildPageCachePath(
      kEpubCacheDir, ".epc", book_path, layout_params);
}

static void EnsureCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kEpubCacheBaseDir, 0777);
  mkdir(kEpubCacheDir, 0777);
  initialized = true;
}

static std::vector<page_cache_utils::CachedPage>
CollectPages(Book *book, bool closing) {
  std::vector<page_cache_utils::CachedPage> pages;
  if (!book)
    return pages;

  const u16 page_count = book->GetPageCount();
  pages.reserve(page_count);
  for (u16 i = 0; i < page_count; i++) {
    if (open_cancel_poll::Poll(closing ? nullptr : book, book->GetStatusReporter(),
                               "epub-cache-collect-pages")) {
      return std::vector<page_cache_utils::CachedPage>();
    }
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    page_cache_utils::CachedPage cached_page;
    if (page && length > 0) {
      const u32 *buffer = page->GetBuffer();
      if (!buffer)
        return std::vector<page_cache_utils::CachedPage>();
      cached_page.assign(buffer, buffer + length);
    }
    pages.push_back(cached_page);
  }
  return pages;
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

  std::string cache_path = BuildCachePath(book_path, params);
  if (cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;
  // Batch SD card reads: 809+ small fread()s become ~handful of 32KB reads.
  setvbuf(fp, NULL, _IOFBF, 32768);

  EpubPageCacheHeader hdr;
  if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
  if (hdr.magic != kEpubPageCacheMagic ||
      hdr.version != kEpubPageCacheVersion || hdr.page_count == 0 ||
      hdr.page_count > 50000 || hdr.chapter_count > 4000 ||
      hdr.title_len > 1000 || hdr.doc_start_count > 4000 ||
      hdr.image_count > 65535) {
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

  bool ok = true;
  std::vector<page_cache_utils::CachedPage> pages;
  ok = page_cache_utils::ReadPages(fp, hdr.page_count, kPageCachePageMaxBytes,
                                   &pages);
  if (ok)
    AppendPages(book, pages);

  if (ok) {
    std::vector<page_cache_utils::CachedChapter> chapters;
    ok = page_cache_utils::ReadChapters(fp, hdr.chapter_count,
                                        kPageCacheChapterTitleMaxBytes,
                                        &chapters);
    if (ok)
      AppendChapters(book, chapters);
  }

  if (ok) {
    for (u32 i = 0; i < hdr.doc_start_count; i++) {
      u16 doc_page = 0;
      if (fread(&doc_page, 1, sizeof(doc_page), fp) != sizeof(doc_page)) {
        ok = false;
        break;
      }
      std::string docpath;
      if (!page_cache_utils::ReadLengthPrefixedString16(
              fp, kPageCachePathMaxBytes, true, &docpath)) {
        ok = false;
        break;
      }
      book->SetChapterDocStartPage(docpath, doc_page);
    }
  }

  if (ok) {
    for (u32 i = 0; i < hdr.image_count; i++) {
      std::string imgpath;
      if (!page_cache_utils::ReadLengthPrefixedString16(
              fp, kPageCachePathMaxBytes, false, &imgpath)) {
        ok = false;
        break;
      }
      book->RegisterInlineImage(imgpath);
    }
  }

  fclose(fp);
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
  EnsureCacheDirs();

  EpubCacheLayoutParams params = BuildLayoutParams(
      book_path, pixel_size, line_spacing, paragraph_spacing, paragraph_indent,
      orientation, margin_left, margin_right, margin_top, margin_bottom,
      regular_font);

  std::string cache_path = BuildCachePath(book_path, params);
  if (cache_path.empty())
    return;

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  title = page_cache_utils::ClampString(title, kPageCacheTitleMaxBytes);
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  const std::unordered_map<std::string, u16> &doc_starts =
      book->GetChapterDocStartPages();
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
  hdr.image_count = book->GetInlineImageCount();

  DBG_LOGF(r, "EPUB cache save: fopen path=%s", cache_path.c_str());
  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp) {
    DBG_LOGF(r, "EPUB cache save: fopen failed");
    return;
  }
  setvbuf(fp, NULL, _IOFBF, 32768);

  bool ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  DBG_LOGF(r, "EPUB cache save: write-hdr ok=%d", (int)ok);
  if (ok)
    ok = page_cache_utils::WriteRawString(fp, title);

  DBG_LOGF(r, "EPUB cache save: write-pages begin count=%d", (int)page_count);
  for (u16 i = 0; ok && i < page_count; i++) {
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    const u16 len16 = (u16)(length > 0 ? length : 0);
    if (len16 > kPageCachePageMaxBytes) { ok = false; break; }
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
                                         kPageCacheChapterTitleMaxBytes);
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
              fp, kv.first, kPageCachePathMaxBytes, true)) {
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
              fp, *imgpath, kPageCachePathMaxBytes, false)) {
        ok = false;
        break;
      }
    }
  }

  fclose(fp);
  if (!ok)
    remove(cache_path.c_str());
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

  cache_path_ = BuildCachePath(book_path, params);
  if (cache_path_.empty())
    return false;

  fp_ = fopen(cache_path_.c_str(), "wb");
  if (!fp_)
    return false;
  // Use a large write buffer to amortise per-page fwrite() calls into
  // block-sized I/O on the SD card. Without this, 800+ tiny fwrite()s
  // (2-16 KB each) serialise through the SD controller one at a time.
  // 32 KB is large enough to batch ~20–100 pages per physical write while
  // staying well within New3DS RAM budget.
  setvbuf(fp_, NULL, _IOFBF, 32768);

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  title = page_cache_utils::ClampString(title, kPageCacheTitleMaxBytes);

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
    if (len16 > kPageCachePageMaxBytes)
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

  bool ok = page_cache_utils::WriteChapters(fp_, cached_chapters,
                                            kPageCacheChapterTitleMaxBytes);

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
              fp_, kv.first, kPageCachePathMaxBytes, true)) {
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
              fp_, *imgpath, kPageCachePathMaxBytes, false)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    const char *title_c = book->GetTitle();
    std::string title = title_c ? title_c : "";
    title = page_cache_utils::ClampString(title, kPageCacheTitleMaxBytes);

    EpubPageCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kEpubPageCacheMagic;
    hdr.version = kEpubPageCacheVersion;
    hdr.title_len = (u16)title.size();
    hdr.page_count = pages_written_;
    hdr.chapter_count = (u32)cached_chapters.size();
    hdr.doc_start_count = (u32)doc_starts.size();
    hdr.image_count = book->GetInlineImageCount();

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

} // namespace epub_page_cache
