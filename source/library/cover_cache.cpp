/*
    3dslibris - cover_cache.cpp
    New 3DS library module by Rigle.

    Summary:
    - Disk I/O for the browser cover thumbnail cache (.cvr files on SD).
    - Extracted from library/app_browser.cpp to isolate cache I/O from browser UI.
    - EnsureCoverCacheDirs/PruneCoverCache/BuildCoverCachePath are private helpers.
*/

#include "library/cover_cache.h"
#include "library/cover_override_utils.h"

#include <dirent.h>
#include <list>
#include <map>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
static const char *kCoverCacheMagic = "CVR4";
static const char *kCoverAbsentMagic = "CVN4";
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

static std::list<CoverCacheEntry> g_cache_entries;
static size_t g_cache_bytes = 0;
static bool g_dirs_initialized = false, g_full_retention = false;
static bool FullRetentionStored() {
  FILE *fp=fopen(paths::GetCoverRetentionPath().c_str(),"rb");
  if(!fp) return false;
  char marker[16]={};const bool ok=fgets(marker,sizeof(marker),fp)!=nullptr && !strcmp(marker,"keep-all-v1\n");
  fclose(fp);return ok;
}

static void LoadCoverCacheIndex() {
  DIR *dp = opendir(kCoverCacheDir.c_str());
  if (!dp)
    return;

  struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    if (!HasExtCI(ent->d_name, ".cvr"))
      continue;

    char full[512];
    snprintf(full, sizeof(full), "%s/%s", kCoverCacheDir.c_str(), ent->d_name);
    struct stat st;
    if (!paths::StatWithMtime(full, &st) || !S_ISREG(st.st_mode))
      continue;

    CoverCacheEntry ce;
    ce.path = full;
    ce.mtime = (long long)st.st_mtime;
    ce.size = (st.st_size > 0) ? (size_t)st.st_size : 0;
    g_cache_bytes += ce.size;
    g_cache_entries.push_back(ce);
  }
  closedir(dp);
}

static void RecordCoverCacheSave(const std::string &path, size_t size) {
  for (auto it = g_cache_entries.begin(); it != g_cache_entries.end(); ++it) {
    if (it->path == path) {
      g_cache_bytes -= it->size;
      g_cache_entries.erase(it);
      break;
    }
  }
  CoverCacheEntry entry;
  entry.path = path;
  entry.mtime = (long long)time(NULL);
  entry.size = size;
  g_cache_entries.push_back(entry);
  g_cache_bytes += size;
}

static void PruneCoverCache() {
  std::list<CoverCacheEntry> &entries = g_cache_entries;
  size_t &total_bytes = g_cache_bytes;

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
  if (g_dirs_initialized)
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

  g_full_retention=FullRetentionStored();
  if(!g_full_retention) {
    LoadCoverCacheIndex();
    PruneCoverCache();
  }
  g_dirs_initialized = true;
}

struct CoverSourcePaths {
  std::string cache_path;
  std::string adjacent_path;
};

// Cache only identities, never decoded images. Rebuilt when the library is
// scanned; bounded independently of the number of directories visited.
static std::map<std::string, CoverSourcePaths> g_source_paths;
static const size_t kMaxSourcePaths = 512;

static CoverSourcePaths ResolveCoverSourcePaths(Book *book,
                                                const std::string &book_path) {
  CoverSourcePaths paths;
  struct stat st;
  long long fsize = 0;
  long long fmtime = 0;
  if (paths::StatWithMtime(book_path.c_str(), &st)) {
    fsize = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }

  std::string key = "src2|" + book_path;
  key.push_back('|');
  key += std::to_string(fsize);
  key.push_back('|');
  key += std::to_string(fmtime);

  struct stat cover_st;
  if (book && book->IsBrowserFolder()) {
    // Folder entries already carry the resolved sidecar path.
    paths.adjacent_path = book_path;
  } else {
    const std::vector<std::string> candidates =
        cover_override_utils::BuildBookCandidates(book_path);
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (paths::StatWithMtime(candidates[i].c_str(), &cover_st) &&
          S_ISREG(cover_st.st_mode)) {
        paths.adjacent_path = candidates[i];
        break;
      }
    }
  }
  if ((!book || !book->IsBrowserFolder()) && !paths.adjacent_path.empty()) {
    key += "|adj:" + paths.adjacent_path;
    key.push_back('|');
    key += std::to_string((long long)cover_st.st_size);
    key.push_back('|');
    key += std::to_string((long long)cover_st.st_mtime);
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
  paths.cache_path = out;
  return paths;
}

static const CoverSourcePaths &GetCoverSourcePaths(
    Book *book, const std::string &book_path) {
  const std::string key = (book && book->IsBrowserFolder() ? "folder:" : "book:") +
                          book_path;
  auto it = g_source_paths.find(key);
  if (it != g_source_paths.end())
    return it->second;
  if (g_source_paths.size() >= kMaxSourcePaths)
    g_source_paths.erase(g_source_paths.begin());
  return g_source_paths.emplace(key, ResolveCoverSourcePaths(book, book_path))
      .first->second;
}

} // namespace

namespace cover_cache {

void ClearPathCache() { g_source_paths.clear(); }
void ResetMaintenance() {
  g_dirs_initialized=false;g_full_retention=false;
  g_cache_entries.clear();g_cache_bytes=0;ClearPathCache();
}
bool EnableFullRetention() {
  mkdir(kCoverCacheBaseDir.c_str(),0777);mkdir(kCoverCacheDir.c_str(),0777);
  if(!FullRetentionStored()) {
    const std::string temporary=paths::GetCoverRetentionPath()+".tmp";
    FILE *fp=fopen(temporary.c_str(),"wb");if(!fp) return false;
    const bool written=fputs("keep-all-v1\n",fp)>=0;
    const bool closed=fclose(fp)==0;
    if(!written || !closed || rename(temporary.c_str(),paths::GetCoverRetentionPath().c_str())) {
      remove(temporary.c_str());return false;
    }
  }
  ResetMaintenance();return true;
}

bool HasAdjacentOverride(Book *book,const std::string &path) {
  return book && !path.empty() && !GetCoverSourcePaths(book,path).adjacent_path.empty();
}
bool TryLoadAdjacentOverride(Book *book, const std::string &book_path) {
  if (!book || book_path.empty())
    return false;

  const std::string chosen_path =
      GetCoverSourcePaths(book, book_path).adjacent_path;
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
  std::string cache_path = GetCoverSourcePaths(book, book_path).cache_path;
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
  const bool closed=fclose(fp)==0;
  ok=ok && closed;
  if (!ok) remove(cache_path.c_str());
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
    if(!g_full_retention) {
      RecordCoverCacheSave(cache_path, sizeof(header) + count * sizeof(u16));
      PruneCoverCache();
    }
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

bool MarkAbsent(Book *book, const std::string &book_path) {
  if (!book || book_path.empty())
    return false;
  EnsureCoverCacheDirs();
  const std::string cache_path = GetCoverSourcePaths(book, book_path).cache_path;
  u8 header[8] = {};
  memcpy(header, kCoverAbsentMagic, 4);
  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return false;
  bool ok = fwrite(header, 1, sizeof(header), fp) == sizeof(header);
  ok = (fclose(fp) == 0) && ok;
  if (!ok) {
    remove(cache_path.c_str());
    return false;
  }
  if (!g_full_retention) {
    RecordCoverCacheSave(cache_path, sizeof(header));
    PruneCoverCache();
  }
  return true;
}

bool IsMarkedAbsent(Book *book, const std::string &book_path) {
  if (!book || book_path.empty())
    return false;
  EnsureCoverCacheDirs();
  FILE *fp = fopen(GetCoverSourcePaths(book, book_path).cache_path.c_str(), "rb");
  if (!fp)
    return false;
  u8 header[8];
  const bool ok = fread(header, 1, sizeof(header), fp) == sizeof(header) &&
                  memcmp(header, kCoverAbsentMagic, 4) == 0 &&
                  !header[4] && !header[5] && !header[6] && !header[7];
  fclose(fp);
  return ok;
}

static bool Load(Book *book, const std::string &book_path, bool allow_fallback) {
  if (!book)
    return false;
  EnsureCoverCacheDirs();

  std::string cache_path = GetCoverSourcePaths(book, book_path).cache_path;
  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp) {
    // All sidecars are decoded by idle jobs; page changes only read .cvr.
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
    if (book->GetStatusReporter()) {
      DBG_LOGF(book->GetStatusReporter(), "COVER: cache miss book=%s path=%s",
               book->GetFileName() ? book->GetFileName() : "(null)",
               cache_path.c_str());
    }
#endif
    if (book->IsBrowserFolder())
      return false;
    if (allow_fallback && debug_runtime::BackgroundWorkersDisabled() &&
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

bool TryLoad(Book *book,const std::string &path) {return Load(book,path,true);}
bool TryLoadCachedOnly(Book *book,const std::string &path) {return Load(book,path,false);}

} // namespace cover_cache
