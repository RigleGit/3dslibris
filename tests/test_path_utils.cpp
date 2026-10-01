#include "../include/shared/path_constants.h"
#include "../include/shared/path_utils.h"
#include "test_assert.h"
#include "library/cover_override_utils.h"

#include <cstring>
#include <string>

#include "font_filename_match.inc"

namespace {

void ExpectNonEmpty(const char *label, const char *value) {
  test::ExpectTrue(label, value != nullptr);
  test::ExpectTrue(label, std::strlen(value) > 0);
}

void ExpectNonEmpty(const char *label, const std::string &value) {
  test::ExpectTrue(label, !value.empty());
}

void ExpectPathPrefix(const char *label, const std::string &value,
                      const char *prefix) {
  test::ExpectStrContains(label, value.c_str(), prefix);
}

void ExpectFontEntry(const char *label, const char *name, const char *path) {
  ExpectNonEmpty(label, name);
  ExpectNonEmpty(label, path);
  test::ExpectStrContains(label, path, "/font/");
  test::ExpectStrContains(label, path, name);
}

void TestFallbackFontConfigurationAtRuntimeMatcher() {
  const char *recognized[] = {
      "NotoSansCJKSC-Regular.ttf",
      "NotoSansCJKtc-Bold.otf",
      "NotoSerifCJKsc-Regular.ttf",
      "SourceHanSansSC-Regular.otf",
      "SourceHanSansTC-Bold.ttc",
      "SourceHanSerifSC-Regular.ttf",
      "WenQuanYiMicroHei.ttf",
      "WenQuanYiZenHei.ttf",
      "ARPLUKaiCN-Regular.ttf",
      "ARPLUKaiTW-MBE.ttf",
      "ARPLUMingCN-Regular.ttf",
      "DroidSansFallbackFull.ttf",
      "HanaMinA.ttf",
      "HanaMinB.ttf",
      "My-CJK-Font.ttf",
      "NotoSansHebrew-Regular.ttf",
      "NotoSansHebrew-Bold.ttf",
      "NotoSerifHebrew-Regular.ttf",
      "FrankRuhlLibre-Regular.ttf",
      "Alef-Regular.ttf",
      "NotoSansArabic-Regular.ttf",
      "NotoNaskhArabic-Regular.ttf",
      "Amiri-Regular.ttf",
      "ScheherazadeNew-Regular.ttf",
      "NanumGothic-Regular.ttf",
      "NanumGothicCoding.ttf",
      "NanumMyeongjo-Regular.ttf",
      "NotoSansKR-Regular.otf",
      "NotoSerifKR-Regular.otf",
      "UnBatang.ttf",
      "UnDotum.ttf",
      "Baekmuk-Batang.ttf",
  };
  for (const char *name : recognized)
    test::ExpectTrue(name, FilenameMatchesCjkPattern(name));
  const char *ordinary[] = {
      "LiberationSerif-Regular.ttf",
      "LiberationSans-Regular.ttf",
      "LiberationSans-Bold.ttf",
      "LiberationSans-Italic.ttf",
      "OpenSans-Regular.ttf",
      "Roboto-Regular.ttf",
      "DejaVuSans.ttf",
      "TimesNewRoman.ttf",
      "",
  };
  for (const char *name : ordinary)
    test::ExpectTrue(name, !FilenameMatchesCjkPattern(name));
  test::ExpectTrue("null font filename", !FilenameMatchesCjkPattern(nullptr));
  const size_t count =
      sizeof(paths::kCjkFontPatterns) / sizeof(paths::kCjkFontPatterns[0]);
  test::ExpectEq("font matcher scans complete configuration",
                 paths::kCjkFontPatternCount, (int)count);
  for (size_t i = 0; i < count; ++i)
    ExpectNonEmpty("fallback font pattern", paths::kCjkFontPatterns[i]);
}

void TestBookCoverSidecarPaths() {
  const std::vector<std::string> candidates =
      cover_override_utils::BuildBookCandidates("sdmc:/books/Novel.epub");
  const char *extensions[] = {".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG"};
  for (const char *extension : extensions) {
    const std::string expected = std::string("sdmc:/books/Novel") + extension;
    bool found = false;
    for (const std::string &candidate : candidates)
      found = found || candidate == expected;
    test::ExpectTrue(expected.c_str(), found);
  }
}

} // namespace

int main() {
  TestFallbackFontConfigurationAtRuntimeMatcher();
  TestBookCoverSidecarPaths();
  ExpectNonEmpty("GetSdmcBase exists", paths::GetSdmcBase());
  ExpectNonEmpty("kLegacySdmcBase exists", paths::kLegacySdmcBase);
  ExpectNonEmpty("kConfigSdmcBase exists", paths::kConfigSdmcBase);
  ExpectNonEmpty("kRomfsBookDir exists", paths::kRomfsBookDir);
  ExpectNonEmpty("GetBookDir exists", paths::GetBookDir());
  ExpectNonEmpty("GetFontDir exists", paths::GetFontDir());
  ExpectNonEmpty("GetCacheBaseDir exists", paths::GetCacheBaseDir());
  ExpectNonEmpty("GetCoverCacheDir exists", paths::GetCoverCacheDir());
  ExpectNonEmpty("GetCoverCacheManifest exists", paths::GetCoverCacheManifest());
  ExpectNonEmpty("GetEpubCacheDir exists", paths::GetEpubCacheDir());
  ExpectNonEmpty("GetMobiCacheDir exists", paths::GetMobiCacheDir());
  ExpectNonEmpty("GetMobiCoverMetaCacheDir exists", paths::GetMobiCoverMetaCacheDir());
  ExpectNonEmpty("kLogFile exists", paths::kLogFile);
  ExpectNonEmpty("GetPrefsFile exists", paths::GetPrefsFile());
  ExpectNonEmpty("GetResourceDir exists", paths::GetResourceDir());
  ExpectNonEmpty("GetIconPngDir exists", paths::GetIconPngDir());
  ExpectNonEmpty("GetIconDir exists", paths::GetIconDir());
  ExpectNonEmpty("GetResourceBase exists", paths::GetResourceBase());

  ExpectPathPrefix("book dir prefix", paths::GetBookDir(), "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("romfs book dir prefix", paths::kRomfsBookDir, "romfs:/3ds/3dslibris/");
  ExpectPathPrefix("font dir prefix", paths::GetFontDir(), "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("resource dir prefix", paths::GetResourceDir(), "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("cache dir prefix", paths::GetCacheBaseDir(), "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("log file prefix", paths::GetLogFile(), "sdmc:/3ds/3dslibris/");
  ExpectPathPrefix("prefs file prefix", paths::GetPrefsFile(), "sdmc:/3ds/3dslibris/");

  {
    const std::string active_base = paths::GetSdmcBase();
    test::ExpectTrue("active sdmc base is legacy or config",
                     active_base == paths::kLegacySdmcBase ||
                         active_base == paths::kConfigSdmcBase);
    test::ExpectTrue("active book dir suffix",
                     paths::GetBookDir().find("/book") != std::string::npos);
    test::ExpectTrue("active font dir suffix",
                     paths::GetFontDir().find("/font") != std::string::npos);
    test::ExpectTrue("active prefs file suffix",
                     paths::GetPrefsFile().find("/3dslibris.xml") != std::string::npos);
  }

  {
    std::vector<std::string> splash = paths::GetSplashPathList();
    test::ExpectTrue("splash path count >= 4", (int)splash.size() >= 4);
    for (size_t i = 0; i < splash.size(); ++i) {
      ExpectNonEmpty("splash path exists", splash[i]);
      test::ExpectStrContains("splash path sdmc prefix", splash[i].c_str(), "sdmc:/");
      test::ExpectStrContains("splash path suffix", splash[i].c_str(), "splash.");
    }
  }

  const int kDefaultFontCount =
      (int)(sizeof(paths::kDefaultFonts) / sizeof(paths::kDefaultFonts[0]));
  test::ExpectEq("default font count", kDefaultFontCount, 9);
  bool hasMonoFont = false;
  bool hasMonoBoldFont = false;
  bool hasMonoItalicFont = false;
  bool hasMonoBoldItalicFont = false;
  for (int i = 0; i < kDefaultFontCount; ++i) {
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
