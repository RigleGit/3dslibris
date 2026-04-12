/*
    3dslibris - app_browser.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Native FS-based book discovery and UTF-8 normalization hardening.
    - Browser cover cache/preload integration and deferred metadata jobs.
    - Touch navigation and footer interactions adapted to 3DS orientation.
*/

#include "app/app.h"
#include "app/library_controller.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <list>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#include <3ds.h>

#include "book/book.h"
#include "ui/browser_nav.h"
#include "formats/common/book_error.h"
#include "formats/cbz/cbz.h"
#include "ui/button.h"
#include "menus/chapter_menu.h"
#include "debug_log.h"
#include "formats/epub/epub.h"
#include "formats/fb2/fb2.h"
#include "formats/mobi/mobi.h"
#include "formats/pdf/pdf.h"
#include "parse.h"
#include "shared/app_flow_utils.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_job_queue_utils.h"
#include "library/browser_warmup_utils.h"
#include "shared/string_utils.h"
#include "path_utils.h"
#include "ui/text.h"
#include "shared/utf8_utils.h"
#include "version.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

namespace {

static const char *kCoverCacheBaseDir = paths::kCacheBaseDir;
static const char *kCoverCacheDir = paths::kCoverCacheDir;
static const char *kCoverCacheMagic = "CVR3";
static const size_t kCoverCacheMaxFiles = 512;
static const size_t kCoverCacheMaxBytes = 16 * 1024 * 1024;
// Max extraction attempts per session before a book is skipped.
// Two retries absorb transient SD-card read failures without retrying forever.
static const uint8_t kCoverMaxAttempts = 3;
static const int kCoverThumbMaxW = 85;
static const int kCoverThumbMaxH = 115;
static const int kBrowserGridCols = 2;
static const int kBrowserGridRows = 2;
static const int kBrowserCoverW = 85;
static const int kBrowserCoverH = 115;
static const int kBrowserCellW = 115;
// Reserve a dedicated footer strip for browser buttons and keep enough visual
// gap so row-1 labels never collide with row-2 covers.
static const int kBrowserCellH = 144;
static const int kBrowserTitleOffsetY = kBrowserCoverH + 10;
static const int kBrowserProgressOffsetY = kBrowserCoverH + 22;
static const int kBrowserGridX0 = 5;
static const int kBrowserGridY0 = 3;
static const int kBrowserFooterY = 296;

static size_t CountQueuedHeavyJobs(const std::deque<app_job_t> &jobs) {
  size_t count = 0;
  for (const auto &job : jobs) {
    if (browser_job_queue_utils::IsHeavyBrowserJobType(
            job.type, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER)) {
      count++;
    }
  }
  return count;
}

static void LayoutBrowserNavButtons(App *app) {
  app->buttonprev.Move(2, kBrowserFooterY);
  app->buttonprev.Resize(66, 22);
  app->buttonprev.Label("prev");

  app->buttonnext.Move(172, kBrowserFooterY);
  app->buttonnext.Resize(66, 22);
  app->buttonnext.Label("next");

  app->buttonprefs.Move(72, kBrowserFooterY);
  app->buttonprefs.Resize(96, 22);
  app->buttonprefs.Label("settings");
}

static std::string TrimSpaces(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && s[start] == ' ')
    start++;
  size_t end = s.size();
  while (end > start && s[end - 1] == ' ')
    end--;
  return s.substr(start, end - start);
}

static std::string BuildBookPath(Book *book) {
  if (!book || !book->GetFolderName() || !book->GetFileName())
    return "";
  std::string path = book->GetFolderName();
  path.push_back('/');
  path.append(book->GetFileName());
  return path;
}

struct CoverCacheEntry {
  std::string path;
  long long mtime;
  size_t size;
};

static bool CoverCacheEntryOlderFirst(const CoverCacheEntry &a,
                                      const CoverCacheEntry &b) {
  if (a.mtime != b.mtime)
    return a.mtime < b.mtime;
  return a.path < b.path;
}

static void PruneCoverCache(bool force) {
  static u64 last_prune_ms = 0;
  u64 now = osGetTime();
  if (!force && now - last_prune_ms < 5000)
    return;
  last_prune_ms = now;

  DIR *dp = opendir(kCoverCacheDir);
  if (!dp)
    return;

  std::list<CoverCacheEntry> entries;
  size_t total_bytes = 0;

  struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    if (!HasExtCI(ent->d_name, ".cvr"))
      continue;

    char full[512];
    snprintf(full, sizeof(full), "%s/%s", kCoverCacheDir, ent->d_name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    CoverCacheEntry ce;
    ce.path = full;
    ce.mtime = (long long)st.st_mtime;
    ce.size = (st.st_size > 0) ? (size_t)st.st_size : 0;
    total_bytes += ce.size;
    entries.push_back(ce);
  }
  closedir(dp);

  if (entries.size() <= kCoverCacheMaxFiles &&
      total_bytes <= kCoverCacheMaxBytes)
    return;

  entries.sort(CoverCacheEntryOlderFirst);

  size_t remaining_count = entries.size();
  bool removed_any = false;
  while ((remaining_count > kCoverCacheMaxFiles ||
          total_bytes > kCoverCacheMaxBytes) &&
         !entries.empty()) {
    const CoverCacheEntry &oldest = entries.front();
    remove(oldest.path.c_str());
    removed_any = true;
    if (remaining_count > 0)
      remaining_count--;
    if (oldest.size <= total_bytes)
      total_bytes -= oldest.size;
    else
      total_bytes = 0;
    entries.pop_front();
  }

  // Rewrite the manifest to drop entries for pruned files.
  if (removed_any) {
    std::set<std::string> alive;
    for (std::list<CoverCacheEntry>::const_iterator it = entries.begin();
         it != entries.end(); ++it) {
      alive.insert(BasenamePath(it->path));
    }
    std::vector<std::string> kept;
    FILE *rf = fopen(paths::kCoverCacheManifest, "r");
    if (rf) {
      char line[1024];
      while (fgets(line, sizeof(line), rf)) {
        std::string l = line;
        size_t tab = l.find('\t');
        std::string fname = (tab != std::string::npos) ? l.substr(0, tab) : l;
        while (!fname.empty() && (fname.back() == '\n' || fname.back() == '\r'))
          fname.pop_back();
        if (alive.count(fname))
          kept.push_back(l);
      }
      fclose(rf);
    }
    FILE *wf = fopen(paths::kCoverCacheManifest, "w");
    if (wf) {
      for (size_t i = 0; i < kept.size(); i++)
        fputs(kept[i].c_str(), wf);
      fclose(wf);
    }
  }
}

static void EnsureCoverCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kCoverCacheBaseDir, 0777);
  mkdir(kCoverCacheDir, 0777);

  // One-time cleanup: remove legacy CVR2 cache files whose names are bare
  // 16-hex-digit hashes (e.g., "a1b2c3d4e5f6g7h8.cvr").
  DIR *legacy_dp = opendir(kCoverCacheDir);
  if (legacy_dp) {
    struct dirent *ent;
    while ((ent = readdir(legacy_dp)) != NULL) {
      if (!HasExtCI(ent->d_name, ".cvr"))
        continue;
      size_t nlen = strlen(ent->d_name);
      if (nlen == 20) { // 16 hex digits + ".cvr"
        bool all_hex = true;
        for (int i = 0; i < 16 && all_hex; i++)
          all_hex = isxdigit((unsigned char)ent->d_name[i]);
        if (all_hex) {
          char full[512];
          snprintf(full, sizeof(full), "%s/%s", kCoverCacheDir, ent->d_name);
          remove(full);
        }
      }
    }
    closedir(legacy_dp);
  }

  PruneCoverCache(true);
  initialized = true;
}

static std::string BuildCoverCachePath(Book *book,
                                       const std::string &book_path) {
  struct stat st;
  long long fsize = 0;
  long long fmtime = 0;
  if (stat(book_path.c_str(), &st) == 0) {
    fsize = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }

  std::string key = book_path;
  key.push_back('|');
  key += std::to_string(fsize);
  key.push_back('|');
  key += std::to_string(fmtime);
  uint64_t h = Fnv1a64(key);

  // Use the book filename (sans extension) as a human-readable label.
  // Title is not used because it may not be available at cache-load time.
  std::string label;
  if (book && book->GetFileName() && book->GetFileName()[0] != '\0') {
    label = book->GetFileName();
    size_t dot = label.rfind('.');
    if (dot != std::string::npos && dot > 0)
      label = label.substr(0, dot);
  }
  if (label.empty())
    label = "book";
  label = SanitizeFat32Name(label, 80);

  char out[512];
  snprintf(out, sizeof(out), "%s/%s_%08llx.cvr", kCoverCacheDir, label.c_str(),
           (unsigned long long)(h & 0xFFFFFFFFULL));
  return std::string(out);
}

static bool TryLoadCoverCache(Book *book, const std::string &book_path) {
  if (!book)
    return false;
  EnsureCoverCacheDirs();

  std::string cache_path = BuildCoverCachePath(book, book_path);
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp) {
#ifdef DSLIBRIS_DEBUG
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(), "COVER: cache miss book=%s path=%s",
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    return false;
  }

  u8 header[8];
  bool ok = fread(header, 1, sizeof(header), fp) == sizeof(header);
  // Ignore thumbnails written by older extractors once the cache format/magic
  // changes, so stale covers do not survive heuristic fixes.
  if (!ok || memcmp(header, kCoverCacheMagic, 4) != 0) {
    fclose(fp);
#ifdef DSLIBRIS_DEBUG
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(),
               "COVER: cache corrupt (bad magic/header) book=%s path=%s",
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    return false;
  }

  u16 w = (u16)header[4] | ((u16)header[5] << 8);
  u16 h = (u16)header[6] | ((u16)header[7] << 8);
  if (w == 0 || h == 0 || w > kCoverThumbMaxW || h > kCoverThumbMaxH) {
    fclose(fp);
#ifdef DSLIBRIS_DEBUG
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(),
               "COVER: cache corrupt (bad dims %ux%u) book=%s path=%s",
               (unsigned)w, (unsigned)h,
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    return false;
  }

  size_t count = (size_t)w * (size_t)h;
  std::vector<u16> pixels(count);
  if (fread(pixels.data(), sizeof(u16), count, fp) != count) {
    fclose(fp);
#ifdef DSLIBRIS_DEBUG
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(),
               "COVER: cache truncated (expected %zu pixels) book=%s path=%s",
               count,
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    return false;
  }
  fclose(fp);

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
  book->coverPixels = new u16[count];
  if (!book->coverPixels)
    return false;
  memcpy(book->coverPixels, pixels.data(), count * sizeof(u16));
  book->coverWidth = w;
  book->coverHeight = h;
#ifdef DSLIBRIS_DEBUG
  if (book->GetStatusReporter()) {
    DBG_LOGF(book->GetStatusReporter(),
             "COVER: cache hit book=%s path=%s size=%ux%u",
             book->GetFileName() ? book->GetFileName() : "(null)",
             cache_path.c_str(), (unsigned)w, (unsigned)h);
  }
#endif
  return true;
}

static bool SaveCoverCache(Book *book, const std::string &book_path) {
  if (!book || !book->coverPixels || book->coverWidth <= 0 ||
      book->coverHeight <= 0) {
    return false;
  }
  if (book->coverWidth > kCoverThumbMaxW || book->coverHeight > kCoverThumbMaxH)
    return false;

  EnsureCoverCacheDirs();
  std::string cache_path = BuildCoverCachePath(book, book_path);
  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return false;

  u8 header[8];
  // Bump the cache magic when extractor heuristics change so stale MOBI
  // thumbnails do not mask newer cover fixes.
  memcpy(header, kCoverCacheMagic, 4);
  header[4] = (u8)(book->coverWidth & 0xFF);
  header[5] = (u8)((book->coverWidth >> 8) & 0xFF);
  header[6] = (u8)(book->coverHeight & 0xFF);
  header[7] = (u8)((book->coverHeight >> 8) & 0xFF);
  bool ok = fwrite(header, 1, sizeof(header), fp) == sizeof(header);
  size_t count = (size_t)book->coverWidth * (size_t)book->coverHeight;
  if (ok) {
    ok = fwrite(book->coverPixels, sizeof(u16), count, fp) == count;
  }
  fclose(fp);
  if (ok) {
    FILE *mf = fopen(paths::kCoverCacheManifest, "a");
    if (mf) {
      const char *t = book->GetTitle();
      const char *f = book->GetFileName();
      const char *display = (t && t[0] != '\0') ? t : (f ? f : "");
      std::string cache_base = BasenamePath(cache_path);
      fprintf(mf, "%s\t%s\t%s\n", cache_base.c_str(), display, book_path.c_str());
      fclose(mf);
    }
    PruneCoverCache(false);
  }
#ifdef DSLIBRIS_DEBUG
  if (book->GetStatusReporter()) {
    DBG_LOGF(book->GetStatusReporter(),
             "COVER: cache save %s book=%s path=%s size=%dx%d",
             ok ? "ok" : "fail",
             book->GetFileName() ? book->GetFileName() : "(null)",
             cache_path.c_str(), book->coverWidth, book->coverHeight);
  }
#endif
  return ok;
}

} // namespace

void LibraryController::UnloadNonVisibleBrowserCoverCaches() {
  if (app_.BookCount() <= 0)
    return;

  const browser_cover_cache_utils::VisibleRange visible =
      browser_cover_cache_utils::ComputeVisibleRange(
          app_.GetBrowserPageStart(), app_.BookCount(), APP_BROWSER_BUTTON_COUNT);
  for (int i = 0; i < app_.BookCount(); i++) {
    if (browser_cover_cache_utils::RangeContains(visible, i))
      continue;

    Book *book = app_.books[i];
    if (!book || !book->coverPixels)
      continue;
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
    book->coverWidth = 0;
    book->coverHeight = 0;
  }
}

void LibraryController::LoadVisibleBrowserCoverCaches() {
  if (app_.BookCount() <= 0)
    return;

  UnloadNonVisibleBrowserCoverCaches();
  const browser_cover_cache_utils::VisibleRange visible =
      browser_cover_cache_utils::ComputeVisibleRange(
          app_.GetBrowserPageStart(), app_.BookCount(), APP_BROWSER_BUTTON_COUNT);
  const int start = visible.start;
  const int end = visible.end;
  for (int i = start; i < end; i++) {
    Book *book = app_.books[i];
    if (!book || book->coverPixels)
      continue;

    std::string path = BuildBookPath(book);
    if (path.empty())
      continue;
    if (TryLoadCoverCache(book, path)) {
      book->coverAttempts = kCoverMaxAttempts;
      book->coverRetryAfterMs = 0;
      if (book->format == FORMAT_EPUB) {
        book->metadataIndexTried = true;
        book->metadataIndexed = true;
      }
    }
  }
}

namespace {

#if UTF8_FILENAME_DIAG
static std::string HexBytesForLog(const std::string &s, size_t max_bytes = 32) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  size_t n = s.size() < max_bytes ? s.size() : max_bytes;
  out.reserve(n * 3 + 8);
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)s[i];
    if (i)
      out.push_back(' ');
    out.push_back(hex[(b >> 4) & 0x0F]);
    out.push_back(hex[b & 0x0F]);
  }
  if (s.size() > max_bytes)
    out += " ...";
  return out;
}

static std::string ClipForLog(const std::string &s, size_t max_chars = 72) {
  if (s.size() <= max_chars)
    return s;
  return s.substr(0, max_chars) + "...";
}
#endif

static void LogUtf8StageOnce(Book *book, const char *stage,
                             const std::string &value) {
#if !UTF8_FILENAME_DIAG
  (void)book;
  (void)stage;
  (void)value;
  return;
#else
  if (!book || !book->GetStatusReporter() || !stage)
    return;

  static std::set<std::string> logged;
  char key[96];
  snprintf(key, sizeof(key), "%p|%s", (void *)book, stage);
  if (!logged.insert(std::string(key)).second)
    return;

  char msg[512];
  std::string bytes = HexBytesForLog(value);
  std::string clipped = ClipForLog(value);
  snprintf(msg, sizeof(msg),
           "UTF8 flow %-18s len=%u valid=%d bytes=[%s] text=\"%s\"", stage,
           (unsigned)value.size(), utf8_utils::IsValidUtf8(value) ? 1 : 0,
           bytes.c_str(), clipped.c_str());
  DBG_LOG(book->GetStatusReporter(), msg);
#endif
}

static std::string NormalizeDisplayUtf8(const std::string &raw,
                                        bool *repaired_fullwidth = nullptr,
                                        bool *repaired_legacy = nullptr,
                                        bool *composed_accents = nullptr) {
  if (repaired_fullwidth)
    *repaired_fullwidth = false;
  if (repaired_legacy)
    *repaired_legacy = false;
  if (composed_accents)
    *composed_accents = false;

  if (raw.empty())
    return raw;

  std::string s = raw;
  std::string repaired;
  if (utf8_utils::TryRepairFullwidthByteMojibake(s, &repaired)) {
    s = repaired;
    if (repaired_fullwidth)
      *repaired_fullwidth = true;
  }

  if (!utf8_utils::IsValidUtf8(s)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    return utf8_utils::DecodeCp1252ToUtf8(s);
  }

  if (utf8_utils::TryRepairMojibakeUtf8(s, &repaired)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    s = repaired;
  }

  std::string composed = utf8_utils::ComposeLatinCombiningMarks(s);
  if (composed != s) {
    s = composed;
    if (composed_accents)
      *composed_accents = true;
  }
  return s;
}

static std::string BuildBrowserDisplayName(Book *book) {
  if (!book)
    return "";
  if (book->HasBrowserDisplayNameCache())
    return book->GetBrowserDisplayNameCache();

  const char *filename_ptr = book->GetFileName();
  const char *source =
      BrowserDisplayNameSource(book->GetTitle(), filename_ptr);
  bool source_is_filename = (source == filename_ptr);
  std::string raw = source ? source : "";
  LogUtf8StageOnce(book, "filename_raw", raw);

  bool repaired_fullwidth = false;
  bool repaired_legacy = false;
  bool composed_accents = false;
  std::string normalized = NormalizeDisplayUtf8(
      raw, &repaired_fullwidth, &repaired_legacy, &composed_accents);
  if (repaired_fullwidth)
    LogUtf8StageOnce(book, "filename_ff_repair", normalized);
  if (repaired_legacy)
    LogUtf8StageOnce(book, "filename_legacy_fix", normalized);
  if (composed_accents)
    LogUtf8StageOnce(book, "filename_compose", normalized);
  LogUtf8StageOnce(book, "filename_norm", normalized);

  if (source_is_filename) {
    size_t dot = normalized.find_last_of('.');
    if (dot != std::string::npos)
      normalized = normalized.substr(0, dot);
  }

  normalized = TrimSpaces(normalized);
  LogUtf8StageOnce(book, "filename_final", normalized);
  book->SetBrowserDisplayNameCache(normalized);
  return book->GetBrowserDisplayNameCache();
}

static size_t Utf8BytesForCharCount(const char *s, size_t char_count) {
  if (!s)
    return 0;
  size_t bytes = 0;
  size_t chars = 0;
  while (s[bytes] && chars < char_count) {
    unsigned char c = (unsigned char)s[bytes];
    size_t step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;

    // Clamp malformed/truncated sequences to avoid overrun.
    for (size_t i = 1; i < step; i++) {
      if (!s[bytes + i]) {
        step = i;
        break;
      }
    }
    bytes += step;
    chars++;
  }
  return bytes;
}

static void DrawWrappedTitleInsideCover(Text *ts, const std::string &title,
                                        int x, int y, int w, int h, u8 style) {
  if (!ts || title.empty() || w <= 8 || h <= 8)
    return;

  const int kPadX = 6;
  const int kPadY = 6;
  const int inner_w = w - kPadX * 2;
  const int inner_h = h - kPadY * 2;
  if (inner_w <= 8 || inner_h <= 8)
    return;

  int line_h = ts->GetHeight();
  int max_lines = inner_h / std::max(1, line_h);
  if (max_lines < 1)
    return;

  size_t pos = 0;
  int drawn = 0;
  while (pos < title.size() && drawn < max_lines) {
    while (pos < title.size() && title[pos] == ' ')
      pos++;
    if (pos >= title.size())
      break;

    u8 fit = ts->GetCharCountInsideWidth(title.c_str() + pos, style, inner_w);
    if (!fit)
      break;

    size_t take = Utf8BytesForCharCount(title.c_str() + pos, fit);
    if (pos + take < title.size()) {
      size_t back = take;
      while (back > 0 && title[pos + back - 1] != ' ')
        back--;
      if (back > 0)
        take = back;
    }
    std::string line = TrimSpaces(title.substr(pos, take));
    if (line.empty()) {
      pos += take;
      continue;
    }

    // Set baseline below top padding to avoid clipping accents/ascenders.
    ts->SetPen(x + kPadX, y + kPadY + (drawn + 1) * line_h);
    ts->PrintString(line.c_str(), style);
    drawn++;
    pos += take;
  }
}

struct MarqueeState {
  Book   *book          = nullptr;
  u16    *strip         = nullptr;
  u16    *bg_strip      = nullptr;
  int     strip_w       = 0;
  int     strip_h       = 0;
  int     strip_x       = 0;
  int     strip_y       = 0;
  int     blit_w        = 0;
  int     scroll_offset = 0;
  int     scroll_timer  = 0;
  int     end_timer     = 0;
  bool    active        = false;

  void Reset() {
    delete[] strip;
    delete[] bg_strip;
    strip    = nullptr;
    bg_strip = nullptr;
    book     = nullptr;
    strip_w = strip_h = strip_x = strip_y = blit_w = 0;
    scroll_offset = scroll_timer = end_timer = 0;
    active = false;
  }
};

static MarqueeState g_marquee;

} // namespace

bool LibraryController::HasQueuedJob(app_job_type_t type, Book *book) const {
  for (const auto &job : job_queue_) {
    if (job.type == type && job.book == book)
      return true;
  }
  return false;
}

void LibraryController::PrioritizeSelectedBookJobs(Book *selected_book) {
  if (!selected_book || job_queue_.empty())
    return;

  // Move the selected book's pending jobs to the front so they run first,
  // but keep all other books' jobs in the queue — they will be processed
  // during subsequent idle periods rather than being discarded.
  std::deque<app_job_t> selected_jobs;
  std::deque<app_job_t> other_jobs;
  for (size_t i = 0; i < job_queue_.size(); i++) {
    if (job_queue_[i].book == selected_book)
      selected_jobs.push_back(job_queue_[i]);
    else
      other_jobs.push_back(job_queue_[i]);
  }
  job_queue_.clear();
  for (size_t i = 0; i < selected_jobs.size(); i++)
    job_queue_.push_back(selected_jobs[i]);
  for (size_t i = 0; i < other_jobs.size(); i++)
    job_queue_.push_back(other_jobs[i]);

#ifdef DSLIBRIS_DEBUG
  if (!selected_jobs.empty()) {
    DBG_LOGF(&app_,
             "BROWSER: prioritized selected book jobs=%u queue=%u book=%s",
             (unsigned)selected_jobs.size(), (unsigned)job_queue_.size(),
             selected_book->GetFileName() ? selected_book->GetFileName()
                                          : "(null)");
  }
#endif
}

void LibraryController::EnqueueJob(app_job_type_t type, Book *book) {
  if (!book)
    return;
  if (HasQueuedJob(type, book))
    return;
  app_job_t job;
  job.type = type;
  job.book = book;
  job_queue_.push_back(job);
}

void LibraryController::QueueBookWarmup(Book *book) {
  if (!book || book->coverPixels || book->coverAttempts >= kCoverMaxAttempts)
    return;
  const u64 now_ms = osGetTime();
  if (book->coverRetryAfterMs != 0 && now_ms < book->coverRetryAfterMs)
    return;
  const size_t queue_before = job_queue_.size();
  const size_t queued_heavy_jobs = CountQueuedHeavyJobs(job_queue_);
  const bool is_selected_book = (book == app_.GetSelectedBook());
  const bool warmup_idle = browser_warmup_utils::IsBrowserWarmupIdle(
      now_ms, app_.GetBrowserLastInteractionMs(),
      app_.IsBrowserWaitingInputRelease());
  const bool heavy_idle = browser_warmup_utils::IsBrowserHeavyWarmupIdle(
      now_ms, app_.GetBrowserLastInteractionMs(),
      app_.IsBrowserWaitingInputRelease());
  const bool should_queue_cover =
      browser_warmup_utils::ShouldQueueCoverWarmupForDevice(
          app_.IsNew3dsDevice(), is_selected_book, warmup_idle, heavy_idle,
          queued_heavy_jobs);

  const format_t format =
      app_flow_utils::DetectBookFormat(book->GetFileName());

  if (book->format == FORMAT_EPUB) {
    if (!book->metadataIndexTried &&
        app_flow_utils::SupportsMetadataIndexing(format))
      EnqueueJob(APP_JOB_INDEX_METADATA, book);
    if (should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_PDF) {
    if (!book->metadataIndexTried &&
        app_flow_utils::SupportsMetadataIndexing(format))
      EnqueueJob(APP_JOB_INDEX_METADATA, book);
    if (should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_CBZ) {
    if (should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_XHTML) {
    if (HasExtCI(book->GetFileName(), ".fb2") ||
        HasExtCI(book->GetFileName(), ".mobi")) {
      if (should_queue_cover)
        EnqueueJob(APP_JOB_EXTRACT_COVER, book);
    }
  }
#ifdef DSLIBRIS_DEBUG
  if (job_queue_.size() != queue_before) {
    DBG_LOGF(&app_,
             "BROWSER: warmup queued added=%u queue=%u book=%s format=%d "
             "selected=%u warmup_idle=%u heavy_idle=%u cover=%u",
             (unsigned)(job_queue_.size() - queue_before),
             (unsigned)job_queue_.size(),
             book->GetFileName() ? book->GetFileName() : "(null)",
             (int)book->format, is_selected_book ? 1u : 0u,
             warmup_idle ? 1u : 0u, heavy_idle ? 1u : 0u,
             should_queue_cover ? 1u : 0u);
  }
#endif
}

void LibraryController::TickBrowserWarmup() {
  if (app_.IsAppletSuspended() || app_.GetMode() != AppMode::Browser ||
      !app_.GetSelectedBook())
    return;
  if (!browser_warmup_utils::IsBrowserWarmupIdle(
          osGetTime(), app_.GetBrowserLastInteractionMs(),
          app_.IsBrowserWaitingInputRelease())) {
    return;
  }
  QueueBookWarmup(app_.GetSelectedBook());
  const int page_start = app_.GetBrowserPageStart();
  for (int i = page_start;
       i < app_.BookCount() && i < page_start + APP_BROWSER_BUTTON_COUNT;
       i++) {
    Book *book = app_.books[i];
    if (!book || book == app_.GetSelectedBook())
      continue;
    QueueBookWarmup(book);
  }
}

void LibraryController::QueueTocResolve(Book *book) {
  if (!book || book->format != FORMAT_EPUB || book->tocResolveTried)
    return;
  EnqueueJob(APP_JOB_RESOLVE_TOC, book);
}

void LibraryController::ProcessJobs(u32 budget_ms) {
  if (app_.IsAppletSuspended())
    return;
  if (job_queue_.empty())
    return;

  auto job_name = [](app_job_type_t t) -> const char * {
    switch (t) {
    case APP_JOB_INDEX_METADATA:
      return "index";
    case APP_JOB_EXTRACT_COVER:
      return "cover";
    case APP_JOB_RESOLVE_TOC:
      return "toc";
    default:
      return "unknown";
    }
  };

  u64 start_ms = osGetTime();
  while (!job_queue_.empty()) {
    while (!job_queue_.empty() && !job_queue_.front().book)
      job_queue_.pop_front();
    if (job_queue_.empty())
      break;

    const bool allow_selected_browser_jobs =
        app_.GetMode() != AppMode::Browser ||
        browser_warmup_utils::IsBrowserWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
    const bool allow_heavy_browser_jobs =
        app_.GetMode() != AppMode::Browser ||
        browser_warmup_utils::IsBrowserHeavyWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
    app_job_t job = {};
    const bool got_job = browser_job_queue_utils::TakeFirstAllowedJob(
        &job_queue_, &job, [&](const app_job_t &candidate) {
          if (!candidate.book)
            return false;
          if (!browser_job_queue_utils::IsHeavyBrowserJobType(
                  candidate.type, APP_JOB_INDEX_METADATA,
                  APP_JOB_EXTRACT_COVER)) {
            return true;
          }
          if (app_.GetMode() != AppMode::Browser)
            return true;
          if (candidate.book == app_.GetSelectedBook())
            return allow_selected_browser_jobs;
          return allow_heavy_browser_jobs;
        });
    if (!got_job)
      break;


    Book *book = job.book;
    int rc = 0;
    u64 t0 = osGetTime();

    if (job.type == APP_JOB_INDEX_METADATA) {
      if (!book->metadataIndexTried &&
          app_flow_utils::SupportsMetadataIndexing(
              app_flow_utils::DetectBookFormat(book->GetFileName()))) {
        rc = book->Index();
        // Do not penalise cover extraction on metadata failure.
        app_.SetBrowserDirty(true);
      }
    } else if (job.type == APP_JOB_EXTRACT_COVER) {
      if (!book->coverPixels && book->coverAttempts < kCoverMaxAttempts) {
        std::string path = BuildBookPath(book);
        if (path.empty()) {
          rc = 1;
          book->coverAttempts = kCoverMaxAttempts; // path failure is permanent
          app_.SetBrowserDirty(true);
        } else {
#ifdef DSLIBRIS_DEBUG
        DBG_LOGF(&app_, "COVER: extract start book=%s format=%d attempt=%u",
                 book->GetFileName() ? book->GetFileName() : "(null)",
                 (int)book->format, (unsigned)book->coverAttempts);
#endif
        const bool is_selected_book = (book == app_.GetSelectedBook());
        const u64 free_bytes = (u64)osGetMemRegionFree(MEMREGION_ALL);
        if (!browser_warmup_utils::HasCoverExtractionHeadroom(
                app_.IsNew3dsDevice(), is_selected_book, free_bytes)) {
          const u64 retry_delay_ms = browser_warmup_utils::CoverRetryDelayMs(
              app_.IsNew3dsDevice(), is_selected_book, 4, false);
          if (retry_delay_ms != 0)
            book->coverRetryAfterMs = osGetTime() + retry_delay_ms;
#ifdef DSLIBRIS_DEBUG
          DBG_LOGF(&app_,
                   "COVER: skip mem-pressure book=%s selected=%u free=%llu",
                   book->GetFileName() ? book->GetFileName() : "(null)",
                   is_selected_book ? 1u : 0u,
                   (unsigned long long)free_bytes);
#endif
          continue;
        }
        if (book->format == FORMAT_EPUB) {
          if (!book->metadataIndexTried) {
            // Metadata not yet attempted; queue it first and retry cover after.
            EnqueueJob(APP_JOB_INDEX_METADATA, book);
            EnqueueJob(APP_JOB_EXTRACT_COVER, book);
          } else {
            if (!book->coverImagePath.empty()) {
              rc = epub_extract_cover(book, path);
              if (rc == 0 && book->coverPixels) {
                SaveCoverCache(book, path);
                book->coverAttempts = kCoverMaxAttempts;
                book->coverRetryAfterMs = 0;
              } else if (rc != 0) {
                book->coverAttempts++;
              } else {
                book->coverAttempts = kCoverMaxAttempts;
              }
            } else {
              book->coverAttempts = kCoverMaxAttempts;
            }
            app_.SetBrowserDirty(true);
          }
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".fb2")) {
          rc = fb2_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
          app_.SetBrowserDirty(true);
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".mobi")) {
          rc = mobi_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
          app_.SetBrowserDirty(true);
        } else if (book->format == FORMAT_PDF) {
          rc = pdf_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
          app_.SetBrowserDirty(true);
        } else if (book->format == FORMAT_CBZ) {
          rc = cbz_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
          app_.SetBrowserDirty(true);
        }
        const u64 retry_delay_ms = browser_warmup_utils::CoverRetryDelayMs(
            app_.IsNew3dsDevice(), is_selected_book, rc,
            book->coverPixels != nullptr);
        if (retry_delay_ms != 0 && book->coverAttempts < kCoverMaxAttempts) {
          book->coverRetryAfterMs = osGetTime() + retry_delay_ms;
#ifdef DSLIBRIS_DEBUG
          DBG_LOGF(&app_,
                   "COVER: retry deferred book=%s rc=%d delay_ms=%llu attempt=%u",
                   book->GetFileName() ? book->GetFileName() : "(null)", rc,
                   (unsigned long long)retry_delay_ms,
                   (unsigned)book->coverAttempts);
#endif
        }
#ifdef DSLIBRIS_DEBUG
        DBG_LOGF(&app_, "COVER: extract end rc=%d book=%s pixels=%u attempts=%u",
                 rc, book->GetFileName() ? book->GetFileName() : "(null)",
                 book->coverPixels ? 1u : 0u, (unsigned)book->coverAttempts);
#endif
        }
      }
    } else if (job.type == APP_JOB_RESOLVE_TOC) {
      if (book->format == FORMAT_EPUB && !book->tocResolveTried) {
        std::string path = BuildBookPath(book);
        if (path.empty())
          continue;
        rc = epub_resolve_toc(book, path);
        book->tocResolveTried = true;
        book->tocResolved = (rc == 0);
        if (rc == 0 && app_.GetMode() == AppMode::Chapters &&
            book == app_.GetCurrentBook() && app_.chaptermenu) {
          app_.chaptermenu->Init();
          app_.chaptermenu->SetDirty(true);
        }
      }
    }

    u64 elapsed = osGetTime() - t0;
    char msg[256];
    if (const char *tag = BookOpenErrorTag(rc)) {
      snprintf(msg, sizeof(msg), "TIMING: job=%s rc=%s ms=%llums book=%s",
               job_name(job.type), tag, (unsigned long long)elapsed,
               book->GetFileName() ? book->GetFileName() : "(null)");
    } else {
      snprintf(msg, sizeof(msg), "TIMING: job=%s rc=%d ms=%llums book=%s",
               job_name(job.type), rc, (unsigned long long)elapsed,
               book->GetFileName() ? book->GetFileName() : "(null)");
    }
    DBG_LOG(&app_, msg);

    if (osGetTime() - start_ms >= budget_ms)
      break;
  }
}

size_t LibraryController::PauseBrowserJobs() {
  std::deque<app_job_t> kept;
  size_t removed = 0;
  while (!job_queue_.empty()) {
    const app_job_t job = job_queue_.front();
    job_queue_.pop_front();
    if (browser_job_queue_utils::IsHeavyBrowserJobType(
            job.type, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER)) {
      removed++;
      continue;
    }
    kept.push_back(job);
  }
  job_queue_.swap(kept);
  return removed;
}

void LibraryController::browser_handleevent() {
  // Re-apply browser layout in case another view reused/moved shared buttons.
  LayoutBrowserNavButtons(&app_);

  u32 keys = hidKeysDown();
#ifdef DSLIBRIS_DEBUG
  if (keys) {
    DBG_LOGF(&app_, "BROWSER handleevent keys=0x%08lx",
             (unsigned long)keys);
  }
#endif
  auto map_grid_nav = [&](u32 key_down, BrowserNavMove *move) -> bool {
    if (!move)
      return false;
    if (!app_.orientation) {
      // Turned Left (right-handed): rotate d-pad mapping so directional input
      // follows the visual page orientation.
      if (key_down & KEY_DOWN) {
        *move = BROWSER_NAV_RIGHT;
        return true;
      }
      if (key_down & KEY_UP) {
        *move = BROWSER_NAV_LEFT;
        return true;
      }
      if (key_down & KEY_LEFT) {
        *move = BROWSER_NAV_DOWN;
        return true;
      }
      if (key_down & KEY_RIGHT) {
        *move = BROWSER_NAV_UP;
        return true;
      }
      return false;
    }

    // Turned Right (left-handed): rotate d-pad mapping so directional intent
    // matches the on-screen grid orientation.
    if (key_down & KEY_DOWN) {
      *move = BROWSER_NAV_LEFT;
      return true;
    }
    if (key_down & KEY_UP) {
      *move = BROWSER_NAV_RIGHT;
      return true;
    }
    if (key_down & KEY_LEFT) {
      *move = BROWSER_NAV_UP;
      return true;
    }
    if (key_down & KEY_RIGHT) {
      *move = BROWSER_NAV_DOWN;
      return true;
    }
    return false;
  };
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y |
                           KEY_START | KEY_SELECT | KEY_UP | KEY_DOWN |
                           KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_CPAD_UP |
                           KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
  if (app_.IsBrowserWaitingInputRelease()) {
    if (hidKeysHeld() & release_mask)
      return;
    app_.SetBrowserWaitingInputRelease(false);
    return;
  }

  auto navigateSelection = [&](BrowserNavMove move) {
    if (app_.BookCount() <= 0)
      return;
    const int old_page_start = app_.GetBrowserPageStart();
    Book *old_selected = app_.GetSelectedBook();
    BrowserNavState state = {app_.GetBookIndex(app_.GetSelectedBook()),
                             app_.GetBrowserPageStart()};
    state = BrowserNavMoveSelection(state, app_.BookCount(), APP_BROWSER_BUTTON_COUNT,
                                    kBrowserGridCols, move);
    if (state.selected_index < 0 || state.selected_index >= app_.BookCount())
      return;
    app_.SetBrowserPageStart(state.page_start);
    app_.SetSelectedBook(app_.books[state.selected_index]);
    if (app_.GetBrowserPageStart() != old_page_start)
      LoadVisibleBrowserCoverCaches();
    if (app_.GetSelectedBook() != old_selected) {
      g_marquee.Reset();
      PrioritizeSelectedBookJobs(app_.GetSelectedBook());
      app_.SetBrowserLastInteractionMs(osGetTime());
    }
    app_.SetBrowserDirty(true);
  };

  BrowserNavMove nav_move = BROWSER_NAV_LEFT;
  const bool has_grid_nav = map_grid_nav(keys, &nav_move);

  if (keys & KEY_A) {
    app_.OpenBook();
  } else if (has_grid_nav) {
    navigateSelection(nav_move);
  } else if (keys & app_.key.l) {
    browser_prevpage();
  } else if (keys & app_.key.r) {
    browser_nextpage();
  }

  else if (keys & (KEY_SELECT | KEY_Y)) {
    app_.ShowSettingsView(false);
  }

  else if (keys & KEY_TOUCH) {
    auto hitsButtonAt = [&](Button &button, int px, int py, int slack) {
      if (slack <= 0)
        return button.EnclosesPoint((u16)px, (u16)py);
      for (int dy = -slack; dy <= slack; dy += slack) {
        for (int dx = -slack; dx <= slack; dx += slack) {
          int x = px + dx;
          int y = py + dy;
          if (x < 0 || y < 0 || x > 239 || y > 319)
            continue;
          if (button.EnclosesPoint((u16)x, (u16)y))
            return true;
        }
      }
      return false;
    };

    auto handleTouchAt = [&](int x, int y) -> bool {
      if (x < 0 || y < 0 || x > 239 || y > 319)
        return false;

      if (hitsButtonAt(app_.buttonnext, x, y, 4)) {
        browser_nextpage();
        return true;
      }
      if (hitsButtonAt(app_.buttonprev, x, y, 4)) {
        browser_prevpage();
        return true;
      }
      if (hitsButtonAt(app_.buttonprefs, x, y, 4)) {
        app_.ShowSettingsView(false);
        return true;
      }

      // Prefer coarse cell hit-test (cover + title/progress area):
      // single tap selects, tapping selected book opens.
      if (x >= kBrowserGridX0 && y >= kBrowserGridY0) {
        int col = (x - kBrowserGridX0) / kBrowserCellW;
        int row = (y - kBrowserGridY0) / kBrowserCellH;
        if (col >= 0 && col < kBrowserGridCols && row >= 0 &&
            row < kBrowserGridRows) {
          int page_idx = row * kBrowserGridCols + col;
          if (page_idx >= 0 && page_idx < APP_BROWSER_BUTTON_COUNT) {
            int book_idx = app_.GetBrowserPageStart() + page_idx;
            if (book_idx >= 0 && book_idx < app_.BookCount()) {
              if (app_.GetSelectedBook() == app_.books[book_idx]) {
                app_.OpenBook();
              } else {
                app_.SetSelectedBook(app_.books[book_idx]);
                g_marquee.Reset();
                PrioritizeSelectedBookJobs(app_.GetSelectedBook());
                app_.SetBrowserLastInteractionMs(osGetTime());
                app_.SetBrowserDirty(true);
              }
              return true;
            }
          }
        }
      }

      // Fallback to original cover hitboxes.
      for (int i = app_.GetBrowserPageStart();
           (i < app_.BookCount()) &&
           (i < app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT);
           i++) {
        if (hitsButtonAt(*app_.buttons[i], x, y, 4)) {
          if (app_.GetSelectedBook() == app_.books[i]) {
            app_.OpenBook();
          } else {
            app_.SetSelectedBook(app_.books[i]);
            g_marquee.Reset();
            PrioritizeSelectedBookJobs(app_.GetSelectedBook());
            app_.SetBrowserLastInteractionMs(osGetTime());
            app_.SetBrowserDirty(true);
          }
          return true;
        }
      }
      return false;
    };

    touchPosition mapped = app_.TouchRead();
    handleTouchAt((int)mapped.px, (int)mapped.py);
  }
}

void LibraryController::browser_init(void) {
  for (int i = 0; i < app_.BookCount(); i++) {
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kBrowserGridCols;
    int row = page_idx / kBrowserGridCols;

    app_.buttons.push_back(new Button());
    app_.buttons[i]->Init(app_.ts);
    app_.buttons[i]->Resize(kBrowserCoverW + 4, kBrowserCoverH + 4);
    app_.buttons[i]->Move(kBrowserGridX0 + col * kBrowserCellW,
                     kBrowserGridY0 + row * kBrowserCellH);

    // Cover extraction moved to browser_draw to avoid freezing at startup
    app_.buttons[i]->SetLabel1(std::string(""));

  }

  app_.buttonprev.Init(app_.ts);
  app_.buttonnext.Init(app_.ts);
  app_.buttonprefs.Init(app_.ts);
  LayoutBrowserNavButtons(&app_);

  if (!app_.GetSelectedBook()) {
    app_.SetBrowserPageStart(0);
    app_.SetSelectedBook(app_.books[0]);
  } else {
    app_.SetBrowserPageStart(
        (app_.GetBookIndex(app_.GetSelectedBook()) / APP_BROWSER_BUTTON_COUNT) *
        APP_BROWSER_BUTTON_COUNT);
  }
  g_marquee.Reset();
  PrioritizeSelectedBookJobs(app_.GetSelectedBook());
  app_.SetBrowserLastInteractionMs(osGetTime());
  LoadVisibleBrowserCoverCaches();
}

void LibraryController::browser_nextpage() {
  if (app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT < app_.BookCount()) {
    app_.SetBrowserPageStart(app_.GetBrowserPageStart() +
                             APP_BROWSER_BUTTON_COUNT);
    app_.SetSelectedBook(app_.books[app_.GetBrowserPageStart()]);
    g_marquee.Reset();
    PrioritizeSelectedBookJobs(app_.GetSelectedBook());
    app_.SetBrowserLastInteractionMs(osGetTime());
    LoadVisibleBrowserCoverCaches();
    app_.SetBrowserDirty(true);
  }
}

void LibraryController::browser_prevpage() {
  if (app_.GetBrowserPageStart() >= APP_BROWSER_BUTTON_COUNT) {
    app_.SetBrowserPageStart(app_.GetBrowserPageStart() -
                             APP_BROWSER_BUTTON_COUNT);
    app_.SetSelectedBook(
        app_.books[app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT - 1]);
    g_marquee.Reset();
    PrioritizeSelectedBookJobs(app_.GetSelectedBook());
    app_.SetBrowserLastInteractionMs(osGetTime());
    LoadVisibleBrowserCoverCaches();
    app_.SetBrowserDirty(true);
  }
}

void LibraryController::browser_draw(void) {
  // Keep footer controls stable after view switches.
  LayoutBrowserNavButtons(&app_);

  // save state
  int colorMode = app_.ts->GetColorMode();
  u16 *screen = app_.ts->GetScreen();
  int style = app_.ts->GetStyle();
  int savedPixelSize = app_.ts->pixelsize;

  app_.ts->SetScreen(app_.ts->screenleft);
  app_.ts->SetColorMode(0);
  app_.ts->SetStyle(TEXT_STYLE_BROWSER);
  app_.ts->PrintSplash(app_.ts->screenleft);
  app_.ts->SetPixelSize(8);
  {
    char versionMsg[16];
    snprintf(versionMsg, sizeof(versionMsg), "v%s", VERSION);
    const int versionWidth =
        app_.ts->GetStringWidth(versionMsg, TEXT_STYLE_BROWSER);
    int versionX = (240 - versionWidth) / 2;
    if (versionX < 0)
      versionX = 0;
    app_.ts->SetPen(versionX, 397);
    app_.ts->PrintString(versionMsg);
  }

  app_.ts->SetScreen(app_.ts->screenright);
  app_.ts->SetColorMode(0);
  app_.ts->ClearScreen();
  app_.DrawBottomGradientBackground();

  for (int i = app_.GetBrowserPageStart();
       (i < app_.BookCount()) &&
       (i < app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT); i++) {
    app_.buttons[i]->Draw(app_.ts->screenright, app_.books[i] == app_.GetSelectedBook());

    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kBrowserGridCols;
    int row = page_idx / kBrowserGridCols;
    int btnX = kBrowserGridX0 + col * kBrowserCellW;
    int btnY = kBrowserGridY0 + row * kBrowserCellH;

    if (app_.books[i]->coverPixels) {
      int cx = btnX + 2 + (kBrowserCoverW - app_.books[i]->coverWidth) / 2;
      int cy = btnY + 2 + (kBrowserCoverH - app_.books[i]->coverHeight) / 2;
      int w = app_.ts->display.height; // buffer stride
      app_.ts->MarkScreenDirtyRect(app_.ts->screenright, cx, cy,
                              cx + app_.books[i]->coverWidth,
                              cy + app_.books[i]->coverHeight);
      for (int py = 0; py < app_.books[i]->coverHeight && (cy + py) < 320; py++) {
        for (int px = 0; px < app_.books[i]->coverWidth && (cx + px) < 240; px++) {
          app_.ts->screenright[(cy + py) * w + (cx + px)] =
              app_.books[i]->coverPixels[py * app_.books[i]->coverWidth + px];
        }
      }
    }

    if (app_.books[i] == app_.GetSelectedBook()) {
      app_.ts->DrawRect(btnX - 2, btnY - 2, btnX + kBrowserCellW + 2,
                   btnY + kBrowserCellH + 2,
                   0xF800);
      app_.ts->DrawRect(btnX - 3, btnY - 3, btnX + kBrowserCellW + 3,
                   btnY + kBrowserCellH + 3, 0xF800);
      app_.ts->SetStyle(TEXT_STYLE_BOLD);
    } else {
      app_.ts->SetStyle(TEXT_STYLE_REGULAR);
    }

    // Draw filename (not EPUB metadata title):
    //  - with cover: below thumbnail (single line)
    //  - without cover: wrapped inside thumbnail rectangle
    app_.ts->SetPixelSize(10);
    std::string display_name = BuildBrowserDisplayName(app_.books[i]);
    LogUtf8StageOnce(app_.books[i], "draw_label", display_name);
    if (app_.books[i]->coverPixels) {
        if (!display_name.empty()) {
        LogUtf8StageOnce(app_.books[i], "draw_label_cut", display_name);
        const char *dname = display_name.c_str();
        u8 cur_style = (u8)app_.ts->GetStyle();
        int full_w = (int)app_.ts->GetStringWidth(dname, cur_style);
        Book *book_i = app_.books[i];
        Book *sel = app_.GetSelectedBook();
        bool overflows = full_w > kBrowserCellW;
        bool is_selected = book_i == sel;

        int saved_margin_right = app_.ts->margin.right;
        bool saved_clip = app_.ts->IsClipToContentEnabled();
        bool saved_wrap = app_.ts->IsAutoWrapEnabled();

        app_.ts->margin.right = app_.ts->display.width - (btnX + kBrowserCellW);
        app_.ts->SetClipToContentEnabled(true);
        app_.ts->SetAutoWrapEnabled(false);

        if (is_selected && overflows) {
          const int glyph_top = btnY + kBrowserTitleOffsetY - app_.ts->GetHeight();
          const int title_y = (glyph_top >= 0) ? glyph_top : 0;
          const int sh = app_.ts->GetHeight() + 2;
          const int sw = full_w;
          const int fb_stride = app_.ts->display.height;

          if (g_marquee.book != book_i) {
            const int vis_w = (btnX + kBrowserCellW <= app_.ts->display.width)
                                  ? kBrowserCellW
                                  : app_.ts->display.width - btnX;

            g_marquee.Reset();
            g_marquee.book    = book_i;
            g_marquee.strip_w = sw;
            g_marquee.strip_h = sh;
            g_marquee.strip_x = btnX;
            g_marquee.strip_y = title_y;
            g_marquee.blit_w  = vis_w;
            g_marquee.strip   = new u16[sw * sh];
            g_marquee.bg_strip = new u16[vis_w * sh];

            for (int r = 0; r < sh; r++) {
              const u16 *src = app_.ts->screenright + (title_y + r) * fb_stride + btnX;
              u16 *dst = g_marquee.bg_strip + r * vis_w;
              memcpy(dst, src, vis_w * sizeof(u16));
            }

            u16 *saved_screen = app_.ts->GetScreen();
            int saved_mr      = app_.ts->margin.right;
            int saved_ml      = app_.ts->margin.left;

            const int off_fb_stride = app_.ts->display.height;
            const int cap_w = (sw < app_.ts->display.width) ? sw : app_.ts->display.width;
            for (int r = 0; r < sh; r++)
              for (int c = 0; c < cap_w; c++)
                app_.ts->offscreen[(title_y + r) * off_fb_stride + c] = 0xFFFF;

            app_.ts->SetScreen(app_.ts->offscreen);
            app_.ts->margin.left  = 0;
            app_.ts->margin.right = 0;
            app_.ts->SetPen(0, btnY + kBrowserTitleOffsetY);
            app_.ts->PrintString(dname, cur_style);
            app_.ts->margin.left  = saved_ml;
            app_.ts->margin.right = saved_mr;
            app_.ts->SetScreen(saved_screen);

            for (int row = 0; row < sh; row++) {
              const u16 *src = app_.ts->offscreen + (title_y + row) * fb_stride;
              u16 *dst = g_marquee.strip + row * sw;
              int copy_w = (sw < app_.ts->display.width) ? sw : app_.ts->display.width;
              memcpy(dst, src, copy_w * sizeof(u16));
            }
            g_marquee.scroll_offset = 0;
            g_marquee.scroll_timer  = 0;
            g_marquee.active = true;
          }
        } else if (is_selected && !overflows) {
          g_marquee.Reset();
        }

        if (!(is_selected && overflows)) {
          app_.ts->SetPen(btnX, btnY + kBrowserTitleOffsetY);
          app_.ts->PrintString(dname, cur_style);
        }

        app_.ts->margin.right = saved_margin_right;
        app_.ts->SetClipToContentEnabled(saved_clip);
        app_.ts->SetAutoWrapEnabled(saved_wrap);
      }
    } else {
      LogUtf8StageOnce(app_.books[i], "draw_label_wrap", display_name);
      DrawWrappedTitleInsideCover(app_.ts, display_name, btnX + 2, btnY + 2,
                                  kBrowserCoverW, kBrowserCoverH,
                                  TEXT_STYLE_BROWSER);
    }

    int pos = app_.books[i]->GetPosition();
    char msg[16];
    if (pos > 0)
      snprintf(msg, sizeof(msg), "Pg %d", pos + 1);
    else
      snprintf(msg, sizeof(msg), "NEW");
    app_.ts->SetPen(btnX, btnY + kBrowserProgressOffsetY);
    app_.ts->PrintString(msg);
  }

  app_.ts->SetPixelSize(savedPixelSize);

  if (app_.GetBrowserPageStart() >= APP_BROWSER_BUTTON_COUNT)
    app_.buttonprev.Draw(app_.ts->screenright, false);
  if (app_.BookCount() > app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT)
    app_.buttonnext.Draw(app_.ts->screenright, false);

  app_.buttonprefs.Draw(app_.ts->screenright, false);

  if (app_.BookCount() > APP_BROWSER_BUTTON_COUNT) {
    int currentPage = (app_.GetBrowserPageStart() / APP_BROWSER_BUTTON_COUNT) + 1;
    int totalPages =
        (app_.BookCount() + APP_BROWSER_BUTTON_COUNT - 1) / APP_BROWSER_BUTTON_COUNT;
    char pageMsg[32];
    snprintf(pageMsg, sizeof(pageMsg), "%d/%d", currentPage, totalPages);
    app_.ts->SetPixelSize(8);
    app_.ts->SetPen(112, kBrowserFooterY + 3);
    app_.ts->PrintString(pageMsg);
    app_.ts->SetPixelSize(savedPixelSize);
  }

  app_.ts->SetColorMode(colorMode);
  app_.ts->SetScreen(screen);
  app_.ts->SetStyle(style);

  app_.SetBrowserDirty(false);
}

void LibraryController::browser_tick_marquee() {
  if (!g_marquee.active || !g_marquee.strip || !g_marquee.bg_strip)
    return;

  const int kPauseFrames = 60;
  const int scroll_max = g_marquee.strip_w - kBrowserCellW;

  if (g_marquee.scroll_timer < kPauseFrames) {
    g_marquee.scroll_timer++;
  } else if (g_marquee.scroll_offset >= scroll_max) {
    if (g_marquee.end_timer < kPauseFrames) {
      g_marquee.end_timer++;
    } else {
      g_marquee.scroll_offset = 0;
      g_marquee.scroll_timer  = 0;
      g_marquee.end_timer     = 0;
    }
  } else {
    g_marquee.scroll_offset++;
  }

  const int fb_stride = app_.ts->display.height;
  const int tx  = g_marquee.strip_x;
  const int ty  = g_marquee.strip_y;
  const int sh  = g_marquee.strip_h;
  const int sw  = g_marquee.strip_w;
  const int off = g_marquee.scroll_offset;
  const int bw  = g_marquee.blit_w;

  int blit_w = bw;
  if (blit_w + off > sw)
    blit_w = sw - off;
  if (blit_w <= 0)
    return;

  for (int row = 0; row < sh; row++) {
    if (ty + row >= app_.ts->display.height)
      break;
    u16 *dst = app_.ts->screenright + (ty + row) * fb_stride + tx;
    memcpy(dst, g_marquee.bg_strip + row * bw, bw * sizeof(u16));
    const u16 *src = g_marquee.strip + row * sw + off;
    for (int col = 0; col < blit_w; col++) {
      if (src[col] != 0xFFFF)
        dst[col] = src[col];
    }
  }

  app_.ts->MarkScreenDirtyRect(app_.ts->screenright,
                               tx, ty,
                               tx + bw, ty + sh);
}
