/*
    3dslibris - cover_cache.cpp
    New 3DS library module by Rigle.

    Summary:
    - Disk I/O for the browser cover thumbnail cache (.cvr files on SD).
    - Extracted from library/app_browser.cpp to isolate cache I/O from browser UI.
    - EnsureCoverCacheDirs/PruneCoverCache/BuildCoverCachePath are private helpers.
*/

#include "library/cover_cache.h"

#include <dirent.h>
#include <list>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#include <3ds.h>

#include "book/book.h"
#include "book/book_parser.h"
#include "book/cover_layout_constants.h"
#include "formats/cbz/cbz_parser.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "formats/common/file_read_utils.h"
#include "shared/cover_decode_utils.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "shared/path_constants.h"
#include "shared/path_utils.h"
#include "shared/string_utils.h"

#ifndef BROWSER_COVER_TRACE
#define BROWSER_COVER_TRACE 0
#endif

namespace {

static const std::string &kCoverCacheBaseDir = paths::GetCacheBaseDir();
static const std::string &kCoverCacheDir = paths::GetCoverCacheDir();
static const char *kCoverCacheMagic = "CVR3";
static const size_t kCoverCacheMaxFiles = 512;
static const size_t kCoverCacheMaxBytes = 16 * 1024 * 1024;

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

  DIR *dp = opendir(kCoverCacheDir.c_str());
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
    snprintf(full, sizeof(full), "%s/%s", kCoverCacheDir.c_str(), ent->d_name);
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
    FILE *rf = fopen(paths::GetCoverCacheManifest().c_str(), "r");
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
    FILE *wf = fopen(paths::GetCoverCacheManifest().c_str(), "w");
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
  mkdir(kCoverCacheBaseDir.c_str(), 0777);
  mkdir(kCoverCacheDir.c_str(), 0777);

  // One-time cleanup: remove legacy CVR2 cache files whose names are bare
  // 16-hex-digit hashes (e.g., "a1b2c3d4e5f6g7h8.cvr").
  DIR *legacy_dp = opendir(kCoverCacheDir.c_str());
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
          snprintf(full, sizeof(full), "%s/%s", kCoverCacheDir.c_str(), ent->d_name);
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
  auto IsRegularFilePath = [](const std::string &path) -> bool {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  };

  auto FindAdjacentOverrideCoverPath =
      [&](const std::string &path, std::string *out_path) -> bool {
    if (out_path)
      out_path->clear();
    if (path.empty())
      return false;
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
      return false;
    std::string dir = path.substr(0, slash);
    std::string filename = path.substr(slash + 1);
    if (filename.empty())
      return false;
    size_t dot = filename.find_last_of('.');
    std::string stem = (dot != std::string::npos && dot > 0)
                           ? filename.substr(0, dot)
                           : filename;
    if (stem.empty())
      return false;

    std::vector<std::string> candidates;
    candidates.reserve(4);
    candidates.push_back(dir + "/" + stem + ".jpg");
    candidates.push_back(dir + "/" + stem + ".png");
    candidates.push_back(dir + "/" + stem + ".JPG");
    candidates.push_back(dir + "/" + stem + ".PNG");

    for (size_t i = 0; i < candidates.size(); i++) {
      if (IsRegularFilePath(candidates[i])) {
        if (out_path)
          *out_path = candidates[i];
        return true;
      }
    }
    return false;
  };

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

  std::string adjacent_cover;
  if (FindAdjacentOverrideCoverPath(book_path, &adjacent_cover)) {
    struct stat cover_st;
    long long cover_size = 0;
    long long cover_mtime = 0;
    if (stat(adjacent_cover.c_str(), &cover_st) == 0) {
      cover_size = (long long)cover_st.st_size;
      cover_mtime = (long long)cover_st.st_mtime;
    }
    key += "|adj:" + adjacent_cover;
    key.push_back('|');
    key += std::to_string(cover_size);
    key.push_back('|');
    key += std::to_string(cover_mtime);
  } else {
    key += "|adj:none";
  }

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
  snprintf(out, sizeof(out), "%s/%s_%08llx.cvr", kCoverCacheDir.c_str(), label.c_str(),
           (unsigned long long)(h & 0xFFFFFFFFULL));
  return std::string(out);
}

} // namespace

namespace cover_cache {

bool TryLoadAdjacentOverride(Book *book, const std::string &book_path) {
  if (!book || book_path.empty())
    return false;

  const size_t slash = book_path.find_last_of('/');
  if (slash == std::string::npos)
    return false;
  const std::string dir = book_path.substr(0, slash);
  const std::string filename = book_path.substr(slash + 1);
  if (filename.empty())
    return false;
  const size_t dot = filename.find_last_of('.');
  const std::string stem =
      (dot != std::string::npos && dot > 0) ? filename.substr(0, dot) : filename;
  if (stem.empty())
    return false;

  const std::string candidates[] = {
      dir + "/" + stem + ".jpg", dir + "/" + stem + ".png",
      dir + "/" + stem + ".JPG", dir + "/" + stem + ".PNG",
  };

  std::string chosen_path;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    struct stat st;
    if (stat(candidates[i].c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      chosen_path = candidates[i];
      break;
    }
  }
  if (chosen_path.empty())
    return false;

  std::string bytes;
  if (!file_read_utils::ReadPathToStringLimited(chosen_path, &bytes,
                                                4u * 1024u * 1024u))
    return false;

  if (!cover_decode_utils::DecodeImageToCoverThumb(
          book, (const unsigned char *)bytes.data(), bytes.size(), 4096))
    return false;

#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
  if (book->GetStatusReporter()) {
    DBG_LOGF(book->GetStatusReporter(),
             "COVER: adjacent override loaded book=%s path=%s",
             book->GetFileName() ? book->GetFileName() : "(null)",
             chosen_path.c_str());
  }
#endif
  return true;
}

bool Save(Book *book, const std::string &book_path) {
  if (!book || !book->coverPixels || book->coverWidth <= 0 ||
      book->coverHeight <= 0) {
    return false;
  }
  if (book->coverWidth > cover_layout::kBrowserCoverThumbWidth ||
      book->coverHeight > cover_layout::kBrowserCoverThumbHeight)
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
    FILE *mf = fopen(paths::GetCoverCacheManifest().c_str(), "a");
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
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
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

bool TryLoad(Book *book, const std::string &book_path) {
  if (!book)
    return false;
  EnsureCoverCacheDirs();

  std::string cache_path = BuildCoverCachePath(book, book_path);
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp) {
    if (!book_path.empty() && !book->coverPixels &&
        TryLoadAdjacentOverride(book, book_path)) {
      Save(book, book_path);
      return true;
    }
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(), "COVER: cache miss book=%s path=%s",
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    if (debug_runtime::BackgroundWorkersDisabled() &&
        debug_runtime::BrowserWarmupDisabled() && !book_path.empty() &&
        !book->coverPixels && book->coverAttempts < kMaxAttempts) {
      if (TryLoadAdjacentOverride(book, book_path)) {
        Save(book, book_path);
        return true;
      }
      int src_rc = -1;
      if (book->format == FORMAT_EPUB) {
        // Metadata indexing jobs may not have run yet; index synchronously
        // here so coverImagePath gets populated before extraction.
        if (!book->metadataIndexTried) {
          if (book_parser::Index(book) == 0)
            book->ClearBrowserDisplayNameCache();
        }
        if (book->metadataIndexTried && !book->coverImagePath.empty())
          src_rc = epub_parser::ExtractCover(book, book_path);
      } else if (book->format == FORMAT_XHTML &&
                 HasExtCI(book->GetFileName(), ".fb2")) {
        src_rc = fb2_parser::ExtractCover(book, book_path);
      } else if (book->format == FORMAT_XHTML &&
                 HasExtCI(book->GetFileName(), ".mobi")) {
        src_rc = mobi_parser::ExtractCover(book, book_path);
      } else if (book->format == FORMAT_PDF) {
        src_rc = pdf_parser::ExtractCover(book, book_path);
      } else if (book->format == FORMAT_CBZ) {
        src_rc = cbz_parser::ExtractCover(book, book_path);
      }
      if (src_rc == 0 && book->coverPixels) {
        Save(book, book_path);
        return true;
      }
      book->coverAttempts++;
    }
    return false;
  }

  u8 header[8];
  bool ok = fread(header, 1, sizeof(header), fp) == sizeof(header);
  // Ignore thumbnails written by older extractors once the cache format/magic
  // changes, so stale covers do not survive heuristic fixes.
  if (!ok || memcmp(header, kCoverCacheMagic, 4) != 0) {
    fclose(fp);
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
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
  if (w == 0 || h == 0 || w > cover_layout::kBrowserCoverThumbWidth ||
      h > cover_layout::kBrowserCoverThumbHeight) {
    fclose(fp);
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
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
  u16 *pixels = new u16[count];
  if (!pixels) {
    fclose(fp);
    return false;
  }
  if (fread(pixels, sizeof(u16), count, fp) != count) {
    delete[] pixels;
    fclose(fp);
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
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
  book->coverPixels = pixels;
  book->coverWidth = w;
  book->coverHeight = h;
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
  if (book->GetStatusReporter()) {
    DBG_LOGF(book->GetStatusReporter(),
             "COVER: cache hit book=%s path=%s size=%ux%u",
             book->GetFileName() ? book->GetFileName() : "(null)",
             cache_path.c_str(), (unsigned)w, (unsigned)h);
  }
#endif
  return true;
}

} // namespace cover_cache
