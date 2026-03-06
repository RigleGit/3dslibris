/*
    3dslibris - app_browser.cpp
    Adapted from dslibris for Nintendo 3DS.
*/

#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <set>

#include <3ds.h>

#include "book.h"
#include "button.h"
#include "chapter_menu.h"
#include "epub.h"
#include "fb2.h"
#include "main.h"
#include "parse.h"
#include "text.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

namespace {

static const char *kCoverCacheBaseDir = "sdmc:/3ds/3dslibris/cache";
static const char *kCoverCacheDir = "sdmc:/3ds/3dslibris/cache/covers";
static const int kCoverThumbMaxW = 85;
static const int kCoverThumbMaxH = 115;
static const int kBrowserGridCols = 2;
static const int kBrowserGridRows = 2;
static const int kBrowserCoverW = 85;
static const int kBrowserCoverH = 115;
static const int kBrowserCellW = 115;
// Reserve a dedicated footer strip for browser buttons.
static const int kBrowserCellH = 132;
static const int kBrowserGridX0 = 5;
static const int kBrowserGridY0 = 3;
static const int kBrowserFooterY = 302;

static std::string TrimSpaces(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && s[start] == ' ')
    start++;
  size_t end = s.size();
  while (end > start && s[end - 1] == ' ')
    end--;
  return s.substr(start, end - start);
}

static bool HasExtCI(const char *name, const char *ext) {
  if (!name || !ext)
    return false;
  size_t nlen = strlen(name);
  size_t elen = strlen(ext);
  if (elen == 0 || nlen < elen)
    return false;
  return strcasecmp(name + nlen - elen, ext) == 0;
}

static uint64_t Fnv1a64(const std::string &s) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < s.size(); i++) {
    hash ^= (uint8_t)s[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static void EnsureCoverCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kCoverCacheBaseDir, 0777);
  mkdir(kCoverCacheDir, 0777);
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
  if (!ok || memcmp(header, "CVR1", 4) != 0) {
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
  memcpy(header, "CVR1", 4);
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
  return ok;
}

static bool LooksLikeValidUtf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    if (i + need >= s.size())
      return false;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80)
        return false;
    }
    i += need + 1;
  }
  return true;
}

static std::string LegacyBytesToUtf8(const std::string &in) {
  // Decode legacy single-byte strings as Windows-1252 (superset commonly used
  // by desktop zippers/file systems for EPUB filenames).
  static const u16 cp1252_map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  std::string out;
  out.reserve(in.size() * 3);

  auto appendUcs = [&](u32 cp) {
    if (cp <= 0x7F) {
      out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  };

  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      appendUcs(c);
      continue;
    }
    if (c >= 0x80 && c <= 0x9F) {
      u16 mapped = cp1252_map[c - 0x80];
      if (mapped != 0x0000)
        appendUcs(mapped);
      else
        appendUcs('?');
      continue;
    }
    appendUcs(c); // 0xA0..0xFF same as Latin-1
  }
  return out;
}

static bool TryRepairMojibakeUtf8(const std::string &in, std::string *out) {
  if (!out)
    return false;

  // Only attempt this conversion when the string actually looks mojibake-ish
  // (e.g. visible "Ã"/"Â"). Applying it unconditionally corrupts valid UTF-8
  // accents like "í" (C3 AD).
  bool has_mojibake_markers = false;
  for (size_t i = 0; i + 1 < in.size(); i++) {
    unsigned char c0 = (unsigned char)in[i];
    unsigned char c1 = (unsigned char)in[i + 1];
    if (c0 == 0xC3 && (c1 == 0x82 || c1 == 0x83)) {
      has_mojibake_markers = true; // "Â" / "Ã"
      break;
    }
  }
  if (!has_mojibake_markers)
    return false;

  // Repair common "UTF-8 bytes decoded as Latin-1 then re-encoded as UTF-8"
  // mojibake patterns.
  std::string collapsed;
  collapsed.reserve(in.size());

  bool changed = false;
  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      collapsed.push_back((char)c);
      i++;
      continue;
    }

    if (i + 1 >= in.size())
      return false;

    unsigned char c2 = (unsigned char)in[i + 1];
    if (c == 0xC2 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)c2);
      changed = true;
      i += 2;
      continue;
    }
    if (c == 0xC3 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)(c2 + 0x40));
      changed = true;
      i += 2;
      continue;
    }

    // Keep any valid UTF-8 sequence untouched so mixed strings
    // (partially mojibake, partially proper UTF-8) are still repairable.
    size_t step = 0;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;

    if (step > 1 && i + step <= in.size()) {
      bool ok = true;
      for (size_t j = 1; j < step; j++) {
        unsigned char cc = (unsigned char)in[i + j];
        if ((cc & 0xC0) != 0x80) {
          ok = false;
          break;
        }
      }
      if (ok) {
        collapsed.append(in, i, step);
        i += step;
        continue;
      }
    }

    // Unknown non-ASCII byte: keep as-is and continue.
    collapsed.push_back((char)c);
    i++;
  }

  if (!changed)
    return false;

  if (!LooksLikeValidUtf8(collapsed)) {
    // Collapsed bytes may now be legacy single-byte text.
    collapsed = LegacyBytesToUtf8(collapsed);
  }
  if (!LooksLikeValidUtf8(collapsed))
    return false;
  *out = collapsed;
  return true;
}

static bool TryRepairFullwidthByteMojibake(const std::string &in,
                                           std::string *out) {
  if (!out || in.empty())
    return false;

  // Some FS/libc paths return each original byte b as U+FF00+b, encoded in
  // UTF-8 (e.g. "í" C3 AD becomes "ￃﾭ": EF BF 83 EF BE AD).
  // Collapse that representation back to raw bytes and accept only if the
  // resulting stream is valid UTF-8 and contains UTF-8-like lead/continuation.
  std::string collapsed;
  collapsed.reserve(in.size());
  std::vector<unsigned char> mapped;
  mapped.reserve(in.size() / 3);

  bool changed = false;
  for (size_t i = 0; i < in.size();) {
    unsigned char b0 = (unsigned char)in[i];
    if (i + 2 < in.size() && b0 == 0xEF) {
      unsigned char b1 = (unsigned char)in[i + 1];
      unsigned char b2 = (unsigned char)in[i + 2];
      if (b1 >= 0xBC && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
        // Inverse of UTF-8 for U+FF00..U+FFFF range:
        // byte = ((second - 0xBC) << 6) | (third - 0x80)
        unsigned char recovered =
            (unsigned char)(((b1 - 0xBC) << 6) | (b2 - 0x80));
        collapsed.push_back((char)recovered);
        mapped.push_back(recovered);
        i += 3;
        changed = true;
        continue;
      }
    }
    collapsed.push_back((char)b0);
    i++;
  }

  if (!changed || mapped.size() < 2)
    return false;

  bool has_utf8_pair = false;
  for (size_t i = 0; i + 1 < mapped.size(); i++) {
    unsigned char lead = mapped[i];
    unsigned char cont = mapped[i + 1];
    if (lead >= 0xC2 && lead <= 0xF4 && cont >= 0x80 && cont <= 0xBF) {
      has_utf8_pair = true;
      break;
    }
  }
  if (!has_utf8_pair)
    return false;
  if (!LooksLikeValidUtf8(collapsed))
    return false;

  *out = collapsed;
  return true;
}

static std::string ComposeLatinCombiningMarks(const std::string &in) {
  if (in.empty())
    return in;

  std::string out;
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (i + 2 < in.size() && (c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
                              c == 'u' || c == 'A' || c == 'E' || c == 'I' ||
                              c == 'O' || c == 'U' || c == 'n' || c == 'N') &&
        (unsigned char)in[i + 1] == 0xCC) {
      unsigned char mark = (unsigned char)in[i + 2];
      const char *rep = nullptr;

      if (mark == 0x81) { // U+0301 combining acute
        switch (c) {
        case 'a':
          rep = "\xC3\xA1";
          break;
        case 'e':
          rep = "\xC3\xA9";
          break;
        case 'i':
          rep = "\xC3\xAD";
          break;
        case 'o':
          rep = "\xC3\xB3";
          break;
        case 'u':
          rep = "\xC3\xBA";
          break;
        case 'A':
          rep = "\xC3\x81";
          break;
        case 'E':
          rep = "\xC3\x89";
          break;
        case 'I':
          rep = "\xC3\x8D";
          break;
        case 'O':
          rep = "\xC3\x93";
          break;
        case 'U':
          rep = "\xC3\x9A";
          break;
        }
      } else if (mark == 0x83) { // U+0303 combining tilde
        switch (c) {
        case 'n':
          rep = "\xC3\xB1";
          break;
        case 'N':
          rep = "\xC3\x91";
          break;
        }
      } else if (mark == 0x88) { // U+0308 combining diaeresis
        switch (c) {
        case 'u':
          rep = "\xC3\xBC";
          break;
        case 'U':
          rep = "\xC3\x9C";
          break;
        }
      }

      if (rep) {
        out.append(rep);
        i += 2;
        continue;
      }
    }

    out.push_back((char)c);
  }

  return out;
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
  book->GetApp()->PrintStatus(msg);
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
  std::string normalized =
      NormalizeDisplayUtf8(raw, &repaired_fullwidth, &repaired_legacy,
                           &composed_accents);
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
  int max_lines = inner_h / MAX(1, line_h);
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
  } else if (book->format == FORMAT_XHTML &&
             HasExtCI(book->GetFileName(), ".fb2")) {
    EnqueueJob(APP_JOB_EXTRACT_COVER, book);
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
        browser_view_dirty = true;
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
            browser_view_dirty = true;
          }
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".fb2")) {
          rc = fb2_extract_cover(book, path);
          if (rc == 0 && book->coverPixels) {
            SaveCoverCache(book, path);
          }
          book->coverTried = true;
          browser_view_dirty = true;
        }
      }
    } else if (job.type == APP_JOB_RESOLVE_TOC) {
      if (book->format == FORMAT_EPUB && !book->tocResolveTried) {
        std::string path = bookdir + "/" + book->GetFileName();
        rc = epub_resolve_toc(book, path);
        book->tocResolveTried = true;
        book->tocResolved = (rc == 0);
        if (rc == 0 && mode == APP_MODE_CHAPTERS && book == bookcurrent &&
            chaptermenu) {
          chaptermenu->Init();
          chaptermenu->SetDirty(true);
        }
      }
    }

    u64 elapsed = osGetTime() - t0;
    char msg[256];
    snprintf(msg, sizeof(msg), "TIMING: job=%s rc=%d ms=%llums book=%s",
             job_name(job.type), rc, (unsigned long long)elapsed,
             book->GetFileName() ? book->GetFileName() : "(null)");
    PrintStatus(msg);

    if (osGetTime() - start_ms >= budget_ms)
      break;
  }
}

void App::browser_handleevent() {
  u32 keys = hidKeysDown();
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y |
                           KEY_START | KEY_SELECT | KEY_UP | KEY_DOWN |
                           KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R |
                           KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT |
                           KEY_CPAD_RIGHT;
  if (browser_wait_input_release) {
    if (hidKeysHeld() & release_mask)
      return;
    browser_wait_input_release = false;
    return;
  }

  if (keys & (KEY_A | key.down)) {
    // Open selected book.
    OpenBook();
  }

  else if (keys & (key.left | key.l)) {
    // Select next book.
    int b = GetBookIndex(bookselected);
    if (b < bookcount - 1) {
      b++;
      bookselected = books[b];
      if (b >= browserstart + APP_BROWSER_BUTTON_COUNT)
        browser_nextpage();
      browser_view_dirty = true;
    }
  }

  else if (keys & (key.right | key.r)) {
    // Select previous book.
    int b = GetBookIndex(bookselected);
    if (b > 0) {
      b--;
      bookselected = books[b];
      if (b < browserstart)
        browser_prevpage();
      browser_view_dirty = true;
    }
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
            int book_idx = browserstart + page_idx;
            if (book_idx >= 0 && book_idx < bookcount) {
              if (bookselected == books[book_idx]) {
                OpenBook();
              } else {
                bookselected = books[book_idx];
                browser_view_dirty = true;
              }
              return true;
            }
          }
        }
      }

      // Fallback to original cover hitboxes.
      for (int i = browserstart;
           (i < bookcount) && (i < browserstart + APP_BROWSER_BUTTON_COUNT);
           i++) {
        if (hitsButtonAt(*buttons[i], x, y, 4)) {
          if (bookselected == books[i]) {
            OpenBook();
          } else {
            bookselected = books[i];
            browser_view_dirty = true;
          }
          return true;
        }
      }
      return false;
    };

    // Browser UI on bottom screen: keep X from raw Y, but keep Y in the same
    // frame as TouchRead (inverted raw X). Using TouchRead() directly mirrors
    // left/right in some Citra/3DS setups.
    touchPosition raw;
    hidTouchRead(&raw);
    handleTouchAt((int)raw.py, 319 - (int)raw.px);
  }
}

void App::browser_init(void) {
  for (int i = 0; i < bookcount; i++) {
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
  buttonprev.Move(2, kBrowserFooterY);
  buttonprev.Resize(50, 16);
  buttonprev.Label("prev");
  buttonnext.Init(ts);
  buttonnext.Move(188, kBrowserFooterY);
  buttonnext.Resize(50, 16);
  buttonnext.Label("next");
  buttonprefs.Init(ts);
  buttonprefs.Move(80, kBrowserFooterY);
  buttonprefs.Resize(78, 16);
  buttonprefs.Label("settings");

  if (!bookselected) {
    browserstart = 0;
    bookselected = books[0];
  } else {
    browserstart = (GetBookIndex(bookselected) / APP_BROWSER_BUTTON_COUNT) *
                   APP_BROWSER_BUTTON_COUNT;
  }
}

void App::browser_nextpage() {
  if (browserstart + APP_BROWSER_BUTTON_COUNT < bookcount) {
    browserstart += APP_BROWSER_BUTTON_COUNT;
    bookselected = books[browserstart];
    browser_view_dirty = true;
  }
}

void App::browser_prevpage() {
  if (browserstart >= APP_BROWSER_BUTTON_COUNT) {
    browserstart -= APP_BROWSER_BUTTON_COUNT;
    bookselected = books[browserstart + APP_BROWSER_BUTTON_COUNT - 1];
    browser_view_dirty = true;
  }
}

void App::browser_draw(void) {
  // save state
  int colorMode = ts->GetColorMode();
  u16 *screen = ts->GetScreen();
  int style = ts->GetStyle();
  int savedPixelSize = ts->pixelsize;

  ts->SetScreen(ts->screenright);
  ts->SetColorMode(0); // Normal for browser text
  ts->ClearScreen();
  if (bookselected)
    QueueBookWarmup(bookselected);

  for (int i = browserstart;
       (i < bookcount) && (i < browserstart + APP_BROWSER_BUTTON_COUNT); i++) {
    QueueBookWarmup(books[i]);
    buttons[i]->Draw(ts->screenright, books[i] == bookselected);

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

    if (books[i] == bookselected) {
      ts->DrawRect(btnX - 2, btnY - 2, btnX + kBrowserCellW + 2,
                   btnY + kBrowserCellH + 2,
                   0xF800); // Red thick outer bounding box
      ts->DrawRect(btnX - 3, btnY - 3, btnX + kBrowserCellW + 3,
                   btnY + kBrowserCellH + 3,
                   0xF800);
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
        ts->SetPen(btnX, btnY + kBrowserCoverH + 12);
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
      sprintf(msg, "Pg %d", pos + 1);
    else
      sprintf(msg, "NEW");
    ts->SetPen(btnX, btnY + kBrowserCoverH + 24);
    ts->PrintString(msg);
  }

  ts->SetPixelSize(savedPixelSize);

  // Navigation buttons at the bottom
  if (browserstart >= APP_BROWSER_BUTTON_COUNT)
    buttonprev.Draw(ts->screenright, false);
  if (bookcount > browserstart + APP_BROWSER_BUTTON_COUNT)
    buttonnext.Draw(ts->screenright, false);

  buttonprefs.Draw(ts->screenright, false);

  // Pagination indicator
  if (bookcount > APP_BROWSER_BUTTON_COUNT) {
    int currentPage = (browserstart / APP_BROWSER_BUTTON_COUNT) + 1;
    int totalPages =
        (bookcount + APP_BROWSER_BUTTON_COUNT - 1) / APP_BROWSER_BUTTON_COUNT;
    char pageMsg[16];
    sprintf(pageMsg, "%d/%d", currentPage, totalPages);
    ts->SetPixelSize(8);
    ts->SetPen(112, kBrowserFooterY + 3);
    ts->PrintString(pageMsg);
    ts->SetPixelSize(savedPixelSize);
  }

  // restore state
  ts->SetColorMode(colorMode);
  ts->SetScreen(screen);
  ts->SetStyle(style);

  browser_view_dirty = false;
}
