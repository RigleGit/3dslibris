#include "app/startup_controller.h"

#include <algorithm>
#include <stdio.h>
#include <sys/stat.h>

#include "app/app.h"
#include "app/library_controller.h"
#include "book/book.h"
#include "debug_log.h"
#include "main.h"
#include "path_utils.h"
#include "settings/prefs.h"
#include "ui/text.h"
#include "version.h"

namespace {

static bool PathExistsAndType(const char *path, bool want_dir) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (want_dir)
    return S_ISDIR(st.st_mode);
  return S_ISREG(st.st_mode);
}

static bool FileReadable(const char *path) {
  if (!path || !*path)
    return false;
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  fclose(fp);
  return true;
}

static bool RuntimePathExistsEither(const char *sdmc_path,
                                    const char *romfs_path, bool want_dir) {
  if (want_dir)
    return PathExistsAndType(sdmc_path, true) || PathExistsAndType(romfs_path, true);
  return FileReadable(sdmc_path) || FileReadable(romfs_path);
}

static bool FontDirLooksUsable(const std::string &dir) {
  if (dir.empty())
    return false;
  static const char *kProbeFont = "LiberationSerif-Regular.ttf";
  std::string probe = dir + "/" + kProbeFont;
  return FileReadable(probe.c_str());
}

static std::string ResolveDefaultFontDir() {
  static const char *kSdmcFontDir = paths::kFontDir;
  static const char *kRomfsFontDir = "romfs:/3ds/3dslibris/font";
  if (FontDirLooksUsable(kSdmcFontDir))
    return std::string(kSdmcFontDir);
  if (FontDirLooksUsable(kRomfsFontDir))
    return std::string(kRomfsFontDir);
  return std::string(kSdmcFontDir);
}

static void NormalizeRuntimeAssetPaths(App *app) {
  if (!app)
    return;
  if (!FontDirLooksUsable(app->fontdir))
    app->fontdir = ResolveDefaultFontDir();
}

static void CollectMissingRuntimeFiles(std::vector<std::string> *missing) {
  if (!missing)
    return;
  missing->clear();

  if (!RuntimePathExistsEither(paths::kBookDir, paths::kRomfsBookDir, true))
    missing->push_back("book/ (sdmc or romfs)");

  struct RuntimeFallbackFile {
    const char *sdmc_path;
    const char *romfs_path;
    const char *label;
  };
  static const RuntimeFallbackFile kBundled[] = {
      {paths::kDefaultFonts[0][1], "romfs:/3ds/3dslibris/font/LiberationSerif-Regular.ttf", paths::kDefaultFonts[0][0]},
      {paths::kDefaultFonts[1][1], "romfs:/3ds/3dslibris/font/LiberationSerif-Bold.ttf", paths::kDefaultFonts[1][0]},
      {paths::kDefaultFonts[2][1], "romfs:/3ds/3dslibris/font/LiberationSerif-Italic.ttf", paths::kDefaultFonts[2][0]},
      {paths::kDefaultFonts[3][1], "romfs:/3ds/3dslibris/font/LiberationSerif-BoldItalic.ttf", paths::kDefaultFonts[3][0]},
      {paths::kDefaultFonts[4][1], "romfs:/3ds/3dslibris/font/LiberationSans-Regular.ttf", paths::kDefaultFonts[4][0]},
  };

  for (size_t i = 0; i < sizeof(kBundled) / sizeof(kBundled[0]); i++) {
    if (!RuntimePathExistsEither(kBundled[i].sdmc_path, kBundled[i].romfs_path,
                                 false))
      missing->push_back(kBundled[i].label);
  }
}

static void PrintInstallHelpToConsole(const std::vector<std::string> &missing) {
  printf("\n[FAIL] Incomplete SD install for 3dslibris.\n\n");
  printf("Download and extract:\n");
  printf("  3dslibris-sdmc.zip\n");
  printf("from GitHub Releases into the SD root:\n");
  printf("  sdmc:/\n\n");
  printf("Expected layout:\n");
  printf("  sdmc:/3ds/3dslibris/3dslibris.3dsx\n");
  printf("  sdmc:/3ds/3dslibris/book/\n");
  printf("  sdmc:/3ds/3dslibris/font/\n");
  printf("  sdmc:/3ds/3dslibris/resources/\n\n");
  if (!missing.empty()) {
    printf("Missing files:\n");
    size_t shown = std::min<size_t>(missing.size(), 8);
    for (size_t i = 0; i < shown; i++)
      printf("  %s\n", missing[i].c_str());
    if (missing.size() > shown)
      printf("  ... and %u more\n", (unsigned)(missing.size() - shown));
    printf("\n");
  }
}

} // namespace

StartupController::StartupController(App &app) : app_(app) {}

void StartupController::DrawBootStatus(const char *title,
                                       const std::vector<std::string> &lines,
                                       bool fatal) {
  int savedStyle = app_.ts->GetStyle();
  int savedColorMode = app_.ts->GetColorMode();
  u16 *savedScreen = app_.ts->GetScreen();
  int savedPixelSize = app_.ts->pixelsize;

  app_.ts->SetStyle(TEXT_STYLE_BROWSER);
  app_.ts->SetColorMode(0);
  app_.ts->SetPixelSize(10);

  app_.ts->SetScreen(app_.ts->screenleft);
  app_.ts->PrintSplash(app_.ts->screenleft);

  app_.ts->SetScreen(app_.ts->screenright);
  app_.ts->ClearScreen();
  app_.DrawBottomGradientBackground();
  app_.ts->DrawRect(8, 10, 232, fatal ? 76 : 64, 0xC618);
  app_.ts->SetPixelSize(14);
  app_.ts->SetPen(14, 20);
  app_.ts->PrintString(title && *title ? title : "Booting");
  app_.ts->SetPixelSize(10);
  for (size_t i = 0; i < lines.size(); i++) {
    app_.ts->SetPen(14, 84 + (int)i * 18);
    app_.ts->PrintString(lines[i].c_str());
  }

  if (!fatal)
    app_.ts->DrawRect(14, 138, 226, 152, 0xBDF7);
  else {
    app_.ts->SetPen(14, 216);
    app_.ts->PrintString("Press START to exit");
  }

  app_.ts->SetStyle(savedStyle);
  app_.ts->SetColorMode(savedColorMode);
  app_.ts->SetScreen(savedScreen);
  app_.ts->SetPixelSize(savedPixelSize);

  if (app_.ts->BlitToFramebuffer()) {
    gfxFlushBuffers();
    gfxSwapBuffers();
  }
}

int StartupController::HaltOnFatalBootStatus() {
  halt(app_.ts, -1);
  return 2;
}

int StartupController::RunBootSequence() {
  const int ok = 0;

  printf("Loading fonts...\n");
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "DBG BUILD SIG: %s %s", __DATE__, __TIME__);
#endif
  if (app_.ts->Init() != ok) {
    std::vector<std::string> missing;
    CollectMissingRuntimeFiles(&missing);
    PrintInstallHelpToConsole(missing);
    return 1;
  }

  app_.StartupInitScreens();
  app_.ts->SetStyle(TEXT_STYLE_BROWSER);
  DrawBootStatus("Booting", {"Searching for books..."}, false);

  std::vector<std::string> missing_runtime;
  CollectMissingRuntimeFiles(&missing_runtime);
  if (!missing_runtime.empty()) {
    app_.PrintStatus("error: incomplete sdmc install");
    std::vector<std::string> lines;
    lines.push_back("Download 3dslibris-sdmc.zip");
    lines.push_back("and extract it to sdmc:/");
    lines.push_back("Expected path: sdmc:/3ds/3dslibris/");
    lines.push_back("Missing files from the SD package");
    lines.push_back(missing_runtime[0]);
    if (missing_runtime.size() > 1) {
      char extra[48];
      snprintf(extra, sizeof(extra), "+%u more",
               (unsigned)(missing_runtime.size() - 1));
      lines.push_back(extra);
    }
    DrawBootStatus("Incomplete installation", lines, true);
    return HaltOnFatalBootStatus();
  }

  DBG_LOG(&app_, "Searching for books...");
#ifdef DSLIBRIS_DEBUG
  u64 t_scan_ms = osGetTime();
#endif
  if (app_.StartupFindBooks() != ok) {
    app_.PrintStatus("error: no book directory");
    DrawBootStatus("Incomplete installation",
                   {"Download 3dslibris-sdmc.zip",
                    "and extract it to sdmc:/",
                    "Expected folder: sdmc:/3ds/3dslibris/book",
                    "or include books in romfs:/3ds/3dslibris/book"},
                   true);
    return HaltOnFatalBootStatus();
  }
  if (app_.BookCount() == 0) {
    app_.PrintStatus("error: no epub files found");
    DrawBootStatus("No books found",
                   {"Copy your EPUB/FB2/TXT/RTF/ODT files",
                    "to sdmc:/3ds/3dslibris/book"},
                   true);
    return HaltOnFatalBootStatus();
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "TIMING: scan_books=%llums count=%u",
           (unsigned long long)(osGetTime() - t_scan_ms),
           (unsigned)app_.BookCount());
#endif

  const int prefs_read_err = app_.prefs->Read();
  if (prefs_read_err != 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "warning: prefs read failed (err=%d)",
             prefs_read_err);
    app_.PrintStatus(msg);
  }
  NormalizeRuntimeAssetPaths(&app_);
  DrawBootStatus("Booting", {"Preparing library..."}, false);
  app_.SetOrientation(app_.orientation);
  DBG_LOG(&app_, "Preparing library...");
#ifdef DSLIBRIS_DEBUG
  u64 t_prepare_ms = osGetTime();
#endif
  app_.StartupPrepareLibrary();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "TIMING: prepare_library=%llums",
           (unsigned long long)(osGetTime() - t_prepare_ms));
#endif
  DBG_LOG(&app_, "Library ready.");

  app_.StartupInitUiAndBrowser();

  DBG_LOG(&app_, VERSION);

  if (app_.reopen && app_.GetCurrentBook()) {
    app_.SetSelectedBook(app_.GetCurrentBook());
    const char *title = app_.GetCurrentBook()->GetTitle();
    DrawBootStatus("Booting",
                   {"Opening last book...",
                    (title && *title) ? title : "(untitled)"},
                   false);
    app_.SetPendingBootReopen(true);
  }
  return 0;
}
