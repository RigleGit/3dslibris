/*
    3dslibris - path_utils.h
    Shared path/URL utilities and project path constants.
    Extracted by Rigle to eliminate duplication across the codebase.
*/

#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// --- Project paths (single source of truth) ---
// All runtime paths on the SD card are defined here.
// If the directory layout changes, update these constants only.

namespace paths {

// Base directories
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
