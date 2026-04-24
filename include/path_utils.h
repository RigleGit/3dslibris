/*
    3dslibris - path_utils.h
    Shared path/URL utilities and project path constants.
    Extracted by Rigle to eliminate duplication across the codebase.
*/

#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <algorithm>
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// --- Project paths (single source of truth) ---
// All runtime paths on the SD card are defined here.
// If the directory layout changes, update these constants only.

namespace paths {

// Base directories
static const char *kLegacySdmcBase  = "sdmc:/3ds/3dslibris";
static const char *kConfigSdmcBase  = "sdmc:/config/3dslibris";
static const char *kSdmcBase        = "sdmc:/3ds/3dslibris";
static const char *kRomfsBase       = "romfs:/3ds/3dslibris";
static const char *kBookDir         = "sdmc:/3ds/3dslibris/book";
static const char *kRomfsBookDir    = "romfs:/3ds/3dslibris/book";
static const char *kFontDir         = "sdmc:/3ds/3dslibris/font";
static const char *kRomfsFontDir    = "romfs:/3ds/3dslibris/font";
static const char *kResourceDir     = "sdmc:/3ds/3dslibris/resources";
static const char *kCacheBaseDir    = "sdmc:/3ds/3dslibris/cache";

// Cache subdirectories
static const char *kCoverCacheDir   = "sdmc:/3ds/3dslibris/cache/covers";
static const char *kCoverCacheManifest = "sdmc:/3ds/3dslibris/cache/covers/manifest.txt";
static const char *kEpubCacheDir    = "sdmc:/3ds/3dslibris/cache/epub";
static const char *kMobiCacheDir    = "sdmc:/3ds/3dslibris/cache/mobi";
static const char *kMobiCoverMetaCacheDir = "sdmc:/3ds/3dslibris/cache/mobi-cover";
static const char *kMetaCacheDir          = "sdmc:/3ds/3dslibris/cache/meta";

// Runtime files
static const char *kLogFile         = "sdmc:/3ds/3dslibris/3dslibris.log";
static const char *kPrefsFile       = "sdmc:/3ds/3dslibris/3dslibris.xml";

// Splash screen search paths
static const char *kSplashPaths[] = {
    "sdmc:/3ds/3dslibris/resources/splash.jpg",
    "sdmc:/3ds/3dslibris/resources/splash.jpeg",
    "sdmc:/3ds/3dslibris/splash.jpg",
    "sdmc:/3ds/3dslibris/splash.jpeg",
};
static const int kSplashPathCount = 4;

// UI icon search paths
static const char *kIconPngDir     = "sdmc:/3ds/3dslibris/resources/ui/icons/png";
static const char *kIconDir        = "sdmc:/3ds/3dslibris/resources/ui/icons";
static const char *kResourceBase   = "sdmc:/3ds/3dslibris/resources";

// Bundled fonts
static const char *kDefaultFonts[][2] = {
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
static const int kDefaultFontCount = 9;

// Fallback font filename patterns (checked in order of preference).
// When a font filename contains any of these substrings, it is auto-loaded
// as a fallback face for non-Latin glyph coverage (CJK, Hebrew, Arabic).
static const char *kCjkFontPatterns[] = {
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
static const int kCjkFontPatternCount = 25;

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

// --- URL decoding ---

inline std::string UrlDecode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int value = 0;
      if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
        out.push_back((char)value);
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

// --- Path normalization (resolve `.`, `..`, backslashes, leading `/`) ---

inline std::string NormalizePath(const std::string &path) {
  std::string in = path;
  std::replace(in.begin(), in.end(), '\\', '/');
  while (!in.empty() && in[0] == '/')
    in.erase(in.begin());

  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= in.size(); i++) {
    if (i == in.size() || in[i] == '/') {
      if (cur == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(in[i]);
    }
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i)
      out.push_back('/');
    out += parts[i];
  }
  return out;
}

// --- Basename extraction ---

inline std::string BasenamePath(const std::string &path) {
  if (path.empty())
    return "";
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return "";
  return path.substr(slash + 1);
}

// --- Strip #fragment and ?query ---

inline std::string StripFragmentAndQuery(const std::string &path) {
  size_t stop = path.find_first_of("#?");
  if (stop == std::string::npos)
    return path;
  return path.substr(0, stop);
}

// --- Resolve a relative path against a base file path ---

inline std::string ResolveRelativePath(const std::string &base_file,
                                       const std::string &ref_raw) {
  std::string ref = UrlDecode(ref_raw);
  if (ref.empty())
    return "";
  if (ref.find("://") != std::string::npos)
    return "";

  if (ref[0] == '/') {
    return NormalizePath(ref);
  }

  std::string base = base_file;
  size_t slash = base.find_last_of('/');
  std::string folder =
      (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
  return NormalizePath(folder + ref);
}

#endif // PATH_UTILS_H
