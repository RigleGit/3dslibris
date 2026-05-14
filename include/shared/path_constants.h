/*
    3dslibris - path_constants.h
    Project-specific SD card path constants and dynamic path resolution.
    Single source of truth for all runtime paths on the SD card.
    If the directory layout changes, update only here.
*/

#ifndef PATH_CONSTANTS_H
#define PATH_CONSTANTS_H

#include <sys/stat.h>
#include <string>
#include <vector>

namespace paths {

// Base directories (used by inline Get*() helpers below)
static const char *kLegacySdmcBase  = "sdmc:/3ds/3dslibris";
static const char *kConfigSdmcBase  = "sdmc:/config/3dslibris";

// Constants used directly by individual translation units.
// __attribute__((unused)) suppresses the per-TU warning for TUs that
// include this header but do not reference these symbols.
static const char *kLogFile      __attribute__((unused)) = "sdmc:/3ds/3dslibris/3dslibris.log";
static const char *kRomfsBookDir __attribute__((unused)) = "romfs:/3ds/3dslibris/book";
static const char *kRomfsFontDir __attribute__((unused)) = "romfs:/3ds/3dslibris/font";

// Bundled fonts (used by startup_controller.cpp)
static const char *kDefaultFonts[][2] __attribute__((unused)) = {
    {"LiberationSerif-Regular.ttf",    "sdmc:/3ds/3dslibris/font/LiberationSerif-Regular.ttf"},
    {"LiberationSerif-Bold.ttf",       "sdmc:/3ds/3dslibris/font/LiberationSerif-Bold.ttf"},
    {"LiberationSerif-Italic.ttf",     "sdmc:/3ds/3dslibris/font/LiberationSerif-Italic.ttf"},
    {"LiberationSerif-BoldItalic.ttf", "sdmc:/3ds/3dslibris/font/LiberationSerif-BoldItalic.ttf"},
    {"LiberationSans-Regular.ttf",     "sdmc:/3ds/3dslibris/font/LiberationSans-Regular.ttf"},
    {"LiberationMono-Regular.ttf",     "sdmc:/3ds/3dslibris/font/LiberationMono-Regular.ttf"},
    {"LiberationMono-Bold.ttf",        "sdmc:/3ds/3dslibris/font/LiberationMono-Bold.ttf"},
    {"LiberationMono-Italic.ttf",      "sdmc:/3ds/3dslibris/font/LiberationMono-Italic.ttf"},
    {"LiberationMono-BoldItalic.ttf",  "sdmc:/3ds/3dslibris/font/LiberationMono-BoldItalic.ttf"},
};

// Fallback font filename patterns (checked in order of preference).
// When a font filename contains any of these substrings, it is auto-loaded
// as a fallback face for non-Latin glyph coverage (CJK, Hebrew, Arabic).
static const char *kCjkFontPatterns[] __attribute__((unused)) = {
    // CJK (Chinese / Japanese / Korean)
    "NotoSansCJK",
    "NotoSerifCJK",
    "SourceHanSans",
    "SourceHanSerif",
    "WenQuanYi",
    "ARPLUKai",
    "ARPLUMing",
    "DroidSansFallback",
    "HanaMin",
    "CJK",
    // Hebrew
    "NotoSansHebrew",
    "NotoSerifHebrew",
    "FrankRuhl",
    "Alef",
    // Arabic
    "NotoSansArabic",
    "NotoNaskhArabic",
    "Amiri",
    "Scheherazade",
    // Korean
    "NanumGothic",
    "NanumMyeongjo",
    "NotoSansKR",
    "NotoSerifKR",
    "UnBatang",
    "UnDotum",
    "Baekmuk",
};
static const int kCjkFontPatternCount __attribute__((unused)) = 25;

inline bool PathExists(const char *path, bool expect_directory = false) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (expect_directory)
    return S_ISDIR(st.st_mode);
  return true;
}

inline bool HasSdmcInstallMarkers(const char *base_path) {
  if (!base_path || !*base_path)
    return false;
  const std::string base(base_path);
  return PathExists((base + "/book").c_str(), true) ||
         PathExists((base + "/font").c_str(), true) ||
         PathExists((base + "/resources").c_str(), true) ||
         PathExists((base + "/3dslibris.xml").c_str(), false);
}

inline const std::string &GetSdmcBase() {
  static const std::string path =
      HasSdmcInstallMarkers(kLegacySdmcBase)
          ? std::string(kLegacySdmcBase)
          : (HasSdmcInstallMarkers(kConfigSdmcBase)
                 ? std::string(kConfigSdmcBase)
                 : std::string(kLegacySdmcBase));
  return path;
}

inline const std::string &GetBookDir() {
  static const std::string path = GetSdmcBase() + "/book";
  return path;
}

inline const std::string &GetFontDir() {
  static const std::string path = GetSdmcBase() + "/font";
  return path;
}

inline const std::string &GetResourceDir() {
  static const std::string path = GetSdmcBase() + "/resources";
  return path;
}

inline const std::string &GetCacheBaseDir() {
  static const std::string path = GetSdmcBase() + "/cache";
  return path;
}

inline const std::string &GetCoverCacheDir() {
  static const std::string path = GetCacheBaseDir() + "/covers";
  return path;
}

inline const std::string &GetCoverCacheManifest() {
  static const std::string path = GetCoverCacheDir() + "/manifest.txt";
  return path;
}

inline const std::string &GetEpubCacheDir() {
  static const std::string path = GetCacheBaseDir() + "/epub";
  return path;
}

inline const std::string &GetMobiCacheDir() {
  static const std::string path = GetCacheBaseDir() + "/mobi";
  return path;
}

inline const std::string &GetMobiCoverMetaCacheDir() {
  static const std::string path = GetCacheBaseDir() + "/mobi-cover";
  return path;
}

inline const std::string &GetMetaCacheDir() {
  static const std::string path = GetCacheBaseDir() + "/meta";
  return path;
}

inline const std::string &GetLogFile() {
  static const std::string path = GetSdmcBase() + "/3dslibris.log";
  return path;
}

inline const std::string &GetPrefsFile() {
  static const std::string path = GetSdmcBase() + "/3dslibris.xml";
  return path;
}

inline const std::string &GetIconPngDir() {
  static const std::string path = GetResourceDir() + "/ui/icons/png";
  return path;
}

inline const std::string &GetIconDir() {
  static const std::string path = GetResourceDir() + "/ui/icons";
  return path;
}

inline const std::string &GetResourceBase() {
  return GetResourceDir();
}

inline std::vector<std::string> GetSplashPathList() {
  const std::string &base = GetSdmcBase();
  std::vector<std::string> paths;
  paths.push_back(base + "/resources/splash.jpg");
  paths.push_back(base + "/resources/splash.jpeg");
  paths.push_back(base + "/splash.jpg");
  paths.push_back(base + "/splash.jpeg");
  if (base != kLegacySdmcBase) {
    paths.push_back(std::string(kLegacySdmcBase) + "/resources/splash.jpg");
    paths.push_back(std::string(kLegacySdmcBase) + "/resources/splash.jpeg");
    paths.push_back(std::string(kLegacySdmcBase) + "/splash.jpg");
    paths.push_back(std::string(kLegacySdmcBase) + "/splash.jpeg");
  }
  return paths;
}

} // namespace paths

#endif // PATH_CONSTANTS_H
