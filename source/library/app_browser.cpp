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
#include "ui/button.h"
#include "menus/chapter_menu.h"
#include "debug_log.h"
#include "formats/epub/epub.h"
#include "formats/fb2/fb2.h"
#include "main.h"
#include "formats/mobi/mobi.h"
#include "parse.h"
#include "string_utils.h"
#include "ui/text.h"
#include "shared/utf8_utils.h"
#include "version.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

namespace {

static const char *kCoverCacheBaseDir = "sdmc:/3ds/3dslibris/cache";
static const char *kCoverCacheDir = "sdmc:/3ds/3dslibris/cache/covers";
static const char *kCoverCacheMagic = "CVR2";
static const size_t kCoverCacheMaxFiles = 256;
static const size_t kCoverCacheMaxBytes = 6 * 1024 * 1024;
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

static void LayoutBrowserNavButtons(App *app) {
  if (!app)
    return;
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
  while ((remaining_count > kCoverCacheMaxFiles ||
          total_bytes > kCoverCacheMaxBytes) &&
         !entries.empty()) {
    const CoverCacheEntry &oldest = entries.front();
    remove(oldest.path.c_str());
    if (remaining_count > 0)
      remaining_count--;
    if (oldest.size <= total_bytes)
      total_bytes -= oldest.size;
    else
      total_bytes = 0;
    entries.pop_front();
  }
}

static void EnsureCoverCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kCoverCacheBaseDir, 0777);
  mkdir(kCoverCacheDir, 0777);
  PruneCoverCache(true);
  initialized = true;
}

static std::string BuildCoverCachePath(const std::string &book_path) {
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

  char out[160];
  snprintf(out, sizeof(out), "%s/%016llx.cvr", kCoverCacheDir,
           (unsigned long long)h);
  return std::string(out);
}

static bool TryLoadCoverCache(Book *book, const std::string &book_path) {
  if (!book)
    return false;
  EnsureCoverCacheDirs();

  std::string cache_path = BuildCoverCachePath(book_path);
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;

  u8 header[8];
  bool ok = fread(header, 1, sizeof(header), fp) == sizeof(header);
  // Ignore thumbnails written by older extractors once the cache format/magic
  // changes, so stale covers do not survive heuristic fixes.
  if (!ok || memcmp(header, kCoverCacheMagic, 4) != 0) {
    fclose(fp);
    return false;
  }

  u16 w = (u16)header[4] | ((u16)header[5] << 8);
  u16 h = (u16)header[6] | ((u16)header[7] << 8);
  if (w == 0 || h == 0 || w > kCoverThumbMaxW || h > kCoverThumbMaxH) {
    fclose(fp);
    return false;
  }

  size_t count = (size_t)w * (size_t)h;
  std::vector<u16> pixels(count);
  if (fread(pixels.data(), sizeof(u16), count, fp) != count) {
    fclose(fp);
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
  std::string cache_path = BuildCoverCachePath(book_path);
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
  if (ok)
    PruneCoverCache(false);
  return ok;
}

static bool LooksLikeValidUtf8(const std::string &s) {
  return utf8_utils::IsValidUtf8(s);
}

static std::string LegacyBytesToUtf8(const std::string &in) {
  return utf8_utils::DecodeCp1252ToUtf8(in);
}

static bool TryRepairMojibakeUtf8(const std::string &in, std::string *out) {
  return utf8_utils::TryRepairMojibakeUtf8(in, out);
}

static bool TryRepairFullwidthByteMojibake(const std::string &in,
                                           std::string *out) {
  return utf8_utils::TryRepairFullwidthByteMojibake(in, out);
}

static std::string ComposeLatinCombiningMarks(const std::string &in) {
  return utf8_utils::ComposeLatinCombiningMarks(in);
}

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
  if (!book || !book->GetApp() || !stage)
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
           (unsigned)value.size(), LooksLikeValidUtf8(value) ? 1 : 0,
           bytes.c_str(), clipped.c_str());
  DBG_LOG(book->GetApp(), msg);
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
  if (TryRepairFullwidthByteMojibake(s, &repaired)) {
    s = repaired;
    if (repaired_fullwidth)
      *repaired_fullwidth = true;
  }

  if (!LooksLikeValidUtf8(s)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    return LegacyBytesToUtf8(s);
  }

  if (TryRepairMojibakeUtf8(s, &repaired)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    s = repaired;
  }

  std::string composed = ComposeLatinCombiningMarks(s);
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

  std::string raw = book->GetFileName() ? book->GetFileName() : "";
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

  size_t dot = normalized.find_last_of('.');
  if (dot != std::string::npos)
    normalized = normalized.substr(0, dot);

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
    if ((c & 0x80) == 0x00)
      step = 1;
    else if ((c & 0xE0) == 0xC0)
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

} // namespace

bool App::HasQueuedJob(app_job_type_t type, Book *book) const {
  for (const auto &job : job_queue) {
    if (job.type == type && job.book == book)
      return true;
  }
  return false;
}

void App::EnqueueJob(app_job_type_t type, Book *book) {
  if (!book)
    return;
  if (HasQueuedJob(type, book))
    return;
  app_job_t job;
  job.type = type;
  job.book = book;
  job_queue.push_back(job);
}

void App::QueueBookWarmup(Book *book) {
  if (!book || book->coverPixels || book->coverTried)
    return;

  if (book->format == FORMAT_EPUB) {
    if (!book->metadataIndexTried)
      EnqueueJob(APP_JOB_INDEX_METADATA, book);
    EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_XHTML) {
    if (HasExtCI(book->GetFileName(), ".fb2") ||
        HasExtCI(book->GetFileName(), ".mobi")) {
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
    }
  }
}

void App::QueueTocResolve(Book *book) {
  if (!book || book->format != FORMAT_EPUB || book->tocResolveTried)
    return;
  EnqueueJob(APP_JOB_RESOLVE_TOC, book);
}

void App::ProcessJobs(u32 budget_ms) {
  if (job_queue.empty())
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
  while (!job_queue.empty()) {
    app_job_t job = job_queue.front();
    job_queue.pop_front();
    if (!job.book)
      continue;

    Book *book = job.book;
    int rc = 0;
    u64 t0 = osGetTime();

    if (job.type == APP_JOB_INDEX_METADATA) {
      if (book->format == FORMAT_EPUB && !book->metadataIndexTried) {
        rc = book->Index();
        if (rc != 0)
          book->coverTried = true;
        browser_.view_dirty = true;
      }
    } else if (job.type == APP_JOB_EXTRACT_COVER) {
      if (!book->coverPixels && !book->coverTried) {
        std::string path = bookdir + "/" + book->GetFileName();
        if (book->format == FORMAT_EPUB) {
          if (!book->metadataIndexTried) {
            EnqueueJob(APP_JOB_INDEX_METADATA, book);
            EnqueueJob(APP_JOB_EXTRACT_COVER, book);
          } else {
            if (!book->coverImagePath.empty()) {
              rc = epub_extract_cover(book, path);
              if (rc == 0 && book->coverPixels) {
                SaveCoverCache(book, path);
              }
            }
            book->coverTried = true;
            browser_.view_dirty = true;
          }
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".fb2")) {
          rc = fb2_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
          }
          book->coverTried = true;
          browser_.view_dirty = true;
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".mobi")) {
          rc = mobi_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
          }
          book->coverTried = true;
          browser_.view_dirty = true;
        }
      }
    } else if (job.type == APP_JOB_RESOLVE_TOC) {
      if (book->format == FORMAT_EPUB && !book->tocResolveTried) {
        std::string path = bookdir + "/" + book->GetFileName();
        rc = epub_resolve_toc(book, path);
        book->tocResolveTried = true;
        book->tocResolved = (rc == 0);
        if (rc == 0 && mode_ == AppMode::Chapters && book == bookcurrent_ &&
            chaptermenu) {
          chaptermenu->Init();
          chaptermenu->SetDirty(true);
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
    DBG_LOG(this, msg);

    if (osGetTime() - start_ms >= budget_ms)
      break;
  }
}

void App::browser_handleevent() {
  // Re-apply browser layout in case another view reused/moved shared buttons.
  LayoutBrowserNavButtons(this);

  u32 keys = hidKeysDown();
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y |
                           KEY_START | KEY_SELECT | KEY_UP | KEY_DOWN |
                           KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_CPAD_UP |
                           KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
  if (browser_.wait_input_release) {
    if (hidKeysHeld() & release_mask)
      return;
    browser_.wait_input_release = false;
    return;
  }

  auto navigateSelection = [&](BrowserNavMove move) {
    if (BookCount() <= 0)
      return;
    BrowserNavState state = {GetBookIndex(browser_.selected_book),
                             browser_.page_start};
    state = BrowserNavMoveSelection(state, BookCount(), APP_BROWSER_BUTTON_COUNT,
                                    kBrowserGridCols, move);
    if (state.selected_index < 0 || state.selected_index >= BookCount())
      return;
    browser_.page_start = state.page_start;
    browser_.selected_book = books[state.selected_index];
    browser_.view_dirty = true;
  };

  if (keys & KEY_A) {
    // Open selected book with the primary confirm button.
    OpenBook();
  }
  else if (keys & key.left) {
    navigateSelection(BROWSER_NAV_LEFT);
  } else if (keys & key.right) {
    navigateSelection(BROWSER_NAV_RIGHT);
  } else if (keys & key.up) {
    navigateSelection(BROWSER_NAV_UP);
  } else if (keys & key.down) {
    navigateSelection(BROWSER_NAV_DOWN);
  } else if (keys & key.l) {
    browser_prevpage();
  } else if (keys & key.r) {
    browser_nextpage();
  }

  else if (keys & (KEY_SELECT | KEY_Y)) {
    ShowSettingsView(false);
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

      if (hitsButtonAt(buttonnext, x, y, 4)) {
        browser_nextpage();
        return true;
      }
      if (hitsButtonAt(buttonprev, x, y, 4)) {
        browser_prevpage();
        return true;
      }
      if (hitsButtonAt(buttonprefs, x, y, 4)) {
        ShowSettingsView(false);
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
            int book_idx = browser_.page_start + page_idx;
            if (book_idx >= 0 && book_idx < BookCount()) {
              if (browser_.selected_book == books[book_idx]) {
                OpenBook();
              } else {
                browser_.selected_book = books[book_idx];
                browser_.view_dirty = true;
              }
              return true;
            }
          }
        }
      }

      // Fallback to original cover hitboxes.
      for (int i = browser_.page_start;
           (i < BookCount()) &&
           (i < browser_.page_start + APP_BROWSER_BUTTON_COUNT);
           i++) {
        if (hitsButtonAt(*buttons[i], x, y, 4)) {
          if (browser_.selected_book == books[i]) {
            OpenBook();
          } else {
            browser_.selected_book = books[i];
            browser_.view_dirty = true;
          }
          return true;
        }
      }
      return false;
    };

    touchPosition mapped = TouchRead();
    handleTouchAt((int)mapped.px, (int)mapped.py);
  }
}

void App::browser_init(void) {
  for (int i = 0; i < BookCount(); i++) {
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kBrowserGridCols;
    int row = page_idx / kBrowserGridCols;

    buttons.push_back(new Button());
    buttons[i]->Init(ts);
    // Button is the cover area - portrait orientation
    buttons[i]->Resize(kBrowserCoverW + 4, kBrowserCoverH + 4);
    buttons[i]->Move(kBrowserGridX0 + col * kBrowserCellW,
                     kBrowserGridY0 + row * kBrowserCellH);

    // Cover extraction moved to browser_draw to avoid freezing at startup

    // In browser_draw we render fallback title manually for no-cover books.
    buttons[i]->SetLabel1(std::string(""));

    // Fast path: preload thumbnail from on-disk cache (if available).
    std::string path = bookdir + "/" + books[i]->GetFileName();
    if (TryLoadCoverCache(books[i], path)) {
      books[i]->coverTried = true;
      if (books[i]->format == FORMAT_EPUB) {
        // With a valid cached cover, skip metadata indexing in browser.
        books[i]->metadataIndexTried = true;
        books[i]->metadataIndexed = true;
      }
    }
  }

  buttonprev.Init(ts);
  buttonnext.Init(ts);
  buttonprefs.Init(ts);
  LayoutBrowserNavButtons(this);

  if (!browser_.selected_book) {
    browser_.page_start = 0;
    browser_.selected_book = books[0];
  } else {
    browser_.page_start =
        (GetBookIndex(browser_.selected_book) / APP_BROWSER_BUTTON_COUNT) *
        APP_BROWSER_BUTTON_COUNT;
  }
}

void App::browser_nextpage() {
  if (browser_.page_start + APP_BROWSER_BUTTON_COUNT < BookCount()) {
    browser_.page_start += APP_BROWSER_BUTTON_COUNT;
    browser_.selected_book = books[browser_.page_start];
    browser_.view_dirty = true;
  }
}

void App::browser_prevpage() {
  if (browser_.page_start >= APP_BROWSER_BUTTON_COUNT) {
    browser_.page_start -= APP_BROWSER_BUTTON_COUNT;
    browser_.selected_book =
        books[browser_.page_start + APP_BROWSER_BUTTON_COUNT - 1];
    browser_.view_dirty = true;
  }
}

void App::browser_draw(void) {
  // Keep footer controls stable after view switches.
  LayoutBrowserNavButtons(this);

  // save state
  int colorMode = ts->GetColorMode();
  u16 *screen = ts->GetScreen();
  int style = ts->GetStyle();
  int savedPixelSize = ts->pixelsize;

  ts->SetScreen(ts->screenleft);
  ts->SetColorMode(0);
  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->PrintSplash(ts->screenleft);
  ts->SetPixelSize(8);
  {
    char versionMsg[16];
    snprintf(versionMsg, sizeof(versionMsg), "v%s", VERSION);
    const int versionWidth =
        ts->GetStringWidth(versionMsg, TEXT_STYLE_BROWSER);
    int versionX = (240 - versionWidth) / 2;
    if (versionX < 0)
      versionX = 0;
    ts->SetPen(versionX, 397);
    ts->PrintString(versionMsg);
  }

  ts->SetScreen(ts->screenright);
  ts->SetColorMode(0); // Normal for browser text
  ts->ClearScreen();
  DrawBottomGradientBackground();
  if (browser_.selected_book)
    QueueBookWarmup(browser_.selected_book);

  for (int i = browser_.page_start;
       (i < BookCount()) &&
       (i < browser_.page_start + APP_BROWSER_BUTTON_COUNT); i++) {
    QueueBookWarmup(books[i]);
    buttons[i]->Draw(ts->screenright, books[i] == browser_.selected_book);

    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kBrowserGridCols;
    int row = page_idx / kBrowserGridCols;
    int btnX = kBrowserGridX0 + col * kBrowserCellW;
    int btnY = kBrowserGridY0 + row * kBrowserCellH;

    if (books[i]->coverPixels) {
      int cx = btnX + 2 + (kBrowserCoverW - books[i]->coverWidth) / 2;
      int cy = btnY + 2 + (kBrowserCoverH - books[i]->coverHeight) / 2;
      int w = ts->display.height; // buffer stride
      for (int py = 0; py < books[i]->coverHeight && (cy + py) < 320; py++) {
        for (int px = 0; px < books[i]->coverWidth && (cx + px) < 240; px++) {
          ts->screenright[(cy + py) * w + (cx + px)] =
              books[i]->coverPixels[py * books[i]->coverWidth + px];
        }
      }
    }

    if (books[i] == browser_.selected_book) {
      ts->DrawRect(btnX - 2, btnY - 2, btnX + kBrowserCellW + 2,
                   btnY + kBrowserCellH + 2,
                   0xF800); // Red thick outer bounding box
      ts->DrawRect(btnX - 3, btnY - 3, btnX + kBrowserCellW + 3,
                   btnY + kBrowserCellH + 3, 0xF800);
      ts->SetStyle(TEXT_STYLE_BOLD);
    } else {
      ts->SetStyle(TEXT_STYLE_REGULAR);
    }

    // Draw filename (not EPUB metadata title):
    //  - with cover: below thumbnail (single line)
    //  - without cover: wrapped inside thumbnail rectangle
    ts->SetPixelSize(10);
    std::string display_name = BuildBrowserDisplayName(books[i]);
    LogUtf8StageOnce(books[i], "draw_label", display_name);
    if (books[i]->coverPixels) {
      if (!display_name.empty()) {
        char truncTitle[20];
        size_t bytes = Utf8BytesForCharCount(display_name.c_str(), 19);
        if (bytes > 19)
          bytes = 19;
        memcpy(truncTitle, display_name.c_str(), bytes);
        truncTitle[bytes] = '\0';
        truncTitle[19] = '\0';
        LogUtf8StageOnce(books[i], "draw_label_cut", std::string(truncTitle));
        ts->SetPen(btnX, btnY + kBrowserTitleOffsetY);
        ts->PrintString(truncTitle);
      }
    } else {
      LogUtf8StageOnce(books[i], "draw_label_wrap", display_name);
      DrawWrappedTitleInsideCover(ts, display_name, btnX + 2, btnY + 2,
                                  kBrowserCoverW, kBrowserCoverH,
                                  TEXT_STYLE_BROWSER);
    }

    // Draw progress indicator
    int pos = books[i]->GetPosition();
    char msg[16];
    if (pos > 0)
      snprintf(msg, sizeof(msg), "Pg %d", pos + 1);
    else
      snprintf(msg, sizeof(msg), "NEW");
    ts->SetPen(btnX, btnY + kBrowserProgressOffsetY);
    ts->PrintString(msg);
  }

  ts->SetPixelSize(savedPixelSize);

  // Navigation buttons at the bottom
  if (browser_.page_start >= APP_BROWSER_BUTTON_COUNT)
    buttonprev.Draw(ts->screenright, false);
  if (BookCount() > browser_.page_start + APP_BROWSER_BUTTON_COUNT)
    buttonnext.Draw(ts->screenright, false);

  buttonprefs.Draw(ts->screenright, false);

  // Pagination indicator
  if (BookCount() > APP_BROWSER_BUTTON_COUNT) {
    int currentPage = (browser_.page_start / APP_BROWSER_BUTTON_COUNT) + 1;
    int totalPages =
        (BookCount() + APP_BROWSER_BUTTON_COUNT - 1) / APP_BROWSER_BUTTON_COUNT;
    char pageMsg[32];
    snprintf(pageMsg, sizeof(pageMsg), "%d/%d", currentPage, totalPages);
    ts->SetPixelSize(8);
    ts->SetPen(112, kBrowserFooterY + 3);
    ts->PrintString(pageMsg);
    ts->SetPixelSize(savedPixelSize);
  }

  // restore state
  ts->SetColorMode(colorMode);
  ts->SetScreen(screen);
  ts->SetStyle(style);

  browser_.view_dirty = false;
}
