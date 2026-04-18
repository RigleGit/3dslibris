#include "formats/mobi/mobi_page_cache.h"

#include "book/book.h"
#include "book/page.h"
#include "formats/common/page_cache_utils.h"
#include "path_utils.h"

#include <3ds.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace mobi_page_cache {
namespace {

static const u32 kMobiPageCacheMagic = 0x4D504347U; // "MPCG"
static const u16 kMobiPageCacheVersion = 17;
static const u16 kPageCacheTitleMaxBytes = 1000;
static const u16 kPageCachePageMaxBytes = 4096;
static const u16 kPageCacheChapterTitleMaxBytes = 2048;
static const u16 kPageCachePathMaxBytes = 2048;

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
  mkdir(paths::kCacheBaseDir, 0777);
  mkdir(paths::kMobiCacheDir, 0777);
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
      paths::kMobiCacheDir, ".mpc", book_path, params);
}

static bool WritePagesFromBook(FILE *fp, Book *book,
                               uint16_t max_page_codepoints) {
  if (!fp || !book)
    return false;
  const u16 page_count = book->GetPageCount();
  for (u16 i = 0; i < page_count; i++) {
    Page *page = book->GetPage((int)i);
    const int length = page ? page->GetLength() : 0;
    if (length < 0 || (uint32_t)length > max_page_codepoints)
      return false;
    const uint16_t len16 = (uint16_t)length;
    if (fwrite(&len16, 1, sizeof(len16), fp) != sizeof(len16))
      return false;
    if (length > 0) {
      const u32 *buf = page->GetBuffer();
      if (!buf)
        return false;
      const size_t byte_count = (size_t)length * sizeof(u32);
      if (fwrite(buf, 1, byte_count, fp) != byte_count)
        return false;
    }
  }
  return true;
}

static bool ReadPagesIntoBook(FILE *fp, uint32_t count,
                              uint16_t max_page_codepoints, Book *book) {
  if (!fp || !book)
    return false;

  book->ReservePageCapacity(count);

  for (uint32_t i = 0; i < count; i++) {
    uint16_t length = 0;
    if (fread(&length, 1, sizeof(length), fp) != sizeof(length) ||
        length > max_page_codepoints)
      return false;

    page_buffer_utils::OwnedPageBuffer owned;
    if (length > 0) {
      owned.codepoints.resize(length);
      const size_t byte_count = (size_t)length * sizeof(uint32_t);
      if (fread(owned.codepoints.data(), 1, byte_count, fp) != byte_count)
        return false;
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

  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;

  MobiPageCacheHeader hdr;
  if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
  if (hdr.magic != kMobiPageCacheMagic ||
      hdr.version != kMobiPageCacheVersion || hdr.page_count == 0 ||
      hdr.page_count > 10000 || hdr.chapter_count > 4000 ||
      hdr.image_count > 65535 ||
      hdr.title_len > 1000) {
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
  ok = ReadPagesIntoBook(fp, hdr.page_count, kPageCachePageMaxBytes, book);

  if (ok) {
    std::vector<page_cache_utils::CachedChapter> chapters;
    ok = page_cache_utils::ReadChapters(fp, hdr.chapter_count,
                                        kPageCacheChapterTitleMaxBytes,
                                        &chapters);
    if (ok)
      AppendCachedChapters(book, chapters);
  }

  if (ok) {
    for (u32 i = 0; i < hdr.image_count; i++) {
      std::string imgpath;
      if (!page_cache_utils::ReadLengthPrefixedString16(
              fp, kPageCachePathMaxBytes, false, &imgpath)) {
        ok = false;
        break;
      }
      u8 follow_lines = 0;
      if (fread(&follow_lines, 1, sizeof(follow_lines), fp) !=
          sizeof(follow_lines)) {
        ok = false;
        break;
      }
      u16 image_id = book->RegisterInlineImage(imgpath);
      book->SetInlineImageFollowTextLines(image_id, follow_lines);
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
  title = page_cache_utils::ClampString(title, kPageCacheTitleMaxBytes);
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

  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return;

  bool ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  if (ok)
    ok = page_cache_utils::WriteRawString(fp, title);

  if (ok)
    ok = WritePagesFromBook(fp, book, kPageCachePageMaxBytes);

  if (ok)
    ok = page_cache_utils::WriteChapters(fp, cached_chapters,
                                         kPageCacheChapterTitleMaxBytes);

  if (ok) {
    u32 img_count = book->GetInlineImageCount();
    for (u32 i = 0; i < img_count; i++) {
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
      const u8 follow_lines = book->GetInlineImageFollowTextLines((u16)i);
      if (fwrite(&follow_lines, 1, sizeof(follow_lines), fp) !=
          sizeof(follow_lines)) {
        ok = false;
        break;
      }
    }
  }

  fclose(fp);
  if (!ok)
    remove(cache_path.c_str());
}

} // namespace mobi_page_cache
