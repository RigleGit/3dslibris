#include "app/library_controller.h"

#include <algorithm>
#include <dirent.h>
#include <strings.h>
#include <vector>

#include <3ds.h>

#include "app/app.h"
#include "book/book.h"
#include "debug_log.h"
#include "shared/app_flow_utils.h"
#include "shared/utf8_utils.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

namespace {

static format_t ToBookFormat(app_flow_utils::BookFileFormat format) {
  switch (format) {
  case app_flow_utils::BookFileFormat::Epub:
    return FORMAT_EPUB;
  case app_flow_utils::BookFileFormat::MuPdf:
    return FORMAT_PDF;
  case app_flow_utils::BookFileFormat::Cbz:
    return FORMAT_CBZ;
  case app_flow_utils::BookFileFormat::XhtmlLike:
    return FORMAT_XHTML;
  case app_flow_utils::BookFileFormat::Unsupported:
  default:
    return FORMAT_UNDEF;
  }
}

static bool BookTitleLessThan(Book *a, Book *b) {
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
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

static void LogFilenameStage(App *app, const char *stage, const char *value) {
#if !UTF8_FILENAME_DIAG
  (void)app;
  (void)stage;
  (void)value;
  return;
#else
  if (!app || !stage || !value)
    return;
  char msg[512];
  std::string bytes = HexBytesForLog(value);
  snprintf(msg, sizeof(msg),
           "FindBooks %-20s len=%u valid=%d bytes=[%s] text=\"%s\"", stage,
           (unsigned)strlen(value), LooksLikeValidUtf8(value) ? 1 : 0,
           bytes.c_str(), value);
  DBG_LOG(app, msg);
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

static bool HasBookWithFileName(const App *app, const char *filename) {
  if (!app || !filename || !*filename)
    return false;
  for (size_t i = 0; i < app->books.size(); i++) {
    if (!app->books[i] || !app->books[i]->GetFileName())
      continue;
    if (strcasecmp(app->books[i]->GetFileName(), filename) == 0)
      return true;
  }
  return false;
}

static void AppendBookFromFilename(App *app, const std::string &source_dir,
                                   const char *filename) {
  if (!app || !app_flow_utils::ShouldIndexBookFilename(filename))
    return;
  format_t format = ToBookFormat(app_flow_utils::DetectBookFormat(filename));
  if (format == FORMAT_UNDEF)
    return;

  std::string raw_name(filename);
  std::string io_name = NormalizeFsFilenameForIo(filename);
  if (HasBookWithFileName(app, io_name.c_str()))
    return;
  LogFilenameStage(app, "d_name", raw_name.c_str());
  if (io_name != raw_name)
    LogFilenameStage(app, "d_name_io_fix", io_name.c_str());
  Book *book = new Book(app);
  book->SetFolderName(source_dir.c_str());
  book->SetFileName(io_name.c_str());
  book->SetTitle(io_name.c_str());
  LogFilenameStage(app, "book.filename", book->GetFileName());
  LogFilenameStage(app, "book.title", book->GetTitle());
  book->format = format;
  app->books.push_back(book);
}

} // namespace

LibraryController::LibraryController(App &app) : app_(app) {}

int LibraryController::FindBooks() {
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
        if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)
          continue;
        std::string filename;
        if (!Utf16NameToUtf8(entries[i].name, &filename))
          continue;
        if (filename.empty())
          continue;
        AppendBookFromFilename(&app_, dir, filename.c_str());
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
    while ((ent = readdir(dp)))
      AppendBookFromFilename(&app_, dir, ent->d_name);
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
  std::sort(app_.books.begin(), app_.books.end(), &BookTitleLessThan);
  for (auto &book : app_.books)
    book->GetBookmarks()->sort();
}

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

void App::PruneBrowserWarmupJobs(Book *selected_book) {
  library_controller_->PruneBrowserWarmupJobs(selected_book);
}

void App::EnqueueJob(app_job_type_t type, Book *book) {
  library_controller_->EnqueueJob(type, book);
}

void App::TickBrowserWarmup() { library_controller_->TickBrowserWarmup(); }

void App::QueueBookWarmup(Book *book) { library_controller_->QueueBookWarmup(book); }

void App::QueueTocResolve(Book *book) { library_controller_->QueueTocResolve(book); }

void App::ProcessJobs(u32 budget_ms) { library_controller_->ProcessJobs(budget_ms); }
