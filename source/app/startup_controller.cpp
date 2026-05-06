/*
    3dslibris - startup_controller.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Runs the 3DS boot sequence: text/screen init, runtime asset validation,
      book discovery, prefs load, library preparation, and initial UI setup.
    - Accepts required assets from either sdmc:/ or romfs:/ and shows fatal
      boot guidance when the runtime installation is incomplete.
    - Applies persisted runtime state and schedules reopening of the last book
      after boot when appropriate.
*/

#include "app/startup_controller.h"

#include <algorithm>
#include <stdio.h>
#include <sys/stat.h>

#include "app/app.h"
#include "app/library_controller.h"
#include "book/book.h"
#include "shared/debug_log.h"
#include "shared/main.h"
#include "shared/path_utils.h"
#include "settings/prefs.h"
#include "ui/text.h"
#include "ui/ui_button_skin.h"
#include "app/version.h"

namespace
{

  // Helper function to check if the SD card and necessary files are present, and print instructions if not.
  static bool PathExistsAndType(const char *path, bool want_dir)
  {
    if (!path || !*path)
      return false;
    struct stat st;
    if (stat(path, &st) != 0)
      return false;
    if (want_dir)
      return S_ISDIR(st.st_mode);
    return S_ISREG(st.st_mode);
  }

  // Checks if the specified file is readable, used for verifying the presence of necessary files.
  static bool FileReadable(const char *path)
  {
    if (!path || !*path)
      return false;
    FILE *fp = fopen(path, "rb");
    if (!fp)
      return false;
    fclose(fp);
    return true;
  }

  // Checks if a resource exists in either the SD card or the ROMFS, allowing for flexible asset loading.
  static bool RuntimePathExistsEither(const char *sdmc_path,
                                      const char *romfs_path, bool want_dir)
  {
    if (want_dir)
    {
      return PathExistsAndType(sdmc_path, true) ||
             PathExistsAndType(romfs_path, true);
    }
    return FileReadable(sdmc_path) || FileReadable(romfs_path);
  }

  // Checks if the default font directory is usable by verifying the presence of a known font file, ensuring that the app can load fonts properly.
  static bool FontDirLooksUsable(const std::string &dir)
  {
    if (dir.empty())
      return false;
    static const char *kProbeFont = "LiberationSerif-Regular.ttf";
    std::string probe = dir + "/" + kProbeFont;
    return FileReadable(probe.c_str());
  }

  // Resolves the default font directory, preferring the SD card but falling back
  // to ROMFS if necessary, so the app can find fonts regardless of install method.
  static std::string ResolveDefaultFontDir()
  {
    if (FontDirLooksUsable(paths::GetFontDir()))
      return paths::GetFontDir();
    if (FontDirLooksUsable(paths::kRomfsFontDir))
      return std::string(paths::kRomfsFontDir);
    return paths::GetFontDir();
  }

  // Normalizes runtime asset paths, ensuring that the app can find necessary resources even if the SD card installation is incomplete, and providing a fallback mechanism for fonts.
  static void NormalizeRuntimeAssetPaths(App *app)
  {
    if (!app)
      return;
    if (!FontDirLooksUsable(app->fontdir)) {
      app->fontdir = ResolveDefaultFontDir();
      if (app->ts)
        app->ts->SetFontDir(app->fontdir);
    }
  }

  // Collects any missing runtime files. Checks the book directory and the
  // bundled fonts against both sdmc:/ and romfs:/ so a partial install is
  // reported accurately regardless of install method.
  static void CollectMissingRuntimeFiles(std::vector<std::string> *missing)
  {
    if (!missing)
      return;
    missing->clear();

    if (!RuntimePathExistsEither(paths::GetBookDir().c_str(),
                                 paths::kRomfsBookDir, true))
      missing->push_back("book/ (sdmc or romfs)");

    // Check the first 5 bundled fonts (the ones shipped with every release).
    // Romfs paths are derived from paths::kRomfsFontDir to stay in sync with
    // the rest of the path constants.
    static const size_t kBundledFontCount = 5;
    for (size_t i = 0; i < kBundledFontCount; i++)
    {
      const char *filename = paths::kDefaultFonts[i][0];
      std::string sdmc_path = paths::GetFontDir() + "/" + filename;
      std::string romfs_path = std::string(paths::kRomfsFontDir) + "/" + filename;
      if (!RuntimePathExistsEither(sdmc_path.c_str(), romfs_path.c_str(), false))
        missing->push_back(filename);
    }
  }

  // Prints instructions to the console for how to properly install the app on the SD card, including which files are missing, to help users resolve installation issues.
  static void PrintInstallHelpToConsole(const std::vector<std::string> &missing)
  {
    printf("\n[FAIL] Incomplete SD install for 3dslibris.\n\n");
    printf("Download and extract:\n");
    printf("  3dslibris-sdmc.zip\n");
    printf("from GitHub Releases into the SD root:\n");
    printf("  sdmc:/\n\n");
    printf("Expected layout:\n");
    printf("  sdmc:/3ds/3dslibris/3dslibris.3dsx\n");
    printf("  sdmc:/3ds/3dslibris/book/\n");
    printf("  sdmc:/3ds/3dslibris/font/\n");
    printf("  sdmc:/3ds/3dslibris/resources/\n");
    printf("Data files are also accepted under:\n");
    printf("  sdmc:/config/3dslibris/\n\n");
    if (!missing.empty())
    {
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

// Runs the boot sequence, including loading fonts, checking for necessary files, and preparing the library. Returns a code indicating success or failure of the boot process.
void StartupController::DrawBootStatus(const char *title,
                                       const std::vector<std::string> &lines,
                                       bool fatal)
{
  // Preserve the caller's Text state; boot UI temporarily overrides style,
  // screen target, and pixel size and must restore them before returning.
  int savedStyle = app_.ts->GetStyle();
  int savedColorMode = app_.ts->GetColorMode();
  u16 *savedScreen = app_.ts->GetScreen();
  int savedPixelSize = app_.ts->pixelsize;

  app_.ts->SetStyle(TEXT_STYLE_BROWSER);
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
  for (size_t i = 0; i < lines.size(); i++)
  {
    app_.ts->SetPen(14, 84 + (int)i * 18);
    app_.ts->PrintString(lines[i].c_str());
  }

  if (!fatal)
    app_.ts->DrawRect(14, 138, 226, 152, 0xBDF7);
  else
  {
    app_.ts->SetPen(14, 216);
    app_.ts->PrintString("Press START to exit");
  }

  app_.ts->SetStyle(savedStyle);
  app_.ts->SetColorMode(savedColorMode);
  app_.ts->SetScreen(savedScreen);
  app_.ts->SetPixelSize(savedPixelSize);

  if (app_.ts->BlitToFramebuffer())
  {
    gfxFlushBuffers();
    gfxSwapBuffers();
  }
}

// Halts the app on a fatal boot status, displaying the error message and waiting for user input to exit, ensuring that users are informed of critical issues during startup.
int StartupController::HaltOnFatalBootStatus()
{
  halt(app_.ts.get(), -1);
  return 2;
}

// Runs the boot sequence, including loading fonts, checking for necessary files, and preparing the library. Returns a code indicating success or failure of the boot process.
int StartupController::RunBootSequence()
{
  const int ok = 0;

  printf("Loading fonts...\n");
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "DBG BUILD SIG: %s %s", __DATE__, __TIME__);
#endif
  if (app_.ts->Init() != ok)
  {
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
  if (!missing_runtime.empty())
  {
    app_.PrintStatus("error: incomplete sdmc install");
    std::vector<std::string> lines;
    lines.push_back("Download 3dslibris-sdmc.zip");
    lines.push_back("and extract it to sdmc:/");
    lines.push_back("Expected path: sdmc:/3ds/3dslibris/");
    lines.push_back("or sdmc:/config/3dslibris/");
    lines.push_back("Missing files from the SD package");
    lines.push_back(missing_runtime[0]);
    if (missing_runtime.size() > 1)
    {
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
  if (app_.StartupFindBooks() != ok)
  {
    app_.PrintStatus("error: no book directory");
    DrawBootStatus("Incomplete installation",
                   {"Download 3dslibris-sdmc.zip",
                    "and extract it to sdmc:/",
                    "Expected folder: sdmc:/3ds/3dslibris/book",
                    "or sdmc:/config/3dslibris/book",
                    "or include books in romfs:/3ds/3dslibris/book"},
                   true);
    return HaltOnFatalBootStatus();
  }
  if (app_.BookCount() == 0)
  {
    // TODO: Consider allowing boot into an empty library view with onboarding
    // text instead of treating "no books found" as a fatal boot condition.
    app_.PrintStatus("error: no epub files found");
    DrawBootStatus("No books found",
                   {"Copy your EPUB/FB2/TXT/RTF/ODT files",
                    "to sdmc:/3ds/3dslibris/book",
                    "or sdmc:/config/3dslibris/book"},
                   true);
    return HaltOnFatalBootStatus();
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "TIMING: scan_books=%llums count=%u",
           (unsigned long long)(osGetTime() - t_scan_ms),
           (unsigned)app_.BookCount());
#endif

  const int prefs_read_err = app_.prefs->Read();
  if (prefs_read_err != 0)
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "warning: prefs read failed (err=%d)",
             prefs_read_err);
    app_.PrintStatus(msg);
  }
  UiButtonSkin_SetColorMode(app_.colorMode);
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

  Book *current_book = app_.GetCurrentBook();
  if (app_.reopen && current_book)
  {
    app_.SetSelectedBook(current_book);
    const char *title = current_book->GetTitle();
    DrawBootStatus("Booting",
                   {"Opening last book...",
                    (title && *title) ? title : "(untitled)"},
                   false);
    app_.SetPendingBootReopen(true);
  }
  return 0;
}
