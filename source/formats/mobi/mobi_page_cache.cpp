#include "formats/mobi/mobi_page_cache.h"

#include "book/book.h"
#include "book/page.h"
#include "shared/debug_log.h"
#include "formats/common/page_cache_field_limits.h"
#include "formats/common/page_cache_utils.h"
#include "shared/path_constants.h"

#include <3ds.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace mobi_page_cache {
namespace {

static const u32 kMobiPageCacheMagic = 0x4D504347U; // "MPCG"
// v20: body section zlib-deflated. See epub_page_cache.cpp for rationale.
static const u16 kMobiPageCacheVersion = 20;
static const size_t kPageCacheIoBufferBytes = 262144;

struct MobiPageCacheHeader {
  u32 magic;
  u16 version;
  u16 title_len;
  u32 page_count;
  u32 chapter_count;
  u32 image_count;
  u8 toc_quality;
  u8 reserved0;
  u16 reserved1;
  u16 toc_direct;
  u16 toc_heuristic;
  u16 toc_unresolved;
};

static void EnsureCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(paths::GetCacheBaseDir().c_str(), 0777);
  mkdir(paths::GetMobiCacheDir().c_str(), 0777);
  initialized = true;
}

static std::string BuildCachePath(const char *book_path,
                                  int pixel_size, int line_spacing,
                                  int paragraph_spacing, int paragraph_indent,
                                  int orientation, int margin_left,
                                  int margin_right, int margin_top,
                                  int margin_bottom, const char *regular_font,
                                  bool line_wrap_fix_enabled) {
  if (!book_path)
    return std::string();

  page_cache_utils::PageCacheLayoutParams params;
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
  params.variant_token = line_wrap_fix_enabled ? "1" : "0";

  return page_cache_utils::BuildPageCachePath(
      paths::GetMobiCacheDir().c_str(), ".mpc", book_path, params);
}

static bool WritePagesFromBook(page_cache_utils::BufWriter *w, Book *book,
                               uint16_t max_page_codepoints) {
  if (!w || !book)
    return false;
  const u16 page_count = book->GetPageCount();
  for (u16 i = 0; i < page_count; i++) {
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    if (length < 0 || (uint32_t)length > max_page_codepoints)
      return false;
    const uint16_t len16 = (uint16_t)length;
    if (!w->WriteRaw(&len16, sizeof(len16)))
      return false;
    if (length > 0) {
      const u32 *buf = page->GetBuffer();
      if (!buf)
        return false;
      if (!w->WriteRaw(buf, (size_t)length * sizeof(u32)))
        return false;
    }
  }
  return true;
}

static bool ReadPagesIntoBook(page_cache_utils::BufReader *r, uint32_t count,
                              uint16_t max_page_codepoints, Book *book) {
  if (!r || !book)
    return false;

  book->ReservePageCapacity(count);

  for (uint32_t i = 0; i < count; i++) {
    uint16_t length = 0;
    if (!r->ReadRaw(&length, sizeof(length)) || length > max_page_codepoints)
      return false;

    page_buffer_utils::OwnedPageBuffer owned;
    if (length > 0) {
      owned.codepoints.resize(length);
      const size_t byte_count = (size_t)length * sizeof(uint32_t);
      if (!r->Remaining(byte_count))
        return false;
      memcpy(owned.codepoints.data(), r->cur, byte_count);
      r->cur += byte_count;
    }

    Page *page = book->AppendPage();
    page->AdoptBuffer(&owned);
  }
  return true;
}

static std::vector<page_cache_utils::CachedChapter>
CollectCachedChapters(const std::vector<ChapterEntry> &chapters) {
  std::vector<page_cache_utils::CachedChapter> cached;
  cached.reserve(chapters.size());
  for (size_t i = 0; i < chapters.size(); i++) {
    const ChapterEntry &chapter = chapters[i];
    cached.push_back(page_cache_utils::CachedChapter(chapter.page, chapter.level,
                                                     chapter.title));
  }
  return cached;
}

static void AppendCachedChapters(
    Book *book,
    const std::vector<page_cache_utils::CachedChapter> &chapters) {
  if (!book)
    return;

  for (size_t i = 0; i < chapters.size(); i++) {
    const page_cache_utils::CachedChapter &chapter = chapters[i];
    if (chapter.page < book->GetPageCount())
      book->AddChapter(chapter.page, chapter.title, chapter.level);
  }
}

} // namespace

bool TryLoad(Book *book, const char *book_path,
             int pixel_size, int line_spacing,
             int paragraph_spacing, int paragraph_indent,
             int orientation, int margin_left, int margin_right,
             int margin_top, int margin_bottom,
             const char *regular_font,
             bool line_wrap_fix_enabled) {
  if (!book || !book_path)
    return false;
  EnsureCacheDirs();
  std::string cache_path =
      BuildCachePath(book_path, pixel_size, line_spacing,
                     paragraph_spacing, paragraph_indent, orientation,
                     margin_left, margin_right, margin_top, margin_bottom,
                     regular_font, line_wrap_fix_enabled);
  if (cache_path.empty())
    return false;

  // Bulk-read the cache file; same rationale as epub_page_cache::TryLoad —
  // per-record fread() over an SD card is the dominant cost on big books.
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  long file_size_long = ftell(fp);
  if (file_size_long <= (long)sizeof(MobiPageCacheHeader)) {
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

  page_cache_utils::BufReader file_reader(file_buf.data(), file_buf.size());

  MobiPageCacheHeader hdr;
  if (!file_reader.ReadRaw(&hdr, sizeof(hdr))) {
    remove(cache_path.c_str());
    return false;
  }
  if (hdr.magic != kMobiPageCacheMagic ||
      hdr.version != kMobiPageCacheVersion || hdr.page_count == 0 ||
      hdr.page_count > 10000 || hdr.chapter_count > 4000 ||
      hdr.image_count > 65535 ||
      hdr.title_len > 1000) {
    remove(cache_path.c_str());
    return false;
  }

  // v20 body layout: uint32 uncompressed_size, uint32 compressed_size,
  // deflated payload.
  uint32_t body_uncompressed = 0;
  uint32_t body_compressed = 0;
  if (!file_reader.ReadRaw(&body_uncompressed, sizeof(body_uncompressed)) ||
      !file_reader.ReadRaw(&body_compressed, sizeof(body_compressed)) ||
      body_uncompressed == 0 || body_compressed == 0 ||
      body_uncompressed > 32 * 1024 * 1024 ||
      !file_reader.Remaining(body_compressed)) {
    remove(cache_path.c_str());
    return false;
  }
  std::vector<uint8_t> body_buf;
  if (!page_cache_utils::DecompressBody(file_reader.cur, body_compressed,
                                         body_uncompressed, &body_buf)) {
    remove(cache_path.c_str());
    return false;
  }
  page_cache_utils::BufReader reader(body_buf.data(), body_buf.size());

  std::string title;
  if (!page_cache_utils::ReadRawString(&reader, hdr.title_len, &title)) {
    remove(cache_path.c_str());
    return false;
  }

  bool ok = true;
  ok = ReadPagesIntoBook(&reader, hdr.page_count,
                         page_cache_limits::kPageMaxBytes, book);

  if (ok) {
    std::vector<page_cache_utils::CachedChapter> chapters;
    ok = page_cache_utils::ReadChapters(&reader, hdr.chapter_count,
                                        page_cache_limits::kChapterTitleMaxBytes,
                                        &chapters);
    if (ok)
      AppendCachedChapters(book, chapters);
  }

  if (ok) {
    for (u32 i = 0; i < hdr.image_count; i++) {
      std::string imgpath;
      if (!page_cache_utils::ReadLengthPrefixedString16(
              &reader, page_cache_limits::kPathMaxBytes, false, &imgpath)) {
        ok = false;
        break;
      }
      u8 follow_lines = 0;
      if (!reader.ReadRaw(&follow_lines, sizeof(follow_lines))) {
        ok = false;
        break;
      }
      u16 image_id = book->RegisterInlineImage(imgpath);
      book->SetInlineImageFollowTextLines(image_id, follow_lines);
    }
  }
  if (!ok) {
    book->Close();
    remove(cache_path.c_str());
    return false;
  }

  if (!title.empty())
    book->SetTitle(title.c_str());

  TocQuality q = TOC_QUALITY_UNKNOWN;
  if (hdr.toc_quality <= TOC_QUALITY_HEURISTIC)
    q = (TocQuality)hdr.toc_quality;
  book->SetTocConfidence(q, hdr.toc_direct, hdr.toc_heuristic,
                         hdr.toc_unresolved);
  book->MarkMobiRenderSettingsApplied(line_wrap_fix_enabled);
  return true;
}

void Save(Book *book, const char *book_path,
          int pixel_size, int line_spacing,
          int paragraph_spacing, int paragraph_indent,
          int orientation, int margin_left, int margin_right,
          int margin_top, int margin_bottom,
          const char *regular_font,
          bool line_wrap_fix_enabled) {
  if (!book || !book_path || book->GetPageCount() == 0)
    return;
#ifdef DSLIBRIS_DEBUG
  IStatusReporter *r_timing = book->GetStatusReporter();
  u64 t0_save = osGetTime();
#endif
  EnsureCacheDirs();
  std::string cache_path =
      BuildCachePath(book_path, pixel_size, line_spacing,
                     paragraph_spacing, paragraph_indent, orientation,
                     margin_left, margin_right, margin_top, margin_bottom,
                     regular_font, line_wrap_fix_enabled);
  if (cache_path.empty())
    return;

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  title = page_cache_utils::ClampString(title, page_cache_limits::kTitleMaxBytes);
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  const std::vector<page_cache_utils::CachedChapter> cached_chapters =
      CollectCachedChapters(chapters);

  MobiPageCacheHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kMobiPageCacheMagic;
  hdr.version = kMobiPageCacheVersion;
  hdr.title_len = (u16)title.size();
  hdr.page_count = (u32)book->GetPageCount();
  hdr.chapter_count = (u32)cached_chapters.size();
  hdr.image_count = book->GetInlineImageCount();
  hdr.toc_quality = (u8)book->GetTocQuality();
  hdr.toc_direct = book->GetTocDirectCount();
  hdr.toc_heuristic = book->GetTocHeuristicCount();
  hdr.toc_unresolved = book->GetTocUnresolvedCount();

  // Build body in memory, then compress before fwrite.
  std::vector<uint8_t> body_buf;
  body_buf.reserve((size_t)book->GetPageCount() * 64);
  page_cache_utils::BufWriter w(&body_buf);

  bool ok = page_cache_utils::WriteRawString(&w, title);
  if (ok)
    ok = WritePagesFromBook(&w, book, page_cache_limits::kPageMaxBytes);
  if (ok)
    ok = page_cache_utils::WriteChapters(&w, cached_chapters,
                                         page_cache_limits::kChapterTitleMaxBytes);
  if (ok) {
    u32 img_count = book->GetInlineImageCount();
    for (u32 i = 0; i < img_count; i++) {
      const std::string *imgpath = book->GetInlineImagePath((u16)i);
      if (!imgpath || imgpath->empty()) { ok = false; break; }
      if (!page_cache_utils::WriteLengthPrefixedString16(
              &w, *imgpath, page_cache_limits::kPathMaxBytes, false)) {
        ok = false;
        break;
      }
      const u8 follow_lines = book->GetInlineImageFollowTextLines((u16)i);
      if (!w.WriteRaw(&follow_lines, sizeof(follow_lines))) {
        ok = false;
        break;
      }
    }
  }

  std::vector<uint8_t> compressed_body;
  if (ok && !page_cache_utils::CompressBody(body_buf, &compressed_body))
    ok = false;

  if (!ok)
    return;

  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return;
  setvbuf(fp, NULL, _IOFBF, kPageCacheIoBufferBytes);

  const uint32_t body_uncompressed = (uint32_t)body_buf.size();
  const uint32_t body_compressed = (uint32_t)compressed_body.size();
  ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  if (ok)
    ok = fwrite(&body_uncompressed, 1, sizeof(body_uncompressed), fp) ==
         sizeof(body_uncompressed);
  if (ok)
    ok = fwrite(&body_compressed, 1, sizeof(body_compressed), fp) ==
         sizeof(body_compressed);
  if (ok)
    ok = fwrite(compressed_body.data(), 1, compressed_body.size(), fp) ==
         compressed_body.size();
  fclose(fp);
  if (!ok)
    remove(cache_path.c_str());
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(r_timing, "MOBI cache save: done ok=%d ms=%u pages=%d",
           (int)ok, (unsigned)(osGetTime() - t0_save), (int)book->GetPageCount());
#endif
}

void SavePending(Book *book) {
  if (!book || !book->HasPendingMobiPageCacheSave())
    return;

  const char *folder = book->GetFolderName();
  const char *file = book->GetFileName();
  if (!folder || !*folder || !file || !*file) {
    book->SetPendingMobiPageCacheSave(false);
    return;
  }

  std::string path(folder);
  path.push_back('/');
  path += file;

  const Book::MobiCacheSaveParams &p = book->GetMobiCacheSaveParams();
  Save(book, path.c_str(), p.pixel_size, p.line_spacing,
       p.paragraph_spacing, p.paragraph_indent, p.orientation, p.margin_left,
       p.margin_right, p.margin_top, p.margin_bottom,
       p.regular_font.empty() ? NULL : p.regular_font.c_str(),
       p.line_wrap_fix_enabled);

  book->SetPendingMobiPageCacheSave(false);
}

} // namespace mobi_page_cache
