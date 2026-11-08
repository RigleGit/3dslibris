#include "book/page_alignment_utils.h"

#include "shared/text_token_constants.h"
#include "test_assert.h"

namespace {

enum TextStyleId {
  kTextStyleBold = 1,
  kTextStyleBoldItalic = 3,
  kTextStyleMonoBold = 6,
  kTextStyleMonoBoldItalic = 8,
};

int MeasureGlyph(uint32_t, unsigned char style, void *) {
  switch (style) {
  case kTextStyleBold:
  case kTextStyleBoldItalic:
  case kTextStyleMonoBold:
  case kTextStyleMonoBoldItalic:
    return 2;
  default:
    return 1;
  }
}

void TestIgnoresControlTokensAndPayloads() {
  const uint32_t buf[] = {TEXT_LINK_START, 99, 'A', TEXT_LINK_END, 'B', '\n'};
  test::ExpectEq("link markers ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestTracksStyleChangesWhileMeasuring() {
  const uint32_t buf[] = {'A', TEXT_BOLD_ON, 'B', TEXT_BOLD_OFF, 'C', '\n'};
  test::ExpectEq("style markers affect width",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 4);
}

void TestStopsAtBlockTokens() {
  const uint32_t buf[] = {'A', 'B', TEXT_IMAGE, 7, 'C', '\n'};
  test::ExpectEq("image token ends line measurement",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestIgnoresFontSizeTokenAndPayload() {
  const uint32_t buf[] = {'A', TEXT_FONT_SIZE, 24, ' ', 'B', '\n'};
  test::ExpectEq("font-size token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 3);
}

void TestIgnoresImageAlignTokenAndPayload() {
  const uint32_t buf[] = {'A', TEXT_IMAGE_ALIGN, 2, 'B', '\n'};
  test::ExpectEq("image-align token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestIgnoresLineStartXTokenAndPayload() {
  const uint32_t buf[] = {TEXT_LINE_START_X, 48, 'A', 'B', '\n'};
  test::ExpectEq("line-start-x token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthFitsCompletely() {
  // "AB" = 2px; available = 10; no clamping needed
  const uint32_t buf[] = {'A', 'B', '\n'};
  test::ExpectEq("short line returned unchanged",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     10, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthBreaksAtWordBoundary() {
  // "AAA BBB" where each char = 1px, available = 5
  // "AAA " = 4px fits; adding 'B' (5th char of "BBB") -> "AAA B" = 5px fits;
  // adding second 'B' = 6px > 5 -> break before "BBB" at last space boundary
  // last_word_end_width recorded when space seen = 3 (width of "AAA")
  const uint32_t buf[] = {'A','A','A',' ','B','B','B','\n'};
  test::ExpectEq("breaks at word boundary on overflow",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     5, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthNoWordBoundaryHardBreak() {
  // Single long word "AAAA" = 4px, available = 3; no space → hard break
  const uint32_t buf[] = {'A','A','A','A','\n'};
  test::ExpectEq("hard break when no word boundary",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     3, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthSpaceAtOverflowEdge() {
  // "AA " = 3px, available = 3; space is the overflowing char itself
  const uint32_t buf[] = {'A','A',' ','B','\n'};
  test::ExpectEq("trailing space at overflow returns pre-space width",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     3, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthTracksStyleChanges() {
  // 'A'=1px, BOLD_ON, 'B'=2px, BOLD_OFF, ' '=1px, 'C'=1px; available=4
  // "AB " = 1+2+1=4px; 'C' would be 5 -> break before 'C' at space boundary
  // last_word_end_width = 3 ("AB" before space)
  const uint32_t buf[] = {'A', TEXT_BOLD_ON, 'B', TEXT_BOLD_OFF, ' ', 'C', '\n'};
  test::ExpectEq("style changes tracked in visual line measurement",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     4, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthStopsAtParagraphAlignToken() {
  // Simulates the suppressed-newline case: measurement starts at 'A' (the
  // current glyph, mirroring the renderer's i-1 after i++) and hits a
  // paragraph boundary token mid-line. The scan must stop at the token and
  // return only the width of the glyphs seen so far.
  const uint32_t buf[] = {'A', 'A', TEXT_PARAGRAPH_CENTER, 'B', 'B', '\n'};
  test::ExpectEq("paragraph-align token stops visual line measurement",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     10, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthSkipsControlTokens() {
  const uint32_t buf[] = {TEXT_LINK_START, 99, 'A', TEXT_LINK_END, 'B', '\n'};
  test::ExpectEq("visual line: link markers ignored",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     10, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthMultipleWordBoundaries() {
  // "A B C D" each 1px, spaces 1px; available = 5
  // "A " = 2, "A B " = 4, "A B C" = 5, "A B C " = 6 > 5
  // last_word_end_width at second space = 3 ("A B" before second space)
  // then 'C' fits (4), third space overflows (5 exact = fits), 'D' overflows at 6
  // Actually let's trace carefully:
  // scan 'A'=1: line=1, no overflow
  // scan ' '=1: last_word_end=1, line=2
  // scan 'B'=1: line=3, no overflow
  // scan ' '=1: last_word_end=3, line=4
  // scan 'C'=1: line=5, no overflow (5 <= 5)
  // scan ' '=1: last_word_end=5, line=6 > 5 → overflow at space → return 5
  const uint32_t buf[] = {'A',' ','B',' ','C',' ','D','\n'};
  test::ExpectEq("last word boundary before overflow used",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     5, MeasureGlyph, NULL),
                 5);
}

void TestBandImageAlignUsesExplicitModeFirst() {
  test::ExpectEq("explicit right align kept",
                 page_alignment_utils::ResolveBandImageAlignMode(2, 0), 2);
  test::ExpectEq("explicit left align kept",
                 page_alignment_utils::ResolveBandImageAlignMode(1, 2), 1);
}

void TestBandImageAlignUsesParagraphRight() {
  test::ExpectEq("paragraph right aligns band image",
                 page_alignment_utils::ResolveBandImageAlignMode(0, 2), 2);
}

void TestRightAlignIgnoresPublisherMarginStart() {
  test::ExpectEq("right align reaches base right edge",
                 page_alignment_utils::ComputeAlignedLineStartX(
                     10, 10, 30, 240, 40, 2),
                 190);
}

void TestCenterAlignIgnoresPublisherMarginStart() {
  test::ExpectEq("center align uses base content box",
                 page_alignment_utils::ComputeAlignedLineStartX(
                     10, 10, 30, 240, 40, 1),
                 100);
}

using page_alignment_utils::JustifyLineScan;

JustifyLineScan Scan(const uint32_t *buf, size_t n, bool bold = false) {
  return page_alignment_utils::ScanJustifyLine(buf, n, 0, bold, false, false,
                                               MeasureGlyph, NULL);
}

void TestJustifyScanCountsGapsAndIgnoresTrailingSpace() {
  // "ab cd " + soft wrap: two words, one gap, trailing space not counted.
  const uint32_t buf[] = {'a', 'b', ' ', 'c', 'd', ' ', TEXT_SOFT_WRAP, '\n'};
  const JustifyLineScan scan = Scan(buf, sizeof(buf) / sizeof(buf[0]));
  test::ExpectTrue("soft wrapped line is justifiable", scan.justifiable);
  test::ExpectEq("width excludes trailing space", scan.width, 5);
  test::ExpectEq("one inter-word gap", scan.gaps, 1);
}

void TestJustifyScanSkipsLastLineAndBr() {
  const uint32_t last[] = {'a', ' ', 'b', '\n'};
  test::ExpectFalse("hard newline (last line or <br>) is not justified",
                    Scan(last, sizeof(last) / sizeof(last[0])).justifiable);
  const uint32_t end_of_buffer[] = {'a', ' ', 'b', TEXT_SOFT_WRAP};
  test::ExpectFalse("line without its newline is not justified",
                    Scan(end_of_buffer, 4).justifiable);
  const uint32_t one_word[] = {'a', 'b', TEXT_SOFT_WRAP, '\n'};
  test::ExpectFalse("a single word has no gap to widen",
                    Scan(one_word, 4).justifiable);
}

void TestJustifyScanRejectsUnsafeLines() {
  const uint32_t image[] = {'a', ' ', TEXT_IMAGE, 7, ' ', 'b', TEXT_SOFT_WRAP, '\n'};
  test::ExpectFalse("inline image disables justification",
                    Scan(image, sizeof(image) / sizeof(image[0])).justifiable);
  const uint32_t font[] = {'a', ' ', TEXT_FONT_SIZE, 20, 'b', TEXT_SOFT_WRAP, '\n'};
  test::ExpectFalse("font size change disables justification",
                    Scan(font, sizeof(font) / sizeof(font[0])).justifiable);
  const uint32_t pre[] = {'a', ' ', TEXT_PRE_ON, 'b', TEXT_SOFT_WRAP, '\n'};
  test::ExpectFalse("preformatted text disables justification",
                    Scan(pre, sizeof(pre) / sizeof(pre[0])).justifiable);
  const uint32_t rtl[] = {TEXT_RTL_LINE_PX, 9, 'a', ' ', 'b', TEXT_SOFT_WRAP, '\n'};
  test::ExpectFalse("RTL line disables justification",
                    Scan(rtl, sizeof(rtl) / sizeof(rtl[0])).justifiable);
}

void TestJustifyScanTracksStyleAndLinks() {
  const uint32_t buf[] = {TEXT_LINK_START, 5, 'a', TEXT_LINK_END, ' ',
                          TEXT_BOLD_ON, 'b', TEXT_BOLD_OFF, TEXT_SOFT_WRAP, '\n'};
  const JustifyLineScan scan = Scan(buf, sizeof(buf) / sizeof(buf[0]));
  test::ExpectTrue("links and styles keep the line justifiable", scan.justifiable);
  test::ExpectEq("bold glyph counted at its own width", scan.width, 1 + 1 + 2);
}

void TestJustifySpacingDistribution() {
  int base = -1, rem = -1;
  test::ExpectTrue("extra pixels are distributed",
                   page_alignment_utils::ComputeJustifySpacing(100, 108, 3, 0, &base, &rem));
  test::ExpectEq("base per gap", base, 2);
  test::ExpectEq("remainder goes to the first gaps", rem, 2);
  test::ExpectFalse("exact fit changes nothing",
                    page_alignment_utils::ComputeJustifySpacing(100, 100, 3, 0, &base, &rem));
  test::ExpectEq("no base when unchanged", base, 0);
  test::ExpectFalse("overflowing line is left alone",
                    page_alignment_utils::ComputeJustifySpacing(110, 100, 3, 0, &base, &rem));
  test::ExpectFalse("zero gaps cannot be distributed",
                    page_alignment_utils::ComputeJustifySpacing(50, 100, 0, 0, &base, &rem));
  test::ExpectFalse("a stretch above the cap is refused",
                    page_alignment_utils::ComputeJustifySpacing(50, 100, 2, 10, &base, &rem));
  test::ExpectTrue("a stretch at the cap is allowed",
                   page_alignment_utils::ComputeJustifySpacing(50, 70, 2, 10, &base, &rem));
  test::ExpectEq("cap boundary base", base, 10);
}

void TestMeasureStopsAtJustifyMarker() {
  const uint32_t buf[] = {'A', 'A', TEXT_PARAGRAPH_JUSTIFY, 'B', '\n'};
  test::ExpectEq("justify marker is a paragraph boundary",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
  const uint32_t soft[] = {'A', ' ', 'B', TEXT_SOFT_WRAP, '\n'};
  test::ExpectEq("soft wrap marker has no width",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     soft, sizeof(soft) / sizeof(soft[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 3);
}

} // namespace

int main() {
  TestJustifyScanCountsGapsAndIgnoresTrailingSpace();
  TestJustifyScanSkipsLastLineAndBr();
  TestJustifyScanRejectsUnsafeLines();
  TestJustifyScanTracksStyleAndLinks();
  TestJustifySpacingDistribution();
  TestMeasureStopsAtJustifyMarker();
  TestIgnoresControlTokensAndPayloads();
  TestTracksStyleChangesWhileMeasuring();
  TestStopsAtBlockTokens();
  TestIgnoresFontSizeTokenAndPayload();
  TestIgnoresImageAlignTokenAndPayload();
  TestIgnoresLineStartXTokenAndPayload();
  TestVisualLineWidthFitsCompletely();
  TestVisualLineWidthBreaksAtWordBoundary();
  TestVisualLineWidthNoWordBoundaryHardBreak();
  TestVisualLineWidthSpaceAtOverflowEdge();
  TestVisualLineWidthTracksStyleChanges();
  TestVisualLineWidthStopsAtParagraphAlignToken();
  TestVisualLineWidthSkipsControlTokens();
  TestVisualLineWidthMultipleWordBoundaries();
  TestBandImageAlignUsesExplicitModeFirst();
  TestBandImageAlignUsesParagraphRight();
  TestRightAlignIgnoresPublisherMarginStart();
  TestCenterAlignIgnoresPublisherMarginStart();
  return 0;
}
