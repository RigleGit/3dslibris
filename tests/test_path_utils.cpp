#include "../include/path_utils.h"
#include "test_assert.h"

#include <cstring>
#include <string>

namespace {

void ExpectNonEmpty(const char *label, const char *value) {
  test::ExpectTrue(label, value != nullptr);
  test::ExpectTrue(label, std::strlen(value) > 0);
}

void ExpectPathPrefix(const char *label, const char *value, const char *prefix) {
  test::ExpectStrContains(label, value, prefix);
}

void ExpectFontEntry(const char *label, const char *name, const char *path) {
  ExpectNonEmpty(label, name);
  ExpectNonEmpty(label, path);
  test::ExpectStrContains(label, path, "/font/");
  test::ExpectStrContains(label, path, name);
}

}

int main() {
  ExpectNonEmpty("kSdmcBase exists", paths::kSdmcBase);
  ExpectNonEmpty("kRomfsBase exists", paths::kRomfsBase);
  ExpectNonEmpty("kBookDir exists", paths::kBookDir);
  ExpectNonEmpty("kRomfsBookDir exists", paths::kRomfsBookDir);
  ExpectNonEmpty("kFontDir exists", paths::kFontDir);
  ExpectNonEmpty("kCacheBaseDir exists", paths::kCacheBaseDir);
  ExpectNonEmpty("kCoverCacheDir exists", paths::kCoverCacheDir);
  ExpectNonEmpty("kCoverCacheManifest exists", paths::kCoverCacheManifest);
  ExpectNonEmpty("kEpubCacheDir exists", paths::kEpubCacheDir);
  ExpectNonEmpty("kMobiCacheDir exists", paths::kMobiCacheDir);
  ExpectNonEmpty("kMobiCoverMetaCacheDir exists", paths::kMobiCoverMetaCacheDir);
  ExpectNonEmpty("kLogFile exists", paths::kLogFile);
  ExpectNonEmpty("kPrefsFile exists", paths::kPrefsFile);
  ExpectNonEmpty("kResourceDir exists", paths::kResourceDir);
  ExpectNonEmpty("kIconPngDir exists", paths::kIconPngDir);
  ExpectNonEmpty("kIconDir exists", paths::kIconDir);
  ExpectNonEmpty("kResourceBase exists", paths::kResourceBase);

  ExpectPathPrefix("book dir prefix", paths::kBookDir, "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("romfs book dir prefix", paths::kRomfsBookDir, "romfs:/3ds/3dslibris/");
  ExpectPathPrefix("font dir prefix", paths::kFontDir, "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("resource dir prefix", paths::kResourceDir, "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("cache dir prefix", paths::kCacheBaseDir, "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("log file prefix", paths::kLogFile, "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("prefs file prefix", paths::kPrefsFile, "sdmc:/3ds/3dslibris/");

  test::ExpectEq("splash path count", paths::kSplashPathCount, 4);
  for (int i = 0; i < paths::kSplashPathCount; ++i) {
    ExpectNonEmpty("splash path exists", paths::kSplashPaths[i]);
    test::ExpectStrContains("splash path prefix", paths::kSplashPaths[i], "sdmc:/3ds/3dslibris/");
    test::ExpectStrContains("splash path suffix", paths::kSplashPaths[i], "splash.");
  }

  test::ExpectEq("default font count", paths::kDefaultFontCount, 9);
  bool hasMonoFont = false;
  bool hasMonoBoldFont = false;
  bool hasMonoItalicFont = false;
  bool hasMonoBoldItalicFont = false;
  for (int i = 0; i < paths::kDefaultFontCount; ++i) {
    ExpectFontEntry("default font entry", paths::kDefaultFonts[i][0], paths::kDefaultFonts[i][1]);
    if (std::strcmp(paths::kDefaultFonts[i][0], "LiberationMono-Regular.ttf") == 0)
      hasMonoFont = true;
    if (std::strcmp(paths::kDefaultFonts[i][0], "LiberationMono-Bold.ttf") == 0)
      hasMonoBoldFont = true;
    if (std::strcmp(paths::kDefaultFonts[i][0], "LiberationMono-Italic.ttf") == 0)
      hasMonoItalicFont = true;
    if (std::strcmp(paths::kDefaultFonts[i][0], "LiberationMono-BoldItalic.ttf") == 0)
      hasMonoBoldItalicFont = true;
  }
  test::ExpectTrue("default mono font entry", hasMonoFont);
  test::ExpectTrue("default mono bold font entry", hasMonoBoldFont);
  test::ExpectTrue("default mono italic font entry", hasMonoItalicFont);
  test::ExpectTrue("default mono bold italic font entry", hasMonoBoldItalicFont);

  {
    std::string decoded = UrlDecode("chapter%2Etxt");
    test::ExpectStrEq("UrlDecode decodes percent escapes", decoded.c_str(), "chapter.txt");
  }
  {
    std::string decoded = UrlDecode("folder%20name/file.txt");
    test::ExpectStrEq("UrlDecode decodes spaces", decoded.c_str(), "folder name/file.txt");
  }
  {
    std::string decoded = UrlDecode("bad%zzvalue");
    test::ExpectStrEq("UrlDecode preserves invalid escapes", decoded.c_str(), "bad%zzvalue");
  }
  {
    std::string decoded = UrlDecode("mixed%2fCASE%3aok");
    test::ExpectStrEq("UrlDecode keeps decoded bytes", decoded.c_str(), "mixed/CASE:ok");
  }

  {
    std::string normalized = NormalizePath("/sdmc:/3ds/./3dslibris/../3dslibris/book/cover.jpg");
    test::ExpectStrEq("NormalizePath strips leading slash and dot segments",
                      normalized.c_str(), "sdmc:/3ds/3dslibris/book/cover.jpg");
  }
  {
    std::string normalized = NormalizePath("sdmc:\\3ds\\3dslibris\\book\\chapter.txt");
    test::ExpectStrEq("NormalizePath converts backslashes",
                      normalized.c_str(), "sdmc:/3ds/3dslibris/book/chapter.txt");
  }
  {
    std::string normalized = NormalizePath("sdmc:/3ds//3dslibris/book/./part/../chapter.txt");
    test::ExpectStrEq("NormalizePath removes empty and parent segments",
                      normalized.c_str(), "sdmc:/3ds/3dslibris/book/chapter.txt");
  }
  {
    std::string normalized = NormalizePath("");
    test::ExpectStrEq("NormalizePath handles empty input", normalized.c_str(), "");
  }

  {
    std::string basename = BasenamePath("sdmc:/3ds/3dslibris/book/novel.epub");
    test::ExpectStrEq("BasenamePath extracts filename", basename.c_str(), "novel.epub");
  }
  {
    std::string basename = BasenamePath("novel.epub");
    test::ExpectStrEq("BasenamePath returns full name for no slash", basename.c_str(), "novel.epub");
  }
  {
    std::string basename = BasenamePath("sdmc:/3ds/3dslibris/book/");
    test::ExpectStrEq("BasenamePath returns empty for trailing slash", basename.c_str(), "");
  }
  {
    std::string basename = BasenamePath("");
    test::ExpectStrEq("BasenamePath handles empty input", basename.c_str(), "");
  }

  {
    std::string stripped = StripFragmentAndQuery("chapter.xhtml#page=3");
    test::ExpectStrEq("StripFragmentAndQuery removes fragment", stripped.c_str(), "chapter.xhtml");
  }
  {
    std::string stripped = StripFragmentAndQuery("chapter.xhtml?ref=nav");
    test::ExpectStrEq("StripFragmentAndQuery removes query", stripped.c_str(), "chapter.xhtml");
  }
  {
    std::string stripped = StripFragmentAndQuery("chapter.xhtml?ref=nav#frag");
    test::ExpectStrEq("StripFragmentAndQuery stops at earliest delimiter", stripped.c_str(), "chapter.xhtml");
  }
  {
    std::string stripped = StripFragmentAndQuery("chapter.xhtml");
    test::ExpectStrEq("StripFragmentAndQuery leaves plain path alone", stripped.c_str(), "chapter.xhtml");
  }

  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub",
                                                "images/cover.jpg");
    test::ExpectStrEq("ResolveRelativePath resolves sibling file",
                      resolved.c_str(), "sdmc:/3ds/3dslibris/book/images/cover.jpg");
  }
  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub",
                                                "../shared/style.css");
    test::ExpectStrEq("ResolveRelativePath resolves parent traversal",
                      resolved.c_str(), "sdmc:/3ds/3dslibris/shared/style.css");
  }
  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub",
                                                "%69mages/%63over.jpg");
    test::ExpectStrEq("ResolveRelativePath decodes relative refs",
                      resolved.c_str(), "sdmc:/3ds/3dslibris/book/images/cover.jpg");
  }
  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub",
                                                "/assets/logo.png");
    test::ExpectStrEq("ResolveRelativePath keeps absolute paths normalized",
                      resolved.c_str(), "assets/logo.png");
  }
  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub",
                                                "https://example.com/image.png");
    test::ExpectStrEq("ResolveRelativePath rejects external URIs",
                      resolved.c_str(), "");
  }
  {
    std::string resolved = ResolveRelativePath("sdmc:/3ds/3dslibris/book/novel.epub", "");
    test::ExpectStrEq("ResolveRelativePath rejects empty refs", resolved.c_str(), "");
  }
  {
    std::string resolved = ResolveRelativePath("chapter.epub", "images/cover.jpg");
    test::ExpectStrEq("ResolveRelativePath works without a base folder",
                      resolved.c_str(), "images/cover.jpg");
  }

  return 0;
}
