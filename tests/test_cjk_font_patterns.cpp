// Host test for CJK font fallback pattern matching in path_utils.h
// This tests the auto-detection logic that decides which fonts to load
// as CJK fallback faces at startup.

#include "shared/path_constants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: expected %d, got %d", label, expected, actual);
    Fail(buf);
  }
}

// Replicate the matching logic from font_manager.cpp so we can test it
// in isolation without FreeType or 3DS dependencies.
static bool FilenameMatchesCjkPattern(const char *filename) {
  if (!filename)
    return false;
  for (int i = 0; i < paths::kCjkFontPatternCount; i++) {
    if (strstr(filename, paths::kCjkFontPatterns[i]))
      return true;
  }
  return false;
}

// --- CJK pattern detection tests ---

void TestNotoSansCjkDetected() {
  ExpectTrue("NotoSansCJKSC-Regular.ttf",
             FilenameMatchesCjkPattern("NotoSansCJKSC-Regular.ttf"));
  ExpectTrue("NotoSansCJKtc-Bold.otf",
             FilenameMatchesCjkPattern("NotoSansCJKtc-Bold.otf"));
}

void TestNotoSerifCjkDetected() {
  ExpectTrue("NotoSerifCJKsc-Regular.ttf",
             FilenameMatchesCjkPattern("NotoSerifCJKsc-Regular.ttf"));
}

void TestSourceHanSansDetected() {
  ExpectTrue("SourceHanSansSC-Regular.otf",
             FilenameMatchesCjkPattern("SourceHanSansSC-Regular.otf"));
  ExpectTrue("SourceHanSansTC-Bold.ttc",
             FilenameMatchesCjkPattern("SourceHanSansTC-Bold.ttc"));
}

void TestSourceHanSerifDetected() {
  ExpectTrue("SourceHanSerifSC-Regular.ttf",
             FilenameMatchesCjkPattern("SourceHanSerifSC-Regular.ttf"));
}

void TestWenQuanYiDetected() {
  ExpectTrue("WenQuanYiMicroHei.ttf",
             FilenameMatchesCjkPattern("WenQuanYiMicroHei.ttf"));
  ExpectTrue("WenQuanYiZenHei.ttf",
             FilenameMatchesCjkPattern("WenQuanYiZenHei.ttf"));
}

void TestArpukaiDetected() {
  ExpectTrue("ARPLUKaiCN-Regular.ttf",
             FilenameMatchesCjkPattern("ARPLUKaiCN-Regular.ttf"));
  ExpectTrue("ARPLUKaiTW-MBE.ttf",
             FilenameMatchesCjkPattern("ARPLUKaiTW-MBE.ttf"));
}

void TestArpumingDetected() {
  ExpectTrue("ARPLUMingCN-Regular.ttf",
             FilenameMatchesCjkPattern("ARPLUMingCN-Regular.ttf"));
}

void TestDroidSansFallbackDetected() {
  ExpectTrue("DroidSansFallbackFull.ttf",
             FilenameMatchesCjkPattern("DroidSansFallbackFull.ttf"));
}

void TestHanaMinDetected() {
  ExpectTrue("HanaMinA.ttf",
             FilenameMatchesCjkPattern("HanaMinA.ttf"));
  ExpectTrue("HanaMinB.ttf",
             FilenameMatchesCjkPattern("HanaMinB.ttf"));
}

void TestGenericCjkDetected() {
  ExpectTrue("My-CJK-Font.ttf",
             FilenameMatchesCjkPattern("My-CJK-Font.ttf"));
}

// --- Hebrew font patterns ---

void TestNotoSansHebrewDetected() {
  ExpectTrue("NotoSansHebrew-Regular.ttf",
             FilenameMatchesCjkPattern("NotoSansHebrew-Regular.ttf"));
  ExpectTrue("NotoSansHebrew-Bold.ttf",
             FilenameMatchesCjkPattern("NotoSansHebrew-Bold.ttf"));
}

void TestNotoSerifHebrewDetected() {
  ExpectTrue("NotoSerifHebrew-Regular.ttf",
             FilenameMatchesCjkPattern("NotoSerifHebrew-Regular.ttf"));
}

void TestFrankRuhlDetected() {
  ExpectTrue("FrankRuhlLibre-Regular.ttf",
             FilenameMatchesCjkPattern("FrankRuhlLibre-Regular.ttf"));
}

void TestAlefDetected() {
  ExpectTrue("Alef-Regular.ttf",
             FilenameMatchesCjkPattern("Alef-Regular.ttf"));
}

// --- Arabic font patterns ---

void TestNotoSansArabicDetected() {
  ExpectTrue("NotoSansArabic-Regular.ttf",
             FilenameMatchesCjkPattern("NotoSansArabic-Regular.ttf"));
}

void TestNotoNaskhArabicDetected() {
  ExpectTrue("NotoNaskhArabic-Regular.ttf",
             FilenameMatchesCjkPattern("NotoNaskhArabic-Regular.ttf"));
}

void TestAmiriDetected() {
  ExpectTrue("Amiri-Regular.ttf",
             FilenameMatchesCjkPattern("Amiri-Regular.ttf"));
}

void TestScheherazadeDetected() {
  ExpectTrue("ScheherazadeNew-Regular.ttf",
             FilenameMatchesCjkPattern("ScheherazadeNew-Regular.ttf"));
}

// --- Korean font patterns ---

void TestNanumGothicDetected() {
  ExpectTrue("NanumGothic-Regular.ttf",
             FilenameMatchesCjkPattern("NanumGothic-Regular.ttf"));
  ExpectTrue("NanumGothicCoding.ttf",
             FilenameMatchesCjkPattern("NanumGothicCoding.ttf"));
}

void TestNanumMyeongjoDetected() {
  ExpectTrue("NanumMyeongjo-Regular.ttf",
             FilenameMatchesCjkPattern("NanumMyeongjo-Regular.ttf"));
}

void TestNotoSansKRDetected() {
  ExpectTrue("NotoSansKR-Regular.otf",
             FilenameMatchesCjkPattern("NotoSansKR-Regular.otf"));
}

void TestNotoSerifKRDetected() {
  ExpectTrue("NotoSerifKR-Regular.otf",
             FilenameMatchesCjkPattern("NotoSerifKR-Regular.otf"));
}

void TestUnBatangDetected() {
  ExpectTrue("UnBatang.ttf",
             FilenameMatchesCjkPattern("UnBatang.ttf"));
}

void TestUnDotumDetected() {
  ExpectTrue("UnDotum.ttf",
             FilenameMatchesCjkPattern("UnDotum.ttf"));
}

void TestBaekmukDetected() {
  ExpectTrue("Baekmuk-Batang.ttf",
             FilenameMatchesCjkPattern("Baekmuk-Batang.ttf"));
}

// --- Non-CJK fonts must NOT match ---

void TestNonCjkNotDetected() {
  ExpectFalse("LiberationSerif-Regular.ttf",
              FilenameMatchesCjkPattern("LiberationSerif-Regular.ttf"));
  ExpectFalse("LiberationSans-Regular.ttf",
              FilenameMatchesCjkPattern("LiberationSans-Regular.ttf"));
  ExpectFalse("LiberationSans-Bold.ttf",
              FilenameMatchesCjkPattern("LiberationSans-Bold.ttf"));
  ExpectFalse("LiberationSans-Italic.ttf",
              FilenameMatchesCjkPattern("LiberationSans-Italic.ttf"));
  ExpectFalse("OpenSans-Regular.ttf",
              FilenameMatchesCjkPattern("OpenSans-Regular.ttf"));
  ExpectFalse("Roboto-Regular.ttf",
              FilenameMatchesCjkPattern("Roboto-Regular.ttf"));
  ExpectFalse("DejaVuSans.ttf",
              FilenameMatchesCjkPattern("DejaVuSans.ttf"));
  ExpectFalse("TimesNewRoman.ttf",
              FilenameMatchesCjkPattern("TimesNewRoman.ttf"));
}

// --- Edge cases ---

void TestNullFilename() {
  ExpectFalse("null filename", FilenameMatchesCjkPattern(NULL));
}

void TestEmptyFilename() {
  ExpectFalse("empty filename", FilenameMatchesCjkPattern(""));
}

void TestCjkPatternCount() {
  ExpectEq("kCjkFontPatternCount", paths::kCjkFontPatternCount, 25);
}

void TestAllPatternsNonEmpty() {
  for (int i = 0; i < paths::kCjkFontPatternCount; i++) {
    char label[64];
    snprintf(label, sizeof(label), "pattern[%d] non-empty", i);
    ExpectTrue(label, strlen(paths::kCjkFontPatterns[i]) > 0);
  }
}

} // namespace

int main() {
  fprintf(stderr, "test_cjk_font_patterns: running...\n");

  TestNotoSansCjkDetected();
  TestNotoSerifCjkDetected();
  TestSourceHanSansDetected();
  TestSourceHanSerifDetected();
  TestWenQuanYiDetected();
  TestArpukaiDetected();
  TestArpumingDetected();
  TestDroidSansFallbackDetected();
  TestHanaMinDetected();
  TestGenericCjkDetected();
  TestNotoSansHebrewDetected();
  TestNotoSerifHebrewDetected();
  TestFrankRuhlDetected();
  TestAlefDetected();
  TestNotoSansArabicDetected();
  TestNotoNaskhArabicDetected();
  TestAmiriDetected();
  TestScheherazadeDetected();
  TestNanumGothicDetected();
  TestNanumMyeongjoDetected();
  TestNotoSansKRDetected();
  TestNotoSerifKRDetected();
  TestUnBatangDetected();
  TestUnDotumDetected();
  TestBaekmukDetected();
  TestNonCjkNotDetected();
  TestNullFilename();
  TestEmptyFilename();
  TestCjkPatternCount();
  TestAllPatternsNonEmpty();

  fprintf(stderr, "test_cjk_font_patterns: ALL PASSED\n");
  return 0;
}
