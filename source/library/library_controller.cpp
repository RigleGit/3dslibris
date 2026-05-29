#include "app/library_controller.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <set>
#include <stdio.h>
#include <sys/stat.h>
#include <strings.h>
#include <vector>

#include "library/library_sort_mode.h"
#include "settings/prefs.h"

// Forward declared from app_book.cpp
void DrawOpeningSplashWithProgress(unsigned done, unsigned total,
                                   void *user_data);

#include <3ds.h>

#include "app/app.h"
#include "book/book.h"
#include "ui/gradient_utils.h"
#include "book/book_context.h"
#include "shared/debug_log.h"
#include "shared/path_constants.h"
#include "shared/app_flow_utils.h"
#include "shared/utf8_utils.h"
#include "stb_image.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

namespace {

static bool BookTitleLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

static bool BookFilenameLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  const char *fa = (a && a->GetFileName()) ? a->GetFileName() : "";
  const char *fb = (b && b->GetFileName()) ? b->GetFileName() : "";
  return strcasecmp(fa, fb) < 0;
}

static bool BookAuthorLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  const char *ea = a->GetAuthor().c_str();
  const char *eb = b->GetAuthor().c_str();
  const bool a_empty = !ea || *ea == '\0';
  const bool b_empty = !eb || *eb == '\0';
  if (a_empty != b_empty)
    return b_empty; // empty author sorts to end
  const int c = strcasecmp(ea, eb);
  return c != 0 ? c < 0 : strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

static const char *BookFileExtension(Book *b) {
  if (!b || !b->GetFileName()) return "";
  const char *dot = strrchr(b->GetFileName(), '.');
  return dot ? dot : "";
}

static bool BookFiletypeLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  int ext_cmp = strcasecmp(BookFileExtension(a), BookFileExtension(b));
  if (ext_cmp != 0)
    return ext_cmp < 0;
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

static time_t BookFileMtime(Book *b) {
  if (!b || b->IsBrowserFolder() || !b->GetFolderName() || !b->GetFileName())
    return 0;
  std::string path = b->GetFolderName();
  path.push_back('/');
  path.append(b->GetFileName());
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

static bool BookDateModifiedLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  return BookFileMtime(a) > BookFileMtime(b); // newer first
}

static bool BookRecentLessThan(Book *a, Book *b) {
  if (a && b && a->IsBrowserFolder() != b->IsBrowserFolder())
    return a->IsBrowserFolder();
  uint32_t ta = a->GetLastOpenedTime();
  uint32_t tb = b->GetLastOpenedTime();
  if (ta != tb)
    return ta > tb; // more recent first
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

static std::string ToLowerAscii(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static bool PathIsDirectory(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool PathIsRegularFile(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string BasenameFromPath(const std::string &path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return std::string();
  return path.substr(slash + 1);
}

#if UTF8_FILENAME_DIAG
static bool LooksLikeValidUtf8(const char *s) { return utf8_utils::IsValidUtf8(s); }

static std::string HexBytesForLog(const char *s, size_t max_bytes = 32) {
  static const char hex[] = "0123456789ABCDEF";
  if (!s)
    return "";
  std::string out;
  size_t n = strlen(s);
  if (n > max_bytes)
    n = max_bytes;
  out.reserve(n * 3 + 8);
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)s[i];
    if (i)
      out.push_back(' ');
    out.push_back(hex[(b >> 4) & 0x0F]);
    out.push_back(hex[b & 0x0F]);
  }
  if (strlen(s) > max_bytes)
    out += " ...";
  return out;
}
#endif

static void LogFilenameStage(IStatusReporter *reporter, const char *stage,
                             const char *value) {
#if !UTF8_FILENAME_DIAG
  (void)reporter;
  (void)stage;
  (void)value;
  return;
#else
  if (!reporter || !stage || !value)
    return;
  char msg[512];
  std::string bytes = HexBytesForLog(value);
  snprintf(msg, sizeof(msg),
           "FindBooks %-20s len=%u valid=%d bytes=[%s] text=\"%s\"", stage,
           (unsigned)strlen(value), LooksLikeValidUtf8(value) ? 1 : 0,
           bytes.c_str(), value);
  DBG_LOG(reporter, msg);
#endif
}

static std::string SdmcToArchiveRelPath(const std::string &path) {
  return app_flow_utils::SdmcToArchiveRelPath(path);
}

static std::string NormalizeFsFilenameForIo(const char *raw_name) {
  if (!raw_name)
    return "";
  return utf8_utils::NormalizeFsFilenameForIo(raw_name);
}

static bool Utf16NameToUtf8(const u16 *name, std::string *out) {
  return utf8_utils::Utf16NameToUtf8(reinterpret_cast<const uint16_t *>(name),
                                     out);
}

static bool HasBookWithFileName(const std::vector<Book *> &books,
                                const char *filename) {
  if (!filename || !*filename)
    return false;
  for (size_t i = 0; i < books.size(); i++) {
    if (!books[i] || !books[i]->GetFileName())
      continue;
    if (strcasecmp(books[i]->GetFileName(), filename) == 0)
      return true;
  }
  return false;
}

static Book *FindBookByFolderAndFile(const std::vector<Book *> &books,
                                     const char *folder,
                                     const char *filename) {
  if (!filename || !*filename)
    return NULL;
  const std::string wanted_norm =
      utf8_utils::NormalizeFsFilenameForIo(filename);
  for (size_t i = 0; i < books.size(); i++) {
    Book *book = books[i];
    if (!book || book->IsBrowserFolder() || !book->GetFileName())
      continue;
    if (folder && *folder && strcasecmp(book->GetFolderName(), folder) != 0)
      continue;
    if (strcasecmp(book->GetFileName(), filename) == 0)
      return book;
    if (!wanted_norm.empty()) {
      const std::string book_norm =
          utf8_utils::NormalizeFsFilenameForIo(book->GetFileName());
      if (!book_norm.empty() &&
          strcasecmp(book_norm.c_str(), wanted_norm.c_str()) == 0)
        return book;
    }
  }
  return NULL;
}

static Book *FindBookByFileNameOnly(const std::vector<Book *> &books,
                                    const char *filename) {
  if (!filename || !*filename)
    return NULL;
  const std::string wanted_norm =
      utf8_utils::NormalizeFsFilenameForIo(filename);
  for (size_t i = 0; i < books.size(); i++) {
    Book *book = books[i];
    if (!book || book->IsBrowserFolder() || !book->GetFileName())
      continue;
    if (strcasecmp(book->GetFileName(), filename) == 0)
      return book;
    if (!wanted_norm.empty()) {
      const std::string book_norm =
          utf8_utils::NormalizeFsFilenameForIo(book->GetFileName());
      if (!book_norm.empty() &&
          strcasecmp(book_norm.c_str(), wanted_norm.c_str()) == 0)
        return book;
    }
  }
  return NULL;
}

static bool PathContainsFileCaseInsensitive(const std::string &dir,
                                            const char *filename) {
  if (!filename || !*filename)
    return false;
  DIR *dp = opendir(dir.c_str());
  if (!dp)
    return false;

  const std::string wanted_norm =
      utf8_utils::NormalizeFsFilenameForIo(filename);
  bool found = false;
  struct dirent *ent;
  while ((ent = readdir(dp))) {
    if (ent->d_name[0] == '.')
      continue;
    std::string child = dir + "/" + ent->d_name;
    if (!PathIsRegularFile(child))
      continue;
    if (strcasecmp(ent->d_name, filename) == 0) {
      found = true;
      break;
    }
    if (!wanted_norm.empty()) {
      const std::string child_norm =
          utf8_utils::NormalizeFsFilenameForIo(ent->d_name);
      if (!child_norm.empty() &&
          strcasecmp(child_norm.c_str(), wanted_norm.c_str()) == 0) {
        found = true;
        break;
      }
    }
  }
  closedir(dp);
  return found;
}

static bool PathContainsExactRegularFile(const std::string &dir,
                                         const char *filename) {
  if (!filename || !*filename)
    return false;
  std::string file_path = dir + "/" + filename;
  if (PathIsRegularFile(file_path))
    return true;
  return PathContainsFileCaseInsensitive(dir, filename);
}

static bool HasBrowserEntryKey(std::set<std::string> *seen,
                               const std::string &key) {
  if (!seen)
    return false;
  const std::string normalized = ToLowerAscii(key);
  if (seen->find(normalized) != seen->end())
    return true;
  seen->insert(normalized);
  return false;
}

static void DrawBottomGradientFromApp(void *user_data) {
  const LibraryGradientContext *ctx =
      static_cast<const LibraryGradientContext *>(user_data);
  if (ctx && ctx->ts)
    gradient_utils::DrawToScreen(ctx->ts, *ctx->color_mode,
                                 ctx->ts->screenright, 320);
}

static void DrawTopGradientFromApp(void *user_data) {
  const LibraryGradientContext *ctx =
      static_cast<const LibraryGradientContext *>(user_data);
  if (ctx && ctx->ts)
    gradient_utils::DrawToScreen(ctx->ts, *ctx->color_mode,
                                 ctx->ts->screenleft, 400);
}

static void AppendBookFromFilename(App *app, LibraryGradientContext *gradient_ctx,
                                   const std::string &source_dir,
                                   const char *filename,
                                   std::set<std::string> *seen_keys = NULL) {
  if (!app || !app_flow_utils::ShouldIndexBookFilename(filename))
    return;
  format_t format = app_flow_utils::DetectBookFormat(filename);
  if (format == FORMAT_UNDEF)
    return;

  std::string raw_name(filename);
  std::string io_name = NormalizeFsFilenameForIo(filename);
  if (seen_keys && HasBrowserEntryKey(seen_keys, std::string("book:") + io_name))
    return;
  if (!seen_keys && HasBookWithFileName(app->books, io_name.c_str()))
    return;
  LogFilenameStage(app, "d_name", raw_name.c_str());
  if (io_name != raw_name)
    LogFilenameStage(app, "d_name_io_fix", io_name.c_str());
  BookContext ctx;
  ctx.text = app->ts.get();
  ctx.prefs = app->prefs.get();
  ctx.paragraph_spacing = &app->paraspacing;
  ctx.paragraph_indent = &app->paraindent;
  ctx.publisher_text_indent = &app->publisher_text_indent;
  ctx.publisher_block_margins = &app->publisher_block_margins;
  ctx.orientation = &app->orientation;
  ctx.status_reporter = app;
  ctx.draw_background = &DrawBottomGradientFromApp;
  ctx.draw_background_user_data = gradient_ctx;
  ctx.draw_top_background = &DrawTopGradientFromApp;
  ctx.draw_top_background_user_data = gradient_ctx;
  ctx.on_spine_progress = &DrawOpeningSplashWithProgress;
  ctx.on_spine_progress_user_data = app;
  Book *book = new Book(ctx);
  book->SetFolderName(source_dir.c_str());
  book->SetFileName(io_name.c_str());
  book->SetTitle(io_name.c_str());
  if (app->prefs)
    app->prefs->ApplySavedBookState(book);
  LogFilenameStage(app, "book.filename", book->GetFileName());
  LogFilenameStage(app, "book.title", book->GetTitle());
  book->format = format;
  app->books.push_back(book);
}

static bool DirectoryHasDirectSupportedBook(const std::string &dir) {
  DIR *dp = opendir(dir.c_str());
  if (!dp)
    return false;
  bool found = false;
  struct dirent *ent;
  while ((ent = readdir(dp))) {
    if (ent->d_name[0] == '.')
      continue;
    std::string child = dir + "/" + ent->d_name;
    if (PathIsDirectory(child))
      continue;
    if (app_flow_utils::ShouldIndexBookFilename(ent->d_name) &&
        app_flow_utils::DetectBookFormat(ent->d_name) != FORMAT_UNDEF) {
      found = true;
      break;
    }
  }
  closedir(dp);
  return found;
}

static void TryLoadFolderCover(Book *book) {
  if (!book || book->GetBrowserFolderCoverPath().empty())
    return;
  FILE *fp = fopen(book->GetBrowserFolderCoverPath().c_str(), "rb");
  if (!fp)
    return;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return;
  }
  long size = ftell(fp);
  if (size <= 0 || size > 4 * 1024 * 1024) {
    fclose(fp);
    return;
  }
  rewind(fp);
  std::vector<unsigned char> compressed((size_t)size);
  if (fread(compressed.data(), 1, compressed.size(), fp) !=
      compressed.size()) {
    fclose(fp);
    return;
  }
  fclose(fp);

  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = stbi_load_from_memory(
      compressed.data(), (int)compressed.size(), &w, &h, &comp, 3);
  if (!pixels || w <= 0 || h <= 0) {
    if (pixels)
      stbi_image_free(pixels);
    return;
  }
  const int max_w = 85;
  const int max_h = 115;
  int draw_w = w;
  int draw_h = h;
  if (draw_w > max_w || draw_h > max_h) {
    float scale_w = (float)max_w / (float)w;
    float scale_h = (float)max_h / (float)h;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    draw_w = std::max(1, (int)(w * scale));
    draw_h = std::max(1, (int)(h * scale));
  }
  u16 *thumb = new u16[(size_t)draw_w * (size_t)draw_h];
  if (!thumb) {
    stbi_image_free(pixels);
    return;
  }
  for (int y = 0; y < draw_h; y++) {
    int sy = (int)((long long)y * (long long)h / (long long)draw_h);
    for (int x = 0; x < draw_w; x++) {
      int sx = (int)((long long)x * (long long)w / (long long)draw_w);
      unsigned char *p = pixels + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
      u16 r = (u16)(p[0] >> 3);
      u16 g = (u16)(p[1] >> 2);
      u16 b = (u16)(p[2] >> 3);
      thumb[(size_t)y * (size_t)draw_w + (size_t)x] =
          (u16)((r << 11) | (g << 5) | b);
    }
  }
  stbi_image_free(pixels);
  if (book->coverPixels)
    delete[] book->coverPixels;
  book->coverPixels = thumb;
  book->coverWidth = draw_w;
  book->coverHeight = draw_h;
  book->coverAttempts = 3;
}

static void AppendFolderEntry(App *app, const std::string &source_dir,
                              const std::string &folder_name,
                              std::set<std::string> *seen_keys) {
  if (!app || folder_name.empty() || folder_name[0] == '.')
    return;
  const std::string folder_path = source_dir + "/" + folder_name;
  if (!PathIsDirectory(folder_path) ||
      !DirectoryHasDirectSupportedBook(folder_path))
    return;
  if (HasBrowserEntryKey(seen_keys, std::string("folder:") + folder_name))
    return;

  BookContext ctx;
  ctx.text = app->ts.get();
  ctx.prefs = app->prefs.get();
  ctx.paragraph_spacing = &app->paraspacing;
  ctx.paragraph_indent = &app->paraindent;
  ctx.publisher_text_indent = &app->publisher_text_indent;
  ctx.publisher_block_margins = &app->publisher_block_margins;
  ctx.orientation = &app->orientation;
  ctx.status_reporter = app;
  ctx.draw_background = &DrawBottomGradientFromApp;
  ctx.draw_background_user_data = app;
  ctx.draw_top_background = &DrawTopGradientFromApp;
  ctx.draw_top_background_user_data = app;
  ctx.on_spine_progress = &DrawOpeningSplashWithProgress;
  ctx.on_spine_progress_user_data = app;

  std::string cover_path = source_dir + "/" + folder_name + ".jpg";
  if (!PathIsRegularFile(cover_path))
    cover_path.clear();
  Book *folder = new Book(ctx);
  folder->SetBrowserFolderEntry(folder_path, folder_name, cover_path);
  TryLoadFolderCover(folder);
  app->books.push_back(folder);
}

static void ClearBrowserBooksAndButtons(App *app) {
  if (!app)
    return;
  Book *current = app->GetCurrentBook();
  if (current) {
    for (size_t i = 0; i < app->books.size(); i++) {
      if (app->books[i] == current) {
        app->CloseBook();
        break;
      }
    }
  }
  for (size_t i = 0; i < app->books.size(); i++) {
    delete app->books[i];
  }
  app->books.clear();
  for (size_t i = 0; i < app->buttons.size(); i++) {
    delete app->buttons[i];
  }
  app->buttons.clear();
  app->SetSelectedBook(NULL);
  app->SetBrowserPageStart(0);
}

static int AppendBooksFromNativeDirectory(App *app,
                                          LibraryGradientContext *gradient_ctx,
                                          const std::string &dir,
                                          std::set<std::string> *seen_keys) {
  if (!app)
    return 1;
#ifdef DSLIBRIS_DEBUG
  const int count_before = app->BookCount();
  DBG_LOGF(app, "BROWSER folder native scan begin dir=%s", dir.c_str());
#endif
  FS_Archive sdmc_archive;
  Result rc = FSUSER_OpenArchive(&sdmc_archive, ARCHIVE_SDMC,
                                 fsMakePath(PATH_EMPTY, ""));
  if (R_FAILED(rc)) {
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(app, "BROWSER folder native scan open-archive failed rc=0x%08lx dir=%s",
             (unsigned long)rc, dir.c_str());
#endif
    return 1;
  }

  std::string rel_path = SdmcToArchiveRelPath(dir);
  Handle dir_handle = 0;
  rc = FSUSER_OpenDirectory(&dir_handle, sdmc_archive,
                            fsMakePath(PATH_ASCII, rel_path.c_str()));
  if (R_FAILED(rc)) {
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(app, "BROWSER folder native scan open-dir failed rc=0x%08lx rel=%s dir=%s",
             (unsigned long)rc, rel_path.c_str(), dir.c_str());
#endif
    FSUSER_CloseArchive(sdmc_archive);
    return 1;
  }

  while (true) {
    FS_DirectoryEntry entries[16];
    u32 read_count = 0;
    rc = FSDIR_Read(dir_handle, &read_count, 16, entries);
    if (R_FAILED(rc) || read_count == 0)
      break;

    for (u32 i = 0; i < read_count; i++) {
      if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)
        continue;
      std::string filename;
      if (!Utf16NameToUtf8(entries[i].name, &filename))
        continue;
      if (filename.empty())
        continue;
      AppendBookFromFilename(app, gradient_ctx, dir, filename.c_str(),
                             seen_keys);
    }
  }

  FSDIR_Close(dir_handle);
  FSUSER_CloseArchive(sdmc_archive);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(app, "BROWSER folder native scan end rc=0x%08lx added=%d total=%d dir=%s",
           (unsigned long)rc, app->BookCount() - count_before,
           app->BookCount(), dir.c_str());
#endif
  return R_FAILED(rc) ? 1 : 0;
}

static int AppendBooksFromPosixDirectory(App *app,
                                         LibraryGradientContext *gradient_ctx,
                                         const std::string &dir,
                                         std::set<std::string> *seen_keys) {
  if (!app)
    return 1;
#ifdef DSLIBRIS_DEBUG
  const int count_before = app->BookCount();
  DBG_LOGF(app, "BROWSER folder posix scan begin dir=%s", dir.c_str());
#endif
  DIR *dp = opendir(dir.c_str());
  if (!dp) {
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(app, "BROWSER folder posix scan opendir failed errno=%d dir=%s",
             errno, dir.c_str());
#endif
    return 1;
  }
  struct dirent *ent;
  while ((ent = readdir(dp))) {
    if (ent->d_name[0] == '.')
      continue;
    std::string child = dir + "/" + ent->d_name;
    if (PathIsDirectory(child))
      continue;
    AppendBookFromFilename(app, gradient_ctx, dir, ent->d_name, seen_keys);
  }
  closedir(dp);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(app, "BROWSER folder posix scan end added=%d total=%d dir=%s",
           app->BookCount() - count_before, app->BookCount(), dir.c_str());
#endif
  return 0;
}

} // namespace

LibraryController::LibraryController(App &app)
    : app_(app), inside_folder_(false) {}

int LibraryController::FindBooks() {
  gradient_ctx_.ts = app_.ts.get();
  gradient_ctx_.color_mode = &app_.colorMode;

  std::set<std::string> seen_keys;
  auto scan_with_native_fs = [&](const std::string &dir) -> int {
    FS_Archive sdmc_archive;
    Result rc = FSUSER_OpenArchive(&sdmc_archive, ARCHIVE_SDMC,
                                   fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(rc))
      return 1;

    std::string rel_path = SdmcToArchiveRelPath(dir);
    Handle dir_handle = 0;
    rc = FSUSER_OpenDirectory(&dir_handle, sdmc_archive,
                              fsMakePath(PATH_ASCII, rel_path.c_str()));
    if (R_FAILED(rc)) {
      FSUSER_CloseArchive(sdmc_archive);
      return 1;
    }

    while (true) {
      FS_DirectoryEntry entries[16];
      u32 read_count = 0;
      rc = FSDIR_Read(dir_handle, &read_count, 16, entries);
      if (R_FAILED(rc) || read_count == 0)
        break;

      for (u32 i = 0; i < read_count; i++) {
        std::string filename;
        if (!Utf16NameToUtf8(entries[i].name, &filename))
          continue;
        if (filename.empty())
          continue;
        if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)
          AppendFolderEntry(&app_, dir, filename, &seen_keys);
        else
          AppendBookFromFilename(&app_, &gradient_ctx_, dir, filename.c_str(), &seen_keys);
      }
    }

    FSDIR_Close(dir_handle);
    FSUSER_CloseArchive(sdmc_archive);
    return 0;
  };

  auto scan_with_posix_fallback = [&](const std::string &dir) -> int {
    DIR *dp = opendir(dir.c_str());
    if (!dp)
      return 1;
    struct dirent *ent;
    while ((ent = readdir(dp))) {
      if (ent->d_name[0] == '.')
        continue;
      std::string child = dir + "/" + ent->d_name;
      if (PathIsDirectory(child))
        AppendFolderEntry(&app_, dir, ent->d_name, &seen_keys);
      else
        AppendBookFromFilename(&app_, &gradient_ctx_, dir, ent->d_name, &seen_keys);
    }
    closedir(dp);
    return 0;
  };

  bool found_any_source = false;
  std::vector<std::string> scan_dirs;
  scan_dirs.push_back(app_.bookdir);
  if (app_.bookdir != paths::kRomfsBookDir)
    scan_dirs.push_back(paths::kRomfsBookDir);

  for (size_t i = 0; i < scan_dirs.size(); i++) {
    const std::string &dir = scan_dirs[i];
    bool scanned = false;

    // Native FS path only supports SDMC archive.
    if (dir.find("sdmc:/") == 0)
      scanned = (scan_with_native_fs(dir) == 0);
    if (!scanned)
      scanned = (scan_with_posix_fallback(dir) == 0);

    if (scanned)
      found_any_source = true;
  }

  return found_any_source ? 0 : 1;
}

void LibraryController::PrepareLibrary() {
  // Pre-populate metadata from the disk cache before sorting so the initial
  // sort uses real titles. Cache hits are ~2ms each; misses return quickly
  // (no source file is opened). Books without a cache entry are left for the
  // warmup job system to handle after the browser opens.
  for (auto &book : app_.books) {
    if (book->IsBrowserFolder())
      continue;
    book->TryLoadMetadataFromCache();
  }

  SortBooks();
  for (auto &book : app_.books)
    book->GetBookmarks().sort();
}

void LibraryController::SortBooks() {
  const LibrarySortMode mode = app_.prefs
      ? app_.prefs->library_sort_mode
      : LIBRARY_SORT_TITLE;
  switch (mode) {
  case LIBRARY_SORT_FILENAME:
    std::sort(app_.books.begin(), app_.books.end(), &BookFilenameLessThan);
    break;
  case LIBRARY_SORT_AUTHOR:
    std::sort(app_.books.begin(), app_.books.end(), &BookAuthorLessThan);
    break;
  case LIBRARY_SORT_FILETYPE:
    std::sort(app_.books.begin(), app_.books.end(), &BookFiletypeLessThan);
    break;
  case LIBRARY_SORT_DATE_MODIFIED:
    std::sort(app_.books.begin(), app_.books.end(), &BookDateModifiedLessThan);
    break;
  case LIBRARY_SORT_RECENT:
    for (auto &book : app_.books) {
      DBG_LOGF(&app_, "recently-opened: pre-sort t=%lu \"%s\"",
               (unsigned long)book->GetLastOpenedTime(),
               book->GetFileName() ? book->GetFileName() : "?");
    }
    std::sort(app_.books.begin(), app_.books.end(), &BookRecentLessThan);
    break;
  default:
    std::sort(app_.books.begin(), app_.books.end(), &BookTitleLessThan);
    break;
  }
}

void LibraryController::RebuildRoot() {
  PauseBrowserJobs();
  ResetBrowserMarquee();
  ClearBrowserBooksAndButtons(&app_);
  inside_folder_ = false;
  current_folder_name_.clear();
  current_folder_path_.clear();
  FindBooks();
  PrepareLibrary();
  browser_init();
  ResetBrowserMarquee();
  app_.ts->MarkAllScreensDirty();
  app_.SetBrowserDirty(true);
}

void LibraryController::EnterFolder(Book *folder) {
  if (!folder || !folder->IsBrowserFolder())
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "BROWSER enter folder name=%s path=%s",
           folder->GetBrowserFolderDisplayName().c_str(),
           folder->GetBrowserFolderPath().c_str());
#endif
  LoadFolderPath(folder->GetBrowserFolderPath(),
                 folder->GetBrowserFolderDisplayName());
}

void LibraryController::LoadFolderPath(const std::string &folder_path,
                                       const std::string &folder_name) {
  const std::string path_copy = folder_path;
  const std::string name_copy = folder_name;

  if (path_copy.empty() || !PathIsDirectory(path_copy)) {
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(&app_, "BROWSER load folder rejected name=%s path=%s empty=%u isdir=%u",
             name_copy.c_str(), path_copy.c_str(),
             path_copy.empty() ? 1u : 0u,
             (!path_copy.empty() && PathIsDirectory(path_copy)) ? 1u : 0u);
#endif
    return;
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "BROWSER load folder begin name=%s path=%s",
           name_copy.c_str(), path_copy.c_str());
#endif
  PauseBrowserJobs();
  ResetBrowserMarquee();
  ClearBrowserBooksAndButtons(&app_);
  inside_folder_ = true;
  current_folder_path_ = path_copy;
  current_folder_name_ = name_copy;

  std::set<std::string> seen_keys;
  bool scanned = false;
  if (path_copy.find("sdmc:/") == 0)
    scanned = (AppendBooksFromNativeDirectory(&app_, &gradient_ctx_,
                                              path_copy, &seen_keys) == 0);
  if (!scanned)
    AppendBooksFromPosixDirectory(&app_, &gradient_ctx_, path_copy,
                                  &seen_keys);
  PrepareLibrary();
  browser_init();
  ResetBrowserMarquee();
  app_.ts->MarkAllScreensDirty();
  app_.SetBrowserDirty(true);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "BROWSER load folder end name=%s path=%s count=%d selected=%s scanned_native=%u",
           name_copy.c_str(), path_copy.c_str(), app_.BookCount(),
           app_.GetSelectedBook() ? app_.GetSelectedBook()->GetFileName()
                                  : "(null)",
           scanned ? 1u : 0u);
#endif
}

Book *LibraryController::RestoreSavedBookSelection(const char *folder,
                                                   const char *filename) {
  if (!filename || !*filename)
    return NULL;

  Book *root_book = FindBookByFolderAndFile(app_.books, folder, filename);
  if (!root_book && (!folder || !*folder))
    root_book = FindBookByFileNameOnly(app_.books, filename);
  if (root_book) {
    app_.SetSelectedBook(root_book);
    return root_book;
  }

  std::string folder_path = folder ? std::string(folder) : std::string();
  if (folder_path.empty()) {
    std::vector<std::string> scan_dirs;
    scan_dirs.push_back(app_.bookdir);
    if (app_.bookdir != paths::kRomfsBookDir)
      scan_dirs.push_back(paths::kRomfsBookDir);
    for (size_t i = 0; i < scan_dirs.size() && folder_path.empty(); i++) {
      DIR *dp = opendir(scan_dirs[i].c_str());
      if (!dp)
        continue;
      struct dirent *ent;
      while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.')
          continue;
        std::string child = scan_dirs[i] + "/" + ent->d_name;
        if (!PathIsDirectory(child))
          continue;
        if (PathContainsExactRegularFile(child, filename)) {
          folder_path = child;
          break;
        }
      }
      closedir(dp);
    }
  }

  if (folder_path.empty() || !PathIsDirectory(folder_path))
    return NULL;

  std::string file_path = folder_path + "/" + filename;
  if (!PathIsRegularFile(file_path))
    return NULL;

  LoadFolderPath(folder_path, BasenameFromPath(folder_path));
  Book *book = FindBookByFolderAndFile(app_.books, folder_path.c_str(), filename);
  if (!book) {
    RebuildRoot();
    return NULL;
  }
  app_.SetSelectedBook(book);
  return book;
}

void LibraryController::LeaveFolder() {
  if (!inside_folder_)
    return;
  RebuildRoot();
}

void LibraryController::OpenSelectedBrowserEntry() {
  Book *selected = app_.GetSelectedBook();
  if (!selected) {
#ifdef DSLIBRIS_DEBUG
    DBG_LOG(&app_, "BROWSER open selected ignored: no selection");
#endif
    return;
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "BROWSER open selected folder=%u title=%s file=%s path=%s",
           selected->IsBrowserFolder() ? 1u : 0u,
           selected->GetTitle() ? selected->GetTitle() : "(null)",
           selected->GetFileName() ? selected->GetFileName() : "(null)",
           selected->IsBrowserFolder()
               ? selected->GetBrowserFolderPath().c_str()
               : (selected->GetFolderName() ? selected->GetFolderName()
                                            : "(null)"));
#endif
  if (selected->IsBrowserFolder()) {
    EnterFolder(selected);
    return;
  }
  app_.OpenBook();
}

bool LibraryController::IsInsideFolder() const { return inside_folder_; }

void App::browser_draw() { library_controller_->browser_draw(); }

void App::browser_handleevent() { library_controller_->browser_handleevent(); }

void App::browser_init() { library_controller_->browser_init(); }

void App::UnloadNonVisibleBrowserCoverCaches() {
  library_controller_->UnloadNonVisibleBrowserCoverCaches();
}

void App::browser_nextpage() { library_controller_->browser_nextpage(); }

void App::browser_prevpage() { library_controller_->browser_prevpage(); }

void App::LoadVisibleBrowserCoverCaches() {
  library_controller_->LoadVisibleBrowserCoverCaches();
}

bool App::HasQueuedJob(app_job_type_t type, Book *book) const {
  return library_controller_->HasQueuedJob(type, book);
}

void App::PrioritizeSelectedBookJobs(Book *selected_book) {
  library_controller_->PrioritizeSelectedBookJobs(selected_book);
}

void App::EnqueueJob(app_job_type_t type, Book *book) {
  library_controller_->EnqueueJob(type, book);
}

void App::TickBrowserWarmup() { library_controller_->TickBrowserWarmup(); }

void App::browser_tick_marquee() { library_controller_->browser_tick_marquee(); }

void App::ResetBrowserMarquee() { library_controller_->ResetBrowserMarquee(); }

void App::QueueBookWarmup(Book *book) { library_controller_->QueueBookWarmup(book); }

void App::QueueTocResolve(Book *book) { library_controller_->QueueTocResolve(book); }

void App::ProcessJobs(u32 budget_ms) { library_controller_->ProcessJobs(budget_ms); }

size_t App::PauseBrowserJobs() { return library_controller_->PauseBrowserJobs(); }

bool App::IsBrowserInsideFolder() const {
  return library_controller_->IsInsideFolder();
}

Book *App::RestoreSavedBookSelection(const char *folder,
                                     const char *filename) {
  return library_controller_->RestoreSavedBookSelection(folder, filename);
}
